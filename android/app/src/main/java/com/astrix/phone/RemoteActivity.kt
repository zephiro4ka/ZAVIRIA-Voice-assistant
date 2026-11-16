package com.astrix.phone

import android.app.Activity
import android.app.AlertDialog
import android.content.pm.ActivityInfo
import android.graphics.BitmapFactory
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import okio.ByteString
import org.json.JSONObject
import java.util.concurrent.TimeUnit
import kotlin.concurrent.thread

// Удалённый рабочий стол: кадры идут с компьютера, касания уходят обратно
class RemoteActivity : Activity() {
    private lateinit var image: ImageView
    private lateinit var status: TextView
    private var socket: WebSocket? = null
    private var frameWidth = 0
    private var frameHeight = 0
    private val client = OkHttpClient.Builder().readTimeout(0, TimeUnit.MILLISECONDS).build()
    private val english get() = when (store.getString("lang", "system")) { "ru" -> false; "en" -> true; else -> java.util.Locale.getDefault().language != "ru" }
    private fun t(ru: String, en: String) = if (english) en else ru

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        @Suppress("DEPRECATION")
        window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_FULLSCREEN or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
        val root = FrameLayout(this).apply { setBackgroundColor(Color.BLACK); isFocusable = true; isFocusableInTouchMode = true }
        image = ImageView(this).apply { scaleType = ImageView.ScaleType.FIT_CENTER; setOnTouchListener { _, event -> touch(event); true } }
        root.addView(image, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        status = TextView(this).apply { text = t("Подключаюсь…", "Connecting…"); setTextColor(Color.WHITE); textSize = 16f; gravity = Gravity.CENTER }
        root.addView(status, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        // Только экран; всё остальное — в шестерёнке в углу
        val gear = TextView(this).apply {
            text = "⚙"; textSize = 20f; gravity = Gravity.CENTER; setTextColor(Color.WHITE); alpha = 0.75f
            background = GradientDrawable().apply { shape = GradientDrawable.OVAL; setColor(Color.argb(140, 30, 15, 60)) }
            setOnClickListener { showSettings() }
        }
        root.addView(gear, FrameLayout.LayoutParams(dp(42), dp(42), Gravity.TOP or Gravity.END).apply { setMargins(0, dp(10), dp(10), 0) })
        setContentView(root)
        connect()
    }

    private fun connect() = thread {
        val session = runCatching { PcLink.remoteSession(this) }.getOrNull()
        if (session == null) { runOnUiThread { status.text = t("Компьютер не отвечает", "Computer is not responding") }; return@thread }
        val url = PcLink.address(this).trimEnd('/').replaceFirst("https://", "wss://") + "/stream"
        socket = client.newWebSocket(Request.Builder().url(url).build(), object : WebSocketListener() {
            override fun onOpen(webSocket: WebSocket, response: Response) { webSocket.send(session) }
            override fun onMessage(webSocket: WebSocket, text: String) {
                when (text) {
                    "ok" -> { sendQuality(); runOnUiThread { status.text = "" } }
                    "locked" -> runOnUiThread { status.text = t("Компьютер заблокирован", "Computer is locked") }
                    "denied" -> runOnUiThread { status.text = t("Нет доступа", "Access denied") }
                }
            }
            override fun onMessage(webSocket: WebSocket, bytes: ByteString) {
                val frame = bytes.toByteArray()
                val bitmap = BitmapFactory.decodeByteArray(frame, 0, frame.size)
                webSocket.send("a")
                if (bitmap != null) runOnUiThread { frameWidth = bitmap.width; frameHeight = bitmap.height; status.text = ""; image.setImageBitmap(bitmap) }
            }
            override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                runOnUiThread { status.text = t("Связь потеряна", "Connection lost") }
            }
        })
    }

    private fun sendEvent(event: JSONObject) { socket?.send(event.toString()) }

    private fun dp(value: Int) = (value * resources.displayMetrics.density).toInt()

    // Разрешение (ширина, 0 — как на компьютере), кадры (0 — без предела) и качество JPEG
    private fun sendQuality() = sendEvent(JSONObject().put("t", "quality").put("p", store.getInt("remote_quality", 0))
        .put("w", store.getInt("remote_w", 1280)).put("f", store.getInt("remote_f", 30)).put("q", store.getInt("remote_q", 70)))

