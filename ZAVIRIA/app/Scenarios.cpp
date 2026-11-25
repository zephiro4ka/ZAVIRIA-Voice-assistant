#include "Scenarios.h"

#include "Brain.h"
#include "Browser.h"
#include "Fuzzy.h"
#include "Input.h"
#include "Lang.h"
#include "Launcher.h"
#include "Log.h"
#include "MediaNow.h"
#include "Pc.h"
#include "Platform.h"
#include "Screen.h"
#include "State.h"
#include "TextUtils.h"
#include "Voice.h"

#include "../third_party/nlohmann/json.hpp"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

using nlohmann::json;
using Scenarios::Step;

namespace {
    std::mutex g_mutex;
    std::atomic<bool> g_stopping{ false };

    const char* L(const char* ru, const char* en) { return Lang::IsEnglish() ? en : ru; }
    std::string Lower(const std::string& s) { return Text::ToUtf8(Text::LowerRu(Text::ToWide(s))); }

    std::filesystem::path File() { return std::filesystem::path(Text::ExecutableDirectory()) / "scenarios.json"; }

    // Мониторы слева направо: 1 — самый левый … (основной обычно первый)
    std::vector<RECT> Monitors() {
        std::vector<RECT> list;
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM param) -> BOOL {
            MONITORINFO info{ sizeof(info) };
            GetMonitorInfoW(monitor, &info);
            reinterpret_cast<std::vector<RECT>*>(param)->push_back(info.rcWork);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&list));
        std::sort(list.begin(), list.end(), [](const RECT& a, const RECT& b) { return a.left < b.left; });
        return list;
    }

    // Окно программы — ждём, пока появится после запуска (до timeoutMs)
    HWND WaitWindow(const std::string& app, int timeoutMs) {
        for (int waited = 0; waited <= timeoutMs; waited += 300) {
            if (HWND w = static_cast<HWND>(Launcher::MainWindowOf(app))) return w;
            Sleep(300);
        }
        return nullptr;
    }

    void Place(HWND window, int monitor, int position) {
        const auto monitors = Monitors();
        if (!window || monitors.empty()) return;
        const RECT r = monitors[std::clamp(monitor, 1, static_cast<int>(monitors.size())) - 1];
        const int w = r.right - r.left, h = r.bottom - r.top;
        ShowWindow(window, SW_RESTORE);
        int x = r.left, y = r.top, cw = w, ch = h;
        switch (static_cast<Scenarios::Position>(position)) {
        case Scenarios::Position::LeftHalf: cw = w / 2; break;
        case Scenarios::Position::RightHalf: x = r.left + w / 2; cw = w - w / 2; break;
        case Scenarios::Position::Center: cw = w * 2 / 3; ch = h * 2 / 3; x = r.left + (w - cw) / 2; y = r.top + (h - ch) / 2; break;
        default: break;
        }
        SetWindowPos(window, nullptr, x, y, cw, ch, SWP_NOZORDER | SWP_NOACTIVATE);
        const auto pos = static_cast<Scenarios::Position>(position);
        if (pos == Scenarios::Position::Maximize || pos == Scenarios::Position::FullScreen) ShowWindow(window, SW_MAXIMIZE);
        if (pos == Scenarios::Position::FullScreen) { // видео на весь экран: F11 в браузере
            Input::Tap(VK_MENU);
            SetForegroundWindow(window);
            Sleep(250);
            Input::Tap(VK_F11);
        }
    }

    // ================================================================ файл
    json Ids(const std::vector<int>& v) { return json(v); }
    std::vector<int> ReadIds(const json& j) {
        std::vector<int> out;
        if (j.is_array()) for (const auto& n : j) if (n.is_number_integer()) out.push_back(n.get<int>());
        return out;
    }

    json ToJson(const Scenarios::Scenario& s) {
        json blocks = json::array();
        for (const auto& b : s.blocks) {
            json alt = json::array();
            for (const auto& port : b.alt) alt.push_back(Ids(port));
            blocks.push_back({ { "step", static_cast<int>(b.step) }, { "text", b.text }, { "monitor", b.monitor }, { "position", b.position }, { "number", b.number },
                               { "id", b.id }, { "x", b.x }, { "y", b.y }, { "next", b.next }, { "color", b.color }, { "sides", b.sides }, { "title", b.title },
                               { "extra", b.extra }, { "alt", alt } });
            if (!b.shape.empty()) { blocks.back()["shape"] = b.shape; blocks.back()["outline"] = b.outline; }
        }
        // link_style — для прошлых версий: там 0 кривые, 1 косые, 2 прямые
        return { { "name", s.name }, { "enabled", s.enabled }, { "blocks", blocks }, { "canvas", true },
                 { "lines", s.linkStyle }, { "link_style", s.linkStyle == 0 ? 0 : 2 } };
    }

    Scenarios::Scenario FromJson(const json& j) {
        Scenarios::Scenario s;
        s.name = j.value("name", s.name);
        s.phrase = j.value("phrase", "");
        s.enabled = j.value("enabled", true);
        for (const auto& b : j.value("blocks", json::array())) {
            Scenarios::Block block;
            block.step = static_cast<Step>(std::clamp(b.value("step", 0), 0, static_cast<int>(Step::kCount) - 1));
            block.text = b.value("text", "");
            block.monitor = b.value("monitor", 1);
            block.position = b.value("position", 0);
            block.number = b.value("number", 0);
            block.id = b.value("id", 0);
            block.x = b.value("x", 0.0f);
            block.y = b.value("y", 0.0f);
            if (b.contains("next")) block.next = ReadIds(b["next"]);
            block.color = b.value("color", -1);
            block.sides = b.value("sides", -1);
            block.title = b.value("title", "");
            block.extra = b.value("extra", "");
            if (b.contains("alt") && b["alt"].is_array())
                for (const auto& port : b["alt"]) block.alt.push_back(ReadIds(port));
            if (b.contains("shape") && b["shape"].is_array())
                for (const auto& v : b["shape"]) if (v.is_number()) block.shape.push_back(std::clamp(v.get<float>(), 0.0f, 1.0f));
            if (block.shape.size() % 2) block.shape.pop_back();
            block.outline = b.value("outline", true);
            s.blocks.push_back(block);
        }
        s.hasLayout = j.value("canvas", false);
        if (j.contains("start") && j["start"].is_object()) {
            const auto& st = j["start"];
            s.hasLayout = true;
            s.startX = st.value("x", 0.0f);
            s.startY = st.value("y", 0.0f);
            if (st.contains("next")) s.startNext = ReadIds(st["next"]);
        }
        // прежние «косые» и «прямые» ближе всего к ровным линиям
        s.relayout = !j.contains("lines");
        s.linkStyle = j.contains("lines") ? std::clamp(j.value("lines", 0), 0, 2) : (j.value("link_style", 0) == 0 ? 0 : 1);
        Scenarios::Normalize(s);
        return s;
    }

    // Пример, чтобы было понятно, как это устроено
    std::vector<Scenarios::Scenario> Example() {
        Scenarios::Scenario s;
        s.name = "Запуск браузера";
        s.phrase = "запусти браузер";
        Scenarios::Block link;
        link.step = Step::OpenLink;
        link.text = "https://www.google.com";
        link.monitor = 0;
        s.blocks = { link };
        return { s };
    }

    // ================================================================ уведомления и триггеры: свой поток с окном
    constexpr UINT kReload = WM_APP + 1, kNotify = WM_APP + 2;
    HWND g_triggerWindow = nullptr;
    std::thread g_triggerThread;
    std::mutex g_noteMutex;
    std::deque<std::pair<std::string, std::string>> g_notes; // заголовок, текст

    void Notify(const std::string& title, const std::string& text) {
        {
            std::lock_guard lock(g_noteMutex);
            g_notes.emplace_back(title, text);
        }
        if (g_triggerWindow) PostMessageW(g_triggerWindow, kNotify, 0, 0);
    }

    std::string Screenshot() {
        const auto jpeg = Screen::CaptureJpeg(0, 92);
        PWSTR pictures = nullptr;
        if (jpeg.empty() || FAILED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pictures))) return {};
        std::filesystem::path folder = std::filesystem::path(pictures) / L"ZAVIRIA";
        CoTaskMemFree(pictures);
        std::error_code ignored;
        std::filesystem::create_directories(folder, ignored);
        const std::time_t now = std::time(nullptr);
        std::tm t{};
        localtime_s(&t, &now);
        wchar_t name[64];
        wcsftime(name, 64, L"%Y-%m-%d %H-%M-%S.jpg", &t);
        const auto path = folder / name;
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
        return Text::ToUtf8(path.wstring());
    }

    bool Number(const std::string& s, double& out) {
        std::string t = Text::Trim(s);
        std::replace(t.begin(), t.end(), ',', '.');
        if (t.empty()) return false;
        char* end = nullptr;
        out = strtod(t.c_str(), &end);
        return end && *end == 0;
    }

    // 0 равно, 1 не равно, 2 содержит, 3 не содержит, 4 больше, 5 меньше, 6 пусто, 7 не пусто
    bool Check(const std::string& left, int op, const std::string& right) {
        const std::string a = Lower(Text::Trim(left)), b = Lower(Text::Trim(right));
        double x = 0, y = 0;
        const bool numbers = Number(a, x) && Number(b, y);
        switch (op) {
        case 0: return numbers ? x == y : a == b;
        case 1: return numbers ? x != y : a != b;
        case 2: return a.find(b) != std::string::npos;
        case 3: return a.find(b) == std::string::npos;
        case 4: return numbers ? x > y : a > b;
        case 5: return numbers ? x < y : a < b;
        case 6: return a.empty();
        default: return !a.empty();
        }
    }

    void SleepSeconds(int seconds) { // долгое ожидание не держит закрытие программы
        for (int i = 0; i < seconds * 5 && !g_stopping; ++i) Sleep(200);
    }

    struct Runner {
        Scenarios::Scenario s;
        std::map<std::string, std::string> vars;
        std::string lastApp; // для блока «Положение окна» без названия
        int steps = 0;

        const Scenarios::Block* Find(int id) const {
            for (const auto& b : s.blocks) if (b.id == id) return &b;
            return nullptr;
        }

        // {имя} → значение переменной; {время}, {дата} и {буфер} — всегда под рукой
        std::string Fill(const std::string& text) const {
            if (text.find('{') == std::string::npos) return text;
            std::string out;
            for (size_t i = 0; i < text.size(); ++i) {
                const size_t close = text[i] == '{' ? text.find('}', i) : std::string::npos;
                if (close == std::string::npos) { out += text[i]; continue; }
                std::string name = Lower(Text::Trim(text.substr(i + 1, close - i - 1)));
                static const std::map<std::string, std::string> kEnglish = { { "answer", "ответ" }, { "step", "шаг" }, { "event", "событие" } };
                if (auto alias = kEnglish.find(name); alias != kEnglish.end()) name = alias->second;
                if (auto it = vars.find(name); it != vars.end()) out += it->second;
                else if (name == "время" || name == "time" || name == "дата" || name == "date") {
                    const std::time_t now = std::time(nullptr);
                    std::tm t{};
                    localtime_s(&t, &now);
                    char buffer[32];
                    strftime(buffer, sizeof(buffer), name == "время" || name == "time" ? "%H:%M" : "%d.%m.%Y", &t);
                    out += buffer;
                } else if (name == "буфер" || name == "clipboard") out += Pc::Clipboard();
                else out += text.substr(i, close - i + 1);
                i = close;
            }
            return out;
        }

        // выполнить блок; вернуть, по какому выходу идти дальше (-1 — дальше не идём)
        int Exec(const Scenarios::Block& b) {
            Log::Write("  блок «" + std::string(b.title.empty() ? Scenarios::StepName(b.step) : b.title.c_str()) + "»");
            switch (b.step) {
            case Step::Phrase: case Step::Hotkey: case Step::Nothing: break;
            case Step::Launch:
                if (const auto r = Launcher::Open(Fill(b.text)); r.found) lastApp = Fill(b.text);
                else Log::Write("сценарий: не нашла программу «" + b.text + "»");
                break;
            case Step::OpenLink: {
                std::string url = Text::Trim(Fill(b.text));
                if (url.find("://") == std::string::npos) url = "https://" + url;
                ShellExecuteW(nullptr, L"open", Text::ToWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                Sleep(1500);
                if (b.monitor > 0) Place(Browser::Window(true), b.monitor, b.position); // ссылку сразу ставим куда сказано
                lastApp = "браузер";
                break;
            }
            case Step::Place: {
                const std::string app = Text::Trim(b.text).empty() ? lastApp : Fill(b.text);
                HWND window = app == "браузер" ? Browser::Window(true) : WaitWindow(app, 15000); // игра может грузиться долго
                Place(window, b.monitor, b.position);
                break;
            }
            case Step::Volume: Pc::SetVolume(b.number); break;
            case Step::Media: MediaNow::Control(b.number == 1 ? MediaNow::Action::Next : b.number == 2 ? MediaNow::Action::Previous : MediaNow::Action::PlayPause); break;
            case Step::Mute: Pc::SetMute(b.position == 2 ? !Pc::IsMuted() : b.position == 0); break;
            case Step::Wait: SleepSeconds(std::clamp(b.number, 0, 3600)); break;
            case Step::Say: Voice::Speak(Fill(b.text)); break;
            case Step::Condition: return Check(Fill(b.text), b.position, Fill(b.extra)) ? 0 : 1;
            case Step::Switch: {
                const std::string value = Lower(Text::Trim(Fill(b.text)));
                const auto cases = Scenarios::Cases(b.extra);
                for (size_t i = 0; i < cases.size(); ++i) if (Lower(Fill(cases[i])) == value) return static_cast<int>(i);
                return static_cast<int>(cases.size());
            }
            case Step::SetVar: if (!Text::Trim(b.text).empty()) vars[Lower(Text::Trim(b.text))] = Fill(b.extra); break;
            case Step::Notify: Notify(Text::Trim(b.extra).empty() ? "ZAVIRIA" : Fill(b.extra), Fill(b.text)); break;
            case Step::ChatReply: Chat::Add(Role::Anya, Fill(b.text)); break;
            case Step::Screenshot: vars["ответ"] = Screenshot(); break;
            case Step::Play:
                if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", Text::ToWide(Text::Trim(Fill(b.text))).c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
                    Log::Write("сценарий: не открылось «" + b.text + "»");
                break;
            case Step::Command: Brain::Submit(Fill(b.text)); break;
            default: break;
            }
            return 0;
        }

        // По стрелкам: развилки — по очереди, сверху вниз; по кругу не ходим (кроме тела цикла)
        void Walk(const std::vector<int>& ids, std::vector<int>& seen) {
            std::vector<const Scenarios::Block*> nodes;
            for (int id : ids) if (const auto* b = Find(id); b && std::find(seen.begin(), seen.end(), id) == seen.end()) nodes.push_back(b);
            std::sort(nodes.begin(), nodes.end(), [](const auto* a, const auto* b) { return a->y < b->y || (a->y == b->y && a->x < b->x); });
            for (const auto* b : nodes) {
                if (std::find(seen.begin(), seen.end(), b->id) != seen.end()) continue;
                if (++steps > 5000 || g_stopping) return;
                seen.push_back(b->id);
                if (b->step == Step::Loop) {
                    for (int i = 0; i < std::clamp(b->number, 1, 1000) && !g_stopping; ++i) {
                        vars["шаг"] = std::to_string(i + 1);
                        std::vector<int> inside = seen;
                        Walk(Scenarios::Port(*b, 0), inside);
                    }
                    Walk(Scenarios::Port(*b, 1), seen);
                    continue;
                }
                const int port = Exec(*b);
                if (port >= 0 && port < Scenarios::PortCount(*b)) Walk(Scenarios::Port(*b, port), seen);
            }
        }
    };

    std::mutex g_startMutex;
    std::unordered_map<std::string, ULONGLONG> g_started; // сценарий сам себя не перезапускает по кругу

    // ================================================================ поток триггеров
    struct Armed { Scenarios::Scenario scenario; int block; };
    std::vector<Armed> g_hotkeys;

    void Reload(HWND window) {
        for (size_t i = 0; i < g_hotkeys.size(); ++i) UnregisterHotKey(window, static_cast<int>(i) + 1);
        g_hotkeys.clear();
        for (const auto& s : Scenarios::Load()) {
            if (!s.enabled) continue;
            for (const auto& b : s.blocks) {
                if (b.step != Step::Hotkey) continue;
                std::vector<WORD> keys;
                if (!Input::ParseChord(b.text, keys)) continue;
                UINT mods = MOD_NOREPEAT, vk = 0;
                for (WORD k : keys) {
                    if (k == VK_CONTROL) mods |= MOD_CONTROL;
                    else if (k == VK_MENU) mods |= MOD_ALT;
                    else if (k == VK_SHIFT) mods |= MOD_SHIFT;
                    else if (k == VK_LWIN) mods |= MOD_WIN;
                    else vk = k;
                }
                if (!vk) continue;
                g_hotkeys.push_back({ s, b.id });
                if (!RegisterHotKey(window, static_cast<int>(g_hotkeys.size()), mods, vk))
                    Log::Write("сценарий «" + s.name + "»: сочетание " + b.text + " уже занято другой программой");
            }
        }
    }

    void ShowNote(HWND window) {
        std::pair<std::string, std::string> note;
        {
            std::lock_guard lock(g_noteMutex);
            if (g_notes.empty()) return;
            note = g_notes.front();
            g_notes.pop_front();
        }
        NOTIFYICONDATAW nid{ sizeof(nid) };
        nid.hWnd = window;
        nid.uID = 1;
        nid.uFlags = NIF_ICON | NIF_INFO | NIF_TIP;
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        nid.hIcon = ExtractIconW(GetModuleHandleW(nullptr), exe, 0);
        wcsncpy_s(nid.szTip, L"ZAVIRIA", _TRUNCATE);
        wcsncpy_s(nid.szInfoTitle, Text::ToWide(note.first).c_str(), _TRUNCATE);
        wcsncpy_s(nid.szInfo, Text::ToWide(note.second.empty() ? " " : note.second).c_str(), _TRUNCATE);
        nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
        if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) Shell_NotifyIconW(NIM_ADD, &nid);
        if (nid.hIcon) DestroyIcon(nid.hIcon);
        SetTimer(window, 2, 12000, nullptr); // значок в трее нужен только на время уведомления
    }

    LRESULT CALLBACK TriggerProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_HOTKEY:
            if (wParam >= 1 && wParam <= g_hotkeys.size()) Scenarios::Run(g_hotkeys[wParam - 1].scenario, g_hotkeys[wParam - 1].block);
            return 0;
        case WM_TIMER:
            if (wParam == 2) {
                KillTimer(window, 2);
                NOTIFYICONDATAW nid{ sizeof(nid) };
                nid.hWnd = window;
                nid.uID = 1;
                Shell_NotifyIconW(NIM_DELETE, &nid);
            }
            return 0;
        case kReload: Reload(window); return 0;
        case kNotify: ShowNote(window); return 0;
        case WM_DESTROY: {
            NOTIFYICONDATAW nid{ sizeof(nid) };
            nid.hWnd = window;
            nid.uID = 1;
            Shell_NotifyIconW(NIM_DELETE, &nid);
            PostQuitMessage(0);
            return 0;
        }
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

