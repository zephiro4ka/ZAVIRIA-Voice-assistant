package com.astrix.phone

import android.Manifest
import android.app.Activity
import android.app.Dialog
import android.app.KeyguardManager
import android.content.Intent
import android.content.pm.ApplicationInfo
import android.content.pm.PackageManager
import android.content.ClipboardManager
import android.content.res.Configuration
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Color
import android.util.Base64
import android.graphics.RenderEffect
import android.graphics.Shader
import android.graphics.Typeface
import android.graphics.drawable.ColorDrawable
import android.graphics.drawable.GradientDrawable
import android.view.Window
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.PowerManager
import android.provider.Settings
import android.text.Editable
import android.text.InputType
import android.text.TextWatcher
import android.text.method.PasswordTransformationMethod
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.view.inputmethod.EditorInfo
import android.widget.Button
import android.widget.EditText
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.FrameLayout
import android.widget.SeekBar
import android.widget.Toast
import org.json.JSONArray
import org.json.JSONObject
import kotlin.concurrent.thread

class MainActivity : Activity() {
    private var ink = Color.rgb(244, 244, 248)
    private var muted = Color.rgb(168, 168, 182)
    private var violet = Color.rgb(140, 116, 255)
    private var deepViolet = Color.rgb(86, 34, 176)
    private var bgColors = intArrayOf(Color.rgb(12, 7, 26), Color.rgb(26, 13, 56), Color.rgb(40, 18, 86))
    private var cardColors = intArrayOf(Color.argb(240, 38, 24, 74), Color.argb(240, 22, 14, 44))
    private var fieldColor = Color.argb(190, 10, 6, 22)

    private fun tint(alpha: Int) = Color.argb(alpha, Color.red(ink), Color.green(ink), Color.blue(ink))

    // Те же темы, что на компьютере, плюс системная и своя
    private fun applyTheme() {
        val night = resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK == Configuration.UI_MODE_NIGHT_YES
        var name = store.getString("theme", "system") ?: "system"
        if (name == "system") name = if (night) "zaviria" else "white"
        val light = name == "white" || name == "paper"
        val c = { r: Int, g: Int, b: Int -> Color.rgb(r, g, b) }
        val a = { r: Int, g: Int, b: Int -> Color.argb(240, r, g, b) }
        when (name) {
            "graphite" -> { bgColors = intArrayOf(c(18, 18, 22), c(26, 26, 32), c(34, 34, 42)); cardColors = intArrayOf(a(44, 44, 52), a(30, 30, 36)); fieldColor = Color.argb(200, 14, 14, 18) }
            "coal" -> { bgColors = intArrayOf(c(8, 8, 10), c(12, 12, 15), c(16, 16, 20)); cardColors = intArrayOf(a(24, 24, 28), a(16, 16, 20)); fieldColor = Color.argb(200, 4, 4, 6) }
            "midnight" -> { bgColors = intArrayOf(c(6, 10, 30), c(10, 20, 54), c(14, 32, 86)); cardColors = intArrayOf(a(22, 36, 84), a(12, 20, 50)); fieldColor = Color.argb(200, 4, 8, 24) }
            "white" -> { bgColors = intArrayOf(c(246, 246, 249), c(240, 240, 244), c(234, 234, 240)); cardColors = intArrayOf(a(255, 255, 255), a(250, 250, 252)); fieldColor = c(236, 236, 241) }
            "paper" -> { bgColors = intArrayOf(c(250, 244, 230), c(246, 238, 220), c(240, 230, 208)); cardColors = intArrayOf(a(255, 251, 240), a(250, 243, 226)); fieldColor = c(243, 235, 216) }
            "night" -> { bgColors = intArrayOf(c(12, 7, 26), c(26, 13, 56), c(40, 18, 86)); cardColors = intArrayOf(a(38, 24, 74), a(22, 14, 44)); fieldColor = Color.argb(190, 10, 6, 22) }
            else -> { bgColors = intArrayOf(c(30, 30, 35), c(26, 26, 30), c(22, 22, 26)); cardColors = intArrayOf(a(44, 44, 50), a(36, 36, 41)); fieldColor = Color.argb(200, 20, 20, 24) } // ZAVIRIA: тёмная «слюда»
        }
        ink = if (name == "paper") c(60, 45, 30) else if (light) c(26, 26, 32) else c(244, 244, 248)
        muted = if (name == "paper") c(120, 100, 78) else if (light) c(100, 100, 112) else c(168, 168, 182)
        violet = if (name == "custom") store.getInt("accent", c(140, 116, 255)) else c(140, 116, 255)
        deepViolet = if (name == "custom") Color.rgb(Color.red(violet) * 11 / 20, Color.green(violet) * 11 / 20, Color.blue(violet) * 11 / 20) else c(70, 130, 250) // вторая половина градиента логотипа — синяя
        window.statusBarColor = bgColors[0]
        window.navigationBarColor = bgColors[2]
    }

    private fun drawThemes() {
        val pick = { name: String -> store.edit().putString("theme", name).apply(); render() }
        // образцы тем: кружок с цветами фона и акцента, выбранная — с галочкой
        val current = store.getString("theme", "system")
        val themes = listOf("system" to "Системная", "zaviria" to "ZAVIRIA", "night" to "Ночь", "graphite" to "Графит", "coal" to "Уголь", "midnight" to "Полночь", "white" to "Белая", "paper" to "Тетрадь", "custom" to "Своя")
        val swatch = mapOf("system" to intArrayOf(Color.rgb(30, 30, 35), Color.rgb(240, 240, 244)), "zaviria" to intArrayOf(Color.rgb(30, 30, 35), Color.rgb(140, 116, 255)), "night" to intArrayOf(Color.rgb(12, 7, 26), Color.rgb(40, 18, 86)),
            "graphite" to intArrayOf(Color.rgb(18, 18, 22), Color.rgb(44, 44, 52)), "coal" to intArrayOf(Color.rgb(4, 4, 6), Color.rgb(24, 24, 28)),
            "midnight" to intArrayOf(Color.rgb(6, 10, 30), Color.rgb(22, 36, 84)), "white" to intArrayOf(Color.rgb(255, 255, 255), Color.rgb(228, 222, 242)),
            "paper" to intArrayOf(Color.rgb(255, 251, 240), Color.rgb(240, 230, 208)), "custom" to intArrayOf(store.getInt("accent", Color.rgb(164, 104, 255)), Color.rgb(26, 13, 56)))
        themes.chunked(4).forEach { row ->
            val line = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
            row.forEach { (id, name) ->
                val box = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; gravity = Gravity.CENTER_HORIZONTAL; setPadding(0, dp(6), 0, dp(6)); setOnClickListener { pick(id) } }
                box.addView(FrameLayout(this).apply {
                    background = GradientDrawable(GradientDrawable.Orientation.TL_BR, swatch.getValue(id)).apply { shape = GradientDrawable.OVAL; setStroke(dp(if (current == id) 3 else 1), if (current == id) violet else Color.argb(80, 160, 150, 190)) }
                    if (current == id) addView(ImageView(this@MainActivity).apply { setImageDrawable(Icons.drawable("check", violet)) }, FrameLayout.LayoutParams(dp(22), dp(22), Gravity.CENTER))
                }, LinearLayout.LayoutParams(dp(48), dp(48)))
                box.addView(TextView(this).apply { text = tr(name); textSize = 11.5f; setTextColor(if (current == id) violet else ink); gravity = Gravity.CENTER; setPadding(0, dp(5), 0, 0) })
                line.addView(box, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            }
            repeat(4 - row.size) { line.addView(View(this), LinearLayout.LayoutParams(0, 0, 1f)) }
            column.addView(line)
        }
        if (store.getString("theme", "system") == "custom") {
            note("Выбери свой цвет:")
            val color = { value: Int -> store.edit().putInt("accent", value).apply(); render() }
            tiles(listOf(
                Triple("🟣", "Фиолетовый") { color(Color.rgb(164, 104, 255)) },
                Triple("🔵", "Синий") { color(Color.rgb(80, 140, 255)) },
                Triple("🟢", "Зелёный") { color(Color.rgb(60, 190, 120)) },
                Triple("🟠", "Оранжевый") { color(Color.rgb(245, 150, 60)) },
                Triple("🔴", "Красный") { color(Color.rgb(235, 80, 90)) },
                Triple("🩷", "Розовый") { color(Color.rgb(240, 110, 170)) },
            ))
        }
    }
    private val warm = Color.rgb(255, 150, 170)
    private lateinit var column: LinearLayout
    private lateinit var codeField: EditText
    private var unlocked = false

    private val english: Boolean
        get() = when (store.getString("lang", "system")) { "ru" -> false; "en" -> true; else -> java.util.Locale.getDefault().language != "ru" }

