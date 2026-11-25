#include <unordered_map>
#include "Settings.h"

#include <atomic>

#include "Platform.h"
#include "Security.h"
#include "TextUtils.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <thread>

namespace {
    std::mutex g_mutex;
    Settings g_settings;

    std::wstring IniPath() { return Text::ExecutableDirectory() + L"\\anya.ini"; }

    std::wstring ReadString(const wchar_t* key, const std::wstring& fallback) {
        wchar_t buffer[4096] = {}; // свой промт бывает длинным
        GetPrivateProfileStringW(L"anya", key, fallback.c_str(), buffer, 4096, IniPath().c_str());
        return buffer;
    }

    int ReadInt(const wchar_t* key, int fallback) {
        return static_cast<int>(GetPrivateProfileIntW(L"anya", key, fallback, IniPath().c_str()));
    }

    void Write(const wchar_t* key, const std::wstring& value) {
        WritePrivateProfileStringW(L"anya", key, value.c_str(), IniPath().c_str());
    }
}

namespace {
    // Настройки лежат рядом с программой, а их копия — в профиле пользователя:
    // так они переживают обновление, переустановку в другую папку и случайное удаление.
    const wchar_t* const kDataFiles[] = { L"anya.ini", L"scenarios.json", L"pult.json", L"notes.txt", L"pie.json" };

    std::filesystem::path BackupFolder() {
        const wchar_t* roaming = _wgetenv(L"APPDATA");
        if (!roaming) return std::filesystem::path();
        const std::filesystem::path base(roaming);
        std::error_code ec; // программа раньше называлась ASTRIX: копия настроек переезжает в новую папку один раз
        if (!std::filesystem::exists(base / L"ZAVIRIA", ec) && std::filesystem::exists(base / L"ASTRIX", ec))
            std::filesystem::rename(base / L"ASTRIX", base / L"ZAVIRIA", ec);
        return base / L"ZAVIRIA" / L"backup";
    }
}

void Config::RestoreBackup() {
    const auto folder = BackupFolder();
    if (folder.empty()) return;
    const std::filesystem::path here = Text::ExecutableDirectory();
    std::error_code error;
    for (const wchar_t* name : kDataFiles)
        if (!std::filesystem::exists(here / name, error) && std::filesystem::exists(folder / name, error))
            std::filesystem::copy_file(folder / name, here / name, error);
}

void Config::BackupNow() {
    const auto folder = BackupFolder();
    if (folder.empty()) return;
    const std::wstring ini = IniPath();
    if (GetPrivateProfileIntW(L"anya", L"setup", 0, ini.c_str()) != 1) return;       // первую настройку ещё не прошли: беречь нечего
    if (GetPrivateProfileIntW(L"debug", L"probe", 0, ini.c_str()) == 1) return;      // служебная проверка не должна попасть в копию
    const std::filesystem::path here = Text::ExecutableDirectory();
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    for (const wchar_t* name : kDataFiles) {
        if (!std::filesystem::exists(here / name, error)) continue;
        const auto temp = folder / (std::wstring(name) + L".tmp"); // через временный файл: оборванная запись не испортит копию
        if (std::filesystem::copy_file(here / name, temp, std::filesystem::copy_options::overwrite_existing, error))
            std::filesystem::rename(temp, folder / name, error);
    }
}

void Config::StartBackups() {
    std::thread([] {
        for (;;) {
            BackupNow();
            std::this_thread::sleep_for(std::chrono::minutes(3)); // компьютер могут выключить в любой момент
        }
    }).detach();
}