std::vector<std::string> Scenarios::Cases(const std::string& extra) {
    std::vector<std::string> out;
    std::string part;
    for (char ch : extra + "|") {
        if (ch != '|') { part += ch; continue; }
        if (!Text::Trim(part).empty()) out.push_back(Text::Trim(part));
        part.clear();
    }
    return out;
}

bool Scenarios::IsTrigger(Step step) { return step == Step::Phrase || step == Step::Hotkey; }

int Scenarios::PortCount(const Block& b) {
    if (b.step == Step::Condition || b.step == Step::Loop) return 2;
    if (b.step == Step::Switch) return static_cast<int>(Cases(b.extra).size()) + 1;
    return 1;
}

std::vector<int>& Scenarios::Port(Block& b, int port) {
    if (port <= 0) return b.next;
    if (static_cast<int>(b.alt.size()) < port) b.alt.resize(port);
    return b.alt[port - 1];
}

const std::vector<int>& Scenarios::Port(const Block& b, int port) {
    static const std::vector<int> kNone;
    if (port <= 0) return b.next;
    return port <= static_cast<int>(b.alt.size()) ? b.alt[port - 1] : kNone;
}

std::vector<Scenarios::Scenario> Scenarios::Load() {
    std::lock_guard lock(g_mutex);
    std::ifstream in(File());
    if (!in) { auto example = Example(); for (auto& e : example) Normalize(e); return example; }
    const json data = json::parse(in, nullptr, false);
    std::vector<Scenario> list;
    if (data.is_array())
        for (const auto& j : data) list.push_back(FromJson(j));
    return list;
}