    private val words = mapOf(
        "Компьютер" to "Computer",
        "Телефон" to "Phone",
        "Настройка" to "Settings",
        "Управление и защита телефона и ПК" to "Control and protection for phone and PC",
        "Подключи компьютер" to "Connect your computer",
        "На компьютере открой ZAVIRIA: Настройки → Синхронизация → «подключить телефон». Телефон должен быть в том же Wi-Fi. Введи код с экрана компьютера и нажми «Подключить» — компьютер найдётся сам." to "On your computer open ZAVIRIA: Settings → Sync → “connect phone”. The phone must be on the same Wi-Fi. Type the code from the computer screen and tap “Connect” — the computer is found automatically.",
        "Адрес компьютера (необязательно)" to "Computer address (optional)",
        "Ищу компьютер в Wi-Fi…" to "Looking for the computer on Wi-Fi…",
        "Включение ПК" to "Turn on the PC",
        "Включить компьютер" to "Turn on the computer",
        "Если компьютер выключен, его можно включить по сети." to "If the computer is off, you can turn it on over the network.",
        "Компьютер включится, если в BIOS и у сетевой карты разрешено «пробуждение по сети» (Wake-on-LAN), а кабель подключён. Дома это работает через Wi-Fi. Издалека — только если роутер пересылает порт UDP 9 на рассылку в домашней сети: укажи его внешний адрес ниже." to "The computer turns on if Wake-on-LAN is enabled in BIOS and for the network card, and the cable is plugged in. At home it works over Wi-Fi. From far away only if your router forwards UDP port 9 to the home network broadcast: enter its public address below.",
        "Адрес роутера для включения издалека (необязательно)" to "Router address for turning on from far away (optional)",
        "Сохранить адрес" to "Save address",
        "Сохранено" to "Saved",
        "Сигнал включения отправлен. Подожди минуту." to "Wake signal sent. Wait a minute.",
        "Компьютер ещё ни разу не сообщил свой адрес: открой приложение, пока он включён." to "The computer hasn't shared its address yet: open the app once while it is on.",
        "Адрес компьютера" to "Computer address",
        "Найти компьютер" to "Find computer",
        "Код с экрана компьютера" to "Code from the computer screen",
        "Подключить" to "Connect",
        "Управление компьютером" to "Computer control",
        "Состояние" to "Status",
        "Снимок экрана" to "Screenshot",
        "Реши с экрана" to "Solve from screen",
        "Что на экране" to "What's on screen",
        "Пауза" to "Pause",
        "Следующий" to "Next",
        "Громче" to "Louder",
        "Тише" to "Quieter",
        "Заблокировать" to "Lock",
        "Выключить экран" to "Screen off",
        "Включить экран" to "Screen on",
        "Показать вход" to "Show sign-in",
        "Выключить ПК" to "Shut down",
        "Перезагрузить" to "Restart",
        "Отменить" to "Cancel",
        "Напиши, что сделать" to "Tell it what to do",
        "Сообщение" to "Message",
        "Отправить" to "Send",
        "🛡 Телефон под защитой" to "🛡 Phone is protected",
        "⚠ Защита выключена" to "⚠ Protection is off",
        "Если телефон украдут, напиши боту защиты с любого устройства." to "If it is stolen, message your guard bot from any device.",
        "Включи, чтобы найти и заблокировать телефон при краже." to "Turn on to find and lock your phone if stolen.",
        "Бот защиты" to "Guard bot",
        "Код из Authenticator" to "Authenticator code",
        "Геолокация" to "Location",
        "Защита от кражи" to "Theft protection",
        "Включить защиту" to "Turn on protection",
        "Выключить защиту" to "Turn off protection",
        "Если телефон украли" to "If your phone is stolen",
        "1. Бот защиты" to "1. Guard bot",
        "Создай в @BotFather отдельного бота, не основного." to "Create a separate bot in @BotFather, not your main one.",
        "Твой ID в Telegram" to "Your Telegram ID",
        "Сохранить" to "Save",
        "2. Код из Authenticator" to "2. Authenticator code",
        "3. Разрешения" to "3. Permissions",
        "Оформление" to "Appearance",
        "Разрешить геолокацию" to "Allow location",
        "Разрешить работу в фоне" to "Allow background work",
        "Отвязать" to "Unlink",
        "Добавить в Google Authenticator" to "Add to Google Authenticator",
        "Подтвердить" to "Confirm",
        "Разрешить" to "Allow",
        "Пропустить" to "Skip",
        "Системная" to "System",
        "Ночь" to "Night",
        "Графит" to "Graphite",
        "Уголь" to "Coal",
        "Полночь" to "Midnight",
        "Белая" to "White",
        "Тетрадь" to "Notebook",
        "Своя" to "Custom",
        "Подтверди, что это ты" to "Confirm it's you",
        "Компьютер подключён" to "Computer connected",
        "Подключаю…" to "Connecting…",
        "Неверный код" to "Wrong code",
        "Сохранено" to "Saved",
        "Язык" to "Language",
        "Запуск" to "Launch",
        "Перетаскивай кнопки, нажми — изменить, ✕ — удалить, «+» — добавить приложение" to "Drag buttons, tap to edit, ✕ to remove, “+” to add an app",
        "Название кнопки" to "Button name",
        "Новая кнопка" to "New button",
        "Изменить кнопку" to "Edit button",
        "Удалить" to "Delete",
        "Загружаю приложения компьютера…" to "Loading the computer's apps…",
        "Компьютер не нашёл приложений" to "The computer found no apps",
        "Поиск приложения" to "Search apps",
        "Приложение с компьютера" to "App from the computer",
        "💻 Приложение с компьютера" to "💻 App from the computer",
        "⚡ Модуль ZAVIRIA" to "⚡ ZAVIRIA module",
        "Модуль ZAVIRIA" to "ZAVIRIA module",
        "Подключён · связь зашифрована" to "Connected · encrypted",
        "Тишина" to "Silence",
        "Играет на компьютере" to "Playing on the PC",
        "На паузе" to "Paused",
        "Перетаскивай кнопки, ✕ — удалить, «+» — добавить модуль" to "Drag buttons, ✕ to remove, “+” to add a module",
        "Нажми «+», чтобы добавить приложение. Зажми кнопку, чтобы переставить" to "Hold any button to edit the panel",
        "Добавить" to "Add",
        "Добавить модуль" to "Add module",
        "Все модули уже на панели" to "All modules are already on the panel",
        "Готово" to "Done",
        "Отмена" to "Cancel",
        "Отменить выключение" to "Cancel shutdown",
        "Защита" to "Protection",
        "Приложение" to "App",
        "Включена" to "On",
        "Выключена" to "Off",
        "Не подключён" to "Not connected",
        "Настроен" to "Set up",
        "Не настроен" to "Not set up",
        "Разрешения" to "Permissions",
        "Геолокация разрешена" to "Location allowed",
        "Нужна геолокация" to "Location needed",
        "Режим владельца" to "Device owner",
        "Включён" to "On",
        "Готовность" to "Readiness",
        "Защита выключена" to "Protection is off",
        "Включи защиту от кражи, чтобы найти и заблокировать телефон." to "Turn on anti-theft to find and lock the phone.",
        "Полный контроль защиты над телефоном. Снимать — только если меняешь телефон или удаляешь приложение." to "Full protection control over the phone. Remove only if you change the phone or uninstall the app.",
        "Оперативная память" to "Memory",
        "Диск C" to "Drive C",
        "Сеть" to "Network",
        "Загрузка" to "Load",
        "Занято" to "Used",
        "нет датчика" to "no sensor",
        "потоков" to "threads",
        "Частота" to "Clock",
        "ГГц" to "GHz",
        "свободно" to "free",
        "ГБ" to "GB",
        "Активность" to "Activity",
        "МБ/с" to "MB/s",
        "КБ/с" to "KB/s",
        "Видеопамять" to "VRAM",
        "Вт" to "W",
        "вентилятор" to "fan",
        "МГц" to "MHz",
        "Встроенная" to "Integrated",
        "Видеокарта не найдена" to "No graphics card found",
        "Проверяю связь…" to "Checking connection…",
        "🔐 Связь защищена: ключ из SPAKE2, все запросы зашифрованы AES-256 от телефона до компьютера. Посредники и туннель видят только шифротекст." to "🔐 The link is protected: the key comes from SPAKE2 and every request is AES-256 encrypted from phone to computer. Middlemen and the tunnel only see ciphertext.",
        "Аккаунт" to "Account",
        "Синхронизация" to "Sync",
        "Копия 2FA на компьютере" to "2FA backup on the computer",
        "Работа в фоне" to "Background work",
        "Копия кода переживёт переустановку приложения: вернётся при новом подключении." to "The code backup survives reinstalling the app: it comes back on the next connection.",
        "Отключение" to "Disconnect",
        "Чтобы отключиться, нужно подтвердить на телефоне (отпечаток или код экрана) и на компьютере (Настройки → Синхронизация)." to "To disconnect, confirm on the phone (fingerprint or screen lock) and on the computer (Settings → Sync).",
        "Отключиться от ПК" to "Disconnect from PC",
        "✓ Компьютер на связи" to "✓ Computer is online",
        "Компьютер сейчас не отвечает" to "Computer is not responding right now",
        "Жду подтверждения на компьютере…" to "Waiting for confirmation on the computer…",
        "Подтверди на компьютере" to "Confirm on the computer",
        "Открой ZAVIRIA на компьютере: Настройки → Синхронизация и нажми «подтвердить отключение». Есть 5 минут." to "Open ZAVIRIA on the computer: Settings → Sync and press “confirm disconnect”. You have 5 minutes.",
        "Понятно" to "Got it",
        "Компьютер отключён" to "Computer disconnected",
        "Компьютер просит отключить телефон" to "The computer asks to disconnect this phone",
        "Если это ты, подтверди отпечатком или кодом экрана. После этого связь с компьютером пропадёт." to "If it's you, confirm with a fingerprint or screen lock. After that the link to the computer is gone.",
        "Отклонить" to "Decline",
        "Открыть экран" to "Open screen",
        "Компьютер ответил неверно" to "The computer answered incorrectly",
        "Компьютер не принял токен" to "The computer rejected the token",
        "Экран ПК" to "PC screen",
        "Главная" to "Home",
        "Команды" to "Commands",
        "Связь с ПК" to "PC link",
        "Компьютер" to "Computer",
        "Процессор" to "CPU",
        "Память" to "Memory",
        "Видеокарта" to "GPU",
        "Громкость" to "Volume",
        "Сейчас играет" to "Now playing",
        "Назад" to "Back",
        "Дальше" to "Next",
        "Без звука" to "Muted",
        "Ничего не играет" to "Nothing is playing",
        "Кнопки настраиваются в пульте ZAVIRIA: свои программы, ссылки и сценарии." to "Buttons are set up in the ZAVIRIA remote: your apps, links and scenarios.",
        "Пока нет своих кнопок." to "No custom buttons yet.",
        "Компьютер не отвечает" to "Computer is not responding",
        "Быстрые команды" to "Quick commands",
        "Удалённый стол" to "Remote desktop",
        "Подключение к ПК" to "PC connection",
        "Настройки" to "Settings",
        "Чат" to "Chat", "Экран" to "Screen", "Аня" to "Anya", "Написать Ане…" to "Message Anya…", "📸 снимок экрана" to "📸 screenshot", "Игровой режим" to "Game mode",
        "Напиши Ане — она сделает на компьютере, что попросишь, и ответит здесь." to "Write to Anya — she'll do what you ask on the computer and answer here.",
        "сеть" to "net", "КБ/с" to "KB/s", "диск" to "disk",
        "Подключаюсь к компьютеру…" to "Connecting to the computer…", "Компьютер подключён" to "Computer connected", "Компьютер не отвечает" to "The computer isn't answering",
        "Музыка в Discord" to "Music in Discord", "Включено" to "On", "Выключено" to "Off", "Обновление приложения" to "App update", "С компьютера, без USB" to "From the computer, no USB",
        "Что играет на телефоне (Spotify, Яндекс Музыка, YouTube Music…), компьютер покажет в статусе Discord, пока на нём самом ничего не играет. Нужен доступ к уведомлениям: так Android разрешает видеть плееры. Сами уведомления никуда не уходят." to "What's playing on the phone (Spotify, Yandex Music, YouTube Music…) shows up in your Discord status while the computer itself plays nothing. It needs notification access — that's how Android lets apps see players. The notifications themselves never leave the phone.",
        "✓ Включено" to "✓ On", "Открыть доступ к уведомлениям" to "Open notification access", "Включить" to "Turn on",
        "Свежая версия лежит на компьютере рядом с ZAVIRIA. Телефон заберёт её по зашифрованной связи и поставит — без USB и отладки." to "The fresh version sits on the computer next to ZAVIRIA. The phone fetches it over the encrypted link and installs it — no USB, no debugging.",
        "Сначала подключи компьютер." to "Connect the computer first.", "Один раз разреши ZAVIRIA ставить обновления." to "Allow ZAVIRIA to install updates once.", "Разрешить" to "Allow",
        "Проверить и обновить" to "Check and update", "Проверяю…" to "Checking…", "Компьютер не отвечает" to "The computer isn't answering",
        "На компьютере нет приложения для телефона — обнови ZAVIRIA на ПК" to "The computer has no phone app — update ZAVIRIA on the PC", "Уже последняя версия" to "Already the latest version", "Ставлю обновление…" to "Installing the update…",
        "Компьютер не подключён" to "Computer not connected",
        "Качество" to "Quality",
        "720p, 30 кадров" to "720p, 30 fps",
        "1080p, 60 кадров" to "1080p, 60 fps",
        "Источник" to "Source",
        "«Источник» — разрешение экрана и максимум кадров. Нужен быстрый интернет." to "Source uses the screen resolution and maximum frames. Needs fast internet.",
        "Касание — нажатие мышью, ведёшь пальцем — двигаешь курсор." to "Tap to click, drag to move the cursor.",
        "Подключиться" to "Connect",
        "Как в системе" to "System",
        "Шесть цифр из Authenticator" to "Six digits from Authenticator"
    )

    private fun tr(text: String) = if (english) words[text] ?: text else text

    // Строка выбора: название и галочка у выбранного
    private fun choiceRow(title: String, selected: Boolean, action: () -> Unit) {
        column.addView(LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL; setPadding(dp(12), dp(12), dp(12), dp(12))
            background = rounded(if (selected) Color.argb(50, Color.red(violet), Color.green(violet), Color.blue(violet)) else tint(14), 14)
            setOnClickListener { action() }
            addView(TextView(this@MainActivity).apply { text = tr(title); textSize = 15f; setTextColor(ink) }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            if (selected) addView(ImageView(this@MainActivity).apply { setImageDrawable(Icons.drawable("check", violet)) }, LinearLayout.LayoutParams(dp(22), dp(22)))
        }, spaced(top = 3, bottom = 3))
    }

    private fun drawLanguage() {
        val pick = { value: String -> store.edit().putString("lang", value).apply(); render() }
        val current = store.getString("lang", "system")
        listOf("system" to "Как в системе", "ru" to "Русский", "en" to "English").forEach { (id, name) -> choiceRow(name, current == id) { pick(id) } }
    }