void Config::Load() {
    Settings s;
    s.assistant = Text::Trim(Text::ToUtf8(ReadString(L"assistant", Text::ToWide(s.assistant))));
    if (s.assistant.empty()) s.assistant = "Аня";
    s.name = Text::ToUtf8(ReadString(L"name", Text::ToWide(s.name)));
    s.spokenName = Text::ToUtf8(ReadString(L"spoken_name", Text::ToWide(s.spokenName)));
    s.voice = ReadString(L"voice", L"");
    s.pitch = std::clamp(ReadInt(L"pitch", s.pitch), -10, 10);
    s.neuralPitch = std::clamp(ReadInt(L"neural_pitch", s.neuralPitch), -6, 6);
    s.rate = std::clamp(ReadInt(L"rate", s.rate), -10, 10);
    s.wakeWord = ReadInt(L"wake_word", 0) != 0;
    s.model = Text::ToUtf8(ReadString(L"model", L""));
    s.silenceMs = std::clamp(ReadInt(L"silence_ms", s.silenceMs), 300, 2500);
    if (s.silenceMs == 850) s.silenceMs = 650; // старое значение по умолчанию: отвечать быстрее после паузы
    s.personality = static_cast<Personality>(std::clamp(ReadInt(L"personality", static_cast<int>(s.personality)), 0, 3));
    s.customPrompt = Text::ToUtf8(ReadString(L"custom_prompt", L""));
    s.bodySounds = ReadInt(L"body_sounds", 1) != 0;
    s.search = static_cast<SearchEngine>(std::clamp(ReadInt(L"search", 0), 0, 3));
    s.quickPhrases = ReadInt(L"quick_phrases", 0) != 0;
    s.weatherWidget = ReadInt(L"weather_widget", 1) != 0;
    s.look = static_cast<Look>(std::clamp(ReadInt(L"look", 0), 0, 4));
    s.lookColor = ReadInt(L"look_color", s.lookColor) & 0xFFFFFF;
    s.accent = std::clamp(ReadInt(L"accent", 0), 0, 6);
    s.accentColor = ReadInt(L"accent_color", s.accentColor) & 0xFFFFFF;
    s.backdrop = static_cast<Backdrop>(std::clamp(ReadInt(L"backdrop", 0), 0, 4));
    s.backdropSpeed = std::clamp(ReadInt(L"backdrop_speed", 100), 20, 300);
    s.animateInactive = ReadInt(L"animate_inactive", 0) != 0;
    s.backgroundColor = ReadInt(L"background_color", -1);
    s.animationColor = ReadInt(L"animation_color", -1);
    s.glass = ReadInt(L"glass", 1) != 0;
    s.buttonPlates = ReadInt(L"button_plates", 0) != 0;
    s.tray = ReadInt(L"tray", 0) != 0;
    s.windowEffect = std::clamp(ReadInt(L"window_effect", 2), 0, 2);
    s.windowOpacity = std::clamp(ReadInt(L"window_opacity", 70), 0, 100);
    s.wallpaper = Text::ToUtf8(ReadString(L"wallpaper", L""));
    s.wallpaperDim = std::clamp(ReadInt(L"wallpaper_dim", 45), 0, 100);
    s.autoColors = ReadInt(L"auto_colors", 0) != 0;
    s.savedLook = ReadInt(L"saved_look", 0);
    s.savedLookColor = ReadInt(L"saved_look_color", 0x1C1030);
    s.savedAccent = ReadInt(L"saved_accent", 0);
    s.savedAccentColor = ReadInt(L"saved_accent_color", 0xA78BFA);
    s.savedAnimationColor = ReadInt(L"saved_animation_color", -1);

    std::lock_guard lock(g_mutex);
    g_settings = s;
}

void Config::Save() {
    const Settings s = Get();
    Write(L"assistant", Text::ToWide(s.assistant));
    Write(L"name", Text::ToWide(s.name));
    Write(L"spoken_name", Text::ToWide(s.spokenName));
    Write(L"voice", s.voice);
    Write(L"pitch", std::to_wstring(s.pitch));
    Write(L"neural_pitch", std::to_wstring(s.neuralPitch));
    Write(L"rate", std::to_wstring(s.rate));
    Write(L"wake_word", s.wakeWord ? L"1" : L"0");
    Write(L"model", Text::ToWide(s.model));
    Write(L"silence_ms", std::to_wstring(s.silenceMs));
    Write(L"personality", std::to_wstring(static_cast<int>(s.personality)));
    std::wstring prompt = Text::ToWide(s.customPrompt); // в ini одна строка — переводы строк заменяем пробелами
    for (wchar_t& ch : prompt) if (ch == L'\r' || ch == L'\n') ch = L' ';
    Write(L"custom_prompt", prompt);
    Write(L"body_sounds", s.bodySounds ? L"1" : L"0");
    Write(L"search", std::to_wstring(static_cast<int>(s.search)));
    Write(L"quick_phrases", s.quickPhrases ? L"1" : L"0");
    Write(L"weather_widget", s.weatherWidget ? L"1" : L"0");
    Write(L"look", std::to_wstring(static_cast<int>(s.look)));
    Write(L"look_color", std::to_wstring(s.lookColor));
    Write(L"accent", std::to_wstring(s.accent));
    Write(L"accent_color", std::to_wstring(s.accentColor));
    Write(L"backdrop", std::to_wstring(static_cast<int>(s.backdrop)));
    Write(L"backdrop_speed", std::to_wstring(s.backdropSpeed));
    Write(L"animate_inactive", s.animateInactive ? L"1" : L"0");
    Write(L"background_color", std::to_wstring(s.backgroundColor));
    Write(L"animation_color", std::to_wstring(s.animationColor));
    Write(L"glass", s.glass ? L"1" : L"0");
    Write(L"button_plates", s.buttonPlates ? L"1" : L"0");
    Write(L"tray", s.tray ? L"1" : L"0");
    Write(L"window_effect", std::to_wstring(s.windowEffect));
    Write(L"window_opacity", std::to_wstring(s.windowOpacity));
    Write(L"wallpaper", Text::ToWide(s.wallpaper));
    Write(L"wallpaper_dim", std::to_wstring(s.wallpaperDim));
    Write(L"auto_colors", s.autoColors ? L"1" : L"0");
    Write(L"saved_look", std::to_wstring(s.savedLook));
    Write(L"saved_look_color", std::to_wstring(s.savedLookColor));
    Write(L"saved_accent", std::to_wstring(s.savedAccent));
    Write(L"saved_accent_color", std::to_wstring(s.savedAccentColor));
    Write(L"saved_animation_color", std::to_wstring(s.savedAnimationColor));
}