void Scenarios::Save(const std::vector<Scenario>& scenarios) {
    {
        std::lock_guard lock(g_mutex);
        json data = json::array();
        for (const auto& s : scenarios) data.push_back(ToJson(s));
        std::ofstream(File()) << data.dump(2);
    }
    if (g_triggerWindow) PostMessageW(g_triggerWindow, kReload, 0, 0); // новые сочетания и время
}

void Scenarios::Normalize(Scenario& s) {
    int maxId = 0;
    for (const auto& b : s.blocks) maxId = std::max(maxId, b.id);
    for (auto& b : s.blocks) if (b.id <= 0) b.id = ++maxId;
    if (!s.hasLayout) { // цепочка → холст: слева направо, по три блока в ряд, змейкой; первое место — у фразы
        s.startX = 0, s.startY = 0;
        s.startNext.clear();
        for (size_t i = 0; i < s.blocks.size(); ++i) {
            auto& b = s.blocks[i];
            const int place = static_cast<int>(i) + 1, row = place / 3;
            const int col = row % 2 == 0 ? place % 3 : 2 - place % 3;
            b.x = static_cast<float>(col) * 380.0f;
            b.y = static_cast<float>(row) * 220.0f;
            b.next.clear();
            if (i + 1 < s.blocks.size()) b.next.push_back(s.blocks[i + 1].id);
        }
        if (!s.blocks.empty()) s.startNext.push_back(s.blocks[0].id);
        s.hasLayout = true;
    }
    if (!Text::Trim(s.phrase).empty() || !s.startNext.empty()) { // прежний «Слушать» — теперь обычный блок-триггер
        Block trigger;
        trigger.step = Step::Phrase;
        trigger.text = Text::Trim(s.phrase);
        trigger.id = ++maxId;
        trigger.x = s.startX, trigger.y = s.startY;
        trigger.next = s.startNext;
        s.blocks.insert(s.blocks.begin(), trigger);
        s.phrase.clear();
        s.startNext.clear();
    }
}

