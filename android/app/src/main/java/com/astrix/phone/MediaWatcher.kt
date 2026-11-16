package com.astrix.phone

import android.content.ComponentName
import android.content.Context
import android.media.MediaMetadata
import android.media.session.MediaController
import android.media.session.MediaSessionManager
import android.media.session.PlaybackState
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.service.notification.NotificationListenerService
import org.json.JSONObject
import kotlin.concurrent.thread

// Что играет на телефоне → ПК → статус в Discord. Android отдаёт чужие плееры только «слушателю уведомлений»,
// поэтому это сервис-слушатель; сами уведомления не читаем и никуда не шлём.
class MediaWatcher : NotificationListenerService() {
    private var manager: MediaSessionManager? = null
    private val watched = mutableListOf<MediaController>()
    private var lastKey = ""
    private var lastSent = 0L
    private val handler = Handler(Looper.getMainLooper())
    private val tick = object : Runnable { override fun run() { send(); handler.postDelayed(this, 30_000) } } // ПК забывает трек через 90 с

    private val callback = object : MediaController.Callback() {
        override fun onMetadataChanged(metadata: MediaMetadata?) = send()
        override fun onPlaybackStateChanged(state: PlaybackState?) = send()
    }
    private val sessions = MediaSessionManager.OnActiveSessionsChangedListener { list -> watch(list.orEmpty()) }

    override fun onListenerConnected() {
        val m = getSystemService(MediaSessionManager::class.java) ?: return
        manager = m
        val me = ComponentName(this, MediaWatcher::class.java)
        runCatching {
            m.addOnActiveSessionsChangedListener(sessions, me)
            watch(m.getActiveSessions(me))
        }
        handler.removeCallbacks(tick)
        handler.postDelayed(tick, 30_000)
    }

    override fun onListenerDisconnected() {
        handler.removeCallbacks(tick)
        runCatching { manager?.removeOnActiveSessionsChangedListener(sessions) }
        watched.forEach { runCatching { it.unregisterCallback(callback) } }
        watched.clear()
    }

    private fun watch(list: List<MediaController>) {
        watched.forEach { runCatching { it.unregisterCallback(callback) } }
        watched.clear()
        list.forEach { runCatching { it.registerCallback(callback); watched.add(it) } }
        send()
    }

    private fun send() {
        if (!PcLink.isPaired(this)) return
        val playing = watched.firstOrNull { it.playbackState?.state == PlaybackState.STATE_PLAYING }
        val c = playing ?: watched.firstOrNull()
        val meta = c?.metadata
        val title = meta?.getString(MediaMetadata.METADATA_KEY_TITLE).orEmpty()
        val artist = meta?.getString(MediaMetadata.METADATA_KEY_ARTIST) ?: meta?.getString(MediaMetadata.METADATA_KEY_ALBUM_ARTIST).orEmpty()
        val app = c?.packageName?.let { pkg ->
            runCatching { packageManager.getApplicationLabel(packageManager.getApplicationInfo(pkg, 0)).toString() }.getOrDefault(pkg)
        }.orEmpty()
        val body = JSONObject()
            .put("present", c != null && title.isNotEmpty())
            .put("playing", playing != null)
            .put("title", title)
            .put("artist", artist)
            .put("app", app)
            .put("position", (c?.playbackState?.position ?: 0L) / 1000.0)
            .put("duration", (meta?.getLong(MediaMetadata.METADATA_KEY_DURATION) ?: 0L) / 1000.0)
        // ПК забывает трек через 90 с — одинаковое состояние повторяем раз в 30 с, новое шлём сразу
        val key = "${body.optBoolean("playing")}|$title|$artist"
        val now = System.currentTimeMillis()
        if (key == lastKey && now - lastSent < 30_000) return
        lastKey = key
        lastSent = now
        val context = applicationContext
        thread { PcLink.media(context, body) }
    }

    companion object {
        fun enabled(context: Context): Boolean {
            val flat = Settings.Secure.getString(context.contentResolver, "enabled_notification_listeners").orEmpty()
            return flat.contains(ComponentName(context, MediaWatcher::class.java).flattenToString())
        }
    }
}