    private fun showSettings() {
        val box = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(18), dp(8), dp(18), dp(4)) }
        lateinit var dialog: AlertDialog
        fun title(text: String) = box.addView(TextView(this).apply { this.text = text; setTextColor(Color.rgb(200, 180, 255)); textSize = 13f; setPadding(0, dp(10), 0, dp(4)) })
        fun chips(key: String, fallback: Int, options: List<Pair<String, Int>>) {
            val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
            val current = store.getInt(key, fallback)
            options.forEach { (label, value) ->
                row.addView(TextView(this).apply {
                    text = label; textSize = 13f; gravity = Gravity.CENTER; setTextColor(Color.WHITE); setPadding(dp(6), dp(9), dp(6), dp(9))
                    background = GradientDrawable().apply { cornerRadius = dp(12).toFloat(); setColor(if (value == current) Color.rgb(124, 77, 255) else Color.argb(60, 255, 255, 255)) }
                    setOnClickListener { store.edit().putInt(key, value).apply(); sendQuality(); dialog.dismiss(); showSettings() }
                }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply { setMargins(dp(3), 0, dp(3), 0) })
            }
            box.addView(row)
        }
        fun action(label: String, run: () -> Unit) = box.addView(TextView(this).apply {
            text = label; textSize = 15f; setTextColor(Color.WHITE); setPadding(dp(4), dp(12), dp(4), dp(12))
            setOnClickListener { dialog.dismiss(); run() }
        })
        title(t("Разрешение", "Resolution"))
        chips("remote_w", 1280, listOf("720p" to 1280, "1080p" to 1920, "1440p" to 2560, t("Как на ПК", "Native") to 0))
        title(t("Кадров в секунду", "Frames per second"))
        chips("remote_f", 30, listOf("30" to 30, "60" to 60, "120" to 120, t("Макс.", "Max") to 0))
        title(t("Качество картинки", "Image quality"))
        chips("remote_q", 70, listOf(t("Экономно", "Saver") to 50, t("Обычно", "Normal") to 70, t("Высокое", "High") to 88))
        title(t("Действия", "Actions"))
        action("🖥  " + t("Сменить монитор", "Switch monitor")) { sendEvent(JSONObject().put("t", "mon")) }
        action("✏  " + t("Напечатать текст", "Type text")) { typeText() }
        action("⌨  " + t("Клавиатура", "Keyboard")) { (getSystemService(INPUT_METHOD_SERVICE) as android.view.inputmethod.InputMethodManager).toggleSoftInput(0, 0) }
        action("✖  " + t("Выйти", "Exit")) { finish() }
        dialog = AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert).setView(android.widget.ScrollView(this).apply { addView(box) }).create()
        dialog.window?.setBackgroundDrawable(GradientDrawable().apply { cornerRadius = dp(22).toFloat(); setColor(Color.rgb(24, 16, 42)) })
        dialog.show()
    }

    private fun touch(event: MotionEvent) {
        if (frameWidth == 0 || image.width == 0) return
        val scale = minOf(image.width.toFloat() / frameWidth, image.height.toFloat() / frameHeight)
        val left = (image.width - frameWidth * scale) / 2
        val top = (image.height - frameHeight * scale) / 2
        val x = ((event.x - left) / (frameWidth * scale)).coerceIn(0f, 1f)
        val y = ((event.y - top) / (frameHeight * scale)).coerceIn(0f, 1f)
        val type = when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> "down"
            MotionEvent.ACTION_MOVE -> "move"
            MotionEvent.ACTION_UP -> "up"
            else -> return
        }
        sendEvent(JSONObject().put("t", type).put("x", x.toDouble()).put("y", y.toDouble()).put("b", 0))
    }

    // Клавиатура и геймпад: клавиши уходят как на настоящей клавиатуре, правый стик двигает мышь
    private fun webKey(code: Int): String? = when (code) {
        in android.view.KeyEvent.KEYCODE_A..android.view.KeyEvent.KEYCODE_Z -> "Key" + ('A' + (code - android.view.KeyEvent.KEYCODE_A))
        in android.view.KeyEvent.KEYCODE_0..android.view.KeyEvent.KEYCODE_9 -> "Digit" + (code - android.view.KeyEvent.KEYCODE_0)
        in android.view.KeyEvent.KEYCODE_F1..android.view.KeyEvent.KEYCODE_F12 -> "F" + (code - android.view.KeyEvent.KEYCODE_F1 + 1)
        android.view.KeyEvent.KEYCODE_ENTER -> "Enter"; android.view.KeyEvent.KEYCODE_DEL -> "Backspace"; android.view.KeyEvent.KEYCODE_ESCAPE -> "Escape"
        android.view.KeyEvent.KEYCODE_TAB -> "Tab"; android.view.KeyEvent.KEYCODE_SPACE -> "Space"; android.view.KeyEvent.KEYCODE_SHIFT_LEFT -> "ShiftLeft"
        android.view.KeyEvent.KEYCODE_CTRL_LEFT -> "ControlLeft"; android.view.KeyEvent.KEYCODE_ALT_LEFT -> "AltLeft"
        android.view.KeyEvent.KEYCODE_DPAD_UP -> "ArrowUp"; android.view.KeyEvent.KEYCODE_DPAD_DOWN -> "ArrowDown"
        android.view.KeyEvent.KEYCODE_DPAD_LEFT -> "ArrowLeft"; android.view.KeyEvent.KEYCODE_DPAD_RIGHT -> "ArrowRight"
        android.view.KeyEvent.KEYCODE_BUTTON_X -> "Space"; android.view.KeyEvent.KEYCODE_BUTTON_Y -> "Enter"; android.view.KeyEvent.KEYCODE_BUTTON_START -> "Escape"
        android.view.KeyEvent.KEYCODE_BUTTON_SELECT -> "Tab"; android.view.KeyEvent.KEYCODE_BUTTON_L1 -> "KeyQ"; android.view.KeyEvent.KEYCODE_BUTTON_R1 -> "KeyE"
        else -> null
    }

    override fun dispatchKeyEvent(event: android.view.KeyEvent): Boolean {
        val down = event.action == android.view.KeyEvent.ACTION_DOWN
        if (event.action != android.view.KeyEvent.ACTION_DOWN && event.action != android.view.KeyEvent.ACTION_UP) return super.dispatchKeyEvent(event)
        when (event.keyCode) {
            android.view.KeyEvent.KEYCODE_BUTTON_A -> { sendEvent(JSONObject().put("t", "btn").put("b", 0).put("d", if (down) 1 else 0)); return true }
            android.view.KeyEvent.KEYCODE_BUTTON_B -> { sendEvent(JSONObject().put("t", "btn").put("b", 2).put("d", if (down) 1 else 0)); return true }
            android.view.KeyEvent.KEYCODE_BACK -> return super.dispatchKeyEvent(event)
        }
        val key = webKey(event.keyCode) ?: return super.dispatchKeyEvent(event)
        if (event.repeatCount == 0) sendEvent(JSONObject().put("t", "vk").put("c", key).put("d", if (down) 1 else 0))
        return true
    }

    private val held = mutableSetOf<String>()
    private var stickX = 0f
    private var stickY = 0f
    @Volatile private var stickRunning = false

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        if (event.source and android.view.InputDevice.SOURCE_JOYSTICK != android.view.InputDevice.SOURCE_JOYSTICK || event.action != MotionEvent.ACTION_MOVE) return super.onGenericMotionEvent(event)
        val x = event.getAxisValue(MotionEvent.AXIS_X); val y = event.getAxisValue(MotionEvent.AXIS_Y)
        mapOf("KeyW" to (y < -0.5f), "KeyS" to (y > 0.5f), "KeyA" to (x < -0.5f), "KeyD" to (x > 0.5f)).forEach { (key, on) -> // левый стик — WASD
            if (on && held.add(key)) sendEvent(JSONObject().put("t", "vk").put("c", key).put("d", 1))
            if (!on && held.remove(key)) sendEvent(JSONObject().put("t", "vk").put("c", key).put("d", 0))
        }
        stickX = event.getAxisValue(MotionEvent.AXIS_Z); stickY = event.getAxisValue(MotionEvent.AXIS_RZ)
        if (!stickRunning) {
            stickRunning = true
            thread {
                while (stickRunning && !isFinishing) {
                    val dx = (if (kotlin.math.abs(stickX) > 0.12f) stickX * 18 else 0f).toInt()
                    val dy = (if (kotlin.math.abs(stickY) > 0.12f) stickY * 18 else 0f).toInt()
                    if (dx != 0 || dy != 0) sendEvent(JSONObject().put("t", "rel").put("x", dx).put("y", dy))
                    Thread.sleep(16)
                }
            }
        }
        return true
    }

    private fun typeText() {
        val input = EditText(this)
        AlertDialog.Builder(this).setTitle(t("Напечатать на компьютере", "Type on computer")).setView(input)
            .setPositiveButton(t("Отправить", "Send")) { _, _ -> sendEvent(JSONObject().put("t", "text").put("s", input.text.toString())) }
            .setNegativeButton(t("Отмена", "Cancel"), null).show()
    }

    override fun onDestroy() {
        stickRunning = false
        socket?.close(1000, null)
        client.dispatcher.executorService.shutdown()
        super.onDestroy()
    }
}
