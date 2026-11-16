#include "Lang.h"

#include "Platform.h"
#include "Settings.h"

#include <atomic>
#include <cstring>
#include <string>
#include <string_view>
#include <mutex>
#include <unordered_map>

extern bool (*GImTranslate)(const char** begin, const char** end);

namespace {
    bool g_english = false;

    const std::unordered_map<std::string_view, const char*>& Table() {
        static const std::unordered_map<std::string_view, const char*> table = {
            // разделы и шапка
            { "Настройки", "Settings" }, { "Главная", "Home" }, { "Процессы", "Processes" }, { "Сценарии", "Scenarios" }, { "поиск настроек…", "search settings…" },
            { "Общее", "General" }, { "Обучение, имена, обновления и запуск", "Tutorial, names, updates and startup" },
            { "Синхронизация", "Phone sync" }, { "Подключение телефона и коды", "Phone connection and codes" },
            { "Связь с телефоном — скоро", "Phone link — coming soon" }, { "В разработке", "In development" }, { "Своё приложение Discord", "Your own Discord app" }, { "Перевод области экрана", "Translate a screen area" }, { "Нажми сочетание, выдели мышкой текст на экране — рядом появится перевод. Щёлкни по переводу, чтобы скопировать.", "Press the shortcut, select text on screen with the mouse — the translation appears next to it. Click it to copy." }, { "Сочетание для перевода", "Translate shortcut" }, { "Клавиши, которые начинают выделение.", "Keys that start the selection." }, { "Переводить на", "Translate to" }, { "Язык, на который переводить текст с экрана.", "The language to translate screen text into." }, { "нажми сочетание клавиш… (Esc — отмена)", "press a key combination… (Esc to cancel)" }, { "попробовать", "try it" }, { "Выдели мышкой текст, который перевести · Esc — отмена", "Select the text to translate with the mouse · Esc to cancel" }, { "перевожу…", "translating…" }, { "Перевод", "Translation" }, { "щёлкни — скопировать", "click to copy" }, { "Текст не нашёлся. Выдели область поточнее.", "No text found. Select the area more precisely." }, { "Нейросеть не ответила — перевести не получилось.", "The AI did not answer — could not translate." }, { "отдых", "break" }, { "+ ещё сочетание", "+ another shortcut" }, { "Одно или несколько сочетаний — любое открывает меню: клавиши (с Ctrl, Alt, Shift, Win) или кнопка мыши — колёсико, правая, боковые, тоже с клавишами.", "One or more shortcuts — any of them opens the menu: keys (with Ctrl, Alt, Shift, Win) or a mouse button — wheel, right, side buttons, also with keys." }, { "Открывать коротким нажатием", "Open with a short press" }, { "Меню открывается и от быстрого нажатия (касание тачпада тремя пальцами) и ждёт выбора. Обычный щелчок этой кнопкой тогда не работает.", "The menu also opens on a quick press (three-finger touchpad tap) and waits for your choice. A normal click with that button then stops working." }, { "Чем открывать круговое меню", "How to open the pie menu" }, { "Своё сочетание: клавиши (с Ctrl, Alt, Shift, Win) или кнопка мыши — колёсико, правая, боковые, тоже с клавишами. Кнопку мыши зажми, клавишу держи, пока выбираешь.", "Your own shortcut: keys (with Ctrl, Alt, Shift, Win) or a mouse button — wheel, right, side buttons, also with keys. Hold the mouse button, keep the key down while choosing." }, { "нажми сочетание или кнопку мыши… (Esc — отмена)", "press a shortcut or mouse button… (Esc to cancel)" }, { "изменить", "change" }, { "как было: колёсико", "reset: mouse wheel" }, { "левая кнопка мыши", "left mouse button" }, { "правая кнопка мыши", "right mouse button" }, { "колёсико мыши", "mouse wheel" }, { "боковая кнопка мыши 1", "mouse side button 1" }, { "боковая кнопка мыши 2", "mouse side button 2" }, { "Показывать активность в Discord", "Show activity in Discord" }, { "В профиле Discord: «Играет в ZAVIRIA», а когда играет музыка или видео — «Слушает» или «Смотрит». Ничего вводить не нужно.", "In your Discord profile: «Playing ZAVIRIA», and «Listening» or «Watching» when music or video plays. Nothing to enter." }, { "встроенное ZAVIRIA", "built-in ZAVIRIA" }, { "Не обязательно: ZAVIRIA уже встроена и показывается сразу. Нужно, только если хочешь своё название и картинку — вставь Application ID своего приложения с discord.com/developers. Пусто — встроенное.", "Optional: ZAVIRIA is built in and shows right away. Only needed for your own name and picture — paste your app's Application ID from discord.com/developers. Empty means built-in." }, { "Круговое меню", "Pie menu" }, { "Программы в круговом меню", "Apps in the pie menu" }, { "Зажми среднюю кнопку мыши (колёсико) в любом месте — вокруг курсора раскроется круг с программами. Отпусти на нужной или щёлкни по ней. Короткое нажатие колёсика работает как обычно.", "Hold the middle mouse button (wheel) anywhere — a ring of apps opens around the cursor. Release over one or click it. A short wheel click works as usual." }, { "Картинка — как выглядит круг. Первый пункт — сверху, дальше по часовой. Папка открывает следующий круг со своими пунктами — вкладывай сколько угодно.", "The picture shows how the ring looks. The first item is at the top, then clockwise. A folder opens the next ring with its own items — nest as deep as you like." }, { "Что в круговом меню", "What's in the pie menu" }, { "Главное", "Main" }, { "папка", "folder" }, { "сайт", "site" }, { "файл", "file" }, { "программа", "app" }, { "открыть папку", "open folder" }, { "удалить папку со всем, что в ней", "delete the folder with everything in it" }, { "Пока пусто — добавь пункты ниже.", "Empty for now — add items below." }, { "+ программа", "+ app" }, { "+ папка", "+ folder" }, { "+ сайт", "+ site" }, { "+ файл", "+ file" }, { "Новая папка", "New folder" }, { "название", "name" }, { "добавить", "add" }, { "выше", "move up" }, { "ниже", "move down" }, { "убрать из меню", "remove from menu" }, { "Пока пусто — добавь программы ниже.", "Empty for now — add apps below." }, { "+ добавить программу", "+ add an app" }, { "поиск…", "search…" }, { "показать меню", "show menu" }, { "Добавь программы в настройках", "Add apps in settings" }, { "Связь с телефоном", "Phone link" },
            { "Приложение ZAVIRIA для телефона и синхронизация скоро появятся. Пока подключить телефон нельзя.", "The ZAVIRIA phone app and sync are coming soon. You can't connect a phone yet." },
            { "Безопасность и связь", "Security & links" }, { "Телефон, Telegram и Discord", "Phone, Telegram and Discord" },
            { "ИИ и голос", "AI & voice" }, { "Нейросеть, её характер, распознавание и озвучка", "Model, personality, speech recognition and voice" },
            { "Оверлей", "Overlay" }, { "Музыка и нагрузка поверх всех окон", "Music and system load over all windows" },
            { "Виджеты", "Widgets" }, { "Что показывать на главном экране", "What to show on the home screen" },
            { "Внешний вид", "Appearance" }, { "Фон, тема и эффекты окна", "Background, theme and window effects" },
            { "О приложении", "About" }, { "Версия ZAVIRIA, сброс и удаление", "ZAVIRIA version, reset and removal" },
            // группы
            { "Помощница", "Assistant" }, { "Обновления", "Updates" }, { "Запуск", "Startup" }, { "Поиск", "Search" }, { "Поведение", "Behaviour" },
            { "Нейросеть", "AI model" }, { "Распознавание", "Recognition" }, { "Озвучка", "Voice" }, { "Музыка", "Music" }, { "Нагрузка", "System load" },
            { "На главном экране", "On the home screen" }, { "Фон", "Background" }, { "Цвета", "Colours" }, { "Окно", "Window" }, { "Текст", "Text" },
            // настройки
            { "Язык", "Language" }, { "Как в системе", "System" }, { "Обучение", "Tutorial" }, { "Имя помощницы", "Assistant name" }, { "Как тебя зовут", "Your name" }, { "Как произносить имя вслух", "How to say your name" },
            { "Слушать всегда", "Always listen" }, { "Версия", "Version" }, { "Автообновления", "Auto-updates" }, { "Запускать вместе с Windows", "Start with Windows" },
            { "Фоновый режим", "Background mode" }, { "Запускать сразу в фоне", "Start hidden" }, { "Поисковик", "Search engine" }, { "Поиск файлов", "File search" }, { "Анимировать в неактивном окне", "Animate when inactive" },
            { "Приложение на телефоне", "Phone app" }, { "Управление телефоном", "Phone control" }, { "Аккаунт Telegram", "Telegram account" }, { "Код для опасных команд", "Code for risky commands" },
            { "Телеграм-бот", "Telegram bot" }, { "Токен бота", "Bot token" }, { "Кому можно", "Who is allowed" }, { "Уведомления в телеграме", "Telegram notifications" }, { "Пульт в телеграме", "Telegram remote" },
            { "Снимки экрана", "Screenshots" }, { "Блокировать при «Выключить»", "Lock on standby" }, { "Статус в Discord", "Discord status" }, { "ID приложения Discord", "Discord application ID" }, { "Показывать музыку", "Show music" }, { "Показывать видео", "Show video" },
            { "Журнал", "Log" }, { "Характер", "Personality" }, { "Свои указания", "Custom instructions" }, { "Что помнить обо мне", "What to remember about me" }, { "Скачать нейросеть", "Download a model" }, { "Профиль ответов", "Answer profile" },
            { "Модель распознавания", "Recognition model" }, { "Голос", "Voice" }, { "Свой голос", "Custom voice" }, { "Скорость речи", "Speech speed" }, { "Пауза — конец фразы", "Pause ends a phrase" }, { "Реакция на кашель и чих", "React to coughs and sneezes" },
            { "Высота нейроголоса", "Neural voice pitch" }, { "Высота голосов Windows", "Windows voice pitch" }, { "Освобождать видеокарту", "Free the graphics card" },
            { "Островок «Сейчас играет»", "Now playing island" }, { "Где появляется островок", "Island position" }, { "Окно нагрузки", "System load window" }, { "Закрепить окно нагрузки", "Pin load window" }, { "Прозрачность окна нагрузки", "Load window opacity" },
            { "Показывать виджеты", "Show widgets" }, { "Время", "Clock" }, { "Погода", "Weather" }, { "Город", "City" }, { "Заметки", "Notes" }, { "Календарь", "Calendar" }, { "Курсы валют", "Currency rates" }, { "Валюта курсов", "Rate currency" }, { "Мониторинг системы", "System monitor" }, { "Быстрые фразы", "Quick phrases" },
            { "Свои обои", "Custom wallpaper" }, { "Цвета по обоям", "Colours from wallpaper" }, { "Затемнение обоев", "Wallpaper dimming" }, { "Скорость анимации", "Animation speed" }, { "Домашний уют", "Cozy" }, { "Тетрадь с рисунками", "Doodle notebook" }, { "Системная", "System" }, { "Свои цвета", "Custom colours" }, { "Тёмная тетрадь", "Dark notebook" }, { "Кто в центре", "Who is in the centre" }, { "Рисунки на листе", "Page doodles" },
            { "Тема", "Theme" }, { "Любимый цвет", "Favourite colour" }, { "Цвет фона", "Background colour" }, { "Цвет анимации", "Animation colour" }, { "Эффект окна", "Window effect" }, { "Плотность фона", "Background density" }, { "Стекло", "Glass" }, { "Подложка под кнопками окна", "Window button backing" },
            { "Шрифт", "Font" }, { "Размер интерфейса", "Interface size" }, { "Полная кастомизация", "Full customisation" }, { "О программе", "About ZAVIRIA" }, { "Настройки по умолчанию", "Default settings" }, { "Удалить программу", "Uninstall" }, { "Как это работает", "How it works" },
            // кнопки
            { "показать обучение", "show tutorial" }, { "проверить обновления", "check for updates" }, { "проверяю…", "checking…" }, { "страница релизов", "releases page" }, { "установить сейчас", "install now" }, { "подключить телефон", "connect phone" }, { "отвязать телефон", "unlink phone" },
            { "заблокировать", "lock" }, { "где телефон", "find phone" }, { "сирена", "siren" }, { "выключить сирену", "stop siren" }, { "показать на карте", "show on map" }, { "подключить аккаунт", "connect account" }, { "разрешить", "allow" }, { "отклонить", "decline" },
            { "настроить код", "set up code" }, { "выключить", "turn off" }, { "скопировать ключ", "copy key" }, { "подтвердить", "confirm" }, { "показать как это сделать", "show how to do it" }, { "скрыть инструкцию", "hide instructions" },
            { "найти", "find" }, { "другая картинка…", "another picture…" }, { "убрать", "remove" }, { "скачать", "download" }, { "открыть папку с голосами", "open voices folder" }, { "послушать", "preview" }, { "сохранить", "save" }, { "изменить", "edit" }, { "сбросить", "reset" }, { "очистить", "clear" }, { "очистить журнал", "clear log" }, { "отмена", "cancel" }, { "включить", "turn on" },
            { "вернуть всё по умолчанию", "reset everything" }, { "всё по умолчанию", "reset everything" }, { "удалить программу полностью", "remove completely" }, { "страница на GitHub", "GitHub page" }, { "перезапустить нейросеть", "restart model" },
            // главный экран
            { "виджеты", "widgets" }, { "оптимизировать память", "optimise memory" }, { "оптимизирую…", "optimising…" }, { "пока пусто", "empty" }, { "разговор", "conversation" }, { "отдыхаю", "resting" }, { "слушаю", "listening" }, { "думаю", "thinking" }, { "говорю", "speaking" }, { "разбираю речь", "transcribing" }, { "микрофон выключен", "microphone off" }, { "секундочку…", "one moment…" },
            { "добрый вечер", "good evening" }, { "доброе утро", "good morning" }, { "привет", "hello" }, { "не спится", "can't sleep" }, { "напиши что-нибудь…   (пробел — говорить голосом)", "type something…   (space to talk)" }, { "нажми на сферу или пробел — и говори", "click the orb or press space and talk" }, { "тут будет наш разговор ✦", "our conversation will appear here ✦" },
            { "Система", "System" }, { "Процессор", "CPU" }, { "Видеокарта", "GPU" }, { "Видеопамять", "Video memory" }, { "Память", "Memory" }, { "сейчас", "now" }, 
#include "LangEn.inc"
        };
        return table;
    }

