package com.astrix.phone

import android.annotation.SuppressLint
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ServiceInfo
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.media.AudioAttributes
import android.media.AudioManager
import android.media.MediaPlayer
import android.media.RingtoneManager
import android.os.BatteryManager
import android.os.Build
import android.os.Bundle
import android.os.IBinder
import android.os.Looper
import org.json.JSONObject
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlin.concurrent.thread

class GuardService : Service() {
    companion object {
        private const val CHANNEL = "guard"
        private const val TRACK_INTERVAL_MS = 5 * 60 * 1000L

        fun start(context: Context) {
            val intent = Intent(context, GuardService::class.java)
            if (Build.VERSION.SDK_INT >= 26) context.startForegroundService(intent) else context.startService(intent)
        }

        fun stop(context: Context) { context.stopService(Intent(context, GuardService::class.java)) }
    }

    @Volatile private var running = false
    private var siren: MediaPlayer? = null
    private val device by lazy { Device(this) }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        showNotification()
        if (!running) {
            running = true
            thread(name = "guard-bot") { pollLoop() }
            thread(name = "guard-track") { trackLoop() }
            thread(name = "guard-pc") { pcLoop() }
        }
        return START_STICKY
    }

    override fun onDestroy() {
        running = false
        stopSiren()
        super.onDestroy()
    }

    private fun showNotification() {
        val builder = if (Build.VERSION.SDK_INT >= 26) {
            val manager = getSystemService(NotificationManager::class.java)
            manager.createNotificationChannel(NotificationChannel(CHANNEL, "Защита ZAVIRIA", NotificationManager.IMPORTANCE_LOW))
            Notification.Builder(this, CHANNEL)
        } else {
            @Suppress("DEPRECATION") Notification.Builder(this)
        }
        val notification = builder.setSmallIcon(R.drawable.ic_zaviria).setContentTitle(if (store.getBoolean(Keys.ARMED, false)) "Защита ZAVIRIA включена" else "ZAVIRIA на связи с компьютером").setOngoing(true).build()
        if (Build.VERSION.SDK_INT >= 29) startForeground(1, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION)
        else startForeground(1, notification)
    }

    private fun bot(): Bot? = store.getString(Keys.TOKEN, "").orEmpty().takeIf { it.isNotEmpty() }?.let { Bot(it) }
    private fun owner(): String = store.getString(Keys.OWNER, "").orEmpty()

    private fun pollLoop() {
        var pause = 5000L
        while (running) {
            try {
                val bot = bot()
                if (bot == null || owner().isEmpty() || !store.getBoolean(Keys.ARMED, false)) { Thread.sleep(30000); continue } // команды бота работают только при включённой защите
                val offset = store.getLong(Keys.OFFSET, 0)
                val reply = bot.call("getUpdates", mapOf("offset" to offset.toString(), "timeout" to "30"), 45000)
                val updates = reply.optJSONArray("result")
                if (updates != null) for (i in 0 until updates.length()) {
                    val update = updates.getJSONObject(i)
                    store.edit().putLong(Keys.OFFSET, update.getLong("update_id") + 1).apply()
                    update.optJSONObject("message")?.let { runCatching { handle(bot, it) } }
                }
                pause = 5000L
            } catch (_: InterruptedException) {
                return
            } catch (_: Exception) {
                Thread.sleep(pause) // нет сети: пробуем снова, всё реже
                pause = (pause * 2).coerceAtMost(120000L)
            }
        }
    }

    private fun handle(bot: Bot, message: JSONObject) {
        val chat = message.getJSONObject("chat")
        val from = message.optJSONObject("from") ?: return
        val owner = owner()
        // Всё, что пришло не от владельца в личном чате, молча игнорируем
        if (chat.optString("type") != "private" || chat.getLong("id").toString() != owner || from.getLong("id").toString() != owner) return

        val parts = message.optString("text").trim().split(Regex("\\s+"), limit = 3)
        val command = parts[0].substringBefore('@').lowercase()
        if (command == "/start" || command == "/help") {
            bot.send(owner, "Команды, после каждой код из Authenticator:\n/status код\n/lock код сообщение на экране\n/locate код\n/track_on код\n/track_off код\n/siren код\n/siren_off код")
            return
        }
        if (!command.startsWith("/")) return
        val minutes = Auth.blockedMinutes(this)
        if (minutes > 0) { bot.send(owner, "Слишком много ошибок. Подожди $minutes мин."); return }
        if (parts.size < 2 || !Auth.check(this, parts[1])) { bot.send(owner, "Неверный код."); return }
        val rest = parts.getOrNull(2)

        when (command) {
            "/status" -> bot.send(owner, statusText())
            "/lock" -> { device.lock(rest); bot.send(owner, if (device.isAdmin) "Телефон заблокирован." else "Нет прав администратора устройства.") }
            "/locate" -> sendLocation(bot, owner)
            "/track_on" -> { store.edit().putBoolean(Keys.TRACKING, true).apply(); bot.send(owner, "Слежение включено: координаты каждые 5 минут."); sendLocation(bot, owner) }
            "/track_off" -> { store.edit().putBoolean(Keys.TRACKING, false).apply(); bot.send(owner, "Слежение выключено.") }
            "/siren" -> { startSiren(); bot.send(owner, "Сирена включена.") }
            "/siren_off" -> { stopSiren(); bot.send(owner, "Сирена выключена.") }
            else -> bot.send(owner, "Не знаю такую команду.")
        }
    }

    private fun statusText(): String {
        val battery = registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        val level = battery?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
        val manager = getSystemService(Context.LOCATION_SERVICE) as LocationManager
        val locationOn = manager.isProviderEnabled(LocationManager.GPS_PROVIDER) || manager.isProviderEnabled(LocationManager.NETWORK_PROVIDER)
        return "Заряд: $level%\nГеолокация: ${if (locationOn) "включена" else "выключена"}\nРежим владельца: ${if (device.isOwner) "да" else "нет"}\nСлежение: ${if (store.getBoolean(Keys.TRACKING, false)) "да" else "нет"}"
    }

    // Команды защиты из настроек на компьютере: отдельный бот для них не нужен
    private fun pcLoop() {
        while (running) {
            try {
                Thread.sleep(8000)
                if (!PcLink.isPaired(this)) continue
                val tasks = PcLink.tasks(this)
                for (i in 0 until tasks.length()) {
                    val task = tasks.getJSONObject(i)
                    if (task.optString("name") == "unlink") { unlinkTask(task.optJSONObject("state") ?: JSONObject()); continue }
                    runPcTask(task.optString("name"), task.optString("arg").ifEmpty { null })
                }
            } catch (_: InterruptedException) {
                return
            } catch (_: Exception) {
            }
        }
    }

    // Компьютер попросил отключить телефон: подтвердить можно только в приложении, отпечатком
    private var unlinkNotified = false
    private fun unlinkTask(state: JSONObject) {
        if (state.optBoolean("done")) { PcLink.forget(this); return }
        if (!state.optBoolean("pc") || state.optBoolean("phone") || unlinkNotified) return
        unlinkNotified = true
        val manager = getSystemService(NotificationManager::class.java)
        val open = android.app.PendingIntent.getActivity(this, 7, android.content.Intent(this, MainActivity::class.java), android.app.PendingIntent.FLAG_IMMUTABLE)
        val builder = if (Build.VERSION.SDK_INT >= 26) {
            manager.createNotificationChannel(NotificationChannel("unlink", "Отключение от компьютера", NotificationManager.IMPORTANCE_HIGH))
            Notification.Builder(this, "unlink")
        } else @Suppress("DEPRECATION") Notification.Builder(this)
        manager.notify(7, builder.setSmallIcon(R.drawable.ic_zaviria).setContentTitle("Компьютер просит отключить телефон")
            .setContentText("Открой ZAVIRIA и подтверди, если это ты").setContentIntent(open).setAutoCancel(true).build())
    }

    private fun say(text: String) = PcLink.report(this, JSONObject().put("text", text))

    private fun runPcTask(name: String, arg: String?) {
        when (name) {
            "lock" -> { device.lock(arg); say(if (device.isAdmin) "Телефон заблокирован" else "Нет прав администратора") }
            "locate" -> reportPlace()
            "track_on" -> { store.edit().putBoolean(Keys.TRACKING, true).apply(); say("Слежение включено") }
            "track_off" -> { store.edit().putBoolean(Keys.TRACKING, false).apply(); say("Слежение выключено") }
            "siren" -> { startSiren(); say("Сирена включена") }
            "siren_off" -> { stopSiren(); say("Сирена выключена") }
        }
    }

    private fun reportPlace() {
        runCatching { device.turnLocationOn() }
        val location = findLocation()
        if (location == null) { say("Координаты получить не удалось"); return }
        PcLink.report(this, JSONObject().put("lat", location.latitude).put("lon", location.longitude)
            .put("text", "Телефон найден, точность ${location.accuracy.toInt()} м"))
    }

    private fun trackLoop() {
        while (running) {
            try {
                Thread.sleep(TRACK_INTERVAL_MS)
                if (store.getBoolean(Keys.TRACKING, false)) {
                    bot()?.let { sendLocation(it, owner()) }
                    if (PcLink.isPaired(this)) reportPlace()
                }
            } catch (_: InterruptedException) {
                return
            } catch (_: Exception) {
            }
        }
    }

    private fun sendLocation(bot: Bot, owner: String) {
        runCatching { device.turnLocationOn() }
        val location = findLocation()
        if (location == null) { bot.send(owner, "Координаты получить не удалось: геолокация выключена или нет разрешения."); return }
        bot.sendLocation(owner, location.latitude, location.longitude)
        val age = (System.currentTimeMillis() - location.time) / 60000
        bot.send(owner, "Точность ${location.accuracy.toInt()} м, источник ${location.provider}, данным $age мин.")
    }

    @SuppressLint("MissingPermission")
    private fun findLocation(): Location? {
        val manager = getSystemService(Context.LOCATION_SERVICE) as LocationManager
        val providers = listOf(LocationManager.GPS_PROVIDER, LocationManager.NETWORK_PROVIDER).filter { runCatching { manager.isProviderEnabled(it) }.getOrDefault(false) }
        var best: Location? = null
        val latch = CountDownLatch(1)
        val listener = object : LocationListener {
            override fun onLocationChanged(location: Location) {
                synchronized(this) { if (best == null || location.accuracy < best!!.accuracy) best = location }
                if (location.provider == LocationManager.GPS_PROVIDER) latch.countDown()
            }
            @Deprecated("Deprecated in Java") override fun onStatusChanged(provider: String?, status: Int, extras: Bundle?) {}
            override fun onProviderEnabled(provider: String) {}
            override fun onProviderDisabled(provider: String) {}
        }
        try {
            providers.forEach { manager.requestLocationUpdates(it, 0L, 0f, listener, Looper.getMainLooper()) }
            if (providers.isNotEmpty()) latch.await(45, TimeUnit.SECONDS)
        } catch (_: SecurityException) {
        } finally {
            manager.removeUpdates(listener)
        }
        best?.let { return it }
        return try {
            manager.allProviders.mapNotNull { manager.getLastKnownLocation(it) }.maxByOrNull { it.time }
        } catch (_: SecurityException) {
            null
        }
    }

    @Synchronized
    private fun startSiren() {
        if (siren != null) return
        val audio = getSystemService(Context.AUDIO_SERVICE) as AudioManager
        audio.setStreamVolume(AudioManager.STREAM_ALARM, audio.getStreamMaxVolume(AudioManager.STREAM_ALARM), 0)
        val uri = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_ALARM) ?: RingtoneManager.getDefaultUri(RingtoneManager.TYPE_RINGTONE)
        siren = MediaPlayer().apply {
            setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_ALARM).setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build())
            setDataSource(this@GuardService, uri)
            isLooping = true
            prepare()
            start()
        }
    }

    @Synchronized
    private fun stopSiren() {
        siren?.runCatching { stop(); release() }
        siren = null
    }

    private val store get() = (this as Context).store
}
