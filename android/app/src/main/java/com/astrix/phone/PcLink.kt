package com.astrix.phone

import android.content.Context
import android.os.Build
import android.provider.Settings
import android.util.Base64
import org.json.JSONArray
import org.json.JSONObject
import java.io.OutputStreamWriter
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import java.security.SecureRandom
import javax.crypto.Cipher
import javax.crypto.Mac
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

// Связь с компьютером: команды подписаны общим ключом, который появляется только после ввода кода с экрана ПК
object PcLink {
    private const val URL_KEY = "pc_url"
    private const val PAIR_KEY = "pc_key"

    fun isPaired(context: Context) = !context.store.getString(PAIR_KEY, "").isNullOrEmpty()

    private fun hex(bytes: ByteArray) = bytes.joinToString("") { "%02x".format(it) }

    private fun deviceName(context: Context): String =
        Settings.Global.getString(context.contentResolver, "device_name")?.takeIf { it.isNotBlank() } ?: "${Build.MANUFACTURER} ${Build.MODEL}"

    private fun request(url: String, method: String, path: String, body: String, headers: Map<String, String> = emptyMap()): Pair<Int, String> {
        val connection = URL(url.trimEnd('/') + path).openConnection() as HttpURLConnection
        try {
            connection.requestMethod = method
            connection.connectTimeout = 10000
            connection.readTimeout = 30000
            headers.forEach { (k, v) -> connection.setRequestProperty(k, v) }
            if (method == "POST") {
                connection.doOutput = true
                connection.setRequestProperty("Content-Type", "application/json")
                OutputStreamWriter(connection.outputStream, Charsets.UTF_8).use { it.write(body) }
            }
            val code = connection.responseCode
            val stream = if (code in 200..299) connection.inputStream else connection.errorStream
            return code to (stream?.bufferedReader()?.use { it.readText() } ?: "")
        } finally {
            connection.disconnect()
        }
    }

    private fun hmac(key: ByteArray, text: String) = Mac.getInstance("HmacSHA256").apply { init(SecretKeySpec(key, "HmacSHA256")) }.doFinal(text.toByteArray())

    // Сквозное шифрование поверх туннеля: Cloudflare и любой посредник видят только шифротекст
    private fun e2eKey(key: String) = MessageDigest.getInstance("SHA-256").digest("zaviria-e2e$key".toByteArray())

    private fun seal(aes: ByteArray, text: String): String {
        val nonce = ByteArray(12).also { SecureRandom().nextBytes(it) }
        val cipher = Cipher.getInstance("AES/GCM/NoPadding").apply { init(Cipher.ENCRYPT_MODE, SecretKeySpec(aes, "AES"), GCMParameterSpec(128, nonce)) }
        return Base64.encodeToString(nonce + cipher.doFinal(text.toByteArray()), Base64.NO_WRAP)
    }

    private fun open(aes: ByteArray, sealed: String): String? = runCatching {
        val raw = Base64.decode(sealed, Base64.DEFAULT)
        val cipher = Cipher.getInstance("AES/GCM/NoPadding").apply { init(Cipher.DECRYPT_MODE, SecretKeySpec(aes, "AES"), GCMParameterSpec(128, raw, 0, 12)) }
        String(cipher.doFinal(raw, 12, raw.size - 12))
    }.getOrNull()

    // Подпись + одноразовая метка (повтор перехваченного запроса не пройдёт) + шифрование тела и ответа
    private fun signed(context: Context, url: String, method: String, path: String, body: String): Pair<Int, String> {
        val key = context.store.getString(PAIR_KEY, "").orEmpty()
        val aes = e2eKey(key)
        val time = (System.currentTimeMillis() / 1000).toString()
        val nonce = hex(ByteArray(16).also { SecureRandom().nextBytes(it) })
        val payload = if (body.isEmpty()) "" else seal(aes, body)
        val sign = hex(hmac(key.toByteArray(), "$time\n$nonce\n$method\n$path\n$payload"))
        val (status, text) = request(url, method, path, payload, mapOf("X-Time" to time, "X-Nonce" to nonce, "X-Sign" to sign))
        val plain = open(aes, text) ?: return status to "" // не от нашего компьютера или ошибка до проверки подписи
        if (!plain.startsWith("$nonce\n")) return 403 to "" // ответ на другой запрос — подмена
        return status to plain.substring(nonce.length + 1)
    }

