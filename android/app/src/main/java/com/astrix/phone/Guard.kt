package com.astrix.phone

import android.Manifest
import android.app.admin.DeviceAdminReceiver
import android.app.admin.DevicePolicyManager
import android.content.BroadcastReceiver
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.os.Build
import android.os.UserManager
import org.json.JSONObject
import java.io.OutputStreamWriter
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder
import java.security.MessageDigest
import java.security.SecureRandom
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

class AdminReceiver : DeviceAdminReceiver()

class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (context.store.getBoolean(Keys.ARMED, false) || PcLink.isPaired(context)) GuardService.start(context)
    }
}

object Keys {
    const val TOKEN = "token"
    const val OWNER = "owner"
    const val SECRET = "secret"
    const val ENROLLED = "enrolled"
    const val RECOVERY = "recovery"
    const val LAST_STEP = "last_step"
    const val FAILS = "fails"
    const val BLOCKED_UNTIL = "blocked_until"
    const val ARMED = "armed"
    const val OFFSET = "offset"
    const val TRACKING = "tracking"
}

// Защита должна работать и после перезагрузки, пока экран ещё ни разу не разблокирован,
// поэтому настройки лежат в хранилище, доступном до первого ввода пароля.
val Context.store: SharedPreferences
    get() = (if (Build.VERSION.SDK_INT >= 24) createDeviceProtectedStorageContext() else this)
        .getSharedPreferences("guard", Context.MODE_PRIVATE)

object Totp {
    private const val ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"

    fun newSecret(): String {
        val bytes = ByteArray(20).also { SecureRandom().nextBytes(it) }
        val out = StringBuilder()
        var buffer = 0
        var bits = 0
        for (b in bytes) {
            buffer = (buffer shl 8) or (b.toInt() and 0xFF)
            bits += 8
            while (bits >= 5) {
                out.append(ALPHABET[(buffer shr (bits - 5)) and 31])
                bits -= 5
            }
            buffer = buffer and ((1 shl bits) - 1)
        }
        if (bits > 0) out.append(ALPHABET[(buffer shl (5 - bits)) and 31])
        return out.toString()
    }

    private fun decode(secret: String): ByteArray {
        val out = ArrayList<Byte>()
        var buffer = 0
        var bits = 0
        for (c in secret) {
            val v = ALPHABET.indexOf(c)
            if (v < 0) continue
            buffer = (buffer shl 5) or v
            bits += 5
            if (bits >= 8) {
                out.add(((buffer shr (bits - 8)) and 0xFF).toByte())
                bits -= 8
                buffer = buffer and ((1 shl bits) - 1)
            }
        }
        return out.toByteArray()
    }

    fun code(secret: String, step: Long): String {
        val mac = Mac.getInstance("HmacSHA1").apply { init(SecretKeySpec(decode(secret), "HmacSHA1")) }
        val message = ByteArray(8) { i -> (step shr ((7 - i) * 8)).toByte() }
        val hash = mac.doFinal(message)
        val offset = hash[hash.size - 1].toInt() and 15
        val number = ((hash[offset].toInt() and 0x7F) shl 24) or ((hash[offset + 1].toInt() and 0xFF) shl 16) or
            ((hash[offset + 2].toInt() and 0xFF) shl 8) or (hash[offset + 3].toInt() and 0xFF)
        return (number % 1_000_000).toString().padStart(6, '0')
    }

    fun nowStep(): Long = System.currentTimeMillis() / 1000 / 30
}

object Auth {
    private const val MAX_FAILS = 5
    private const val BLOCK_MS = 15 * 60 * 1000L

    private fun hash(text: String): String =
        MessageDigest.getInstance("SHA-256").digest(text.toByteArray()).joinToString("") { "%02x".format(it) }

    fun isEnrolled(context: Context) = context.store.getBoolean(Keys.ENROLLED, false)

    fun newRecoveryCodes(context: Context): List<String> {
        val random = SecureRandom()
        val codes = List(8) { (1..10).map { random.nextInt(10) }.joinToString("") }
        context.store.edit().putStringSet(Keys.RECOVERY, codes.map { hash(it) }.toSet()).apply()
        return codes
    }

    fun blockedMinutes(context: Context): Long {
        val left = context.store.getLong(Keys.BLOCKED_UNTIL, 0) - System.currentTimeMillis()
        return if (left > 0) left / 60000 + 1 else 0
    }

