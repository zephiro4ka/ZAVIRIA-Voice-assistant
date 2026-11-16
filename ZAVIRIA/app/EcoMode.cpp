#include "EcoMode.h"

#include "Com.h"
#include "Lang.h"
#include "LocalAi.h"
#include "Log.h"
#include "Overlay.h"
#include "Platform.h"
#include "Settings.h"
#include "TextUtils.h"

#include "../third_party/nlohmann/json.hpp"

#include <audiopolicy.h>
#include <dwmapi.h>
#include <mmdeviceapi.h>
#include <powrprof.h>
#include <tlhelp32.h>
#include <winternl.h>

#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "psapi.lib")

using nlohmann::json;

namespace {
    // ---------------------------------------------------------------- состояние
    struct Throttled {
        DWORD pid = 0; ULONGLONG created = 0;
        DWORD priority = NORMAL_PRIORITY_CLASS;
        ULONG control = 0, state = 0; // как было с EcoQoS до нас (0/0 — решает Windows)
        std::string exe;
        bool boost = false;           // это игра: приоритет подняли, а не опустили
    };
    struct Suspended { DWORD pid = 0; ULONGLONG created = 0; std::string exe; };

    struct State {
        bool active = false;
        bool schemeChanged = false; GUID scheme{};
        bool overlayChanged = false; GUID overlay{};
        int brightness = -1;        // яркость до режима; -1 — не меняли
        std::vector<Throttled> throttled;
        std::vector<Suspended> suspended;
        std::vector<std::string> closedNames;
        std::vector<std::wstring> closedCommands; // командные строки — чтобы запустить так же, как было
    };

    std::mutex g_mutex;                 // g_state
    State g_state;
    std::atomic<bool> g_active{ false }, g_busy{ false };
    std::atomic<int> g_generation{ 0 }; // растёт при каждом включении — старый поток пересканирования завершается

    std::mutex g_reportMutex;
    EcoMode::Report g_report{ true };

    std::mutex g_previewMutex;
    EcoMode::Preview g_preview;
    std::atomic<bool> g_previewRunning{ false };

    // «Максимальная производительность» в ползунке режима питания Windows 10/11
    constexpr GUID kOverlayPerformance = { 0xded574b5, 0x45a0, 0x4f42, { 0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38 } };
    // Схемы «Максимальная производительность» (Ultimate) и «Высокая производительность»
    constexpr GUID kUltimate = { 0xe9a42b02, 0xd5df, 0x448d, { 0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61 } };
    constexpr GUID kHighPerformance = { 0x8c5e7fda, 0xe8bf, 0x4a96, { 0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c } };

    std::filesystem::path StateFile() { return std::filesystem::path(Text::ExecutableDirectory()) / L"eco_state.json"; }

    std::string GuidText(const GUID& g) {
        wchar_t text[64] = {};
        StringFromGUID2(g, text, 64);
        return Text::ToUtf8(text);
    }
    GUID GuidFrom(const std::string& text) {
        GUID g{};
        CLSIDFromString(Text::ToWide(text).c_str(), &g);
        return g;
    }

    void SaveStateLocked() {
        std::error_code ec;
        if (!g_state.active) { std::filesystem::remove(StateFile(), ec); return; }
        json j;
        j["schemeChanged"] = g_state.schemeChanged;
        j["scheme"] = GuidText(g_state.scheme);
        j["overlayChanged"] = g_state.overlayChanged;
        j["overlay"] = GuidText(g_state.overlay);
        j["brightness"] = g_state.brightness;
        j["throttled"] = json::array();
        for (const auto& t : g_state.throttled)
            j["throttled"].push_back({ { "pid", t.pid }, { "created", t.created }, { "priority", t.priority }, { "control", t.control }, { "state", t.state }, { "exe", t.exe }, { "boost", t.boost } });
        j["suspended"] = json::array();
        for (const auto& s : g_state.suspended) j["suspended"].push_back({ { "pid", s.pid }, { "created", s.created }, { "exe", s.exe } });
        std::ofstream out(StateFile(), std::ios::binary | std::ios::trunc);
        out << j.dump(1);
    }

    // ---------------------------------------------------------------- списки
    std::string NormalizeExe(std::string name) {
        name = Text::Trim(Text::ToUtf8(Text::LowerRu(Text::ToWide(name))));
        if (const size_t slash = name.find_last_of("\\/"); slash != std::string::npos) name = name.substr(slash + 1);
        if (!name.empty() && (name.size() < 4 || name.compare(name.size() - 4, 4, ".exe") != 0)) name += ".exe";
        return name;
    }

    const char* DefaultList(const char* key) {
        if (strcmp(key, "suspend") == 0) return "onedrive.exe,dropbox.exe,yandexdisk2.exe,phoneexperiencehost.exe,widgets.exe";
        if (strcmp(key, "close") == 0) return "epicgameslauncher.exe,eadesktop.exe,battle.net.exe,upc.exe,galaxyclient.exe";
        return "";
    }

    std::unordered_set<std::string> ListSet(const char* key) {
        std::unordered_set<std::string> set;
        for (const auto& item : EcoMode::List(key)) set.insert(item);
        return set;
    }