void Scenarios::Run(const Scenario& source, int trigger) {
    Scenario scenario = source;
    Normalize(scenario);
    {
        std::lock_guard lock(g_startMutex);
        g_started[scenario.name] = GetTickCount64();
    }
    std::thread([scenario, trigger] {
        Log::Write("сценарий «" + scenario.name + "»: старт");
        Runner runner{ scenario };
        std::vector<int> roots, seen;
        for (const auto& b : scenario.blocks) if (IsTrigger(b.step) && (trigger < 0 || b.id == trigger)) roots.push_back(b.id);
        if (roots.empty() && trigger < 0) { // проверка без триггера — от блоков, в которые не входит ни одна стрелка
            for (const auto& b : scenario.blocks) {
                bool incoming = false;
                for (const auto& o : scenario.blocks)
                    for (int p = 0; p < PortCount(o); ++p) {
                        const auto& v = Port(o, p);
                        incoming |= std::find(v.begin(), v.end(), b.id) != v.end();
                    }
                if (!incoming) roots.push_back(b.id);
            }
        }
        runner.Walk(roots, seen);
        Log::Write("сценарий «" + scenario.name + "»: готово");
    }).detach();
}

std::optional<std::string> Scenarios::TryRun(const std::string& command) {
    const std::string said = Lower(command);
    for (const auto& s : Load()) {
        if (!s.enabled) continue;
        for (const auto& b : s.blocks) {
            if (b.step != Step::Phrase) continue;
            std::string phrase;
            for (char ch : b.text + ",") { // несколько фраз через запятую
                if (ch != ',' && ch != ';') { phrase += ch; continue; }
                const std::string p = Lower(Text::Trim(phrase));
                phrase.clear();
                if (p.empty() || (said.find(p) == std::string::npos && Fuzzy::TitleMatch(p, said) < 0.85f)) continue;
                {
                    std::lock_guard lock(g_startMutex);
                    if (auto it = g_started.find(s.name); it != g_started.end() && GetTickCount64() - it->second < 3000) return std::nullopt;
                }
                Run(s, b.id);
                return std::string(L("Запускаю «", "Starting “")) + s.name + L("»", "”");
            }
        }
    }
    return std::nullopt;
}