    // Проверяет код из приложения-аутентификатора или одноразовый код восстановления.
    // Один и тот же код дважды не сработает, после пяти ошибок проверка закрывается на 15 минут.
    @Synchronized
    fun check(context: Context, input: String): Boolean {
        val store = context.store
        if (blockedMinutes(context) > 0) return false
        val code = input.trim()
        var ok = false
        if (code.length == 6) {
            val secret = store.getString(Keys.SECRET, "").orEmpty()
            val last = store.getLong(Keys.LAST_STEP, 0)
            val now = Totp.nowStep()
            for (step in now - 1..now + 1) {
                if (secret.isNotEmpty() && step > last && Totp.code(secret, step) == code) {
                    store.edit().putLong(Keys.LAST_STEP, step).apply()
                    ok = true
                    break
                }
            }
        } else if (code.length == 10) {
            val codes = store.getStringSet(Keys.RECOVERY, emptySet()).orEmpty()
            val hashed = hash(code)
            if (hashed in codes) {
                store.edit().putStringSet(Keys.RECOVERY, codes - hashed).apply()
                ok = true
            }
        }
        if (ok) {
            store.edit().putInt(Keys.FAILS, 0).apply()
        } else {
            val fails = store.getInt(Keys.FAILS, 0) + 1
            val edit = store.edit().putInt(Keys.FAILS, fails)
            if (fails >= MAX_FAILS) edit.putInt(Keys.FAILS, 0).putLong(Keys.BLOCKED_UNTIL, System.currentTimeMillis() + BLOCK_MS)
            edit.apply()
        }
        return ok
    }
}

class Device(private val context: Context) {
    private val dpm = context.getSystemService(Context.DEVICE_POLICY_SERVICE) as DevicePolicyManager
    val admin = ComponentName(context, AdminReceiver::class.java)

    val isOwner get() = dpm.isDeviceOwnerApp(context.packageName)
    val isAdmin get() = dpm.isAdminActive(admin)

    private val restrictions: List<String>
        get() {
            val list = mutableListOf(UserManager.DISALLOW_FACTORY_RESET, UserManager.DISALLOW_SAFE_BOOT)
            if (Build.VERSION.SDK_INT >= 28) {
                list += UserManager.DISALLOW_AIRPLANE_MODE
                list += UserManager.DISALLOW_CONFIG_LOCATION
            }
            return list
        }

    fun lock(message: String?) {
        if (isOwner && Build.VERSION.SDK_INT >= 24 && !message.isNullOrBlank()) dpm.setDeviceOwnerLockScreenInfo(admin, message)
        if (isAdmin) dpm.lockNow()
    }

    fun clearLockMessage() {
        if (isOwner && Build.VERSION.SDK_INT >= 24) dpm.setDeviceOwnerLockScreenInfo(admin, null)
    }

    fun turnLocationOn() {
        if (isOwner && Build.VERSION.SDK_INT >= 30) dpm.setLocationEnabled(admin, true)
    }

    fun arm() {
        if (!isOwner) return
        restrictions.forEach { dpm.addUserRestriction(admin, it) }
        dpm.setUninstallBlocked(admin, context.packageName, true)
        val permissions = mutableListOf(Manifest.permission.ACCESS_FINE_LOCATION, Manifest.permission.ACCESS_COARSE_LOCATION)
        if (Build.VERSION.SDK_INT >= 29) permissions += Manifest.permission.ACCESS_BACKGROUND_LOCATION
        if (Build.VERSION.SDK_INT >= 33) permissions += Manifest.permission.POST_NOTIFICATIONS
        permissions.forEach {
            runCatching { dpm.setPermissionGrantState(admin, context.packageName, it, DevicePolicyManager.PERMISSION_GRANT_STATE_GRANTED) }
        }
    }

    fun disarm() {
        if (!isOwner) return
        restrictions.forEach { dpm.clearUserRestriction(admin, it) }
        dpm.setUninstallBlocked(admin, context.packageName, false)
    }

    fun removeOwner() {
        if (!isOwner) return
        disarm()
        clearLockMessage()
        dpm.clearDeviceOwnerApp(context.packageName)
    }
}

// Отдельный бот только для защиты: токен основного бота ZAVIRIA на телефон не попадает
class Bot(private val token: String) {
    fun call(method: String, params: Map<String, String>, readTimeoutMs: Int = 20000): JSONObject {
        val connection = URL("https://api.telegram.org/bot$token/$method").openConnection() as HttpURLConnection
        try {
            connection.requestMethod = "POST"
            connection.connectTimeout = 15000
            connection.readTimeout = readTimeoutMs
            connection.doOutput = true
            connection.setRequestProperty("Content-Type", "application/x-www-form-urlencoded")
            OutputStreamWriter(connection.outputStream, Charsets.UTF_8).use { writer ->
                writer.write(params.entries.joinToString("&") { "${it.key}=${URLEncoder.encode(it.value, "UTF-8")}" })
            }
            val stream = if (connection.responseCode in 200..299) connection.inputStream else connection.errorStream
            return JSONObject(stream.bufferedReader().use { it.readText() })
        } finally {
            connection.disconnect()
        }
    }

    fun send(chat: String, text: String) { call("sendMessage", mapOf("chat_id" to chat, "text" to text)) }

    fun sendLocation(chat: String, latitude: Double, longitude: Double) {
        call("sendLocation", mapOf("chat_id" to chat, "latitude" to latitude.toString(), "longitude" to longitude.toString()))
    }
}