    // ---------------------------------------------------------------- процессы
    ULONGLONG CreationTime(HANDLE process) {
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
        return (static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    }

    // Открыть процесс, только если это тот же процесс, что был (pid мог достаться другому)
    HANDLE OpenSame(DWORD pid, ULONGLONG created, DWORD access) {
        HANDLE process = OpenProcess(access | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) return nullptr;
        if (created && CreationTime(process) != created) { CloseHandle(process); return nullptr; }
        return process;
    }

    std::wstring FileDescription(const std::wstring& path) {
        static std::mutex s_mutex;
        static std::unordered_map<std::wstring, std::wstring> s_cache;
        std::lock_guard lock(s_mutex);
        if (const auto it = s_cache.find(path); it != s_cache.end()) return it->second;
        std::wstring result;
        DWORD ignored = 0;
        if (const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored)) {
            std::vector<BYTE> data(size);
            struct Translation { WORD language, codepage; }* tr = nullptr;
            UINT length = 0;
            wchar_t* text = nullptr;
            if (GetFileVersionInfoW(path.c_str(), 0, size, data.data()) &&
                VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&tr), &length) && length >= sizeof(Translation)) {
                wchar_t key[64];
                swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr->language, tr->codepage);
                if (VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&text), &length) && text && *text) result = text;
            }
        }
        s_cache[path] = result;
        return result;
    }

    std::wstring CommandLine(HANDLE process) {
        using Query = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
        static auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
        if (!query) return {};
        std::vector<BYTE> buffer(8192);
        ULONG needed = 0;
        if (query(process, 60 /* ProcessCommandLineInformation */, buffer.data(), static_cast<ULONG>(buffer.size()), &needed) < 0) return {};
        const auto* text = reinterpret_cast<const UNICODE_STRING*>(buffer.data());
        return text->Buffer ? std::wstring(text->Buffer, text->Length / sizeof(wchar_t)) : L"";
    }

    struct Proc {
        DWORD pid = 0, parent = 0, session = 0;
        std::string exe;        // имя exe, нижний регистр
        std::wstring path;
        ULONGLONG created = 0;
        bool windowsDir = false;
    };

    struct WindowInfo { bool onScreen = false, minimized = false; };

    BOOL CALLBACK CollectWindow(HWND hwnd, LPARAM param) {
        auto& map = *reinterpret_cast<std::unordered_map<DWORD, WindowInfo>*>(param);
        if (!IsWindowVisible(hwnd)) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (!pid) return TRUE;
        if (IsIconic(hwnd)) { if (!GetWindow(hwnd, GW_OWNER)) map[pid].minimized = true; return TRUE; }
        int cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
        RECT r{};
        if (!GetWindowRect(hwnd, &r) || r.right - r.left < 8 || r.bottom - r.top < 8) return TRUE;
        map[pid].onScreen = true;
        return TRUE;
    }

    // Программы, у которых сейчас играет звук: их не замедляем, иначе звук может заикаться
    std::unordered_set<DWORD> AudioPids() {
        std::unordered_set<DWORD> pids;
        ComScope com;
        Com<IMMDeviceEnumerator> devices;
        Com<IMMDevice> speakers;
        Com<IAudioSessionManager2> manager;
        Com<IAudioSessionEnumerator> sessions;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&devices))) ||
            FAILED(devices->GetDefaultAudioEndpoint(eRender, eConsole, &speakers)) ||
            FAILED(speakers->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&manager))) ||
            FAILED(manager->GetSessionEnumerator(&sessions)))
            return pids;
        int count = 0;
        sessions->GetCount(&count);
        for (int i = 0; i < count; ++i) {
            Com<IAudioSessionControl> control;
            Com<IAudioSessionControl2> control2;
            if (FAILED(sessions->GetSession(i, &control)) ||
                FAILED(control->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&control2))))
                continue;
            AudioSessionState state = AudioSessionStateInactive;
            DWORD pid = 0;
            if (SUCCEEDED(control->GetState(&state)) && state == AudioSessionStateActive && SUCCEEDED(control2->GetProcessId(&pid)) && pid) pids.insert(pid);
        }
        return pids;
    }

    enum class Action { None, Throttle, ThrottleLight, Suspend, Close };

    struct Target { Proc proc; Action action = Action::None; std::string title; bool root = false; };

    // Кого и как ограничить прямо сейчас. Решаем по «программам»: процесс и все его дети с тем же exe — одно целое
    // (браузер и его вкладки), а детей защищённой программы не трогаем вовсе.
    std::vector<Target> Plan(bool allowClose) {
        std::vector<Target> out;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return out;
        wchar_t windowsDir[MAX_PATH] = {};
        GetWindowsDirectoryW(windowsDir, MAX_PATH);
        const std::wstring winPrefix = Text::LowerRu(windowsDir) + L"\\";
        DWORD mySession = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &mySession);

        std::unordered_map<DWORD, Proc> procs;
        PROCESSENTRY32W e{ sizeof(e) };
        for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
            Proc p;
            p.pid = e.th32ProcessID;
            p.parent = e.th32ParentProcessID;
            p.exe = Text::ToUtf8(Text::LowerRu(e.szExeFile));
            if (p.pid <= 4) continue;
            if (!ProcessIdToSessionId(p.pid, &p.session) || p.session != mySession) continue; // службы — сеанс 0
            if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p.pid)) {
                wchar_t path[MAX_PATH] = {};
                DWORD size = MAX_PATH;
                if (QueryFullProcessImageNameW(h, 0, path, &size)) p.path = path;
                p.created = CreationTime(h);
                CloseHandle(h);
            }
            if (p.path.empty()) continue; // не открыть — значит, и ограничить не выйдет (системный или с правами администратора)
            const std::wstring lowerPath = Text::LowerRu(p.path);
            // папка Windows и защита Windows (Defender живёт и в Program Files, и в ProgramData) — как системное
            p.windowsDir = lowerPath.rfind(winPrefix, 0) == 0 || lowerPath.find(L"\\windows defender") != std::wstring::npos;
            procs[p.pid] = std::move(p);
        }
        CloseHandle(snap);

        // родитель настоящий, только если он старше ребёнка (pid мог перейти другому процессу)
        auto parentOf = [&](const Proc& p) -> const Proc* {
            const auto it = procs.find(p.parent);
            return it != procs.end() && it->second.created && it->second.created <= p.created ? &it->second : nullptr;
        };
        // корень программы: поднимаемся, пока родитель — тот же exe
        auto rootOf = [&](DWORD pid) {
            const Proc* p = &procs[pid];
            for (int guard = 0; guard < 64; ++guard) {
                const Proc* parent = parentOf(*p);
                if (!parent || parent->exe != p->exe) break;
                p = parent;
            }
            return p->pid;
        };
        std::unordered_map<DWORD, std::vector<DWORD>> children;
        for (const auto& [pid, p] : procs) if (const Proc* parent = parentOf(p)) children[parent->pid].push_back(pid);
        auto addTree = [&](DWORD root, std::unordered_set<DWORD>& set) {
            std::vector<DWORD> stack{ root };
            while (!stack.empty()) {
                const DWORD pid = stack.back();
                stack.pop_back();
                if (!set.insert(pid).second) continue;
                if (const auto it = children.find(pid); it != children.end()) stack.insert(stack.end(), it->second.begin(), it->second.end());
            }
        };

        std::unordered_map<DWORD, WindowInfo> windows;
        EnumWindows(CollectWindow, reinterpret_cast<LPARAM>(&windows));
        DWORD foreground = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &foreground);

        // защищено: сама ZAVIRIA и её помощники, окно впереди, всё, что видно на экране, звук, белый список
        std::unordered_set<DWORD> protectedSet;
        addTree(GetCurrentProcessId(), protectedSet);
        auto protectApp = [&](DWORD pid) {
            if (!procs.count(pid)) return;
            addTree(rootOf(pid), protectedSet);
            for (const Proc* p = parentOf(procs[pid]); p && !p->windowsDir; p = parentOf(*p)) protectedSet.insert(p->pid); // лаунчер игры
        };
        if (foreground) protectApp(foreground);
        for (const auto& [pid, info] : windows) if (info.onScreen) protectApp(pid);
        for (const DWORD pid : AudioPids()) protectApp(pid);
        // специальные возможности и ввод — их замедлять нельзя
        static const std::unordered_set<std::string> kNever = { "nvda.exe", "nvda_slave.exe", "jfw.exe", "zoomtext.exe", "zaviria.exe", "llama-server.exe" };
        const auto white = ListSet("whitelist"), suspend = ListSet("suspend"), close = ListSet("close");

        // программа в целом: свёрнута (есть окно, но не на экране) или совсем без окон
        std::unordered_map<DWORD, bool> rootMinimized;
        for (const auto& [pid, info] : windows) if (info.minimized && procs.count(pid)) rootMinimized[rootOf(pid)] = true;

        for (const auto& [pid, p] : procs) {
            if (p.windowsDir || protectedSet.count(pid) || kNever.count(p.exe) || white.count(p.exe)) continue;
            const DWORD root = rootOf(pid);
            const Proc& rootProc = procs[root];
            if (white.count(rootProc.exe) || protectedSet.count(root)) continue;
            const bool minimized = rootMinimized.count(root) > 0;
            Target t;
            t.proc = p;
            t.root = root == pid;
            // игровой режим: свёрнутые программы замирают целиком (процессор и память — игре); нажмёшь на неё — оживёт сама.
            // Discord, OBS и запись экрана не трогаем — они нужны во время игры
            static const std::unordered_set<std::string> kKeepAwake = { "discord.exe", "obs64.exe", "obs32.exe", "nvidia share.exe", "nvcontainer.exe", "steamwebhelper.exe", "steam.exe" };
            if (!minimized && allowClose && close.count(rootProc.exe)) t.action = Action::Close;
            else if (suspend.count(rootProc.exe) || (minimized && !kKeepAwake.count(rootProc.exe))) t.action = Action::Suspend;
            else t.action = kKeepAwake.count(rootProc.exe) ? Action::ThrottleLight : Action::Throttle;
            const std::wstring description = FileDescription(rootProc.path);
            t.title = description.empty() ? rootProc.exe : Text::ToUtf8(description);
            out.push_back(std::move(t));
        }
        return out;
    }

    bool SetEcoQos(HANDLE process, ULONG control, ULONG state) {
        PROCESS_POWER_THROTTLING_STATE s{};
        s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        s.ControlMask = control;
        s.StateMask = state;
        return SetProcessInformation(process, ProcessPowerThrottling, &s, sizeof(s)) != FALSE;
    }

    // Фоновой программе — выгрузить из оперативки всё, что она сейчас не трогает (Windows вернёт с диска, если понадобится).
    // Именно это освобождает память под игру; EcoQoS и приоритет экономят только процессор.
    uint64_t TrimMemory(const Proc& p) {
        HANDLE h = OpenSame(p.pid, p.created, PROCESS_SET_QUOTA | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ);
        if (!h) h = OpenSame(p.pid, p.created, PROCESS_SET_QUOTA);
        if (!h) return 0;
        PROCESS_MEMORY_COUNTERS before{ sizeof(before) }, after{ sizeof(after) };
        const bool known = GetProcessMemoryInfo(h, &before, sizeof(before)) != FALSE;
        SetProcessWorkingSetSize(h, static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
        uint64_t freed = 0;
        if (known && GetProcessMemoryInfo(h, &after, sizeof(after)) && before.WorkingSetSize > after.WorkingSetSize) freed = before.WorkingSetSize - after.WorkingSetSize;
        CloseHandle(h);
        return freed;
    }
    std::atomic<uint64_t> g_trimmed{ 0 };

    bool Throttle(const Proc& p, bool light, Throttled& saved) {
        g_trimmed += TrimMemory(p);
        HANDLE h = OpenSame(p.pid, p.created, PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION);
        if (!h) h = OpenSame(p.pid, p.created, PROCESS_SET_INFORMATION);
        if (!h) return false;
        saved.pid = p.pid;
        saved.created = p.created;
        saved.exe = p.exe;
        saved.priority = GetPriorityClass(h);
        PROCESS_POWER_THROTTLING_STATE before{};
        before.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        if (GetProcessInformation(h, ProcessPowerThrottling, &before, sizeof(before))) { saved.control = before.ControlMask; saved.state = before.StateMask; }
        const bool eco = SetEcoQos(h, PROCESS_POWER_THROTTLING_EXECUTION_SPEED, PROCESS_POWER_THROTTLING_EXECUTION_SPEED);
        bool priority = false;
        const DWORD target = light ? BELOW_NORMAL_PRIORITY_CLASS : IDLE_PRIORITY_CLASS;
        // ниже, чем уже есть, не опускаем, и «реального времени» не касаемся (его ставят драйверы и звук)
        if (saved.priority && saved.priority != REALTIME_PRIORITY_CLASS && saved.priority != IDLE_PRIORITY_CLASS &&
            !(saved.priority == BELOW_NORMAL_PRIORITY_CLASS && light))
            priority = SetPriorityClass(h, target) != FALSE;
        else saved.priority = 0; // приоритет не меняли — и возвращать нечего
        CloseHandle(h);
        return eco || priority;
    }

    bool Restore(const Throttled& t) {
        HANDLE h = OpenSame(t.pid, t.created, PROCESS_SET_INFORMATION);
        if (!h) return false;
        if (t.priority) SetPriorityClass(h, t.priority);
        SetEcoQos(h, t.control, t.state);
        CloseHandle(h);
        return true;
    }

    using NtProcessCall = LONG(NTAPI*)(HANDLE);
    NtProcessCall NtCall(const char* name) { return reinterpret_cast<NtProcessCall>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), name)); }

    bool Suspend(const Proc& p) {
        g_trimmed += TrimMemory(p); // замороженная программа память не трогает — отдаём её игре
        static const auto suspend = NtCall("NtSuspendProcess");
        HANDLE h = OpenSame(p.pid, p.created, PROCESS_SUSPEND_RESUME);
        if (!h || !suspend) { if (h) CloseHandle(h); return false; }
        const bool ok = suspend(h) >= 0;
        CloseHandle(h);
        return ok;
    }

    bool Resume(const Suspended& s) {
        static const auto resume = NtCall("NtResumeProcess");
        HANDLE h = OpenSame(s.pid, s.created, PROCESS_SUSPEND_RESUME);
        if (!h || !resume) { if (h) CloseHandle(h); return false; }
        const bool ok = resume(h) >= 0;
        CloseHandle(h);
        return ok;
    }

    // ---------------------------------------------------------------- питание и яркость
    using GetOverlay = DWORD(WINAPI*)(GUID*);
    using SetOverlay = DWORD(WINAPI*)(GUID);
    GetOverlay OverlayGetter() { return reinterpret_cast<GetOverlay>(GetProcAddress(LoadLibraryW(L"powrprof.dll"), "PowerGetActualOverlayScheme")); }
    SetOverlay OverlaySetter() { return reinterpret_cast<SetOverlay>(GetProcAddress(LoadLibraryW(L"powrprof.dll"), "PowerSetActiveOverlayScheme")); }

    bool HasScheme(const GUID& wanted) {
        for (ULONG i = 0;; ++i) {
            GUID g{};
            DWORD size = sizeof(g);
            if (PowerEnumerate(nullptr, nullptr, nullptr, ACCESS_SCHEME, i, reinterpret_cast<UCHAR*>(&g), &size) != ERROR_SUCCESS) return false;
            if (IsEqualGUID(g, wanted)) return true;
        }
    }
    const GUID* FastScheme() { return HasScheme(kUltimate) ? &kUltimate : HasScheme(kHighPerformance) ? &kHighPerformance : nullptr; }

    // ---------------------------------------------------------------- игра
    // Игра — программа, чьё окно сейчас впереди и занимает весь монитор (полный экран или «окно без рамки»).
    // found = false — впереди не игра (рабочий стол, браузер в окне): прошлую игру не трогаем.
    std::vector<Proc> GameTree(bool& found) {
        found = false;
        std::vector<Proc> out;
        HWND fgWindow = GetForegroundWindow();
        DWORD fg = 0;
        GetWindowThreadProcessId(fgWindow, &fg);
        if (!fg || fg == GetCurrentProcessId()) return out;
        RECT r{};
        MONITORINFO mi{ sizeof(mi) };
        if (!GetWindowRect(fgWindow, &r) || !GetMonitorInfoW(MonitorFromWindow(fgWindow, MONITOR_DEFAULTTONEAREST), &mi)) return out;
        if (r.left > mi.rcMonitor.left || r.top > mi.rcMonitor.top || r.right < mi.rcMonitor.right || r.bottom < mi.rcMonitor.bottom) return out;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return out;
        std::unordered_map<DWORD, PROCESSENTRY32W> all;
        PROCESSENTRY32W e{ sizeof(e) };
        for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) all[e.th32ProcessID] = e;
        CloseHandle(snap);
        if (!all.count(fg)) return out;
        static const std::unordered_set<std::string> kNot = { "zaviria.exe", "explorer.exe", "applicationframehost.exe", "searchhost.exe",
                                                              "startmenuexperiencehost.exe", "shellexperiencehost.exe", "lockapp.exe" };
        wchar_t windowsDir[MAX_PATH] = {};
        GetWindowsDirectoryW(windowsDir, MAX_PATH);
        const std::wstring winPrefix = Text::LowerRu(windowsDir) + L"\\";
        DWORD root = fg;
        for (int guard = 0; guard < 64; ++guard) { // корень программы — поднимаемся, пока родитель с тем же exe
            const auto it = all.find(all[root].th32ParentProcessID);
            if (it == all.end() || it->first == root || _wcsicmp(it->second.szExeFile, all[root].szExeFile) != 0) break;
            root = it->first;
        }
        std::unordered_map<DWORD, std::vector<DWORD>> children;
        for (const auto& [pid, p] : all) if (p.th32ParentProcessID != pid) children[p.th32ParentProcessID].push_back(pid);
        std::vector<DWORD> stack{ root };
        std::unordered_set<DWORD> seen;
        while (!stack.empty()) {
            const DWORD pid = stack.back();
            stack.pop_back();
            if (seen.size() > 64 || !seen.insert(pid).second) continue;
            Proc p;
            p.pid = pid;
            p.exe = Text::ToUtf8(Text::LowerRu(all[pid].szExeFile));
            if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
                wchar_t path[MAX_PATH] = {};
                DWORD size = MAX_PATH;
                if (QueryFullProcessImageNameW(h, 0, path, &size)) p.path = path;
                p.created = CreationTime(h);
                CloseHandle(h);
            }
            if (pid == root && (p.path.empty() || kNot.count(p.exe) || Text::LowerRu(p.path).rfind(winPrefix, 0) == 0)) return out; // впереди сама Windows
            if (!p.path.empty() && !kNot.count(p.exe)) out.push_back(p);
            if (const auto it = children.find(pid); it != children.end()) stack.insert(stack.end(), it->second.begin(), it->second.end());
        }
        found = !out.empty();
        return out;
    }

    // Игре — высокий приоритет и полная частота (EcoQoS явно выключен)
    bool Boost(const Proc& p, Throttled& saved) {
        HANDLE h = OpenSame(p.pid, p.created, PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION);
        if (!h) return false;
        saved.pid = p.pid, saved.created = p.created, saved.exe = p.exe, saved.boost = true;
        saved.priority = GetPriorityClass(h);
        PROCESS_POWER_THROTTLING_STATE before{};
        before.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        if (GetProcessInformation(h, ProcessPowerThrottling, &before, sizeof(before))) { saved.control = before.ControlMask; saved.state = before.StateMask; }
        const bool fast = SetEcoQos(h, PROCESS_POWER_THROTTLING_EXECUTION_SPEED, 0);
        bool priority = false;
        if (saved.priority == NORMAL_PRIORITY_CLASS || saved.priority == ABOVE_NORMAL_PRIORITY_CLASS || saved.priority == BELOW_NORMAL_PRIORITY_CLASS)
            priority = SetPriorityClass(h, HIGH_PRIORITY_CLASS) != FALSE;
        else saved.priority = 0;
        CloseHandle(h);
        return fast || priority;
    }

    // Запустить PowerShell без окна и прочитать, что он напечатал
    std::string RunCapture(const std::wstring& script, DWORD timeoutMs = 15000) {
        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
        HANDLE read = nullptr, write = nullptr;
        if (!CreatePipe(&read, &write, &sa, 0)) return {};
        SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{ sizeof(si) };
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = write;
        si.hStdError = write;
        PROCESS_INFORMATION pi{};
        std::wstring line = L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"" + script + L"\"";
        const bool started = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != FALSE;
        CloseHandle(write);
        std::string output;
        if (started) {
            char buffer[512];
            DWORD got = 0;
            while (ReadFile(read, buffer, sizeof(buffer), &got, nullptr) && got) output.append(buffer, got);
            WaitForSingleObject(pi.hProcess, timeoutMs);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        CloseHandle(read);
        return output;
    }

    int ParseInt(const std::string& text, int fallback) {
        for (size_t i = 0; i < text.size(); ++i)
            if ((text[i] >= '0' && text[i] <= '9') || (text[i] == '-' && i + 1 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '9')) return std::atoi(text.c_str() + i);
        return fallback;
    }

    const wchar_t* kReadBrightness = L"$m = Get-CimInstance -Namespace root/WMI -ClassName WmiMonitorBrightness -ErrorAction SilentlyContinue | Select-Object -First 1; "
                                     L"if ($m) { $m.CurrentBrightness } else { -1 }";
    int CurrentBrightness() { return ParseInt(RunCapture(kReadBrightness), -1); }
    void SetBrightness(int level) {
        RunCapture(L"Get-CimInstance -Namespace root/WMI -ClassName WmiMonitorBrightnessMethods -ErrorAction SilentlyContinue | "
                   L"Invoke-CimMethod -MethodName WmiSetBrightness -Arguments @{Timeout=1;Brightness=" + std::to_wstring(std::clamp(level, 0, 100)) + L"} | Out-Null");
    }
    int TargetBrightness() { return std::clamp(std::atoi(Config::Value("eco", "brightness", "40").c_str()), 5, 100); }

    // ---------------------------------------------------------------- сама ZAVIRIA
    void ApplyOwnFeatures(bool eco) {
        Overlay::Reload();         // оверлеи сами читают EcoMode::Active()
        if (eco) Ai::Unload();     // нейросеть — из памяти и видеокарты; понадобится — загрузится снова
    }

    // ---------------------------------------------------------------- восстановление
    struct RestoreCounts { int restored = 0, resumed = 0; };
    RestoreCounts RestoreAll(State& s) {
        RestoreCounts c;
        for (const auto& t : s.throttled) if (Restore(t)) ++c.restored;
        for (const auto& p : s.suspended) if (Resume(p)) ++c.resumed;
        if (s.schemeChanged) PowerSetActiveScheme(nullptr, &s.scheme);
        if (s.overlayChanged) if (const auto set = OverlaySetter()) set(s.overlay);
        if (s.brightness >= 0) SetBrightness(s.brightness);
        s.throttled.clear();
        s.suspended.clear();
        s.schemeChanged = s.overlayChanged = false;
        s.brightness = -1;
        return c;
    }

    // Пока режим включён: новые фоновые программы — замедлить, то, что вышло на передний план, — вернуть
    void Rescan() {
        const auto plan = Plan(false);
        bool gameFound = false;
        const auto game = GameTree(gameFound);
        std::unordered_set<DWORD> gamePids;
        for (const auto& p : game) gamePids.insert(p.pid);
        std::unordered_map<DWORD, const Target*> byPid;
        for (const auto& t : plan) byPid[t.proc.pid] = &t;
        std::lock_guard lock(g_mutex);
        if (!g_state.active) return;
        bool changed = false;
        // вышло на экран (или в белый список) — вернуть как было; впереди другая игра — прежней вернуть приоритет
        for (auto it = g_state.throttled.begin(); it != g_state.throttled.end();) {
            if (it->boost) {
                if (gameFound && !gamePids.count(it->pid)) { Restore(*it); it = g_state.throttled.erase(it); changed = true; }
                else ++it;
                continue;
            }
            const auto found = byPid.find(it->pid);
            if (found == byPid.end() || found->second->proc.created != it->created) {
                Restore(*it); // если процесс закрылся, Restore просто ничего не сделает
                it = g_state.throttled.erase(it);
                changed = true;
            } else ++it;
        }
        for (auto it = g_state.suspended.begin(); it != g_state.suspended.end();) {
            const auto found = byPid.find(it->pid);
            if (found == byPid.end() || found->second->action != Action::Suspend) { Resume(*it); it = g_state.suspended.erase(it); changed = true; }
            else ++it;
        }
        std::unordered_set<DWORD> done;
        for (const auto& t : g_state.throttled) done.insert(t.pid);
        for (const auto& s : g_state.suspended) done.insert(s.pid);
        for (const auto& t : plan) {
            if (done.count(t.proc.pid)) continue;
            if (t.action == Action::Suspend) {
                if (Suspend(t.proc)) { g_state.suspended.push_back({ t.proc.pid, t.proc.created, t.proc.exe }); changed = true; }
            } else if (t.action == Action::Throttle || t.action == Action::ThrottleLight) {
                Throttled saved;
                if (Throttle(t.proc, t.action == Action::ThrottleLight, saved)) { g_state.throttled.push_back(saved); changed = true; }
            }
        }
        for (const auto& p : game) {
            if (done.count(p.pid)) continue;
            Throttled saved;
            if (Boost(p, saved)) { g_state.throttled.push_back(saved); changed = true; }
        }
        if (changed) SaveStateLocked();
    }

    // Раз в минуту, пока режим включён: выгрузить лишнее из памяти у ВСЕХ программ, кроме игры, окна впереди и звука —
    // и у видимых окон (браузер, Discord), и у самой ZAVIRIA. Одного раза при включении мало: фоновые программы
    // за минуту набирают память обратно. Это не закрывает программы — Windows вернёт им страницы, когда понадобятся.
    uint64_t TrimAll() {
        bool gameFound = false;
        std::unordered_set<DWORD> skip;
        for (const auto& p : GameTree(gameFound)) skip.insert(p.pid);
        DWORD fg = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &fg);
        if (fg != GetCurrentProcessId()) skip.insert(fg);
        for (const DWORD pid : AudioPids()) skip.insert(pid); // музыку не дёргаем — иначе может заикаться
        wchar_t windowsDir[MAX_PATH] = {};
        GetWindowsDirectoryW(windowsDir, MAX_PATH);
        const std::wstring winPrefix = Text::LowerRu(windowsDir) + L"\\";
        uint64_t freed = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;
        PROCESSENTRY32W e{ sizeof(e) };
        for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
            if (e.th32ProcessID <= 4 || skip.count(e.th32ProcessID)) continue;
            HANDLE h = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID);
            if (!h) continue;
            wchar_t path[MAX_PATH] = {};
            DWORD size = MAX_PATH;
            const bool system = QueryFullProcessImageNameW(h, 0, path, &size) && Text::LowerRu(path).rfind(winPrefix, 0) == 0;
            PROCESS_MEMORY_COUNTERS before{ sizeof(before) }, after{ sizeof(after) };
            if (!system && GetProcessMemoryInfo(h, &before, sizeof(before)) && before.WorkingSetSize > (64ull << 20)) {
                SetProcessWorkingSetSize(h, static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
                if (GetProcessMemoryInfo(h, &after, sizeof(after)) && before.WorkingSetSize > after.WorkingSetSize) freed += before.WorkingSetSize - after.WorkingSetSize;
            }
            CloseHandle(h);
        }
        CloseHandle(snap);
        HeapCompact(GetProcessHeap(), 0); // и своя память: в игре окно ZAVIRIA обычно свёрнуто
        return freed;
    }

    // Нажали на замороженную программу (она вышла вперёд) — сразу разморозить, не ждать пересканирования
    void WakeForeground() {
        DWORD fg = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &fg);
        if (!fg) return;
        std::lock_guard lock(g_mutex);
        for (auto it = g_state.suspended.begin(); it != g_state.suspended.end();) {
            if (it->pid == fg) { Resume(*it); it = g_state.suspended.erase(it); SaveStateLocked(); }
            else ++it;
        }
    }

    void RescanLoop(int generation) {
        for (int round = 1;; ++round) {
            for (int i = 0; i < 40; ++i) { // каждые 4 секунды; выключили — выходим сразу
                Sleep(100);
                if (!g_active.load() || g_generation.load() != generation) return;
                if (i % 3 == 0) WakeForeground();
            }
            if (g_busy.load()) continue;
            Rescan();
            if (round % 15 == 0) { // раз в минуту
                const uint64_t freed = TrimAll();
                if (freed > (128ull << 20)) Log::Write("игровой режим: освобождено ещё " + std::to_string(freed >> 20) + " МБ");
            }
        }
    }

    std::string JoinNames(const std::vector<std::string>& names, size_t limit = 6) {
        std::set<std::string> unique(names.begin(), names.end());
        std::string out;
        size_t n = 0;
        for (const auto& name : unique) {
            if (n == limit) { out += Lang::IsEnglish() ? " and more" : " и другие"; break; }
            if (n++) out += ", ";
            out += name;
        }
        return out;
    }

    std::string Plural(int n, const char* one, const char* few, const char* many) {
        const int mod10 = n % 10, mod100 = n % 100;
        return std::to_string(n) + " " + (mod10 == 1 && mod100 != 11 ? one : mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14) ? few : many);
    }
}