    // Подписи, собранные на лету: единицы измерения и короткие слова внутри строки с числами
    const std::string* Fragments(std::string_view text) {
        struct Pair { const char* from; const char* to; bool needsDigit; };
        static const Pair kPairs[] = {
            { "процессов · обновляется каждые 2 секунды", "processes · updates every 2 seconds", false }, { "авто (Silero — Ксения)", "auto (Silero — Xenia)", false }, { "Нейроголос — милый", "Neural voice, sweet", false }, { "слушать всегда (на «", "always listen (for «", false },
            { "ощущается как", "feels like", false }, { "сегодня от", "today from", false }, { "влажность", "humidity", false }, { "ветер", "wind", false }, { "км/ч", "km/h", false },
            { "ГБ видеопамяти", "GB VRAM", false }, { "МБ/с", "MB/s", false }, { "КБ/с", "KB/s", false }, { "ГГц", "GHz", false }, { "МГц", "MHz", false }, { "ГБ", "GB", false }, { "МБ", "MB", false }, { "КБ", "KB", false }, { " Вт", " W", false },
            { "потоков", "threads", false }, { "чтение", "read", false }, { "запись", "write", false }, { "свободно", "free", false }, { "занято", "used", false }, { "процессов", "processes", false }, { "вентилятор", "fan", false },
            { "(живой, мужской)", "(natural, male)", false }, { "(живой)", "(natural)", false }, { "Нейроголос", "Neural voice", false }, { "Свой голос", "Custom voice", false }, { "Ксения", "Xenia", false }, { "Байя", "Baya", false }, { "Айдар", "Aidar", false }, { "Евгений", "Eugene", false },
            { "на связи как", "online as", false }, { "монитор", "screen", false }, { "(основной)", "(main)", false }, { "— сжатая память", "— compressed memory", false }, { "Подключён:", "Connected:", false }, { "ZAVIRIA, версия", "ZAVIRIA, version", false }, { "ошибка:", "error:", false },
            { " из ", " of ", true }, { " до ", " to ", true },
        };
        // Кэш на два поколения: при переполнении старое поколение живёт ещё круг, поэтому строка, которую интерфейс
        // уже получил и рисует, не освобождается под ним (раньше clear() давал мусор вроде «niSniSniS»). Плюс замок: зовут из разных потоков
        static std::mutex mutex;
        static std::unordered_map<std::string, std::string> cache, retired;
        std::lock_guard lock(mutex);
        std::string key(text);
        if (const auto hit = cache.find(key); hit != cache.end()) return hit->second == key ? nullptr : &hit->second;
        if (const auto old = retired.find(key); old != retired.end()) return old->second == key ? nullptr : &old->second;
        if (cache.size() > 3000) { retired.clear(); retired.swap(cache); }
        std::string out = key;
        const bool digit = key.find_first_of("0123456789") != std::string::npos;
        for (const Pair& pair : kPairs) {
            if (pair.needsDigit && !digit) continue;
            const size_t length = std::strlen(pair.from);
            for (size_t at = out.find(pair.from); at != std::string::npos; at = out.find(pair.from, at)) out.replace(at, length, pair.to);
        }
        auto& stored = cache[key];
        stored = out;
        return out == key ? nullptr : &stored;
    }