void Scenarios::StartTriggers() {
    if (g_triggerThread.joinable()) return;
    std::promise<void> ready;
    auto started = ready.get_future();
    g_triggerThread = std::thread([&ready] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = TriggerProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ZaviriaScenarioTriggers";
        RegisterClassW(&wc);
        HWND window = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
        g_triggerWindow = window;
        ready.set_value();
        if (!window) return;
        Reload(window);
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
        g_triggerWindow = nullptr;
    });
    started.wait();
}

void Scenarios::SystemNotify(const std::string& title, const std::string& text) { Notify(title, text); }

void Scenarios::StopTriggers() {
    g_stopping = true;
    if (HWND window = g_triggerWindow) PostMessageW(window, WM_CLOSE, 0, 0);
    if (g_triggerThread.joinable()) g_triggerThread.join();
}

const char* Scenarios::StepName(Step step) {
    static const char* kNames[] = { "Открыть", "Открыть ссылку", "Положение окна", "Громкость", "Музыка", "Задержка", "Произнести",
        "Текстовая фраза", "Горячая клавиша", "Условие", "Цикл", "Переключатель", "Установить переменную", "Ничего не делать",
        "Без звука", "Уведомление", "Ответ в чат", "Скриншот", "Воспроизвести", "Запустить команду" };
    static_assert(sizeof(kNames) / sizeof(kNames[0]) == static_cast<size_t>(Step::kCount));
    return kNames[static_cast<int>(step)];
}

int Scenarios::MonitorCount() { return static_cast<int>(Monitors().size()); }