// =====================================================================
bool EcoMode::Active() { return g_active.load(); }
bool EcoMode::Busy() { return g_busy.load(); }

std::vector<std::string> EcoMode::List(const char* key) {
    std::vector<std::string> items;
    const std::string raw = Config::Value("eco", key, DefaultList(key));
    size_t start = 0;
    while (start <= raw.size()) {
        const size_t comma = raw.find(',', start);
        const std::string item = NormalizeExe(raw.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (!item.empty() && std::find(items.begin(), items.end(), item) == items.end()) items.push_back(item);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return items;
}

void EcoMode::SetList(const char* key, const std::vector<std::string>& items) {
    std::string raw;
    for (const auto& item : items) {
        const std::string name = NormalizeExe(item);
        if (name.empty()) continue;
        if (!raw.empty()) raw += ",";
        raw += name;
    }
    Config::SetValue("eco", key, raw);
}

bool EcoMode::PreviewSeen() { return Config::Value("eco", "preview_seen", "0") == "1"; }
void EcoMode::MarkPreviewSeen() { Config::SetValue("eco", "preview_seen", "1"); }

void EcoMode::StartPreview() {
    if (g_previewRunning.exchange(true)) return;
    {
        std::lock_guard lock(g_previewMutex);
        g_preview.ready = false;
    }
    std::thread([] {
        Preview p;
        std::set<std::string> throttle, suspend, close;
        for (const auto& t : Plan(true)) {
            if (t.action == Action::Close) close.insert(t.title);
            else if (t.action == Action::Suspend) suspend.insert(t.title);
            else throttle.insert(t.title);
        }
        for (const auto& name : close) { suspend.erase(name); throttle.erase(name); }
        for (const auto& name : suspend) throttle.erase(name);
        p.throttle.assign(throttle.begin(), throttle.end());
        p.suspend.assign(suspend.begin(), suspend.end());
        p.close.assign(close.begin(), close.end());
        p.powerScheme = FastScheme() != nullptr || OverlaySetter() != nullptr;
        p.brightness = CurrentBrightness();
        p.targetBrightness = TargetBrightness();
        p.ready = true;
        std::lock_guard lock(g_previewMutex);
        g_preview = std::move(p);
        g_previewRunning = false;
    }).detach();
}

EcoMode::Preview EcoMode::GetPreview() {
    std::lock_guard lock(g_previewMutex);
    return g_preview;
}

std::string EcoMode::Enable() {
    const bool en = Lang::IsEnglish();
    // игровой режим убран по просьбе владелицы: включить нельзя (выключение и восстановление после сбоя — остаются)
    if (!g_active.load()) return en ? "Game mode has been removed" : "Игрового режима больше нет";
    if (g_active.load()) return en ? "Game mode is already on" : "Игровой режим уже включён";
    if (g_busy.exchange(true)) return en ? "Just a moment, I'm already switching game mode" : "Секунду, уже переключаю игровой режим";
    Log::Write("игровой режим: включаю");

    State s;
    s.active = true;
    // 1. Питание: схема «Максимальная/Высокая производительность», а если её нет (Windows 11 часто прячет) — режим «Максимальная производительность»
    GUID* current = nullptr;
    if (PowerGetActiveScheme(nullptr, &current) == ERROR_SUCCESS && current) {
        s.scheme = *current;
        LocalFree(current);
        const GUID* fast = FastScheme();
        if (fast && !IsEqualGUID(s.scheme, *fast) && PowerSetActiveScheme(nullptr, fast) == ERROR_SUCCESS) s.schemeChanged = true;
    }
    if (!s.schemeChanged) {
        const auto get = OverlayGetter();
        const auto set = OverlaySetter();
        GUID overlay{};
        if (get && set && get(&overlay) == ERROR_SUCCESS && !IsEqualGUID(overlay, kOverlayPerformance) && set(kOverlayPerformance) == ERROR_SUCCESS) {
            s.overlay = overlay;
            s.overlayChanged = true;
        }
    }

    // 2. Программы (и их память — в файл подкачки)
    g_trimmed = 0;
    int throttled = 0, suspended = 0;
    std::vector<std::string> closedNames;
    for (const auto& t : Plan(true)) {
        if (t.action == Action::Close) {
            HANDLE h = OpenSame(t.proc.pid, t.proc.created, PROCESS_TERMINATE | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ);
            if (!h) h = OpenSame(t.proc.pid, t.proc.created, PROCESS_TERMINATE);
            if (!h) continue;
            const std::wstring commandLine = CommandLine(h);
            if (TerminateProcess(h, 0)) {
                // запоминаем по программе (корню), не по каждому процессу-помощнику
                if (t.root) {
                    s.closedNames.push_back(t.title);
                    s.closedCommands.push_back(commandLine.empty() ? L"\"" + t.proc.path + L"\"" : commandLine);
                }
                closedNames.push_back(t.title);
            }
            CloseHandle(h);
        } else if (t.action == Action::Suspend) {
            if (Suspend(t.proc)) { s.suspended.push_back({ t.proc.pid, t.proc.created, t.proc.exe }); ++suspended; }
        } else {
            Throttled saved;
            if (Throttle(t.proc, t.action == Action::ThrottleLight, saved)) { s.throttled.push_back(saved); ++throttled; }
        }
    }

    {
        std::lock_guard lock(g_mutex);
        g_state = s;
        SaveStateLocked();
    }
    g_active = true;
    const int generation = ++g_generation;
    std::thread(RescanLoop, generation).detach();
    ApplyOwnFeatures(true);

    Rescan(); // игра впереди — сразу поднять ей приоритет (яркость в игровом режиме не трогаем)
    g_trimmed += TrimAll();
    (void)generation;

    g_busy = false;
    Log::Write("игровой режим: включён — замедлено " + std::to_string(throttled) + ", приостановлено " + std::to_string(suspended) + ", закрыто " + std::to_string(closedNames.size()) + ", освобождено " + std::to_string(g_trimmed.load() >> 20) + " МБ");
    std::string reply;
    if (en) {
        reply = "Game mode is on: slowed down " + std::to_string(throttled) + " background processes so the game gets everything";
        if (g_trimmed > 64ull << 20) reply += ", freed " + std::to_string(g_trimmed >> 20) + " MB of memory";
        if (suspended) reply += ", paused " + std::to_string(suspended);
        if (!closedNames.empty()) reply += ", closed " + JoinNames(closedNames);
        reply += s.schemeChanged ? ". Power plan: High performance" : s.overlayChanged ? ". Power mode: Best performance" : "";
    } else {
        reply = "Игровой режим включён: замедлила " + Plural(throttled, "фоновый процесс", "фоновых процесса", "фоновых процессов") + ", все ресурсы — игре";
        if (g_trimmed > 64ull << 20) reply += ", освободила " + std::to_string(g_trimmed >> 20) + " МБ памяти";
        if (suspended) reply += ", приостановила " + std::to_string(suspended);
        if (!closedNames.empty()) reply += ", закрыла " + JoinNames(closedNames);
        reply += s.schemeChanged ? ". Схема питания — «Высокая производительность»" : s.overlayChanged ? ". Режим питания — «Максимальная производительность»" : "";
    }
    return reply;
}

std::string EcoMode::Disable() {
    const bool en = Lang::IsEnglish();
    if (!g_active.load()) return en ? "Game mode is already off" : "Игровой режим и так выключен";
    if (g_busy.exchange(true)) return en ? "Just a moment, I'm already switching game mode" : "Секунду, уже переключаю игровой режим";
    Log::Write("экономия: выключаю");
    g_active = false;
    ++g_generation;
    State s;
    {
        std::lock_guard lock(g_mutex); // дождаться пересканирования и яркости, если они сейчас идут
        s = g_state;
        g_state = State{};
    }
    const RestoreCounts counts = RestoreAll(s);
    {
        std::lock_guard lock(g_mutex);
        SaveStateLocked(); // active = false — файл состояния удаляется
    }
    ApplyOwnFeatures(false);
    {
        std::lock_guard lock(g_reportMutex);
        g_report = Report{};
        g_report.shown = s.closedNames.empty();
        g_report.closedNames = s.closedNames;
        g_report.closedPaths = s.closedCommands;
        g_report.restored = counts.restored;
        g_report.resumed = counts.resumed;
    }
    g_busy = false;
    Log::Write("экономия: выключена — возвращено " + std::to_string(counts.restored) + ", разморожено " + std::to_string(counts.resumed));
    std::string reply;
    if (en) {
        reply = "Game mode is off, everything is back as it was";
        if (!s.closedNames.empty()) reply += ". Closed during game mode: " + JoinNames(s.closedNames, 20) + " — you can start them again from the ZAVIRIA window";
    } else {
        reply = "Игровой режим выключен, всё вернула как было";
        if (!s.closedNames.empty()) reply += ". Во время игрового режима закрыла: " + JoinNames(s.closedNames, 20) + " — запустить снова можно кнопкой в окне ZAVIRIA";
    }
    return reply;
}

std::string EcoMode::Toggle() { return g_active.load() ? Disable() : Enable(); } // включить уже нельзя — см. Enable

EcoMode::Report EcoMode::LastReport() {
    std::lock_guard lock(g_reportMutex);
    return g_report;
}

void EcoMode::DismissReport() {
    std::lock_guard lock(g_reportMutex);
    g_report.shown = true;
}

void EcoMode::RelaunchClosed() {
    std::vector<std::wstring> commands;
    {
        std::lock_guard lock(g_reportMutex);
        commands = g_report.closedPaths;
        g_report.shown = true;
        g_report.closedPaths.clear();
        g_report.closedNames.clear();
    }
    for (auto command : commands) {
        STARTUPINFOW si{ sizeof(si) };
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        } else Log::Write("экономия: не запустилось снова — " + Text::ToUtf8(command));
    }
}

void EcoMode::RecoverAfterCrash() {
    std::ifstream in(StateFile(), std::ios::binary);
    if (!in) return;
    const json j = json::parse(in, nullptr, false);
    in.close();
    State s;
    if (j.is_object()) {
        s.schemeChanged = j.value("schemeChanged", false);
        s.scheme = GuidFrom(j.value("scheme", ""));
        s.overlayChanged = j.value("overlayChanged", false);
        s.overlay = GuidFrom(j.value("overlay", ""));
        s.brightness = j.value("brightness", -1);
        if (j.contains("throttled") && j["throttled"].is_array())
            for (const auto& t : j["throttled"])
                s.throttled.push_back({ t.value("pid", 0u), t.value("created", 0ull), t.value("priority", 0u), t.value("control", 0u), t.value("state", 0u), t.value("exe", ""), t.value("boost", false) });
        if (j.contains("suspended") && j["suspended"].is_array())
            for (const auto& p : j["suspended"]) s.suspended.push_back({ p.value("pid", 0u), p.value("created", 0ull), p.value("exe", "") });
    }
    const RestoreCounts counts = RestoreAll(s);
    std::error_code ec;
    std::filesystem::remove(StateFile(), ec);
    Log::Write("экономия: прошлый запуск закрылся с включённым режимом — вернула как было (" + std::to_string(counts.restored) + " + " + std::to_string(counts.resumed) + ")");
}

void EcoMode::Shutdown() {
    if (g_active.load()) Disable();
}