    bool Translate(const char** begin, const char** end) {
        if (!g_english) return false;
        const char* last = *end ? *end : *begin + std::strlen(*begin);
        const auto& table = Table();
        const std::string_view text(*begin, static_cast<size_t>(last - *begin));
        const auto it = table.find(text);
        if (it != table.end()) {
            *begin = it->second;
            *end = it->second + std::strlen(it->second);
            return true;
        }
        const std::string* mixed = Fragments(text);
        if (!mixed) return false;
        *begin = mixed->data();
        *end = mixed->data() + mixed->size();
        return true;
    }
}

void Lang::Reload() {
    GImTranslate = Translate;
    const std::string choice = Config::Value("anya", "lang", "system");
    if (choice == "ru") g_english = false;
    else if (choice == "en") g_english = true;
    else g_english = PRIMARYLANGID(GetUserDefaultUILanguage()) != LANG_RUSSIAN;
}

bool Lang::IsEnglish() { return g_english; }

namespace {
    std::atomic<int> g_replyEnglish{ -1 }; // -1 — как интерфейс
}

bool Lang::AutoSpeech() { return Config::Value("ears", "autolang", "1") == "1"; }

bool Lang::IsEnglishText(const std::string& text) {
    if (!AutoSpeech()) return g_english;
    bool latin = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = text[i];
        if (c == 0xD0 || c == 0xD1) return false; // кириллица в UTF-8
        if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') latin = true;
    }
    return latin ? true : g_english;
}