    // Адрес компьютера меняется после перезапуска: новый лежит зашифрованным в описании бота защиты
    private fun refreshUrl(context: Context): String? {
        val token = context.store.getString(Keys.TOKEN, "").orEmpty()
        val key = context.store.getString(PAIR_KEY, "").orEmpty()
        if (token.isEmpty() || key.isEmpty()) return null
        val sealed = Bot(token).call("getMyDescription", emptyMap()).optJSONObject("result")?.optString("description").orEmpty()
        if (sealed.isEmpty()) return null
        val raw = Base64.decode(sealed, Base64.DEFAULT)
        val aes = MessageDigest.getInstance("SHA-256").digest(("zaviria-url$key").toByteArray())
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE, SecretKeySpec(aes, "AES"), GCMParameterSpec(128, raw, 0, 12))
        val url = String(cipher.doFinal(raw, 12, raw.size - 12))
        context.store.edit().putString(URL_KEY, url).apply()
        return url
    }

    private fun call(context: Context, method: String, path: String, body: String): Pair<Int, String> {
        val url = context.store.getString(URL_KEY, "").orEmpty()
        val first = runCatching { signed(context, url, method, path, body) }.getOrNull()
        if (first != null && first.first !in 502..530) return first
        val fresh = refreshUrl(context) ?: return first ?: throw java.io.IOException("no pc")
        return signed(context, fresh, method, path, body)
    }

    // Возвращает текст ошибки или null, если всё получилось
    // Привязка по SPAKE2: код с экрана — пароль, по сети идут только X и Y; ключ получается у обоих сразу
    fun pair(context: Context, address: String, code: String): String? {
        val url = Regex("https?://[^\\s]+").find(address)?.value?.trimEnd('/', '.', ',') ?: return "Не вижу адрес. Скопируй сообщение бота."
        val exchange = Spake2.Exchange(code.replace(" ", "").trim().uppercase())
        val (status, text) = request(url, "POST", "/phone/pair", JSONObject().put("x", exchange.message).put("name", deviceName(context)).toString())
        val reply = runCatching { JSONObject(text) }.getOrDefault(JSONObject())
        if (status != 200) return reply.optString("error").ifEmpty { "Компьютер не отвечает" }
        val shared = exchange.finish(reply.optString("y")) ?: return "Компьютер ответил неверно"
        // Компьютер доказал, что знает тот же код; иначе это чужой сервер или код введён с ошибкой
        if (hex(hmac(shared, "zaviria-pc-confirm")) != reply.optString("confirm")) return "Неверный код."
        val (status2, text2) = request(url, "POST", "/phone/pair/finish", JSONObject().put("proof", hex(hmac(shared, "zaviria-phone-confirm"))).toString())
        val done = runCatching { JSONObject(text2) }.getOrDefault(JSONObject())
        if (status2 != 200) return done.optString("error").ifEmpty { "Компьютер не отвечает" }
        val key = hex(hmac(shared, "zaviria-key"))
        context.store.edit().putString(URL_KEY, url).putString(PAIR_KEY, key).apply()
        open(e2eKey(key), done.optString("backup"))?.let { restore(context, it) }
        sendGuard(context)
        backup(context)
        return null
    }

    // Токен бота защиты нужен компьютеру, чтобы публиковать новый адрес; идёт только по зашифрованной связи
    fun sendGuard(context: Context): String? {
        val token = context.store.getString(Keys.TOKEN, "").orEmpty()
        if (!isPaired(context)) return null
        val (status, text) = runCatching { call(context, "POST", "/phone/guard", JSONObject().put("guard", token).toString()) }.getOrElse { return "Компьютер не отвечает" }
        return if (status == 200) null else runCatching { JSONObject(text).optString("error") }.getOrDefault("").ifEmpty { "Компьютер не принял токен" }
    }

    // После переустановки возвращаем 2FA с компьютера: сюда попадают только прошедшие код с его экрана
    private fun restore(context: Context, blob: String) {
        if (blob.isEmpty() || Auth.isEnrolled(context)) return
        val data = runCatching { JSONObject(blob) }.getOrNull() ?: return
        val hashes = data.optJSONArray("recovery") ?: JSONArray()
        context.store.edit().putString(Keys.SECRET, data.getString("secret")).putBoolean(Keys.ENROLLED, true)
            .putLong(Keys.LAST_STEP, Totp.nowStep()).putStringSet(Keys.RECOVERY, (0 until hashes.length()).map { hashes.getString(it) }.toSet()).apply()
    }

    fun backup(context: Context) {
        if (!isPaired(context) || !Auth.isEnrolled(context)) return
        val store = context.store
        val blob = JSONObject().put("secret", store.getString(Keys.SECRET, "")).put("recovery", JSONArray(store.getStringSet(Keys.RECOVERY, emptySet()).orEmpty().toList())).toString()
        runCatching { call(context, "POST", "/phone/backup", JSONObject().put("blob", blob).toString()) }
    }

    fun command(context: Context, text: String): Boolean =
        runCatching { call(context, "POST", "/phone/cmd", JSONObject().put("text", text).toString()).first == 200 }.getOrDefault(false)

    fun poll(context: Context): JSONArray {
        val (status, text) = call(context, "GET", "/phone/poll", "")
        return if (status == 200) JSONArray(text) else JSONArray()
    }

    fun status(context: Context): JSONObject? = runCatching { val (s, t) = call(context, "GET", "/phone/status", ""); if (s == 200) JSONObject(t) else null }.getOrNull()
        ?.also { saveWol(context, it) }

    // ------------------------------------------------------------ включение ПК по сети (Wake-on-LAN)
    // Компьютер в статусе сообщает MAC сетевой карты и адрес рассылки своей сети: запоминаем, пока он включён
    private fun saveWol(context: Context, s: JSONObject) {
        val wol = s.optJSONObject("wol") ?: return
        val mac = wol.optString("mac")
        if (!Regex("^([0-9A-F]{2}:){5}[0-9A-F]{2}$").matches(mac)) return
        context.store.edit().putString("wol_mac", mac).putString("wol_bcast", wol.optString("bcast")).apply()
    }

    fun wolMac(context: Context) = context.store.getString("wol_mac", "").orEmpty()
    fun wolHost(context: Context) = context.store.getString("wol_host", "").orEmpty()
    fun setWolHost(context: Context, host: String) { context.store.edit().putString("wol_host", host.trim()).apply() }

    /** «Волшебный пакет»: 6×FF и 16 раз MAC. Дома — рассылкой по Wi-Fi; издалека — на адрес роутера (если он пересылает порт). */
    fun wake(context: Context): Int {
        val mac = wolMac(context).split(":").map { it.toInt(16).toByte() }
        if (mac.size != 6) return 0
        val packet = ByteArray(6) { 0xFF.toByte() } + (1..16).flatMap { mac }.toByteArray()
        val targets = mutableListOf("255.255.255.255" to 9)
        context.store.getString("wol_bcast", "").orEmpty().takeIf { it.isNotEmpty() }?.let { targets += it to 9 }
        val far = wolHost(context)
        if (far.isNotEmpty()) {
            val host = if (far.contains(":")) far.substringBeforeLast(":") else far
            val port = if (far.contains(":")) far.substringAfterLast(":").toIntOrNull() ?: 9 else 9
            targets += host to port
        }
        var sent = 0
        java.net.DatagramSocket().use { socket ->
            socket.broadcast = true
            for ((host, port) in targets) runCatching {
                val address = java.net.InetAddress.getByName(host)
                repeat(3) { socket.send(java.net.DatagramPacket(packet, packet.size, address, port)) }
                sent++
            }
        }
        return sent
    }
    fun config(context: Context): JSONObject? = runCatching { val (s, t) = call(context, "GET", "/phone/config", ""); if (s == 200) JSONObject(t) else null }.getOrNull()
    fun action(context: Context, name: String, arg: String = ""): String? = runCatching {
        val (_, t) = call(context, "POST", "/phone/action", JSONObject().put("a", name).put("arg", arg).toString())
        JSONObject(t).let { it.optString("text").ifEmpty { it.optString("error") } }
    }.getOrNull()

    /** Все программы компьютера из «Пуска» (и из Store) — для вкладки «Запуск». null — компьютер не ответил. */
    fun apps(context: Context): List<String>? = runCatching {
        val (s, t) = call(context, "GET", "/phone/apps", "")
        if (s != 200) null else JSONArray(t).let { list -> (0 until list.length()).map { list.getString(it) } }
    }.getOrNull()

    fun address(context: Context) = context.store.getString(URL_KEY, "").orEmpty()

    /** Что играет на телефоне — ПК покажет это в статусе Discord */
    fun media(context: Context, body: JSONObject) { runCatching { call(context, "POST", "/phone/media", body.toString()) } }

    /** Версия приложения, которая лежит рядом с ZAVIRIA на ПК (0 — нет), null — ПК не ответил */
    fun apkVersion(context: Context): Long? = runCatching {
        val (s, t) = call(context, "GET", "/phone/apk/info", "")
        if (s == 200) JSONObject(t).optLong("version") else null
    }.getOrNull()

    fun apk(context: Context): ByteArray? = runCatching {
        val (s, t) = call(context, "GET", "/phone/apk", "")
        if (s != 200) null else Base64.decode(JSONObject(t).optString("apk"), Base64.DEFAULT).takeIf { it.size > 1000 }
    }.getOrNull()

    fun remoteSession(context: Context): String? {
        val (status, text) = call(context, "POST", "/phone/session", "{}")
        return if (status == 200) JSONObject(text).optString("session").ifEmpty { null } else null
    }

    fun tasks(context: Context): JSONArray {
        val (status, text) = call(context, "GET", "/phone/tasks", "")
        return if (status == 200) JSONArray(text) else JSONArray()
    }

    fun report(context: Context, data: JSONObject) { runCatching { call(context, "POST", "/phone/report", data.toString()) } }

    // Компьютер рядом сам сообщает о себе в сети Wi-Fi, пока открыт код на его экране
    /** Приглашение с ПК: пока там открыт QR «установить на телефон», в рассылке есть и код — адрес и код, null — не услышали */
    fun invite(context: Context): Pair<String, String>? {
        val wifi = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as android.net.wifi.WifiManager
        val lock = wifi.createMulticastLock("zaviria").apply { setReferenceCounted(false); acquire() }
        val socket = java.net.DatagramSocket(null).apply { reuseAddress = true; broadcast = true; soTimeout = 2000; bind(java.net.InetSocketAddress(47820)) }
        try {
            val deadline = System.currentTimeMillis() + 8000
            while (System.currentTimeMillis() < deadline) {
                val packet = java.net.DatagramPacket(ByteArray(1024), 1024)
                try { socket.receive(packet) } catch (_: java.net.SocketTimeoutException) { continue }
                val parts = String(packet.data, 0, packet.length).split("\n")
                if (parts.size >= 4 && parts[0] == "ZAVIRIA1" && (parts[1].startsWith("https://") || parts[1].startsWith("http://")) && parts[3].isNotBlank()) return parts[1] to parts[3].trim()
            }
            return null
        } finally {
            socket.close()
            lock.release()
        }
    }

    fun discover(context: Context): Pair<String, String>? {
        val wifi = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as android.net.wifi.WifiManager
        val lock = wifi.createMulticastLock("zaviria").apply { setReferenceCounted(false); acquire() }
        val socket = java.net.DatagramSocket(null).apply { reuseAddress = true; broadcast = true; soTimeout = 2000; bind(java.net.InetSocketAddress(47820)) }
        try {
            val deadline = System.currentTimeMillis() + 25000 // ПК может ещё поднимать связь
            while (System.currentTimeMillis() < deadline) {
                val packet = java.net.DatagramPacket(ByteArray(1024), 1024)
                try { socket.receive(packet) } catch (_: java.net.SocketTimeoutException) { continue }
                val parts = String(packet.data, 0, packet.length).split("\n")
                if (parts.size >= 3 && parts[0] == "ZAVIRIA1" && (parts[1].startsWith("https://") || parts[1].startsWith("http://"))) return parts[1] to parts[2]
            }
            return null
        } finally {
            socket.close()
            lock.release()
        }
    }

    // Отключение: телефон (после отпечатка) и компьютер должны согласиться оба. Ответ: {phone, pc, done}
    fun unlinkState(context: Context): JSONObject? = runCatching { val (s, t) = call(context, "GET", "/phone/unlink", ""); if (s == 200) JSONObject(t) else null }.getOrNull()
    fun requestUnlink(context: Context): JSONObject? = runCatching { val (s, t) = call(context, "POST", "/phone/unlink", "{}"); if (s == 200) JSONObject(t) else null }.getOrNull()
    fun cancelUnlink(context: Context) { runCatching { call(context, "POST", "/phone/unlink", JSONObject().put("cancel", true).toString()) } }
    fun forget(context: Context) { context.store.edit().remove(PAIR_KEY).remove(URL_KEY).apply() }
}