    private fun dp(value: Int) = (value * resources.displayMetrics.density).toInt()
    private fun dp(value: Float) = value * resources.displayMetrics.density

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val debuggable = applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE != 0
        applyTheme()
        val keyguard = getSystemService(KEYGUARD_SERVICE) as KeyguardManager
        if (Auth.isEnrolled(this) && keyguard.isKeyguardSecure) {
            @Suppress("DEPRECATION")
            startActivityForResult(keyguard.createConfirmDeviceCredentialIntent("ZAVIRIA", "Подтверди, что это ты"), 1)
        } else {
            unlocked = true
            render()
        }
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        @Suppress("DEPRECATION") super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == 3) { if (resultCode == RESULT_OK) pendingAction?.invoke(); pendingAction = null; return }
        if (requestCode != 1) return
        if (resultCode == RESULT_OK) { unlocked = true; render() } else finish()
    }

    override fun onResume() {
        super.onResume()
        if (unlocked) render() // разрешения могли поменять в настройках
    }

    // ------------------------------------------------------------ автоподключение по QR-коду с экрана ПК
    // Страничка QR-кода кладёт в буфер «zaviria://pair?u=адрес&c=код» (и даёт ту же ссылку кнопкой) — привязываемся без ввода кода
    private var autoPairTried = false

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        autoPair(intent.dataString)
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (!hasFocus || autoPairTried || PcLink.isPaired(this)) return
        autoPairTried = true
        val clip = runCatching { (getSystemService(CLIPBOARD_SERVICE) as android.content.ClipboardManager).primaryClip?.getItemAt(0)?.text?.toString() }.getOrNull()
        val link = intent?.dataString?.takeIf { it.startsWith("zaviria:") } ?: clip?.takeIf { it.contains("zaviria://pair") }
        if (link != null) { autoPair(link); return }
        // свежая установка по QR с ПК: компьютер рассылает по Wi-Fi адрес и код — подключаемся сами
        thread {
            for (attempt in 0 until 4) {
                if (PcLink.isPaired(this)) return@thread
                val found = runCatching { PcLink.invite(this) }.getOrNull() ?: continue
                runOnUiThread { autoPair("zaviria://pair?u=${found.first}&c=${found.second}") }
                return@thread
            }
        }
    }

    private fun autoPair(text: String?) {
        if (text == null || PcLink.isPaired(this)) return
        val m = Regex("zaviria://pair\\?u=([^&\\s]+)&c=([A-Za-z0-9]+)").find(text) ?: return
        val url = android.net.Uri.decode(m.groupValues[1])
        val code = m.groupValues[2]
        toast(tr("Подключаюсь к компьютеру…"))
        thread {
            val problem = runCatching { PcLink.pair(this, url, code) }.getOrElse { "Компьютер не отвечает" }
            runOnUiThread {
                toast(tr(problem ?: "Компьютер подключён"))
                if (problem == null) {
                    if (android.os.Build.VERSION.SDK_INT >= 28) runCatching { (getSystemService(CLIPBOARD_SERVICE) as android.content.ClipboardManager).clearPrimaryClip() }
                    syncGuard(); render()
                }
            }
        }
    }

    // ------------------------------------------------------------ оформление
    private fun rounded(color: Int, radius: Int, stroke: Int = 0) = GradientDrawable().apply {
        setColor(color)
        cornerRadius = dp(radius).toFloat()
        if (stroke != 0) setStroke(dp(1), stroke)
    }

    private fun spaced(top: Int = 0, bottom: Int = 0) = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
        topMargin = dp(top); bottomMargin = dp(bottom)
    }

    private fun card(title: String, content: () -> Unit) {
        val outer = column
        val inner = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = GradientDrawable(GradientDrawable.Orientation.TL_BR, cardColors).apply {
                cornerRadius = dp(24).toFloat()
                setStroke(dp(1), Color.argb(60, 190, 160, 255))
            }
            setPadding(dp(20), dp(18), dp(20), dp(20))
        }
        outer.addView(inner, spaced(top = 14))
        column = inner
        column.addView(TextView(this).apply { text = tr(title); textSize = 19f; setTextColor(ink); typeface = Typeface.DEFAULT_BOLD }, spaced(bottom = 6))
        content()
        column = outer
    }

    private fun note(text: String, color: Int = muted) = TextView(this).apply {
        this.text = tr(text); textSize = 14f; setTextColor(color); setLineSpacing(dp(2).toFloat(), 1f)
    }.also { column.addView(it, spaced(bottom = 10)) }

    private fun button(text: String, primary: Boolean = true, action: () -> Unit) = column.addView(Button(this).apply {
        this.text = tr(text)
        textSize = 15f
        isAllCaps = false
        stateListAnimator = null
        setTextColor(if (primary) Color.WHITE else ink)
        background = if (primary) GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT, intArrayOf(violet, deepViolet)).apply { cornerRadius = dp(16).toFloat() }
        else rounded(tint(14), 16, Color.argb(90, 190, 160, 255))
        setOnClickListener { action() }
    }, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(50)).apply { topMargin = dp(6); bottomMargin = dp(4) })

    private fun styleInput(view: EditText) {
        view.setTextColor(ink)
        view.setHintTextColor(Color.argb(170, 178, 162, 212))
        view.textSize = 15f
        view.background = rounded(fieldColor, 14, Color.argb(70, 190, 160, 255))
        view.setPadding(dp(14), dp(13), dp(14), dp(13))
    }

    private fun field(hint: String, value: String, type: Int) = EditText(this).apply {
        this.hint = tr(hint); setText(value); inputType = type; styleInput(this)
    }.also { column.addView(it, spaced(top = 4, bottom = 8)) }

    // Секретное поле: ни одной видимой буквы, размыто, клавиатура не запоминает ввод
    private fun hiddenField(hint: String) = EditText(this).apply {
        this.hint = hint
        inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
        imeOptions = EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING
        transformationMethod = object : PasswordTransformationMethod() {
            override fun getTransformation(source: CharSequence, view: View): CharSequence =
                object : CharSequence {
                    override val length get() = source.length
                    override fun get(index: Int) = '•'
                    override fun subSequence(startIndex: Int, endIndex: Int): CharSequence = "•".repeat(endIndex - startIndex)
                    override fun toString() = "•".repeat(source.length)
                }
        }
        isLongClickable = false
        styleInput(this)
        if (Build.VERSION.SDK_INT >= 31) {
            addTextChangedListener(object : TextWatcher {
                override fun afterTextChanged(s: Editable?) {
                    setRenderEffect(if (s.isNullOrEmpty()) null else RenderEffect.createBlurEffect(14f, 14f, Shader.TileMode.CLAMP))
                }
                override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
                override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {}
            })
        }
    }.also { column.addView(it, spaced(top = 4, bottom = 8)) }

    private fun statusRow(label: String, on: Boolean) {
        val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        row.addView(View(this).apply { background = rounded(if (on) violet else Color.argb(90, 178, 162, 212), 5) }, LinearLayout.LayoutParams(dp(10), dp(10)).apply { rightMargin = dp(10) })
        row.addView(TextView(this).apply { text = tr(label); textSize = 14f; setTextColor(if (on) ink else muted) })
        column.addView(row, spaced(top = 4, bottom = 4))
    }

    private fun toast(text: String) = Toast.makeText(this, tr(text), Toast.LENGTH_LONG).show()

    private fun passCode(): Boolean {
        if (!Auth.isEnrolled(this)) { toast("Сначала включи 2FA"); return false }
        val minutes = Auth.blockedMinutes(this)
        if (minutes > 0) { toast("Слишком много ошибок. Подожди $minutes мин."); return false }
        val ok = Auth.check(this, codeField.text.toString())
        codeField.setText("")
        if (!ok) toast("Неверный код")
        return ok
    }

    // ------------------------------------------------------------ разрешения при первом запуске: сначала объясняем зачем
    private var introRunning = false

    private fun startIntro() {
        if (introRunning || store.getBoolean("intro_done", false)) return
        introRunning = true
        introStep(0)
    }

    private fun explain(title: String, text: String, next: Int, action: () -> Unit) {
        styledDialog(title, text, "Разрешить", "Пропустить", action) { introStep(next) }
    }

    // Окно в стиле приложения вместо серого системного
    private fun styledDialog(title: String, text: String, primary: String, secondary: String?, onPrimary: () -> Unit, onSecondary: () -> Unit = {}) {
        val dialog = Dialog(this)
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE)
        val panel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = GradientDrawable(GradientDrawable.Orientation.TL_BR, intArrayOf(cardColors[0] or (0xFF shl 24), cardColors[1] or (0xFF shl 24))).apply {
                cornerRadius = dp(28).toFloat()
                setStroke(dp(1), Color.argb(110, 190, 160, 255))
            }
            setPadding(dp(24), dp(24), dp(24), dp(20))
        }
        panel.addView(ImageView(this).apply { setImageResource(R.drawable.ic_zaviria) }, LinearLayout.LayoutParams(dp(54), dp(54)).apply { gravity = Gravity.CENTER_HORIZONTAL; bottomMargin = dp(10) })
        panel.addView(TextView(this).apply { this.text = title; textSize = 21f; setTextColor(ink); typeface = Typeface.DEFAULT_BOLD; gravity = Gravity.CENTER }, spaced(bottom = 10))
        panel.addView(TextView(this).apply { this.text = text; textSize = 15f; setTextColor(muted); gravity = Gravity.CENTER; setLineSpacing(dp(3).toFloat(), 1f) }, spaced(bottom = 16))
        val saved = if (::column.isInitialized) column else panel
        column = panel
        button(primary) { dialog.dismiss(); onPrimary() }
        if (secondary != null) button(secondary, primary = false) { dialog.dismiss(); onSecondary() }
        column = saved
        dialog.setContentView(panel)
        dialog.setCancelable(false)
        dialog.window?.apply {
            setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT))
            addFlags(WindowManager.LayoutParams.FLAG_SECURE)
            setLayout((resources.displayMetrics.widthPixels * 0.88f).toInt(), ViewGroup.LayoutParams.WRAP_CONTENT)
        }
        dialog.show()
    }

    private fun introStep(step: Int) {
        when (step) {
            0 -> explain("Геолокация и уведомления", "Геолокация нужна, чтобы прислать тебе координаты, если телефон украдут или потеряется. Уведомление показывает, что защита работает.", 1) {
                val list = mutableListOf(Manifest.permission.ACCESS_FINE_LOCATION, Manifest.permission.ACCESS_COARSE_LOCATION)
                if (Build.VERSION.SDK_INT >= 33) list += Manifest.permission.POST_NOTIFICATIONS
                requestPermissions(list.toTypedArray(), 10)
            }
            1 -> if (Build.VERSION.SDK_INT >= 29 && locationGranted() && checkSelfPermission(Manifest.permission.ACCESS_BACKGROUND_LOCATION) != PackageManager.PERMISSION_GRANTED) {
                explain("Геолокация всегда", "Чтобы координаты приходили при выключенном экране, на следующем экране выбери «Разрешить всегда».", 2) {
                    requestPermissions(arrayOf(Manifest.permission.ACCESS_BACKGROUND_LOCATION), 11)
                }
            } else introStep(2)
            2 -> if (!(getSystemService(POWER_SERVICE) as PowerManager).isIgnoringBatteryOptimizations(packageName)) {
                explain("Работа в фоне", "Иначе система усыпит приложение, и команды от бота не дойдут до телефона.", 3) {
                    introStep(3)
                    askBattery()
                }
            } else introStep(3)
            else -> { store.edit().putBoolean("intro_done", true).apply(); introRunning = false }
        }
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == 10) introStep(1)
        if (requestCode == 11) introStep(2)
        render()
    }

    private fun deviceName(): String =
        Settings.Global.getString(contentResolver, "device_name")?.takeIf { it.isNotBlank() } ?: "${Build.MANUFACTURER} ${Build.MODEL}"

    // Токен сохраняется, только если Telegram его принял и бот смог написать владельцу
    private fun connectBot(token: String, owner: String) {
        if (token.isEmpty() || owner.isEmpty()) { toast("Нужны токен и ID"); return }
        toast("Проверяю подключение…")
        val device = deviceName()
        thread {
            val problem = try {
                val bot = Bot(token)
                val me = bot.call("getMe", emptyMap())
                if (!me.optBoolean("ok")) "Telegram не принял токен"
                else {
                    val sent = bot.call("sendMessage", mapOf("chat_id" to owner, "text" to "✅ Подключение успешно\nУстройство: $device"))
                    if (!sent.optBoolean("ok")) "Открой своего бота, нажми «Старт» и сохрани ещё раз"
                    else {
                        store.edit().putString(Keys.TOKEN, token).putString(Keys.OWNER, owner)
                            .putString("bot_name", me.getJSONObject("result").optString("username")).apply()
                        PcLink.sendGuard(this) // компьютеру нужен бот защиты, чтобы сообщать новый адрес
                        null
                    }
                }
            } catch (_: Exception) {
                "Нет связи с Telegram"
            }
            runOnUiThread {
                toast(problem ?: "Подключение успешно")
                if (problem == null) { syncGuard(); render() }
            }
        }
    }

    private fun locationGranted() = checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED

    // ------------------------------------------------------------ экран
    private var tab = 0
    private lateinit var feed: LinearLayout
    private val messages = ArrayList<Pair<String?, Bitmap?>>()
    @Volatile private var polling = false
    private var pollCount = 0
    private var pendingAction: (() -> Unit)? = null

    override fun onPause() {
        super.onPause()
        polling = false
    }

    private var guardChecked = false

    // Служба нужна и для защиты, и чтобы получать команды с компьютера
    private fun syncGuard() {
        if ((store.getBoolean(Keys.ARMED, false) || PcLink.isPaired(this)) && locationGranted()) GuardService.start(this)
        else GuardService.stop(this)
    }

    private fun startPolling() {
        if (polling || !PcLink.isPaired(this)) return
        polling = true
        thread {
            while (polling) {
                val items = runCatching { PcLink.poll(this) }.getOrNull()
                if (items != null) for (i in 0 until items.length()) {
                    val item = items.getJSONObject(i)
                    val bitmap = item.optString("photo").takeIf { it.isNotEmpty() }?.let {
                        val bytes = Base64.decode(it, Base64.DEFAULT)
                        BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
                    }
                    val text = item.optString("text").takeIf { it.isNotEmpty() }
                    runOnUiThread { addMessage(text, bitmap) }
                }
                if (++pollCount % 3 == 0 && PcLink.isPaired(this)) PcLink.unlinkState(this)?.let { state -> runOnUiThread { checkUnlink(state) } }
                Thread.sleep(2000)
            }
        }
    }

    private fun addMessage(text: String?, bitmap: Bitmap?) {
        chat.add(Triple(false, text, bitmap))
        while (chat.size > 60) chat.removeAt(0)
        chatBox?.takeIf { tab == 6 && it.isAttachedToWindow }?.let { if (chat.size == 1) it.removeAllViews(); it.addView(bubble(false, text, bitmap), spaced(top = 4, bottom = 4)) }
        messages.add(0, text to bitmap)
        while (messages.size > 12) messages.removeAt(messages.size - 1)
        if (::feed.isInitialized) drawFeed()
    }

    private fun drawFeed() {
        feed.removeAllViews()
        messages.forEach { (text, bitmap) ->
            bitmap?.let { feed.addView(ImageView(this).apply { setImageBitmap(it); adjustViewBounds = true }, spaced(top = 4, bottom = 4)) }
            text?.let {
                feed.addView(TextView(this).apply {
                    this.text = it; textSize = 14f; setTextColor(ink); setTextIsSelectable(true)
                    background = rounded(tint(30), 14); setPadding(dp(12), dp(9), dp(12), dp(9))
                }, spaced(top = 4, bottom = 4))
            }
        }
    }

    // Опасные действия подтверждаются отпечатком или паролем телефона
    private fun confirmThen(action: () -> Unit) {
        val keyguard = getSystemService(KEYGUARD_SERVICE) as KeyguardManager
        if (!keyguard.isKeyguardSecure) { toast("Сначала поставь на телефон пароль или отпечаток"); return }
        pendingAction = action
        @Suppress("DEPRECATION")
        startActivityForResult(keyguard.createConfirmDeviceCredentialIntent("ZAVIRIA", "Подтверди, что это ты"), 3)
    }

    private fun send(text: String) {
        thread {
            val ok = PcLink.command(this, text)
            if (!ok) runOnUiThread { toast("Компьютер не отвечает") }
        }
    }

    // Значок в кружке цвета акцента; если это не наш значок (эмодзи из пульта) — показываем как есть
    private fun iconBubble(icon: String, size: Int): View = if (Icons.has(icon)) FrameLayout(this).apply {
        background = GradientDrawable(GradientDrawable.Orientation.TL_BR, intArrayOf(Color.argb(90, Color.red(violet), Color.green(violet), Color.blue(violet)), Color.argb(40, Color.red(violet), Color.green(violet), Color.blue(violet)))).apply { shape = GradientDrawable.OVAL }
        addView(ImageView(this@MainActivity).apply { setImageDrawable(Icons.drawable(icon, ink)) }, FrameLayout.LayoutParams(dp(size * 55 / 100), dp(size * 55 / 100), Gravity.CENTER))
    } else FrameLayout(this).apply {
        background = GradientDrawable(GradientDrawable.Orientation.TL_BR, intArrayOf(Color.argb(80, Color.red(violet), Color.green(violet), Color.blue(violet)), Color.argb(45, Color.red(deepViolet), Color.green(deepViolet), Color.blue(deepViolet)))).apply { cornerRadius = dp(size * 0.32f) }
        addView(TextView(this@MainActivity).apply { text = icon; textSize = size * 0.52f; gravity = Gravity.CENTER; includeFontPadding = false }, FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER))
    }

    private fun tiles(items: List<Triple<String, String, () -> Unit>>) {
        items.chunked(3).forEach { row ->
            val line = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
            row.forEach { (icon, label, action) ->
                val tile = LinearLayout(this).apply {
                    orientation = LinearLayout.VERTICAL
                    gravity = Gravity.CENTER
                    background = rounded(tint(22), 16, Color.argb(50, 190, 160, 255))
                    setOnClickListener { action() }
                }
                tile.addView(iconBubble(icon, 38), LinearLayout.LayoutParams(dp(38), dp(38)).apply { gravity = Gravity.CENTER_HORIZONTAL; bottomMargin = dp(5) })
                tile.addView(TextView(this).apply { text = tr(label); textSize = 12f; setTextColor(ink); gravity = Gravity.CENTER; maxLines = 2 })
                line.addView(tile, LinearLayout.LayoutParams(0, dp(76), 1f).apply { setMargins(dp(3), dp(3), dp(3), dp(3)) })
            }
            repeat(3 - row.size) { line.addView(View(this), LinearLayout.LayoutParams(0, 0, 1f)) }
            column.addView(line)
        }
    }

    private fun render() {
        if (!unlocked) return
        applyTheme()
        window.clearFlags(WindowManager.LayoutParams.FLAG_SECURE) // показ экрана работает; закрываются только окна с ключами
        startIntro()
        if (!guardChecked) { guardChecked = true; syncGuard() }
        startPolling()
        startHomeLoop()
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            fitsSystemWindows = true // шапка не заезжает под строку состояния
            background = GradientDrawable(GradientDrawable.Orientation.TL_BR, bgColors)
        }
        val header = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL; setPadding(dp(16), dp(10), dp(16), dp(4)) }
        header.addView(ImageView(this).apply { setImageResource(R.drawable.ic_zaviria) }, LinearLayout.LayoutParams(dp(38), dp(38)).apply { rightMargin = dp(10) })
        val names = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        names.addView(TextView(this).apply { text = "ZAVIRIA"; textSize = 20f; setTextColor(ink); typeface = Typeface.DEFAULT_BOLD })
        names.addView(TextView(this).apply { text = tr("Управление и защита телефона и ПК"); textSize = 12f; setTextColor(muted) })
        header.addView(names)
        root.addView(header)

        column = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(12), 0, dp(12), dp(12)) }
        root.addView(ScrollView(this).apply { addView(column) }, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))

        // Нижняя панель как в Telegram: парит над краем, выбранный раздел подсвечен
        val bar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(dp(6), dp(6), dp(6), dp(6))
            background = rounded(cardColors[0] or (0xFF shl 24), 26, Color.argb(60, Color.red(violet), Color.green(violet), Color.blue(violet)))
            elevation = dp(8).toFloat()
        }
        // «Связь с ПК» теперь в «Настройках»; номера вкладок прежние, четвёртой в панели просто нет
        listOf(Triple(0, "home", "Главная"), Triple(6, "send", "Чат"), Triple(1, "bolt", "Запуск"), Triple(3, "desktop", "Экран"), Triple(5, "settings", "Настройки")).forEach { (index, icon, label) ->
            val selected = tab == index || (index == 5 && tab == 4)
            val item = LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL
                gravity = Gravity.CENTER
                if (selected) background = rounded(Color.argb(60, Color.red(violet), Color.green(violet), Color.blue(violet)), 20)
                setOnClickListener {
                    when {
                        index == 3 && PcLink.isPaired(this@MainActivity) -> startActivity(Intent(this@MainActivity, RemoteActivity::class.java)) // сразу экран компьютера
                        else -> { tab = index; render() }
                    }
                }
            }
            item.addView(ImageView(this).apply { setImageDrawable(Icons.drawable(icon, if (selected) violet else muted)) }, LinearLayout.LayoutParams(dp(if (selected) 25 else 23), dp(if (selected) 25 else 23)).apply { gravity = Gravity.CENTER_HORIZONTAL; bottomMargin = dp(3) })
            item.addView(TextView(this).apply { text = tr(label); textSize = 10f; maxLines = 1; setTextColor(if (selected) violet else muted); gravity = Gravity.CENTER; typeface = if (selected) Typeface.DEFAULT_BOLD else Typeface.DEFAULT })
            val cell = FrameLayout(this)
            cell.addView(item, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
            bar.addView(cell, LinearLayout.LayoutParams(0, dp(56), 1f).apply { setMargins(dp(2), 0, dp(2), 0) })
        }
        root.addView(bar, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { setMargins(dp(12), dp(4), dp(12), dp(10)) })
        setContentView(root)

        codeField = EditText(this).apply {
            hint = tr("Шесть цифр из Authenticator"); inputType = InputType.TYPE_CLASS_NUMBER; imeOptions = EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING; styleInput(this)
        }
        when (tab) {
            0 -> drawHome()
            1 -> drawQuick()
            6 -> drawChat()
            3 -> drawRemote()
            4 -> { tab = 5; settingsPage = "connect"; drawSettingsTab() }
            else -> drawSettingsTab()
        }
    }

    private fun codeCard() {
        if (Auth.isEnrolled(this)) card("Код из Authenticator") { column.addView(codeField, spaced(top = 2)) }
    }

    // Отключение с согласия обеих сторон: здесь отпечаток, на компьютере — кнопка в настройках
    private var unlinkDialog = false

    private fun askUnlink() = confirmThen {
        toast("Жду подтверждения на компьютере…")
        thread {
            val state = PcLink.requestUnlink(this)
            runOnUiThread {
                if (state == null) toast("Компьютер не отвечает")
                else if (!state.optBoolean("pc")) styledDialog("Подтверди на компьютере", "Открой ZAVIRIA на компьютере: Настройки → Синхронизация и нажми «подтвердить отключение». Есть 5 минут.", "Понятно", "Отменить", {}, { thread { PcLink.cancelUnlink(this) } })
            }
        }
    }

    private fun checkUnlink(state: JSONObject) {
        if (state.optBoolean("done")) { PcLink.forget(this); syncGuard(); toast("Компьютер отключён"); render(); return }
        if (state.optBoolean("pc") && !state.optBoolean("phone") && !unlinkDialog) {
            unlinkDialog = true
            styledDialog("Компьютер просит отключить телефон", "Если это ты, подтверди отпечатком или кодом экрана. После этого связь с компьютером пропадёт.", "Подтвердить", "Отклонить",
                { unlinkDialog = false; askUnlink() }, { unlinkDialog = false; thread { PcLink.cancelUnlink(this) } })
        }
    }

    private fun drawComputer() {
        if (!PcLink.isPaired(this)) {
            card("Компьютер не подключён") { button("Связь с ПК") { tab = 5; settingsPage = "connect"; render() } }
            return
        }
        card("Управление компьютером") {
            tiles(listOf(
                Triple("🛰️", "Состояние") { send("/status") },
                Triple("🖼️", "Снимок экрана") { send("/screen") },
                Triple("🧩", "Реши с экрана") { send("/solve") },
                Triple("❓", "Что на экране") { send("/ask") },
                Triple("⏯", "Пауза") { send("/pause") },
                Triple("⏭", "Следующий") { send("/next") },
                Triple("📢", "Громче") { send("/louder") },
                Triple("🤫", "Тише") { send("/quieter") },
                Triple("🔐", "Заблокировать") { send("/lock") },
                Triple("🌚", "Выключить экран") { send("/off") },
                Triple("☀", "Включить экран") { send("/on") },
                Triple("🔆", "Показать вход") { send("/showlock") },
                Triple("⏻", "Выключить ПК") { confirmThen { send("phone:power_off") } },
                Triple("♻️", "Перезагрузить") { confirmThen { send("phone:power_restart") } },
                Triple("✖", "Отменить") { send("phone:power_cancel") },
            ))
        }
        card("Напиши, что сделать") {
            note("Как помощнице: «открой стим», «громкость 30», «какая погода».")
            val input = field("Сообщение", "", InputType.TYPE_CLASS_TEXT)
            button("Отправить") {
                val text = input.text.toString().trim()
                if (text.isNotEmpty()) { send(text); input.setText("") }
            }
        }
        val holder = column
        feed = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        holder.addView(feed, spaced(top = 8))
        drawFeed()
    }

    private fun drawConnect() {
        if (PcLink.isPaired(this)) {
            // Здесь только состояние: связь, аккаунт и синхронизация. Управление — во вкладке «Команды»
            lateinit var online: TextView
            card("Состояние") {
                online = note("Проверяю связь…")
                note("🔐 Связь защищена: ключ из SPAKE2, все запросы зашифрованы AES-256 от телефона до компьютера. Посредники и туннель видят только шифротекст.")
            }
            card("Аккаунт") {
                val bot = store.getString("bot_name", null)
                statusRow(if (bot != null) "Бот защиты @$bot" else "Бот защиты", bot != null)
                statusRow("Код из Authenticator", Auth.isEnrolled(this))
            }
            card("Синхронизация") {
                statusRow("Копия 2FA на компьютере", Auth.isEnrolled(this))
                statusRow("Работа в фоне", (getSystemService(POWER_SERVICE) as PowerManager).isIgnoringBatteryOptimizations(packageName))
                note("Копия кода переживёт переустановку приложения: вернётся при новом подключении.")
            }
            card("Включение ПК") {
                note("Компьютер включится, если в BIOS и у сетевой карты разрешено «пробуждение по сети» (Wake-on-LAN), а кабель подключён. Дома это работает через Wi-Fi. Издалека — только если роутер пересылает порт UDP 9 на рассылку в домашней сети: укажи его внешний адрес ниже.")
                val host = field("Адрес роутера для включения издалека (необязательно)", PcLink.wolHost(this), InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS)
                button("Сохранить адрес", primary = false) { PcLink.setWolHost(this, host.text.toString()); toast("Сохранено") }
                button("Включить компьютер") { wakePc() }
            }
            card("Отключение") {
                note("Чтобы отключиться, нужно подтвердить на телефоне (отпечаток или код экрана) и на компьютере (Настройки → Синхронизация).")
                button("Отключиться от ПК", primary = false) { askUnlink() }
            }
            thread {
                val state = PcLink.status(this)
                runOnUiThread { online.text = tr(if (state != null) "✓ Компьютер на связи" else "Компьютер сейчас не отвечает") }
            }
            return
        }
            card("Подключи компьютер") {
                note("На компьютере открой ZAVIRIA: Настройки → Синхронизация → «подключить телефон». Телефон должен быть в том же Wi-Fi. Введи код с экрана компьютера и нажми «Подключить» — компьютер найдётся сам.")
                val code = field("Код с экрана компьютера", "", InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_CAP_CHARACTERS or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS)
                val address = field("Адрес компьютера (необязательно)", "", InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS)
                button("Найти компьютер", primary = false) {
                    toast("Ищу компьютер…")
                    thread {
                        val found = runCatching { PcLink.discover(this) }.getOrNull()
                        runOnUiThread {
                            if (found == null) toast("Не нашла. Проверь, что на компьютере открыт код и Wi-Fi один")
                            else { address.setText(found.first); toast("Найден: ${found.second}") }
                        }
                    }
                }
                button("Подключить") {
                    val typed = code.text.toString()
                    val typedAddress = address.text.toString()
                    confirmThen {
                        toast(if (typedAddress.isBlank()) "Ищу компьютер в Wi-Fi…" else "Подключаю…")
                        thread {
                            // адрес не введён — компьютер сам сообщает его по Wi-Fi, пока на экране открыт код
                            val link = typedAddress.ifBlank { runCatching { PcLink.discover(this) }.getOrNull()?.first.orEmpty() }
                            val problem = if (link.isEmpty()) "Не нашла. Проверь, что на компьютере открыт код и Wi-Fi один"
                                else runCatching { PcLink.pair(this, link, typed) }.getOrElse { "Компьютер не отвечает" }
                            runOnUiThread { toast(problem ?: "Компьютер подключён"); if (problem == null) { syncGuard(); render() } }
                        }
                    }
                }
            }
    }

    // ------------------------------------------------------------ главная
    private var homeRefresh: (() -> Unit)? = null

    private fun meter(label: String): Pair<TextView, View> {
        val row = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        val head = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        head.addView(TextView(this).apply { text = tr(label); textSize = 14f; setTextColor(ink) }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        val value = TextView(this).apply { textSize = 14f; setTextColor(muted) }
        head.addView(value)
        row.addView(head)
        val track = FrameLayout(this).apply { background = rounded(tint(28), 4) }
        val fill = View(this).apply { background = GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT, intArrayOf(violet, deepViolet)).apply { cornerRadius = dp(4).toFloat() } }
        track.addView(fill, FrameLayout.LayoutParams(0, dp(8)))
        row.addView(track, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(8)).apply { topMargin = dp(4) })
        column.addView(row, spaced(top = 6, bottom = 6))
        return value to fill
    }

    private fun setMeter(m: Pair<TextView, View>, percent: Double, text: String) {
        m.first.text = text
        val parent = m.second.parent as View
        m.second.layoutParams = (m.second.layoutParams as FrameLayout.LayoutParams).apply { width = (parent.width * (percent / 100.0).coerceIn(0.0, 1.0)).toInt() }
    }

    // Плитка показателя: значок, название, температура, крупное значение, мини-график и строка подробностей
    private class Tile(val value: TextView, val temp: TextView, val spark: SparkView, val detail: TextView, val name: TextView)
    private val history = HashMap<String, ArrayDeque<Float>>() // последние ~1,5 минуты замеров для графиков

    private fun push(key: String, value: Double): List<Float> {
        val list = history.getOrPut(key) { ArrayDeque() }
        list.addLast(if (value.isNaN()) 0f else value.toFloat())
        while (list.size > 45) list.removeFirst()
        return list.toList()
    }

    private fun tile(parent: LinearLayout, icon: String, title: String, color: Int): Tile {
        val box = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL; setPadding(dp(12), dp(11), dp(12), dp(10))
            background = rounded(tint(20), 18, Color.argb(40, Color.red(color), Color.green(color), Color.blue(color)))
        }
        val head = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        head.addView(ImageView(this).apply { setImageDrawable(Icons.drawable(icon, color)) }, LinearLayout.LayoutParams(dp(17), dp(17)))
        head.addView(TextView(this).apply { text = tr(title); textSize = 12.5f; setTextColor(muted); setPadding(dp(6), 0, 0, 0); maxLines = 1 }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        val temp = TextView(this).apply { textSize = 11f; typeface = Typeface.DEFAULT_BOLD; setPadding(dp(6), dp(1), dp(6), dp(1)); visibility = View.GONE }
        head.addView(temp)
        box.addView(head)
        val value = TextView(this).apply { textSize = 21f; setTextColor(ink); typeface = Typeface.DEFAULT_BOLD; text = "—" }
        box.addView(value, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(4) })
        val spark = SparkView(this, color)
        box.addView(spark, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(30)).apply { topMargin = dp(2) })
        val name = TextView(this).apply { textSize = 10.5f; setTextColor(muted); maxLines = 1; ellipsize = android.text.TextUtils.TruncateAt.END; visibility = View.GONE }
        box.addView(name, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(3) })
        val detail = TextView(this).apply { textSize = 10.5f; setTextColor(muted); maxLines = 2 }
        box.addView(detail, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(1) })
        parent.addView(box, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply { setMargins(dp(4), dp(4), dp(4), dp(4)) })
        return Tile(value, temp, spark, detail, name)
    }

    // Зелёный — спокойно, жёлтый — тепло, красный — горячо
    private fun showTemp(view: TextView, value: Double, hot: Double) {
        if (value <= 0 || value.isNaN()) { view.visibility = View.GONE; return }
        val color = when { value >= hot -> Color.rgb(255, 95, 95); value >= hot - 15 -> Color.rgb(255, 190, 80); else -> Color.rgb(90, 220, 140) }
        view.visibility = View.VISIBLE
        view.text = "${value.toInt()}°"
        view.setTextColor(color)
        view.background = rounded(Color.argb(45, Color.red(color), Color.green(color), Color.blue(color)), 9)
    }

    private fun setName(tile: Tile, text: String) { tile.name.text = text; tile.name.visibility = if (text.isEmpty()) View.GONE else View.VISIBLE }

    private fun speed(kbps: Double) = if (kbps >= 1024) "%.1f ".format(kbps / 1024) + tr("МБ/с") else "${kbps.toInt()} " + tr("КБ/с")

    private fun fillTiles(s: JSONObject, cpu: Tile, ram: Tile, disk: Tile, gpu: Tile, net: Tile) {
        val gb = tr("ГБ")
        // процессор
        val load = s.optDouble("cpu")
        cpu.value.text = "${load.toInt()}%"
        cpu.spark.set(push("cpu", load))
        showTemp(cpu.temp, s.optDouble("cpuTemp", -1.0), 90.0)
        setName(cpu, s.optString("cpuName"))
        cpu.detail.text = listOfNotNull(s.optDouble("cpuGhz", 0.0).takeIf { it > 0 }?.let { "%.2f ".format(it) + tr("ГГц") },
            s.optInt("cores").takeIf { it > 0 }?.let { "$it " + tr("потоков") }).joinToString(" · ")
        // память
        ram.value.text = "${s.optDouble("ram").toInt()}%"
        ram.spark.set(push("ram", s.optDouble("ram")))
        ram.detail.text = "%.1f / %.1f $gb".format(s.optDouble("ramUsed"), s.optDouble("ramTotal"))
        // диск: значение — активность, ниже — место и скорость
        val total = s.optDouble("diskTotal"); val free = s.optDouble("diskFree")
        disk.value.text = "${s.optDouble("diskActive").toInt()}%"
        disk.spark.set(push("disk", s.optDouble("diskActive")))
        showTemp(disk.temp, s.optDouble("diskTemp", -1.0), 70.0)
        disk.detail.text = tr("свободно") + " %.0f $gb".format(free) + (if (total > 0) " / %.0f".format(total) else "") + "\n↓ %.1f ↑ %.1f ".format(s.optDouble("diskRead"), s.optDouble("diskWrite")) + tr("МБ/с")
        // видеокарта: отдельная, иначе встроенная
        val g = s.optDouble("gpu", -1.0)
        if (g >= 0) {
            gpu.value.text = "${g.toInt()}%"
            gpu.spark.set(push("gpu", g))
            showTemp(gpu.temp, s.optDouble("gpuTemp", -1.0), 85.0)
            setName(gpu, s.optString("gpuName"))
            gpu.detail.text = listOfNotNull(
                s.optDouble("vramTotal").takeIf { it > 0 }?.let { "%.1f / %.1f $gb".format(s.optDouble("vramUsed"), it) },
                s.optDouble("gpuPower", -1.0).takeIf { it >= 0 }?.let { "${it.toInt()} " + tr("Вт") },
                s.optDouble("gpuFan", -1.0).takeIf { it >= 0 }?.let { "🌀 ${it.toInt()}%" },
            ).joinToString(" · ")
        } else if (s.optDouble("igpu", -1.0) >= 0) {
            gpu.value.text = "${s.optDouble("igpu").toInt()}%"
            gpu.spark.set(push("gpu", s.optDouble("igpu")))
            setName(gpu, s.optString("igpuName"))
            gpu.detail.text = tr("Встроенная")
        } else { gpu.value.text = "—"; gpu.detail.text = tr("Видеокарта не найдена") }
        // сеть: график загрузки, шкала по самому большому замеру
        val down = s.optDouble("netDown")
        net.value.text = "↓ " + speed(down)
        net.spark.set(push("net", down), max = null)
        net.detail.text = "↑ " + speed(s.optDouble("netUp"))
    }

    // Мини-плеер «Сейчас играет»: обложка, название и исполнитель, большие кнопки и громкость
    private inner class Player(val title: TextView, val artist: TextView, val state: TextView, val play: ImageView, val volume: SeekBar, val volumeText: TextView, val eq: List<View>) {
        var playing = false
        fun update(s: JSONObject) {
            val full = s.optString("track")
            val parts = full.split(" — ", limit = 2)
            title.text = parts[0].ifEmpty { tr("Ничего не играет") }
            artist.text = parts.getOrNull(1).orEmpty()
            artist.visibility = if (artist.text.isEmpty()) View.GONE else View.VISIBLE
            playing = s.optBoolean("playing")
            state.text = tr(if (full.isEmpty()) "Тишина" else if (playing) "Играет на компьютере" else "На паузе")
            play.setImageDrawable(Icons.drawable(if (playing) "pause" else "play", violet))
            eq.forEach { it.animate().cancel() }
            if (playing) eq.forEachIndexed { i, bar -> pulse(bar, i) } else eq.forEach { it.scaleY = 0.35f }
            if (!volume.isPressed) { volume.progress = s.optInt("volume"); volumeText.text = if (s.optBoolean("muted")) tr("Без звука") else "${s.optInt("volume")}%" }
        }
        private fun pulse(bar: View, i: Int) {
            bar.animate().scaleY(0.3f + Math.random().toFloat() * 0.7f).setDuration(260L + i * 60).withEndAction { if (playing) pulse(bar, i) }.start()
        }
    }

    private fun miniPlayer(): Player {
        val box = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL; setPadding(dp(16), dp(16), dp(16), dp(14))
            background = GradientDrawable(GradientDrawable.Orientation.TL_BR, intArrayOf(Color.rgb(124, 58, 237), Color.rgb(192, 38, 211), Color.rgb(236, 72, 153))).apply { cornerRadius = dp(26).toFloat() }
            elevation = dp(6).toFloat()
        }
        val top = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        // «обложка»: нота и эквалайзер
        val art = FrameLayout(this).apply { background = rounded(Color.argb(60, 255, 255, 255), 16) }
        art.addView(ImageView(this).apply { setImageDrawable(Icons.drawable("music", Color.WHITE)) }, FrameLayout.LayoutParams(dp(26), dp(26), Gravity.CENTER or Gravity.TOP).apply { topMargin = dp(9) })
        val eqRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.BOTTOM }
        val bars = (0 until 4).map { View(this).apply { background = rounded(Color.WHITE, 2); pivotY = dp(14).toFloat(); scaleY = 0.35f } }
        bars.forEach { eqRow.addView(it, LinearLayout.LayoutParams(dp(4), dp(14)).apply { setMargins(dp(1), 0, dp(1), 0) }) }
        art.addView(eqRow, FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(14), Gravity.CENTER_HORIZONTAL or Gravity.BOTTOM).apply { bottomMargin = dp(8) })
        top.addView(art, LinearLayout.LayoutParams(dp(64), dp(64)))
        val texts = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(14), 0, 0, 0) }
        val state = TextView(this).apply { textSize = 11f; setTextColor(Color.argb(210, 255, 255, 255)); text = "…"; letterSpacing = 0.04f }
        val title = TextView(this).apply {
            textSize = 17f; setTextColor(Color.WHITE); typeface = Typeface.DEFAULT_BOLD; isSingleLine = true
            ellipsize = android.text.TextUtils.TruncateAt.MARQUEE; marqueeRepeatLimit = -1; isSelected = true; text = "—"
        }
        val artist = TextView(this).apply { textSize = 13f; setTextColor(Color.argb(220, 255, 255, 255)); isSingleLine = true; ellipsize = android.text.TextUtils.TruncateAt.END }
        texts.addView(state); texts.addView(title); texts.addView(artist)
        top.addView(texts, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        box.addView(top)
        // кнопки: назад, большая пауза, дальше
        val controls = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER }
        fun round(icon: String, size: Int, filled: Boolean, action: () -> Unit): ImageView {
            val image = ImageView(this).apply { setImageDrawable(Icons.drawable(icon, if (filled) violet else Color.WHITE)) }
            controls.addView(FrameLayout(this).apply {
                background = GradientDrawable().apply { shape = GradientDrawable.OVAL; setColor(if (filled) Color.WHITE else Color.argb(55, 255, 255, 255)) }
                elevation = if (filled) dp(4).toFloat() else 0f
                isClickable = true; foreground = android.graphics.drawable.RippleDrawable(android.content.res.ColorStateList.valueOf(Color.argb(60, 255, 255, 255)), null, GradientDrawable().apply { shape = GradientDrawable.OVAL; setColor(Color.WHITE) })
                setOnClickListener { animate().scaleX(0.9f).scaleY(0.9f).setDuration(80).withEndAction { animate().scaleX(1f).scaleY(1f).setDuration(120).start() }.start(); action() }
                addView(image, FrameLayout.LayoutParams(dp(size * 45 / 100), dp(size * 45 / 100), Gravity.CENTER))
            }, LinearLayout.LayoutParams(dp(size), dp(size)).apply { setMargins(dp(14), 0, dp(14), 0) })
            return image
        }
        lateinit var player: Player
        round("prev", 48, false) { thread { PcLink.action(this, "prev"); homeRefresh?.let { runOnUiThread(it) } } }
        val play = round("play", 64, true) {
            player.playing = !player.playing // сразу меняем значок, не дожидаясь компьютера
            player.play.setImageDrawable(Icons.drawable(if (player.playing) "pause" else "play", violet))
            thread { PcLink.action(this, "play"); Thread.sleep(400); homeRefresh?.let { runOnUiThread(it) } }
        }
        round("next", 48, false) { thread { PcLink.action(this, "next"); Thread.sleep(400); homeRefresh?.let { runOnUiThread(it) } } }
        box.addView(controls, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(14) })
        // громкость
        val volumeRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        volumeRow.addView(ImageView(this).apply { setImageDrawable(Icons.drawable("volume_down", Color.WHITE)) }, LinearLayout.LayoutParams(dp(20), dp(20)))
        val volumeText = TextView(this).apply { textSize = 12f; setTextColor(Color.WHITE); typeface = Typeface.DEFAULT_BOLD; gravity = Gravity.END }
        val volume = SeekBar(this).apply {
            max = 100
            progressTintList = android.content.res.ColorStateList.valueOf(Color.WHITE)
            progressBackgroundTintList = android.content.res.ColorStateList.valueOf(Color.argb(120, 255, 255, 255))
            thumbTintList = android.content.res.ColorStateList.valueOf(Color.WHITE)
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(bar: SeekBar?, value: Int, fromUser: Boolean) { if (fromUser) volumeText.text = "$value%" }
                override fun onStartTrackingTouch(bar: SeekBar?) {}
                override fun onStopTrackingTouch(bar: SeekBar?) { val level = bar?.progress ?: return; thread { PcLink.action(this@MainActivity, "volume_set", level.toString()) } }
            })
        }
        volumeRow.addView(volume, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        volumeRow.addView(volumeText, LinearLayout.LayoutParams(dp(40), ViewGroup.LayoutParams.WRAP_CONTENT))
        box.addView(volumeRow, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(10) })
        column.addView(box, spaced(top = 4, bottom = 10))
        player = Player(title, artist, state, play, volume, volumeText, bars)
        return player
    }

    private fun drawHome() {
        if (!PcLink.isPaired(this)) {
            card("Компьютер не подключён") { button("Связь с ПК") { tab = 5; settingsPage = "connect"; render() } }
            return
        }
        card("Компьютер не отвечает") {
            note("Если компьютер выключен, его можно включить по сети.")
            button("Включить компьютер") { wakePc() }
        }
        val wakeCard = column.getChildAt(column.childCount - 1).apply { visibility = View.GONE }
        var misses = 0
        val player = miniPlayer()
        // Компактно: сетка плиток с мини-графиками, температура цветом в углу
        // Загрузка компьютера кольцами — как на главной ZAVIRIA на ПК
        val rings = ArrayList<RingView>()
        val subs = ArrayList<TextView>()
        lateinit var line: TextView
        card("Компьютер") {
            val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
            for (label in listOf("Процессор", "Память", "Видеокарта")) {
                val cell = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; gravity = Gravity.CENTER_HORIZONTAL }
                val ring = RingView(this, ink, tint(30)); rings += ring
                cell.addView(ring, LinearLayout.LayoutParams(dp(74), dp(74)))
                cell.addView(TextView(this).apply { text = tr(label); textSize = 12.5f; setTextColor(ink); gravity = Gravity.CENTER; setPadding(0, dp(6), 0, 0) })
                val sub = TextView(this).apply { textSize = 11f; setTextColor(muted); gravity = Gravity.CENTER }
                subs += sub; cell.addView(sub)
                row.addView(cell, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            }
            column.addView(row)
            line = TextView(this).apply { textSize = 12f; setTextColor(muted); setPadding(dp(4), dp(12), dp(4), 0) }
            column.addView(line)
        }
        homeRefresh = {
            thread {
                val s = PcLink.status(this)
                misses = if (s == null) misses + 1 else 0
                runOnUiThread { wakeCard.visibility = if (misses >= 2 && PcLink.wolMac(this).isNotEmpty()) View.VISIBLE else View.GONE }
                if (s == null) return@thread
                runOnUiThread {
                    if (tab != 0) return@runOnUiThread
                    fillRings(s, rings, subs, line)
                    player.update(s)
                }
            }
        }
        homeRefresh?.invoke()
    }

    private fun fillRings(s: JSONObject, rings: List<RingView>, subs: List<TextView>, line: TextView) {
        val gb = tr("ГБ")
        rings[0].set(s.optDouble("cpu"))
        val cpuTemp = s.optDouble("cpuTemp", -1.0)
        subs[0].text = if (cpuTemp > 0) "${cpuTemp.toInt()}°" else s.optDouble("cpuGhz", 0.0).takeIf { it > 0 }?.let { "%.1f ".format(it) + tr("ГГц") }.orEmpty()
        rings[1].set(s.optDouble("ram"))
        subs[1].text = "%.0f/%.0f $gb".format(s.optDouble("ramUsed"), s.optDouble("ramTotal"))
        val g = s.optDouble("gpu", -1.0)
        if (g >= 0) {
            rings[2].set(g)
            val t = s.optDouble("gpuTemp", -1.0)
            subs[2].text = if (t > 0) "${t.toInt()}°" else "%.1f/%.0f $gb".format(s.optDouble("vramUsed"), s.optDouble("vramTotal"))
            subs[2].setTextColor(if (t >= 80) Color.rgb(248, 113, 113) else muted)
        } else { rings[2].set(s.optDouble("diskActive")); subs[2].text = tr("диск") }
        subs[0].setTextColor(if (cpuTemp >= 85) Color.rgb(248, 113, 113) else muted)
        line.text = tr("сеть") + " ↓ %.0f ↑ %.0f ".format(s.optDouble("netDown"), s.optDouble("netUp")) + tr("КБ/с") + "   ·   C: " + tr("свободно") + " %.0f $gb".format(s.optDouble("diskFree"))
    }

    // ------------------------------------------------------------ чат с Аней (как на главной ZAVIRIA на ПК)
    private val chat = ArrayList<Triple<Boolean, String?, Bitmap?>>() // true — моё сообщение
    private var chatBox: LinearLayout? = null

    private fun bubble(mine: Boolean, text: String?, bitmap: Bitmap?): View {
        val holder = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = if (mine) Gravity.END else Gravity.START }
        val body = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(12), dp(9), dp(12), dp(9))
            background = if (mine) GradientDrawable(GradientDrawable.Orientation.TL_BR, intArrayOf(Color.argb(210, Color.red(violet), Color.green(violet), Color.blue(violet)), Color.argb(200, Color.red(deepViolet), Color.green(deepViolet), Color.blue(deepViolet)))).apply { cornerRadii = floatArrayOf(dp(16f), dp(16f), dp(16f), dp(16f), dp(4f), dp(4f), dp(16f), dp(16f)) }
                         else rounded(tint(22), 16)
        }
        bitmap?.let { body.addView(ImageView(this).apply { setImageBitmap(it); adjustViewBounds = true }, LinearLayout.LayoutParams(dp(220), ViewGroup.LayoutParams.WRAP_CONTENT)) }
        text?.let { body.addView(TextView(this).apply { this.text = it; textSize = 14.5f; setTextColor(if (mine) Color.WHITE else ink); setTextIsSelectable(true) }) }
        holder.addView(body, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { if (mine) leftMargin = dp(48) else rightMargin = dp(48) })
        return holder
    }

    private fun drawChat() {
        if (!PcLink.isPaired(this)) {
            card("Компьютер не подключён") { button("Связь с ПК") { tab = 5; settingsPage = "connect"; render() } }
            return
        }
        column.addView(TextView(this).apply { text = tr("Аня"); textSize = 22f; setTextColor(ink); typeface = Typeface.DEFAULT_BOLD; setPadding(dp(6), dp(4), 0, dp(8)) })
        val box = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        chatBox = box
        if (chat.isEmpty()) box.addView(TextView(this).apply { text = tr("Напиши Ане — она сделает на компьютере, что попросишь, и ответит здесь."); textSize = 14f; setTextColor(muted); gravity = Gravity.CENTER; setPadding(dp(20), dp(40), dp(20), dp(40)) })
        chat.forEach { (mine, text, bitmap) -> box.addView(bubble(mine, text, bitmap), spaced(top = 4, bottom = 4)) }
        column.addView(box)
        // поле ввода: «📸» — снимок экрана ПК, микрофон — голос телефона, ➤ — отправить
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(6), dp(4), dp(6), dp(4)); background = rounded(cardColors[0] or (0xFF shl 24), 20, tint(20))
        }
        row.addView(TextView(this).apply { text = "📸"; textSize = 20f; setPadding(dp(8), 0, dp(6), 0); setOnClickListener { chatSend("/screen", tr("📸 снимок экрана")) } })
        val input = EditText(this).apply { hint = tr("Написать Ане…"); setTextColor(ink); setHintTextColor(muted); background = null; textSize = 15f; maxLines = 4 }
        row.addView(input, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        row.addView(FrameLayout(this).apply {
            background = GradientDrawable(GradientDrawable.Orientation.TL_BR, intArrayOf(violet, deepViolet)).apply { shape = GradientDrawable.OVAL }
            addView(ImageView(this@MainActivity).apply { setImageDrawable(Icons.drawable("send", Color.WHITE)) }, FrameLayout.LayoutParams(dp(18), dp(18), Gravity.CENTER))
            setOnClickListener { val t = input.text.toString().trim(); if (t.isNotEmpty()) { input.setText(""); chatSend(t, t) } }
        }, LinearLayout.LayoutParams(dp(40), dp(40)))
        column.addView(row, spaced(top = 10))
    }

    private fun chatSend(command: String, shown: String) {
        chat.add(Triple(true, shown, null))
        while (chat.size > 60) chat.removeAt(0)
        if (tab == 6) render()
        send(command)
    }

    private fun wakePc() {
        if (PcLink.wolMac(this).isEmpty()) { toast("Компьютер ещё ни разу не сообщил свой адрес: открой приложение, пока он включён."); return }
        thread {
            val sent = runCatching { PcLink.wake(this) }.getOrDefault(0)
            runOnUiThread { toast(if (sent > 0) "Сигнал включения отправлен. Подожди минуту." else "Компьютер не отвечает") }
        }
    }

    private fun startHomeLoop() {
        if (homeLooping) return
        homeLooping = true
        thread {
            while (homeLooping) {
                Thread.sleep(2000)
                if (tab == 0) runOnUiThread { homeRefresh?.invoke() }
            }
        }
    }
    @Volatile private var homeLooping = false

    // ------------------------------------------------------------ свои быстрые команды из пульта
    // Быстрые команды: свои кнопки из пульта + модули ZAVIRIA. Зажми кнопку — правка: перетаскивание, ✕ удалить, + добавить
    private class Quick(val id: String, val icon: String, val label: String, val kind: String, val a: String, val arg: String = "")
    private var editQuick = false
    private var pultQuick: List<Quick> = emptyList()

    private val modules = listOf(
        Quick("m:status", "🛰️", "Состояние", "cmd", "/status"),
        Quick("m:screen", "🖼️", "Снимок экрана", "cmd", "/screen"),
        Quick("m:solve", "🧩", "Реши с экрана", "cmd", "/solve"),
        Quick("m:ask", "🔮", "Что на экране", "cmd", "/ask"),
        Quick("m:play", "🎧", "Пауза", "action", "play"),
        Quick("m:prev", "⏪", "Назад", "action", "prev"),
        Quick("m:next", "⏩", "Дальше", "action", "next"),
        Quick("m:louder", "📢", "Громче", "cmd", "/louder"),
        Quick("m:quieter", "🤫", "Тише", "cmd", "/quieter"),
        Quick("m:lock", "🔐", "Заблокировать", "cmd", "/lock"),
        Quick("m:off", "🌚", "Выключить экран", "cmd", "/off"),
        Quick("m:on", "🌞", "Включить экран", "cmd", "/on"),
        Quick("m:power", "💤", "Выключить ПК", "cmd", "phone:power_off"),
        Quick("m:restart", "♻️", "Перезагрузить", "cmd", "phone:power_restart"),
        Quick("m:cancel", "🙅", "Отменить выключение", "cmd", "phone:power_cancel"),
        Quick("m:game", "🎮", "Игровой режим", "cmd", "игровой режим"),
    )

    private fun pultItems(config: JSONObject): List<Quick> {
        val list = ArrayList<Quick>()
        for (key in listOf("home", "apps")) {
            val array = config.optJSONArray(key) ?: continue
            for (i in 0 until array.length()) {
                val item = array.getJSONObject(i)
                val action = item.optString("a").ifEmpty { "launch" }
                val arg = item.optString("arg").ifEmpty { item.optString("target") }
                val label = item.optString("label").ifEmpty { arg }
                val icon = item.optString("icon").ifEmpty { when (action) { "launch" -> "🚀"; "url", "open" -> "🪐"; "scenario" -> "🪄"; "shutdown" -> "💤"; "restart" -> "♻️"; else -> "⚡" } }
                list += Quick("p:$action|$arg", icon, label, "action", action, arg)
            }
        }
        return list.distinctBy { it.id }
    }

    // Свои кнопки запуска: любое приложение компьютера, название и эмодзи — какие захочешь
    private fun customQuick(): List<Quick> {
        val list = runCatching { JSONArray(store.getString("quick_custom", "[]")) }.getOrDefault(JSONArray())
        return (0 until list.length()).map { list.getJSONObject(it) }.map { Quick(it.getString("id"), it.optString("icon", "🚀"), it.getString("label"), "action", "launch", it.getString("app")) }
    }

    private fun saveCustom(items: List<Quick>) = store.edit().putString("quick_custom", JSONArray().apply {
        items.forEach { put(JSONObject().put("id", it.id).put("icon", it.icon).put("label", it.label).put("app", it.arg)) }
    }.toString()).apply()

    // Свои название и эмодзи для кнопок пульта и модулей
    private fun overrides(): JSONObject = runCatching { JSONObject(store.getString("quick_over", "{}")) }.getOrDefault(JSONObject())

    private fun allQuick(): List<Quick> {
        val over = overrides()
        return (customQuick() + pultQuick + modules).map { q ->
            over.optJSONObject(q.id)?.let { o -> Quick(q.id, o.optString("icon", q.icon), o.optString("label", q.label), q.kind, q.a, q.arg) } ?: q
        }
    }

    private fun guessEmoji(name: String): String {
        val n = name.lowercase()
        return listOf("steam" to "🎮", "epic" to "🎮", "game" to "🎮", "игр" to "🎮", "discord" to "💬", "telegram" to "✈️", "whatsapp" to "💬", "chrome" to "🌐", "edge" to "🌐",
            "opera" to "🌐", "firefox" to "🦊", "yandex" to "🌐", "spotify" to "🎵", "music" to "🎵", "музык" to "🎵", "vlc" to "🎬", "obs" to "🎥", "code" to "💻", "studio" to "💻",
            "word" to "📝", "excel" to "📊", "powerpoint" to "📽️", "notepad" to "🗒️", "блокнот" to "🗒️", "photoshop" to "🎨", "paint" to "🎨", "blender" to "🧊", "roblox" to "🧱",
            "minecraft" to "⛏️", "wallpaper" to "🖼️", "calc" to "🧮", "калькул" to "🧮", "explorer" to "📁", "проводник" to "📁", "settings" to "⚙️", "параметр" to "⚙️",
            "terminal" to "⌨️", "cmd" to "⌨️", "zoom" to "📹", "skype" to "📞", "torrent" to "🧲", "mail" to "✉️", "почт" to "✉️", "camera" to "📷", "камер" to "📷")
            .firstOrNull { (k, _) -> k in n }?.second ?: "🚀"
    }

    private val emojiChoices = listOf("🚀", "🎮", "💬", "🌐", "🎵", "🎬", "🎥", "💻", "📝", "📊", "🎨", "🧱", "⛏️", "🖼️", "📁", "⚙️", "⌨️", "📞", "✈️", "🦊",
        "⭐", "🔥", "💜", "✨", "🧠", "📸", "🔊", "🌙", "☀️", "🔒", "🔌", "🔄", "🍿", "🎧", "📚", "🛒", "🧮", "🗒️", "🐱", "👾")

    // Окно правки кнопки: название, эмодзи и удаление
    private fun editQuickDialog(q: Quick, isNew: Boolean, ids: MutableList<String>, redraw: () -> Unit) {
        val box = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(20), dp(8), dp(20), dp(4)) }
        if (q.kind == "action" && q.a == "launch") box.addView(TextView(this).apply { text = tr("Приложение") + ": " + q.arg; textSize = 12.5f; setTextColor(muted); setPadding(0, 0, 0, dp(8)) })
        val name = EditText(this).apply { setText(tr(q.label)); setSingleLine(); styleInput(this); hint = tr("Название кнопки") }
        box.addView(name)
        var icon = q.icon
        val preview = TextView(this).apply { text = icon; textSize = 30f; gravity = Gravity.CENTER; setPadding(0, dp(10), 0, dp(4)) }
        box.addView(preview)
        val grid = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        emojiChoices.chunked(8).forEach { row ->
            val line = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
            row.forEach { e -> line.addView(TextView(this).apply { text = e; textSize = 21f; gravity = Gravity.CENTER; setPadding(0, dp(6), 0, dp(6)); setOnClickListener { icon = e; preview.text = e } }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)) }
            grid.addView(line)
        }
        box.addView(grid)
        val dialog = android.app.AlertDialog.Builder(this).setTitle(tr(if (isNew) "Новая кнопка" else "Изменить кнопку")).setView(android.widget.ScrollView(this).apply { addView(box) })
            .setPositiveButton(tr("Сохранить")) { _, _ ->
                val label = name.text.toString().trim().ifEmpty { q.label }
                if (q.id.startsWith("c:")) {
                    val list = customQuick().filter { it.id != q.id } + Quick(q.id, icon, label, q.kind, q.a, q.arg)
                    saveCustom(list)
                } else {
                    val over = overrides(); over.put(q.id, JSONObject().put("label", label).put("icon", icon)); store.edit().putString("quick_over", over.toString()).apply()
                }
                if (q.id !in ids) { ids += q.id; saveLayout(ids) }
                redraw()
            }
            .setNegativeButton(tr("Отмена"), null)
        if (!isNew) dialog.setNeutralButton(tr("Удалить")) { _, _ ->
            ids.remove(q.id); saveLayout(ids)
            if (q.id.startsWith("c:")) saveCustom(customQuick().filter { it.id != q.id })
            redraw()
        }
        dialog.show()
    }

    // Выбор любого приложения компьютера: список из «Пуска» с поиском
    private fun pickPcApp(ids: MutableList<String>, redraw: () -> Unit) {
        toast(tr("Загружаю приложения компьютера…"))
        thread {
            val apps = PcLink.apps(this)
            runOnUiThread {
                if (apps.isNullOrEmpty()) { toast(tr(if (apps == null) "Компьютер не отвечает" else "Компьютер не нашёл приложений")); return@runOnUiThread }
                val sorted = apps.distinct().sortedBy { it.lowercase() }
                val box = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(16), dp(8), dp(16), 0) }
                val search = EditText(this).apply { hint = tr("Поиск приложения"); setSingleLine(); styleInput(this) }
                box.addView(search)
                val list = android.widget.ListView(this)
                val adapter = android.widget.ArrayAdapter(this, android.R.layout.simple_list_item_1, sorted.toMutableList())
                list.adapter = adapter
                box.addView(list, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(380)))
                val dialog = android.app.AlertDialog.Builder(this).setTitle(tr("Приложение с компьютера")).setView(box).setNegativeButton(tr("Отмена"), null).create()
                search.addTextChangedListener(object : android.text.TextWatcher {
                    override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
                    override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
                    override fun afterTextChanged(s: android.text.Editable?) {
                        val q = s?.toString().orEmpty().trim().lowercase()
                        adapter.clear(); adapter.addAll(sorted.filter { q.isEmpty() || q in it.lowercase() }); adapter.notifyDataSetChanged()
                    }
                })
                list.setOnItemClickListener { _, _, position, _ ->
                    val app = adapter.getItem(position) ?: return@setOnItemClickListener
                    dialog.dismiss()
                    val q = Quick("c:" + java.util.UUID.randomUUID().toString().take(8), guessEmoji(app), app, "action", "launch", app)
                    saveCustom(customQuick() + q)
                    editQuickDialog(q, isNew = true, ids = ids, redraw = redraw)
                }
                dialog.show()
            }
        }
    }

    private fun quickLayout(): MutableList<String> {
        val saved = store.getString("quick_layout", null)?.let { s -> runCatching { JSONArray(s) }.getOrNull() }
        if (saved != null) return (0 until saved.length()).map { saved.getString(it) }.toMutableList()
        return (pultQuick.map { it.id } + listOf("m:status", "m:screen", "m:play", "m:next", "m:louder", "m:quieter", "m:lock", "m:off")).toMutableList()
    }

    private fun saveLayout(ids: List<String>) = store.edit().putString("quick_layout", JSONArray(ids).toString()).apply()

    private fun runQuick(q: Quick) {
        val dangerous = q.a in setOf("phone:power_off", "phone:power_restart", "shutdown", "restart")
        val run = {
            if (q.kind == "cmd") send(q.a)
            else thread { val reply = PcLink.action(this, q.a, q.arg); runOnUiThread { toast(reply ?: tr("Компьютер не отвечает")) } }
        }
        if (dangerous) confirmThen { run() } else run()
    }

    private fun drawQuick() {
        if (!PcLink.isPaired(this)) {
            card("Компьютер не подключён") { button("Связь с ПК") { tab = 5; settingsPage = "connect"; render() } }
            return
        }
        store.getString("pult_cache", null)?.let { s -> runCatching { pultQuick = pultItems(JSONObject(s)) } }
        val holder = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        lateinit var hint: TextView
        card("Запуск") {
            hint = note("")
            column.addView(holder)
        }
        fun redraw() {
            hint.text = tr(if (editQuick) "Перетаскивай кнопки, нажми — изменить, ✕ — удалить, «+» — добавить приложение" else "Нажми «+», чтобы добавить приложение. Зажми кнопку, чтобы переставить")
            buildQuickGrid(holder) { redraw() }
        }
        redraw()
        thread { // свежие кнопки пульта с компьютера
            val config = PcLink.config(this) ?: return@thread
            store.edit().putString("pult_cache", config.toString()).apply()
            runOnUiThread { pultQuick = pultItems(config); if (tab == 1) redraw() }
        }
        val holderFeed = column
        feed = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL } // ответы компьютера (снимки, состояние)
        holderFeed.addView(feed, spaced(top = 8))
        drawFeed()
    }

    private fun buildQuickGrid(holder: LinearLayout, redraw: () -> Unit) {
        holder.removeAllViews()
        val known = allQuick().associateBy { it.id }
        val ids = quickLayout().filter { it in known }.toMutableList()
        val cells = ids.map { known.getValue(it) }.map { it as Quick? }.toMutableList()
        cells += null // плитка «+ Добавить» видна всегда
        cells.chunked(3).forEach { row ->
            val line = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
            row.forEach { q ->
                val tile = FrameLayout(this)
                val body = LinearLayout(this).apply {
                    orientation = LinearLayout.VERTICAL; gravity = Gravity.CENTER
                    background = rounded(if (q == null) tint(10) else tint(22), 18, Color.argb(if (editQuick) 110 else 50, Color.red(violet), Color.green(violet), Color.blue(violet)))
                }
                body.addView(iconBubble(q?.icon ?: "add", 44), LinearLayout.LayoutParams(dp(44), dp(44)).apply { gravity = Gravity.CENTER_HORIZONTAL; bottomMargin = dp(5) })
                body.addView(TextView(this).apply { text = tr(q?.label ?: "Добавить"); textSize = 12f; setTextColor(ink); gravity = Gravity.CENTER; maxLines = 2; setPadding(dp(4), 0, dp(4), 0) })
                tile.addView(body, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
                if (q == null) body.setOnClickListener { addModule(ids, redraw) }
                else {
                    body.setOnClickListener { if (editQuick) editQuickDialog(q, isNew = false, ids = ids, redraw = redraw) else runQuick(q) }
                    body.setOnLongClickListener { v ->
                        if (!editQuick) { editQuick = true; redraw() }
                        else v.startDragAndDrop(android.content.ClipData.newPlainText("id", q.id), View.DragShadowBuilder(v), q.id, 0)
                        true
                    }
                    // перетаскивание: бросили на другую кнопку — встаёт на её место
                    body.setOnDragListener { v, e ->
                        when (e.action) {
                            android.view.DragEvent.ACTION_DRAG_ENTERED -> { v.alpha = 0.55f; true }
                            android.view.DragEvent.ACTION_DRAG_EXITED, android.view.DragEvent.ACTION_DRAG_ENDED -> { v.alpha = 1f; true }
                            android.view.DragEvent.ACTION_DROP -> {
                                val moving = e.localState as? String
                                if (moving != null && moving != q.id) {
                                    ids.remove(moving); ids.add(ids.indexOf(q.id).coerceAtLeast(0), moving)
                                    saveLayout(ids); v.post { redraw() }
                                }
                                true
                            }
                            else -> true
                        }
                    }
                    if (editQuick) {
                        body.startAnimation(android.view.animation.RotateAnimation(-1.5f, 1.5f, android.view.animation.Animation.RELATIVE_TO_SELF, 0.5f, android.view.animation.Animation.RELATIVE_TO_SELF, 0.5f).apply {
                            duration = 140; repeatCount = android.view.animation.Animation.INFINITE; repeatMode = android.view.animation.Animation.REVERSE
                        })
                        tile.addView(FrameLayout(this).apply {
                            background = GradientDrawable().apply { shape = GradientDrawable.OVAL; setColor(Color.rgb(239, 68, 68)) }
                            addView(ImageView(this@MainActivity).apply { setImageDrawable(Icons.drawable("close", Color.WHITE)) }, FrameLayout.LayoutParams(dp(14), dp(14), Gravity.CENTER))
                            setOnClickListener { ids.remove(q.id); saveLayout(ids); redraw() }
                        }, FrameLayout.LayoutParams(dp(24), dp(24), Gravity.TOP or Gravity.END))
                    }
                }
                line.addView(tile, LinearLayout.LayoutParams(0, dp(94), 1f).apply { setMargins(dp(4), dp(4), dp(4), dp(4)) })
            }
            repeat(3 - row.size) { line.addView(View(this), LinearLayout.LayoutParams(0, 0, 1f)) }
            holder.addView(line)
        }
        val saved = column // правка — и кнопкой, и долгим нажатием на любую плитку
        column = holder
        if (editQuick) button("Готово") { editQuick = false; redraw() }
        else button("Изменить панель", primary = false) { editQuick = true; redraw() }
        column = saved
    }

    private fun addModule(ids: MutableList<String>, redraw: () -> Unit) {
        val free = (pultQuick + modules).filter { it.id !in ids }
        val choices = arrayOf(tr("💻 Приложение с компьютера"), tr("⚡ Модуль ZAVIRIA"))
        android.app.AlertDialog.Builder(this).setTitle(tr("Добавить"))
            .setItems(choices) { _, which ->
                if (which == 0) pickPcApp(ids, redraw)
                else {
                    if (free.isEmpty()) { toast(tr("Все модули уже на панели")); return@setItems }
                    val names = free.map { (if (it.id.startsWith("p:")) "★ " else "") + it.icon + "  " + tr(it.label) }.toTypedArray()
                    android.app.AlertDialog.Builder(this).setTitle(tr("Модуль ZAVIRIA"))
                        .setItems(names) { _, i -> ids += free[i].id; saveLayout(ids); redraw() }
                        .setNegativeButton(tr("Отмена"), null).show()
                }
            }
            .setNegativeButton(tr("Отмена"), null).show()
    }

    private fun drawRemote() { // при подключённом компьютере вкладка сразу открывает экран
        if (!PcLink.isPaired(this)) {
            card("Компьютер не подключён") { button("Связь с ПК") { tab = 5; settingsPage = "connect"; render() } }
            return
        }
        card("Экран ПК") { button("Открыть экран") { startActivity(Intent(this, RemoteActivity::class.java)) } }
    }

    private var settingsPage: String? = null
    private var updateText: String? = null

    private fun drawUpdate() = card("Обновление приложения") {
        note("Свежая версия лежит на компьютере рядом с ZAVIRIA. Телефон заберёт её по зашифрованной связи и поставит — без USB и отладки.")
        updateText?.let { note(it, ink) }
        if (!PcLink.isPaired(this)) { note("Сначала подключи компьютер.", warm); return@card }
        if (!AppUpdate.allowed(this)) {
            note("Один раз разреши ZAVIRIA ставить обновления.", warm)
            button("Разрешить", primary = false) { AppUpdate.askPermission(this) }
        }
        button("Проверить и обновить") {
            updateText = tr("Проверяю…"); render()
            thread {
                val mine = AppUpdate.installed(this)
                val there = AppUpdate.available(this)
                val text = when {
                    there == null -> "Компьютер не отвечает"
                    there <= 0L -> "На компьютере нет приложения для телефона — обнови ZAVIRIA на ПК"
                    there <= mine -> "Уже последняя версия"
                    else -> AppUpdate.install(this) ?: "Ставлю обновление…"
                }
                runOnUiThread { updateText = tr(text); if (settingsPage == "update") render() }
            }
        }
    }

    private fun settingsRow(parent: LinearLayout, icon: String, color: Int, title: String, value: String, page: String) {
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL; setPadding(dp(4), dp(10), dp(2), dp(10))
            background = android.graphics.drawable.RippleDrawable(android.content.res.ColorStateList.valueOf(Color.argb(40, 255, 255, 255)), null, null)
            setOnClickListener { settingsPage = page; render() }
        }
        row.addView(FrameLayout(this).apply {
            background = GradientDrawable(GradientDrawable.Orientation.TL_BR, intArrayOf(color, Color.rgb(Color.red(color) * 3 / 4, Color.green(color) * 3 / 4, Color.blue(color) * 3 / 4))).apply { cornerRadius = dp(10).toFloat() }
            addView(ImageView(this@MainActivity).apply { setImageDrawable(Icons.drawable(icon, Color.WHITE)) }, FrameLayout.LayoutParams(dp(19), dp(19), Gravity.CENTER))
        }, LinearLayout.LayoutParams(dp(34), dp(34)))
        val texts = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(14), 0, dp(8), 0) }
        texts.addView(TextView(this).apply { text = tr(title); textSize = 15.5f; setTextColor(ink) })
        if (value.isNotEmpty()) texts.addView(TextView(this).apply { text = tr(value); textSize = 12.5f; setTextColor(muted) })
        row.addView(texts, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        row.addView(ImageView(this).apply { setImageDrawable(Icons.drawable("chevron", muted)) }, LinearLayout.LayoutParams(dp(22), dp(22)))
        parent.addView(row)
    }

    private fun settingsGroup(title: String, fill: (LinearLayout) -> Unit) {
        column.addView(TextView(this).apply { text = tr(title).uppercase(); textSize = 11.5f; setTextColor(muted); letterSpacing = 0.08f; setPadding(dp(8), dp(14), 0, dp(6)) })
        val box = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL; setPadding(dp(14), dp(4), dp(10), dp(4))
            background = GradientDrawable(GradientDrawable.Orientation.TL_BR, cardColors).apply { cornerRadius = dp(20).toFloat(); setStroke(dp(1), Color.argb(50, 190, 160, 255)) }
        }
        fill(box)
        column.addView(box, spaced(bottom = 4))
    }

    private fun drawSettingsTab() {
        val page = settingsPage
        if (page != null) {
            // шапка страницы: назад к списку
            column.addView(LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL; setPadding(dp(4), dp(6), 0, dp(10))
                setOnClickListener { settingsPage = null; render() }
                addView(ImageView(this@MainActivity).apply { setImageDrawable(Icons.drawable("back", violet)) }, LinearLayout.LayoutParams(dp(22), dp(22)))
                addView(TextView(this@MainActivity).apply { text = tr("Настройки"); textSize = 15f; setTextColor(violet); setPadding(dp(8), 0, 0, 0) })
            })
            when (page) {
                "protect" -> drawPhone()
                "bot" -> { codeCard(); drawBotSettings() }
                "code" -> card("Код из Authenticator") {
                    if (!Auth.isEnrolled(this)) drawEnroll()
                    else note("✓ Настроен. Запасных кодов: ${store.getStringSet(Keys.RECOVERY, emptySet())?.size ?: 0}", ink)
                }
                "connect" -> drawConnect()
                "music" -> card("Музыка в Discord") {
                    note("Что играет на телефоне (Spotify, Яндекс Музыка, YouTube Music…), компьютер покажет в статусе Discord, пока на нём самом ничего не играет. Нужен доступ к уведомлениям: так Android разрешает видеть плееры. Сами уведомления никуда не уходят.")
                    if (MediaWatcher.enabled(this)) note("✓ Включено", ink)
                    button(if (MediaWatcher.enabled(this)) "Открыть доступ к уведомлениям" else "Включить", primary = !MediaWatcher.enabled(this)) {
                        startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
                    }
                }
                "update" -> drawUpdate()
                "lang" -> card("Язык") { drawLanguage() }
                "theme" -> card("Оформление") { drawThemes() }
                "perm" -> card("Разрешения") {
                    note(if (locationGranted()) "Геолокация разрешена. Выбери «Разрешить всегда»." else "Без геолокации защита не включится.", if (locationGranted()) muted else warm)
                    button("Разрешить геолокацию", primary = false) { askPermissions() }
                    button("Разрешить работу в фоне", primary = false) { askBattery() }
                }
                "owner" -> { codeCard(); drawOwner() }
            }
            return
        }
        val armed = store.getBoolean(Keys.ARMED, false)
        // сводка сверху
        card(if (armed) "Телефон под защитой" else "Защита выключена") {
            note(if (armed) "Если телефон украдут, напиши боту защиты с любого устройства." else "Включи защиту от кражи, чтобы найти и заблокировать телефон.", if (armed) muted else warm)
        }
        val theme = when (store.getString("theme", "system")) { "night" -> "Ночь"; "graphite" -> "Графит"; "coal" -> "Уголь"; "midnight" -> "Полночь"; "white" -> "Белая"; "paper" -> "Тетрадь"; "custom" -> "Своя"; else -> "Системная" }
        val lang = when (store.getString("lang", "system")) { "ru" -> "Русский"; "en" -> "English"; else -> "Как в системе" }
        settingsGroup("Компьютер") { box ->
            settingsRow(box, "desktop", Color.rgb(168, 85, 247), "Связь с ПК", if (PcLink.isPaired(this)) "Подключён · связь зашифрована" else "Не подключён", "connect")
            settingsRow(box, "music", Color.rgb(88, 101, 242), "Музыка в Discord", if (MediaWatcher.enabled(this)) "Включено" else "Выключено", "music")
            settingsRow(box, "restart", Color.rgb(14, 165, 233), "Обновление приложения", "С компьютера, без USB", "update")
        }
        settingsGroup("Защита") { box ->
            settingsRow(box, "shield", Color.rgb(124, 58, 237), "Защита от кражи", if (armed) "Включена" else "Выключена", "protect")
            settingsRow(box, "send", Color.rgb(14, 165, 233), "Бот защиты", store.getString("bot_name", null)?.let { "@$it" } ?: "Не подключён", "bot")
            settingsRow(box, "key", Color.rgb(234, 179, 8), "Код из Authenticator", if (Auth.isEnrolled(this)) "Настроен" else "Не настроен", "code")
            settingsRow(box, "stop", Color.rgb(16, 185, 129), "Разрешения", if (locationGranted()) "Геолокация разрешена" else "Нужна геолокация", "perm")
            if (Device(this).isOwner) settingsRow(box, "phone", Color.rgb(239, 68, 68), "Режим владельца", "Включён", "owner")
        }
        settingsGroup("Приложение") { box ->
            settingsRow(box, "globe", Color.rgb(59, 130, 246), "Язык", lang, "lang")
            settingsRow(box, "palette", Color.rgb(236, 72, 153), "Оформление", theme, "theme")
        }
        column.addView(TextView(this).apply { text = "ZAVIRIA · " + deviceName(); textSize = 11.5f; setTextColor(muted); gravity = Gravity.CENTER; setPadding(0, dp(16), 0, dp(8)) })
    }

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        when {
            tab == 5 && settingsPage != null -> { settingsPage = null; render() }
            editQuick -> { editQuick = false; render() }
            else -> @Suppress("DEPRECATION") super.onBackPressed()
        }
    }

    private fun drawPhone() {
        val device = Device(this)
        val armed = store.getBoolean(Keys.ARMED, false)
        val hasBot = !store.getString(Keys.TOKEN, "").isNullOrEmpty() && !store.getString(Keys.OWNER, "").isNullOrEmpty()
        card("Готовность") {
            statusRow("Бот защиты", hasBot)
            statusRow("Код из Authenticator", Auth.isEnrolled(this))
            statusRow("Геолокация", locationGranted())
        }
        codeCard()
        card("Защита от кражи") {
            if (!armed) {
                button("Включить защиту") {
                    if (!Auth.isEnrolled(this)) { tab = 5; settingsPage = "code"; render(); toast("Сначала настрой код"); return@button }
                    if (!passCode()) return@button
                    if (!hasBot && !PcLink.isPaired(this)) { toast("Подключи компьютер или бота"); return@button }
                    device.arm()
                    if (!locationGranted()) { toast("Сначала разреши геолокацию"); return@button }
                    store.edit().putBoolean(Keys.ARMED, true).apply()
                    GuardService.start(this)
                    render()
                }
            } else {
                button("Выключить защиту", primary = false) {
                    if (!passCode()) return@button
                    device.disarm()
                    store.edit().putBoolean(Keys.ARMED, false).putBoolean(Keys.TRACKING, false).apply()
                    syncGuard()
                    render()
                }
            }
        }
        card("Если телефон украли") {
            note("Напиши боту защиты, после команды добавь код:\n/lock — заблокировать\n/locate — где телефон\n/track_on — следить каждые 5 минут\n/siren — громкая сирена")
        }
    }

    private fun drawBotSettings() {
        val enrolled = Auth.isEnrolled(this)
        card("Бот защиты") {
            note("Создай в @BotFather отдельного бота, не основного.")
            val hasToken = !store.getString(Keys.TOKEN, "").isNullOrEmpty()
            val token = hiddenField(if (hasToken) "Токен сохранён. Вставь новый, чтобы заменить" else "Токен бота") // сохранённый токен в поле не возвращается
            val owner = field("Твой ID в Telegram", store.getString(Keys.OWNER, "").orEmpty(), InputType.TYPE_CLASS_NUMBER)
            button("Сохранить") {
                if (enrolled && !passCode()) return@button
                connectBot(token.text.toString().trim().ifEmpty { store.getString(Keys.TOKEN, "").orEmpty() }, owner.text.toString().trim())
            }
            store.getString("bot_name", null)?.let { note("✓ Подключение успешно\nБот: @$it\nУстройство: ${deviceName()}", ink) }
        }
    }

    private fun drawOwner() {
        val device = Device(this)
        card("Режим владельца") {
            note("Полный контроль защиты над телефоном. Снимать — только если меняешь телефон или удаляешь приложение.")
            button("Снять режим владельца", primary = false) {
                if (!passCode()) return@button
                device.removeOwner()
                store.edit().putBoolean(Keys.ARMED, false).putBoolean(Keys.TRACKING, false).apply()
                syncGuard()
                settingsPage = null
                render()
            }
        }
    }

    private fun drawEnroll() {
        window.addFlags(WindowManager.LayoutParams.FLAG_SECURE) // на экране секретный ключ
        // Ключ хранится до подтверждения: после перехода в Authenticator или перезапуска он не должен меняться
        val secret = store.getString("pending_secret", null) ?: Totp.newSecret().also { store.edit().putString("pending_secret", it).apply() }
        note("Нажми кнопку: Google Authenticator откроется и сам предложит добавить ZAVIRIA. Подтверди там, вернись сюда и введи шесть цифр.")
        button("Добавить в Google Authenticator") { addToAuthenticator(secret) }
        note("Если Authenticator стоит на другом устройстве, введи там этот ключ вручную. Так надёжнее: телефон могут украсть вместе с кодами.")
        column.addView(TextView(this).apply {
            text = secret; textSize = 15f; setTextColor(ink); typeface = Typeface.MONOSPACE; setTextIsSelectable(true)
            background = rounded(fieldColor, 14, Color.argb(70, 190, 160, 255)); setPadding(dp(14), dp(13), dp(14), dp(13))
        }, spaced(bottom = 8))
        column.addView(codeField, spaced(top = 6, bottom = 6))
        button("Подтвердить") {
            val code = codeField.text.toString().trim()
            val step = Totp.nowStep()
            if ((step - 1..step + 1).none { Totp.code(secret, it) == code }) { toast("Код не подошёл"); return@button }
            store.edit().putString(Keys.SECRET, secret).putBoolean(Keys.ENROLLED, true).putLong(Keys.LAST_STEP, step + 1).remove("pending_secret").apply()
            val codes = Auth.newRecoveryCodes(this)
            thread { PcLink.backup(this) } // копия на компьютере переживёт удаление приложения
            styledDialog("Запиши на бумагу", "Каждый код работает один раз и больше не покажется:\n\n" + codes.joinToString("\n"), "Записал", null, { render() })
        }
    }

    // Сразу создаёт запись в Google Authenticator; если его нет — другое приложение кодов или страница установки
    private fun addToAuthenticator(secret: String) {
        val link = Uri.parse("otpauth://totp/ZAVIRIA:Phone?secret=$secret&issuer=ZAVIRIA&algorithm=SHA1&digits=6&period=30")
        val google = "com.google.android.apps.authenticator2"
        val attempts = listOf(
            Intent(Intent.ACTION_VIEW, link).setPackage(google),
            Intent(Intent.ACTION_VIEW, link),
            Intent(Intent.ACTION_VIEW, Uri.parse("market://details?id=$google")),
            Intent(Intent.ACTION_VIEW, Uri.parse("https://play.google.com/store/apps/details?id=$google")),
        )
        if (attempts.none { runCatching { startActivity(it) }.isSuccess }) toast("Не нашла приложение для кодов, введи ключ вручную")
    }

    private fun askPermissions() {
        val list = mutableListOf(Manifest.permission.ACCESS_FINE_LOCATION, Manifest.permission.ACCESS_COARSE_LOCATION)
        if (Build.VERSION.SDK_INT >= 33) list += Manifest.permission.POST_NOTIFICATIONS
        if (locationGranted() && Build.VERSION.SDK_INT >= 29) requestPermissions(arrayOf(Manifest.permission.ACCESS_BACKGROUND_LOCATION), 2)
        else requestPermissions(list.toTypedArray(), 2)
    }

    @Suppress("BatteryLife")
    private fun askBattery() {
        val power = getSystemService(POWER_SERVICE) as PowerManager
        if (power.isIgnoringBatteryOptimizations(packageName)) { toast("Уже разрешено. На Honor проверь ещё запуск приложений в настройках."); return }
        startActivity(Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:$packageName")))
    }
}