static void ForgetValue(const char* section, const char* key);
static bool IsSecret(const char* section, const char* key) {
    const std::string s(section), k(key);
    return (s == "telegram" && (k == "token" || k == "totp_secret" || k == "login_secret")) || (s == "phone" && (k == "key" || k == "guard" || k == "backup")) ||
           (s == "ai" && k == "cloud_key");
}

void Config::SetValue(const char* section, const char* key, const std::string& value) {
    ForgetValue(section, key);
    std::string stored = value;
    if (IsSecret(section, key) && !value.empty()) {
        stored = Security::Protect(value);
        if (stored.empty()) return; // открытым текстом секрет не пишем
    }
    WritePrivateProfileStringW(Text::ToWide(section).c_str(), Text::ToWide(key).c_str(), Text::ToWide(stored).c_str(), IniPath().c_str());
}

namespace {
    std::mutex g_valuesMutex; // значения из файла помним: чтение диска в каждом кадре давало рывки
    std::unordered_map<std::string, std::string> g_values;
    std::string ValueKey(const char* section, const char* key) { return std::string(section) + '\n' + key; }
}

std::string Config::Value(const char* section, const char* key, const char* fallback) {
    {
        std::lock_guard lock(g_valuesMutex);
        if (const auto it = g_values.find(ValueKey(section, key)); it != g_values.end()) return it->second;
    }
    const auto remember = [&](const std::string& value) {
        std::lock_guard lock(g_valuesMutex);
        g_values[ValueKey(section, key)] = value;
        return value;
    };
    const std::wstring ini = IniPath(), s = Text::ToWide(section), k = Text::ToWide(key);
    wchar_t buffer[2048] = {};
    GetPrivateProfileStringW(s.c_str(), k.c_str(), L"\x1", buffer, 2048, ini.c_str());
    if (wcscmp(buffer, L"\x1") != 0) { // только пробелы по краям: «-12.5» должно остаться с минусом
        std::wstring value = buffer;
        value.erase(0, value.find_first_not_of(L" \t"));
        value.erase(value.find_last_not_of(L" \t") + 1);
        if (IsSecret(section, key) && !value.empty()) {
            const std::string stored = Text::ToUtf8(value);
            if (Security::IsProtected(stored)) return remember(Security::Unprotect(stored)); // другой компьютер — секрет придётся ввести заново
            const std::string sealed = Security::Protect(stored); // старый файл: шифруем при первом чтении
            if (!sealed.empty()) WritePrivateProfileStringW(s.c_str(), k.c_str(), Text::ToWide(sealed).c_str(), ini.c_str());
        }
        return remember(Text::ToUtf8(value));
    }
    WritePrivateProfileStringW(s.c_str(), k.c_str(), Text::ToWide(fallback).c_str(), ini.c_str());
    return remember(fallback);
}

std::vector<std::pair<std::string, std::string>> Config::Contacts() {
    const std::wstring ini = IniPath();
    wchar_t buffer[8192] = {};
    DWORD length = GetPrivateProfileSectionW(L"contacts", buffer, 8192, ini.c_str());
    std::vector<std::pair<std::string, std::string>> contacts;
    for (const wchar_t* entry = buffer; *entry; entry += wcslen(entry) + 1) { // «ключ=значение\0ключ=значение\0\0»
        const std::wstring line = entry;
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        contacts.emplace_back(Text::Trim(Text::ToUtf8(line.substr(0, eq))), Text::Trim(Text::ToUtf8(line.substr(eq + 1))));
    }
    return contacts;
}

Settings Config::Get() {
    std::lock_guard lock(g_mutex);
    return g_settings;
}

namespace {
    std::atomic<bool> g_dirty{ false };   // изменили без сохранения (ползунок, палитра, текст) — записать чуть позже
    std::atomic<ULONGLONG> g_dirtyAt{ 0 };
}

void Config::Update(const std::function<void(Settings&)>& edit, bool save) {
    {
        std::lock_guard lock(g_mutex);
        edit(g_settings);
    }
    if (save) { g_dirty = false; Save(); }
    else { g_dirtyAt = GetTickCount64(); g_dirty = true; } // раньше такие правки иногда так и не попадали в файл
}

void Config::SaveIfDirty(bool now) {
    if (!g_dirty.load()) return;
    if (!now && GetTickCount64() - g_dirtyAt.load() < 800) return; // ползунок ещё двигают — не пишем на диск каждый кадр
    g_dirty = false;
    Save();
}

void Config::ForgetSection(const char* section) {
    std::lock_guard lock(g_valuesMutex);
    const std::string prefix = ValueKey(section, "");
    for (auto it = g_values.begin(); it != g_values.end();) it = it->first.rfind(prefix, 0) == 0 ? g_values.erase(it) : std::next(it);
}

static void ForgetValue(const char* section, const char* key) {
    std::lock_guard lock(g_valuesMutex);
    g_values.erase(ValueKey(section, key));
}
