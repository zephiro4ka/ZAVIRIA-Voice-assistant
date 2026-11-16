package com.astrix.phone

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.net.Uri
import android.os.Build
import android.provider.Settings

// Обновление приложения с компьютера: свежий APK лежит рядом с ZAVIRIA на ПК (его кладёт установщик),
// телефон забирает его по зашифрованной связи и ставит сам — без USB и отладки.
// В режиме владельца устройства Android ставит молча, иначе один раз покажет «Обновить?».
object AppUpdate {
    fun installed(context: Context): Long {
        val info = context.packageManager.getPackageInfo(context.packageName, 0)
        return if (Build.VERSION.SDK_INT >= 28) info.longVersionCode else @Suppress("DEPRECATION") info.versionCode.toLong()
    }

    /** Версия на компьютере; null — ПК не ответил, 0 — APK рядом с программой нет */
    fun available(context: Context): Long? = PcLink.apkVersion(context)

    /** Можно ли ставить самим (Android 8+ спрашивает разрешение «Установка неизвестных приложений» один раз) */
    fun allowed(context: Context) = Build.VERSION.SDK_INT < 26 || context.packageManager.canRequestPackageInstalls() || Device(context).isOwner

    fun askPermission(context: Context) {
        if (Build.VERSION.SDK_INT >= 26)
            context.startActivity(Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:${context.packageName}")).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
    }

    /** Скачать и поставить. Возвращает текст ошибки или null. Вызывать не из главного потока. */
    fun install(context: Context): String? {
        val apk = PcLink.apk(context) ?: return "Компьютер не отдал приложение — проверь связь с ПК"
        val installer = context.packageManager.packageInstaller
        val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL).apply {
            setAppPackageName(context.packageName)
            if (Build.VERSION.SDK_INT >= 31) setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED)
        }
        return runCatching {
            val id = installer.createSession(params)
            installer.openSession(id).use { session ->
                session.openWrite("zaviria.apk", 0, apk.size.toLong()).use { it.write(apk); session.fsync(it) }
                val flags = PendingIntent.FLAG_UPDATE_CURRENT or (if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0)
                val done = PendingIntent.getBroadcast(context, id, Intent(context, UpdateReceiver::class.java), flags)
                session.commit(done.intentSender)
            }
            null
        }.getOrElse { "Не получилось начать установку: ${it.message}" }
    }
}

// Ответ установщика: если Android хочет подтверждения — показать его окно
class UpdateReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.getIntExtra(PackageInstaller.EXTRA_STATUS, -999) != PackageInstaller.STATUS_PENDING_USER_ACTION) return
        @Suppress("DEPRECATION")
        val confirm = (if (Build.VERSION.SDK_INT >= 33) intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent::class.java) else intent.getParcelableExtra(Intent.EXTRA_INTENT)) ?: return
        context.startActivity(confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
    }
}