void Lang::SetReplyEnglish(bool english) { g_replyEnglish = english ? 1 : 0; }
bool Lang::ReplyEnglish() { const int v = g_replyEnglish.load(); return v < 0 || !AutoSpeech() ? g_english : v == 1; }
const char* Lang::SpeechCode(const std::string& text) { return AutoSpeech() ? (IsEnglishText(text) ? "en" : "ru") : Code(); }
const char* Lang::Tr(const char* text) {
    if (!g_english) return text;
    const auto& table = Table();
    const auto it = table.find(text);
    return it == table.end() ? text : it->second;
}

std::string Lang::Latin(const std::string& text) {
    static const char* const kLower[] = { "a","b","v","g","d","e","zh","z","i","y","k","l","m","n","o","p","r","s","t","u","f","kh","ts","ch","sh","sch","","y","","e","yu","ya" };
    std::string out;
    for (size_t i = 0; i < text.size();) {
        const unsigned char a = text[i];
        if (a < 0x80 || i + 1 >= text.size()) { out += text[i++]; continue; }
        const unsigned code = ((a & 0x1F) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3F);
        i += 2;
        if (code == 0x401) { out += "Yo"; continue; }
        if (code == 0x451) { out += "yo"; continue; }
        if (code >= 0x410 && code <= 0x42F) { std::string s = kLower[code - 0x410]; if (!s.empty()) s[0] = static_cast<char>(toupper(s[0])); out += s; }
        else if (code >= 0x430 && code <= 0x44F) out += kLower[code - 0x430];
    }
    return out;
}

std::string Lang::TranslateText(const std::string& text) {
    if (!g_english) return text;
    struct Pair { const char* from; const char* to; };
    static const Pair kPairs[] = { // длинные фразы раньше коротких
#include "LangText.inc"
    };
    const auto cyrillic = [](const std::string& s, size_t at) { // байт at начинает русскую букву?
        return at < s.size() && (static_cast<unsigned char>(s[at]) == 0xD0 || static_cast<unsigned char>(s[at]) == 0xD1);
    };
    const auto cyrillicBefore = [](const std::string& s, size_t at) {
        return at >= 2 && (static_cast<unsigned char>(s[at - 2]) == 0xD0 || static_cast<unsigned char>(s[at - 2]) == 0xD1);
    };
    std::string out = text;
    for (const Pair& pair : kPairs) {
        const size_t length = std::strlen(pair.from);
        for (size_t at = out.find(pair.from); at != std::string::npos; at = out.find(pair.from, at)) {
            if (cyrillicBefore(out, at) || cyrillic(out, at + length)) { at += length; continue; } // только целые слова
            out.replace(at, length, pair.to);
            at += std::strlen(pair.to);
        }
    }
    return out;
}

const char* Lang::Code() { return g_english ? "en" : "ru"; }
