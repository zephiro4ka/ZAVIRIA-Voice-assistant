#include "Notch.h"

#include "AudioBands.h"
#include "Brain.h"
#include "EcoMode.h"
#include "Http.h"
#include "Json.h"
#include "Lang.h"
#include "Log.h"
#include "MediaNow.h"
#include "Platform.h"
#include "Settings.h"
#include "Skills.h"
#include "State.h"
#include "TextUtils.h"
#include "Theme.h"
#include "IslandSettings.h"
#include "Weather.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

using namespace Gdiplus;

namespace {
    // Окно постоянного размера (запас под «отскок» пружины, «ушки» и свечение), рисуем внутри.
    constexpr float kWinW = 900, kWinH = 270;
    constexpr float kSideIdleW = 50, kSideIdleH = 122; // маленький островок у бокового края: вглубь и вдоль края
    constexpr float kIdleW = 172, kIdleH = 39, kWaveW = 26, kPanelW = 440, kPanelH = 152, kLyricsW = 250, kEar = 18, kTabH = 46, kLyricsNarrow = 110; // со словами плеер уже на столько — островок меньше
    constexpr int kShelfMax = 8;
    enum class Mode { Hidden, Idle, Toast, Panel, Listening, Thinking, Speaking };
    enum Page { kMusic, kShelf, kWeather, kTimers, kStopwatch, kPomodoro, kWater, kPageCount, kConfig = kPageCount };
    enum HitId {
        kNone = 0, kPrev = 1, kPlay, kNext, kArrowL = 10, kArrowR, kClear = 20, kSwStart = 30, kSwLap, kSwReset,
        kPomoStart = 40, kPomoReset, kDrink = 45, kUndo, kBell, kTimerX = 60, kItem = 100, kRemove = 200, kTab = 300, kGear = 400, kLyrics,
        kChip = 500, kVoice = 1000
    };

    float g_scale = 0.93f; // размер островка: обычный 0.93, крупнее 1.07 ([overlay] notch_size)
    float S(float v) { return v * Theme::dpi * g_scale; }
    float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
    float Ease(float t) { t = Clamp01(t); return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); } // мягкий старт и мягкая остановка
    std::wstring Tw(const char* ru) { return Text::ToWide(Lang::IsEnglish() ? Lang::Tr(ru) : ru); }
    bool Opt(const char* key, const char* def = "1") { return Config::Value("overlay", key, def) == "1"; }

    // К какому краю прилип островок (0 верх, 1 низ, 2 лево, 3 право) и где вдоль края (0…1, 0.5 — середина).
    // Всё содержимое рисуется как у верхнего края; форма и место окна подстраиваются под край
    int g_edge = 0;
    float g_along = 0.5f;
    float g_offY = 0;      // сдвиг островка вниз внутри окна (низ и бока) — для попаданий мышью
    bool Side() { return g_edge >= 2; }
    int g_eyeShape = 0;    // форма глазок: 0 овалы, 1 сердечки, 2 звёздочки, 3 стрелочки, 4 плюсики, 5 искры

    std::mutex g_dumpMutex;
    std::string g_dumpFolder;

    // Пружина: k — жёсткость, zeta < 1 даёт лёгкий отскок, как у Dynamic Island.
    struct Spring {
        float x = 0, v = 0, to = 0, stiff = 1.0f; // stiff > 1 — быстрее (всплывашки короткие, должны успеть раскрыться)
        void Step(float dt) {
            const bool closing = to < x; // сворачивается мягче и дольше, раскрывается быстро
            const float k = (closing ? 80.0f : 110.0f) * stiff, zeta = closing ? 1.0f : 0.85f; // плавнее: меньше отскока, без рывка
            const float c = 2.0f * std::sqrt(k) * zeta;
            v += (k * (to - x) - c * v) * dt;
            x += v * dt;
        }
        bool Settled() const { return std::fabs(to - x) < 0.25f && std::fabs(v) < 0.25f; }
    };

    struct Surface {
        HWND hwnd = nullptr;
        int width = 0, height = 0;
        HDC dc = nullptr;
        HBITMAP bitmap = nullptr;
        void* bits = nullptr;
        void Resize(int w, int h) {
            if (w == width && h == height && bits) return;
            if (bitmap) DeleteObject(bitmap);
            if (!dc) dc = CreateCompatibleDC(nullptr);
            BITMAPINFO info{};
            info.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
            bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
            SelectObject(dc, bitmap);
            width = w;
            height = h;
        }
        ~Surface() {
            if (bitmap) DeleteObject(bitmap);
            if (dc) DeleteDC(dc);
        }
        void Present(int x, int y) const { // попиксельная альфа: прозрачные места мышь проходит насквозь
            POINT source{ 0, 0 }, target{ x, y };
            SIZE size{ width, height };
            BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            UpdateLayeredWindow(hwnd, nullptr, &target, &size, dc, &source, 0, &blend, ULW_ALPHA);
        }
    };

    // Перо с круглыми концами
    struct RPen : Pen {
        RPen(const Color& c, REAL w) : Pen(c, w) { SetStartCap(LineCapRound); SetEndCap(LineCapRound); SetLineJoin(LineJoinRound); }
        RPen(const Brush* b, REAL w) : Pen(b, w) { SetStartCap(LineCapRound); SetEndCap(LineCapRound); SetLineJoin(LineJoinRound); }
    };

    // ---------------------------------------------------------------- всплывашки (раскладка, помодоро, вода, зарядка)
    enum ToastIcon { kIconLayout, kIconTomato, kIconDrop, kIconPower, kIconShelf };
    struct Toast { std::wstring text, badge; int icon = 0; ULONGLONG until = 0; } g_toast;
    std::wstring g_flash;          // то же, но когда панель открыта — строкой в шапке
    ULONGLONG g_flashUntil = 0;
    bool g_panelOpen = false;

    void ShowToast(const std::wstring& text, int icon, const std::wstring& badge = L"", ULONGLONG ms = 1800) {
        if (g_panelOpen) { g_flash = text; g_flashUntil = GetTickCount64() + ms; return; }
        g_toast = { text, badge, icon, GetTickCount64() + ms };
    }
    void Flash(const char* ru) { g_flash = Tw(ru); g_flashUntil = GetTickCount64() + 1400; }

    // ---------------------------------------------------------------- полка
    struct Thumb { std::vector<uint32_t> px; std::unique_ptr<Bitmap> bmp; };
    struct ShelfItem {
        std::wstring path, name;
        std::unique_ptr<Thumb> thumb;
        bool tried = false;
    };
    std::vector<ShelfItem> g_shelf;

    void SaveShelf() {
        std::wstring all;
        for (const auto& it : g_shelf) { if (!all.empty()) all += L'|'; all += it.path; }
        Config::SetValue("overlay", "shelf", Text::ToUtf8(all));
    }

    void AddToShelf(const std::wstring& path) {
        if (path.empty()) return;
        for (const auto& it : g_shelf) if (_wcsicmp(it.path.c_str(), path.c_str()) == 0) return;
        if (static_cast<int>(g_shelf.size()) >= kShelfMax) { g_shelf.erase(g_shelf.begin()); Flash("Полка полна — убрала самый старый файл"); }
        ShelfItem item;
        item.path = path;
        const size_t slash = path.find_last_of(L"\\/");
        item.name = slash == std::wstring::npos ? path : path.substr(slash + 1);
        g_shelf.push_back(std::move(item));
    }

    // «Удаление»: брошенное в островок удаляется насовсем, мимо корзины. Корень диска, папки Windows и профиля не трогаем
    void DeleteForever(std::vector<std::wstring> paths) {
        wchar_t windows[MAX_PATH] = {}, profile[MAX_PATH] = {};
        GetWindowsDirectoryW(windows, MAX_PATH);
        GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
        int done = 0, refused = 0;
        for (const auto& p : paths) {
            std::wstring lower = Text::LowerRu(p);
            while (!lower.empty() && (lower.back() == L'\\' || lower.back() == L'/')) lower.pop_back();
            const bool root = lower.size() <= 2, system = lower == Text::LowerRu(windows) || lower == Text::LowerRu(profile) || lower.find(Text::LowerRu(windows) + L"\\") == 0;
            if (root || system) { ++refused; continue; }
            std::wstring from = p;
            from.push_back(L'\0'); // двойной ноль в конце — так хочет SHFileOperation
            SHFILEOPSTRUCTW op{};
            op.wFunc = FO_DELETE;
            op.pFrom = from.c_str();
            op.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI; // без FOF_ALLOWUNDO — не в корзину
            if (SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted) ++done;
        }
        if (done) Flash(("Удалено навсегда: " + std::to_string(done)).c_str());
        else if (refused) Flash("Это удалять нельзя — системная папка");
        else Flash("Не получилось удалить — файл занят?");
    }

    void LoadShelf() {
        const std::wstring all = Text::ToWide(Config::Value("overlay", "shelf", ""));
        size_t from = 0;
        while (from <= all.size()) {
            const size_t bar = all.find(L'|', from);
            const std::wstring p = all.substr(from, bar == std::wstring::npos ? std::wstring::npos : bar - from);
            if (!p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) AddToShelf(p);
            if (bar == std::wstring::npos) break;
            from = bar + 1;
        }
    }

    // Значок или миниатюра файла — как в проводнике
    std::unique_ptr<Thumb> LoadThumb(const std::wstring& path, int size) {
        IShellItemImageFactory* factory = nullptr;
        if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory)))) return nullptr;
        HBITMAP hb = nullptr;
        const HRESULT hr = factory->GetImage(SIZE{ size, size }, SIIGBF_BIGGERSIZEOK, &hb);
        factory->Release();
        if (FAILED(hr) || !hb) return nullptr;
        BITMAP bm{};
        GetObjectW(hb, sizeof(bm), &bm);
        const int w = bm.bmWidth, h = std::abs(bm.bmHeight);
        if (w <= 0 || h <= 0) { DeleteObject(hb); return nullptr; }
        auto t = std::make_unique<Thumb>();
        t->px.resize(static_cast<size_t>(w) * h);
        BITMAPINFO bi{};
        bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
        HDC dc = GetDC(nullptr);
        GetDIBits(dc, hb, 0, h, t->px.data(), &bi, DIB_RGB_COLORS);
        ReleaseDC(nullptr, dc);
        DeleteObject(hb);
        if (std::none_of(t->px.begin(), t->px.end(), [](uint32_t p) { return (p >> 24) != 0; }))
            for (auto& p : t->px) p |= 0xFF000000; // у фото альфы нет — считаем непрозрачным
        t->bmp = std::make_unique<Bitmap>(w, h, w * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(t->px.data()));
        return t;
    }

    IDataObject* FileData(const std::wstring& path) {
        IShellItem* item = nullptr;
        if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) return nullptr;
        IDataObject* data = nullptr;
        item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
        item->Release();
        return data;
    }

    // ---------------------------------------------------------------- страницы
    std::array<bool, kPageCount> g_pageOn{ true, true, true, true, true, true, true }; // какие модули включены
    std::atomic<bool> g_dragOver{ false };
    int g_page = kMusic, g_pageDir = 1;
    float g_pageT = 1;
    bool g_lyricsOpen = false, g_optLyrics = false;

    bool AnyPage() { return std::any_of(g_pageOn.begin(), g_pageOn.end(), [](bool b) { return b; }); }
    int StepPage(int from, int dir) {
        for (int i = 1; i <= kPageCount; ++i) { const int p = ((from + dir * i) % kPageCount + kPageCount) % kPageCount; if (g_pageOn[p]) return p; }
        return from;
    }
    void Go(int page, int dir) { if (page == g_page) return; g_pageDir = dir; g_page = page; g_pageT = 0; }
    void Turn(int dir) { Go(StepPage(g_page, dir), dir); }
    void SetPage(int page) {
        if (page < 0 || page > kConfig || (page < kPageCount && !g_pageOn[page])) return;
        Go(page, page > g_page ? 1 : -1);
    }

    struct DropTarget : IDropTarget {
        bool ok = false;
        HRESULT __stdcall QueryInterface(REFIID id, void** out) override {
            if (id == IID_IUnknown || id == IID_IDropTarget) { *out = static_cast<IDropTarget*>(this); return S_OK; }
            *out = nullptr;
            return E_NOINTERFACE;
        }
        ULONG __stdcall AddRef() override { return 1; }  // живёт всё время работы программы
        ULONG __stdcall Release() override { return 1; }
        static FORMATETC Format() { return { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL }; }
        HRESULT __stdcall DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
            FORMATETC f = Format();
            ok = g_pageOn[kShelf] && data && data->QueryGetData(&f) == S_OK;
            g_dragOver = ok;
            if (ok) SetPage(kShelf);
            *effect = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
            return S_OK;
        }
        HRESULT __stdcall DragOver(DWORD, POINTL, DWORD* effect) override { *effect = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE; return S_OK; }
        HRESULT __stdcall DragLeave() override { g_dragOver = false; return S_OK; }
        HRESULT __stdcall Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
            g_dragOver = false;
            *effect = DROPEFFECT_NONE;
            FORMATETC f = Format();
            STGMEDIUM m{};
            if (!ok || FAILED(data->GetData(&f, &m))) return S_OK;
            std::vector<std::wstring> dropped;
            if (auto drop = static_cast<HDROP>(GlobalLock(m.hGlobal))) {
                const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < n; ++i) {
                    const UINT len = DragQueryFileW(drop, i, nullptr, 0);
                    std::wstring p(len, L'\0');
                    DragQueryFileW(drop, i, p.data(), len + 1);
                    if (!p.empty() && p.back() == L'\0') p.pop_back();
                    dropped.push_back(p);
                }
                GlobalUnlock(m.hGlobal);
            }
            ReleaseStgMedium(&m);
            if (!dropped.empty()) std::thread(DeleteForever, dropped).detach();
            *effect = DROPEFFECT_MOVE; // файл уходит насовсем
            return S_OK;
        }
    } g_drop;

    // ---------------------------------------------------------------- секундомер, помодоро, вода
    struct Stopwatch {
        bool running = false;
        ULONGLONG since = 0, total = 0;
        std::vector<ULONGLONG> laps;
        ULONGLONG Elapsed() const { return total + (running ? GetTickCount64() - since : 0); }
    } g_sw;

    struct Pomodoro {
        int phase = 0, session = 1; // 0 фокус, 1 короткий перерыв, 2 длинный
        bool running = false;
        ULONGLONG end = 0, left = 25 * 60000ull;
        static ULONGLONG Len(int phase) { return (phase == 0 ? 25 : phase == 1 ? 5 : 15) * 60000ull; }
        ULONGLONG Left() const { const ULONGLONG now = GetTickCount64(); return running ? (end > now ? end - now : 0) : left; }
        void Toggle() {
            if (running) { left = Left(); running = false; }
            else { end = GetTickCount64() + left; running = true; }
        }
        void Reset() { *this = {}; }
        void Tick() { // фаза кончилась — следующая начинается сама
            if (!running || Left() > 0) return;
            if (phase == 0) phase = session < 4 ? 1 : 2;
            else { session = phase == 2 ? 1 : session + 1; phase = 0; }
            left = Len(phase);
            end = GetTickCount64() + left;
            if (phase == 0) {
                wchar_t b[96];
                swprintf(b, 96, L"%ls · %d %ls 4", Tw("Снова фокус").c_str(), session, Tw("из").c_str());
                ShowToast(b, kIconTomato, L"", 3000);
            } else ShowToast(Tw(phase == 1 ? "Перерыв 5 минут — отдохни" : "Длинный перерыв 15 минут"), kIconTomato, L"", 3000);
        }
    } g_pomo;

    struct Water {
        int day = 0, ml = 0, goal = 1500, cups = 0;
        bool bell = true;
        ULONGLONG next = 0; // когда напомнить
        static int Today() { SYSTEMTIME t{}; GetLocalTime(&t); return t.wYear * 10000 + t.wMonth * 100 + t.wDay; }
        void Load() {
            day = std::atoi(Config::Value("overlay", "water_day", "0").c_str());
            ml = std::atoi(Config::Value("overlay", "water_ml", "0").c_str());
            goal = std::clamp(std::atoi(Config::Value("overlay", "water_goal", "1500").c_str()), 500, 5000);
            bell = Opt("water_bell");
            if (day != Today()) { day = Today(); ml = 0; }
            cups = ml / 250;
            next = GetTickCount64() + 60 * 60000ull;
        }
        void Save() const {
            Config::SetValue("overlay", "water_day", std::to_string(day));
            Config::SetValue("overlay", "water_ml", std::to_string(ml));
        }
        void Drink(int delta) {
            if (day != Today()) { day = Today(); ml = 0; }
            ml = std::max(0, ml + delta);
            cups = ml / 250;
            next = GetTickCount64() + 60 * 60000ull;
            Save();
        }
        void Tick() {
            if (!bell || !g_pageOn[kWater] || GetTickCount64() < next) return;
            next = GetTickCount64() + 60 * 60000ull;
            SYSTEMTIME t{};
            GetLocalTime(&t);
            if (t.wHour >= 9 && t.wHour < 22 && ml < goal) ShowToast(Tw("Пора выпить воды"), kIconDrop, L"", 3500); // ночью не будим
        }
    } g_water;

    // ---------------------------------------------------------------- текст песни (lrclib.net, только если включено)
    struct LyricLine { double at; std::wstring text; };
    struct Lyrics {
        std::mutex mutex;
        std::string key;            // «название|исполнитель»
        int state = 0;              // 0 нет, 1 ищу, 2 есть синхронный, 3 только простой текст, 4 не нашлось
        std::vector<LyricLine> lines;
    } g_lyrics;

    std::vector<LyricLine> ParseLrc(const std::string& lrc) {
        std::vector<LyricLine> out;
        size_t pos = 0;
        while (pos < lrc.size()) {
            size_t end = lrc.find('\n', pos);
            if (end == std::string::npos) end = lrc.size();
            std::string line = lrc.substr(pos, end - pos);
            pos = end + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            int mm = 0;
            double ss = 0;
            const size_t close = line.find(']');
            if (line.size() > 1 && line[0] == '[' && close != std::string::npos && std::sscanf(line.c_str() + 1, "%d:%lf", &mm, &ss) == 2)
                out.push_back({ mm * 60 + ss, Text::ToWide(Text::Trim(line.substr(close + 1))) });
        }
        return out;
    }

    void FetchLyrics(const MediaNow::Track& t) {
        const std::string key = t.title + "|" + t.artist;
        {
            std::lock_guard lock(g_lyrics.mutex);
            if (g_lyrics.key == key) return;
            g_lyrics.key = key;
            g_lyrics.state = 1;
            g_lyrics.lines.clear();
        }
        std::thread([key, title = t.title, artist = t.artist, length = t.duration] {
            // Названия из YouTube и браузера грязные: «EP Эллен — BITE! | Zenless Zone Zero», «Song (Official Video) [4K]».
            // Чистим и пробуем по очереди: точное совпадение, «исполнитель — песня» из самого названия, поиск по словам
            auto clean = [](std::string s) {
                for (const char* cut : { " | ", " (Official", " (official", " [Official", " (Lyric", " (lyric", " (Audio", " (Music Video", " (MV", " ft.", " feat.", " Feat." })
                    if (const size_t at = s.find(cut); at != std::string::npos && at > 0) s = s.substr(0, at);
                for (char open : { '(', '[' }) { // хвост в скобках
                    const size_t at = s.rfind(open);
                    if (at != std::string::npos && at > 2 && s.find(open == '(' ? ')' : ']', at) != std::string::npos) s = Text::Trim(s.substr(0, at));
                }
                if (s.rfind("EP ", 0) == 0 || s.rfind("OST ", 0) == 0) s = s.substr(s.find(' ') + 1);
                return Text::Trim(s);
            };
            std::vector<std::pair<std::string, std::string>> tries; // песня, исполнитель
            const std::string t0 = clean(title);
            tries.push_back({ t0, artist });
            for (const char* dash : { " — ", " – ", " - " })
                if (const size_t at = t0.find(dash); at != std::string::npos) { tries.push_back({ Text::Trim(t0.substr(at + strlen(dash))), Text::Trim(t0.substr(0, at)) }); break; }
            std::string body;
            std::vector<std::string> synced, plain;
            // Поиск отдаёт много похожих песен: «NEON» — десятки разных, у тех же исполнителей — другие песни
            // («Live Forever» вместо «The Greatest»). Берём только ту, где совпало НАЗВАНИЕ и ещё исполнитель или длина;
            // из подходящих — с синхронным текстом и ближе всего по длине. Лучше без слов, чем чужие слова
            auto squash = [](const std::string& s) { // латиницей: «ЧИЧИХСАН» и «CHICHIHSAN» сравниваются одинаково
                std::wstring w = Text::LowerRu(Text::ToWide(Lang::Latin(s))), out;
                for (wchar_t c : w) if (iswalnum(c)) out += c;
                return out;
            };
            auto firstArtist = [](std::string by) { // «burnboy, Brooke Williams» → «burnboy»
                for (const char* cut : { ", ", " & ", " feat", " ft.", " x ", " и ", "; " })
                    if (const size_t at = by.find(cut); at != std::string::npos && at > 0) by = by.substr(0, at);
                return Text::Trim(by);
            };
            auto pick = [&](const std::string& json, const std::string& song, const std::string& by) {
                synced.clear(); plain.clear();
                const std::wstring want = squash(by), wantTitle = squash(song);
                double best = 1e18;
                size_t at = json.find("{\"id\":");
                while (at != std::string::npos) {
                    const size_t next = json.find("{\"id\":", at + 1);
                    const std::string item = json.substr(at, next == std::string::npos ? std::string::npos : next - at);
                    at = next;
                    if (item.find("\"instrumental\":true") != std::string::npos) continue;
                    const auto titles = Json::FindStrings(item, "trackName", 1);
                    const std::wstring gotTitle = titles.empty() ? L"" : squash(clean(titles[0]));
                    // название: точно то же или отличается только хвостом вроде «(Remix)», «feat. …»
                    const bool sameTitle = !wantTitle.empty() && !gotTitle.empty() &&
                        (gotTitle == wantTitle || (gotTitle.rfind(wantTitle, 0) == 0 && wantTitle.size() >= 4) || (wantTitle.rfind(gotTitle, 0) == 0 && gotTitle.size() >= 4));
                    if (!sameTitle) continue;
                    const auto names = Json::FindStrings(item, "artistName", 1);
                    const std::wstring got = names.empty() ? L"" : squash(names[0]);
                    const bool sameArtist = !want.empty() && !got.empty() && (got.find(want) != std::wstring::npos || want.find(got) != std::wstring::npos ||
                                                                              got.find(squash(firstArtist(by))) != std::wstring::npos);
                    double secs = 0;
                    if (const size_t d = item.find("\"duration\":"); d != std::string::npos) secs = std::atof(item.c_str() + d + 11);
                    const bool known = length > 1 && secs > 1;
                    const bool sameLength = known && std::abs(secs - length) <= 2.0;
                    // только длина без исполнителя — не повод: так подставлялись чужие песни с тем же названием
                    if (!sameArtist && !(sameLength && want.empty())) continue;
                    if (known && std::abs(secs - length) > 20.0) continue; // другая версия песни: слова поедут мимо
                    auto s1 = Json::FindStrings(item, "syncedLyrics", 1), p1 = Json::FindStrings(item, "plainLyrics", 1);
                    const bool hasSynced = !s1.empty() && !s1[0].empty();
                    if (!hasSynced && (p1.empty() || p1[0].empty())) continue;
                    const double score = (hasSynced ? 0 : 1000) + (sameArtist ? 0 : 100) + (known ? std::abs(secs - length) : 10) + (gotTitle == wantTitle ? 0 : 5);
                    if (score < best) { best = score; synced = std::move(s1); plain = std::move(p1); }
                }
                return best < 1e18;
            };
            const std::wstring dur = length > 1 ? L"&duration=" + std::to_wstring(static_cast<int>(std::lround(length))) : L"";
            for (const auto& [song, by] : tries) {
                if (song.empty()) continue;
                const std::string by1 = firstArtist(by);
                // 1) точное совпадение: название + исполнитель (+ длина), сначала весь список исполнителей, потом первый
                std::vector<std::string> whos{ by };
                if (!by1.empty() && by1 != by) whos.push_back(by1);
                for (const std::string& who : whos) {
                    if (who.empty()) continue;
                    body = Http::HttpsGet(L"lrclib.net", L"/api/get?track_name=" + Text::ToWide(Text::UrlEncode(song)) + L"&artist_name=" + Text::ToWide(Text::UrlEncode(who)) + dur, 8000);
                    if (pick(body.empty() ? body : "[" + body + "]", song, by)) break;
                }
                if (!synced.empty() || !plain.empty()) break;
                // 2) поиск — среди результатов выбираем строго по названию
                body = Http::HttpsGet(L"lrclib.net", L"/api/search?track_name=" + Text::ToWide(Text::UrlEncode(song)) + L"&artist_name=" + Text::ToWide(Text::UrlEncode(by1)), 8000);
                if (pick(body, song, by)) break;
                body = Http::HttpsGet(L"lrclib.net", L"/api/search?q=" + Text::ToWide(Text::UrlEncode(song + " " + by)), 8000);
                if (pick(body, song, by)) break;
                body = Http::HttpsGet(L"lrclib.net", L"/api/search?track_name=" + Text::ToWide(Text::UrlEncode(song)), 8000); // только по названию
                if (pick(body, song, by)) break;
            }
            std::vector<LyricLine> lines;
            int state = 4;
            if (!synced.empty() && !synced[0].empty()) { lines = ParseLrc(synced[0]); state = lines.empty() ? 4 : 2; }
            if (state == 4 && !plain.empty() && !plain[0].empty()) {
                const std::string& s = plain[0];
                size_t p = 0;
                while (p < s.size()) {
                    size_t e = s.find('\n', p);
                    if (e == std::string::npos) e = s.size();
                    lines.push_back({ 0, Text::ToWide(Text::Trim(s.substr(p, e - p))) });
                    p = e + 1;
                }
                state = lines.empty() ? 4 : 3;
            }
            Log::Write("текст песни: «" + title + "» — " + artist + (state == 2 ? ": есть по времени" : state == 3 ? ": есть без времени" : ": не нашёлся"));
            std::lock_guard lock(g_lyrics.mutex);
            if (g_lyrics.key != key) return; // пока искали, трек сменился
            g_lyrics.lines = std::move(lines);
            g_lyrics.state = state;
        }).detach();
    }

    // ---------------------------------------------------------------- рисование
    struct Hit { RectF r; int id; };
    std::vector<Hit> g_hits;
    int g_hover = kNone, g_pressed = kNone;
    POINT g_pressAt{};

    int HitAt(float x, float y) {
        y -= g_offY;
        for (auto it = g_hits.rbegin(); it != g_hits.rend(); ++it) if (it->r.Contains(x, y)) return it->id;
        return kNone;
    }

    struct View {
        Mode mode = Mode::Idle;
        float w = 0, h = 0, t = 0, mouth = 0;
        std::array<float, 40> levels{}; // последние значения микрофона, новые справа
        std::wstring text;
        MediaNow::Track track;
        double position = 0;            // позиция трека с поправкой на время между опросами
        bool wave = false;              // в маленькой чёлке — волна музыки
        bool eyes = false;              // глазки в маленькой чёлке
        float lookX = 0, lookY = 0;     // куда смотрят глазки: -1…1, следом за мышкой
        float blink = 0;                // 0 открыты … 1 закрыты (моргание по своему, неровному ритму)
        float sleepy = 0;               // мышку давно не трогали — глазки сонно прикрываются
        float bands[4] = {};            // волна: уровни звука по частотам (бас … высокие)
        bool bandsLive = false;         // звук из колонок слушается
        float nearK = 0;                 // мышка близко (0…1) — глазки радуются ^ ^
        Weather::Today weather;
        std::vector<Skills::Countdown> timers;
        SYSTEMTIME local{};
    };

    // Фиолетовый → розовый, как логотип
    // Палитра островка — как у самого приложения (тема и атмосфера): фон, текст, любимый цвет.
    // Gray(v): 0 — фон островка, 255 — цвет текста; всё между — смесь, поэтому серые оттенки подстраиваются под тему
    struct NotchColors { float bg[3] = { 0, 0, 0 }, ink[3] = { 255, 255, 255 }, acc0[3] = { 139, 92, 246 }, acc1[3] = { 236, 72, 153 }; } g_col;
    void RefreshColors() {
        const Theme::Color top = Theme::Background(0), bottom = Theme::Background(1), acc = Theme::Accent();
        const Theme::Color bg = bottom.Mix(top, 0.5f).Mix(Theme::Light() ? Theme::Color{ 255, 255, 255 } : Theme::Color{ 0, 0, 0 }, Theme::Light() ? 0.35f : 0.25f);
        const ImU32 ink = Theme::kInk;
        g_col.bg[0] = bg.r; g_col.bg[1] = bg.g; g_col.bg[2] = bg.b;
        g_col.ink[0] = static_cast<float>(ink & 0xFF); g_col.ink[1] = static_cast<float>((ink >> 8) & 0xFF); g_col.ink[2] = static_cast<float>((ink >> 16) & 0xFF);
        const Theme::Color a1 = acc.Mix({ 244, 114, 182 }, 0.4f);
        g_col.acc0[0] = acc.r; g_col.acc0[1] = acc.g; g_col.acc0[2] = acc.b;
        g_col.acc1[0] = a1.r; g_col.acc1[1] = a1.g; g_col.acc1[2] = a1.b;
    }
    Color Grad(float t, float a) {
        t = Clamp01(t);
        const auto mix = [t](float p, float q) { return static_cast<BYTE>(p + (q - p) * t); };
        return Color(static_cast<BYTE>(255 * Clamp01(a)), mix(g_col.acc0[0], g_col.acc1[0]), mix(g_col.acc0[1], g_col.acc1[1]), mix(g_col.acc0[2], g_col.acc1[2]));
    }
    Color Gray(int v, float a) {
        const float k = std::clamp(v, 0, 255) / 255.0f;
        const auto mix = [k](float p, float q) { return static_cast<BYTE>(p + (q - p) * k); };
        return Color(static_cast<BYTE>(255 * Clamp01(a)), mix(g_col.bg[0], g_col.ink[0]), mix(g_col.bg[1], g_col.ink[1]), mix(g_col.bg[2], g_col.ink[2]));
    }
    Color Rgb(int r, int g, int b, float a) { return Color(static_cast<BYTE>(255 * Clamp01(a)), static_cast<BYTE>(r), static_cast<BYTE>(g), static_cast<BYTE>(b)); }

    struct Fonts {
        float dpi = 0;
        std::unique_ptr<FontFamily> family;
        std::unique_ptr<Font> text, label, title, tiny, note, big, lyric, idle;
        void Ensure() {
            if (dpi == Theme::dpi * g_scale && text) return;
            dpi = Theme::dpi * g_scale;
            family = std::make_unique<FontFamily>(L"Segoe UI");
            text = std::make_unique<Font>(family.get(), S(15), FontStyleRegular, UnitPixel);
            label = std::make_unique<Font>(family.get(), S(14), FontStyleBold, UnitPixel);
            title = std::make_unique<Font>(family.get(), S(15), FontStyleBold, UnitPixel);
            tiny = std::make_unique<Font>(family.get(), S(12), FontStyleRegular, UnitPixel);
            note = std::make_unique<Font>(family.get(), S(21), FontStyleRegular, UnitPixel);
            big = std::make_unique<Font>(family.get(), S(30), FontStyleBold, UnitPixel);
            lyric = std::make_unique<Font>(family.get(), S(17), FontStyleBold, UnitPixel);
            idle = std::make_unique<Font>(family.get(), S(20.5f), FontStyleBold, UnitPixel);
        }
    } g_fonts;

    void Rounded(GraphicsPath& p, REAL x, REAL y, REAL w, REAL h, REAL r) {
        r = std::min({ r, w * 0.5f, h * 0.5f });
        const REAL d = r * 2;
        p.AddArc(x, y, d, d, 180, 90);
        p.AddArc(x + w - d, y, d, d, 270, 90);
        p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
        p.AddArc(x, y + h - d, d, d, 90, 90);
        p.CloseFigure();
    }

    // Форма: верх ровно по краю экрана, по бокам у рамки — вогнутые «ушки» (островок плавно
    // перетекает в рамку монитора, а не висит под ней), снизу скругление.
    void Shape(GraphicsPath& p, REAL cx, REAL w, REAL h) {
        const REAL r = std::min({ S(26), h * 0.5f, w * 0.5f });
        const REAL f = std::min(S(kEar), h - r);
        const REAL x0 = cx - w * 0.5f, x1 = cx + w * 0.5f;
        if (f > 0.5f) p.AddArc(x0 - 2 * f, 0.0f, 2 * f, 2 * f, 270.0f, 90.0f); else p.AddLine(x0, 0.0f, x0, 0.0f);
        if (r > 0.5f) p.AddArc(x0, h - 2 * r, 2 * r, 2 * r, 180.0f, -90.0f); else p.AddLine(x0, h, x0, h);
        if (r > 0.5f) p.AddArc(x1 - 2 * r, h - 2 * r, 2 * r, 2 * r, 90.0f, -90.0f); else p.AddLine(x1, h, x1, h);
        if (f > 0.5f) p.AddArc(x1, 0.0f, 2 * f, 2 * f, 180.0f, 90.0f); else p.AddLine(x1, 0.0f, x1, 0.0f);
        p.CloseFigure();
    }

    // Та же форма, но прижатая к выбранному краю: ровная сторона — к краю, «ушки» — вдоль края
    void EdgeShape(GraphicsPath& p, REAL cx, REAL w, REAL h) {
        if (g_edge == 0) { Shape(p, cx, w, h); return; }
        if (g_edge == 1) { Shape(p, cx, w, h); Matrix m(1, 0, 0, -1, 0, h); p.Transform(&m); return; }
        Shape(p, 0, h, w); // вдоль края — высота островка, вглубь — ширина
        const REAL x0 = cx - w * 0.5f, x1 = cx + w * 0.5f;
        Matrix m = g_edge == 2 ? Matrix(0, 1, 1, 0, x0, h * 0.5f) : Matrix(0, 1, -1, 0, x1, h * 0.5f);
        p.Transform(&m);
    }

    void Text1(Graphics& g, const std::wstring& s, const Font* font, RectF r, const Color& c, StringAlignment align = StringAlignmentNear, bool wrap = false) {
        if (s.empty() || c.GetA() == 0) return;
        StringFormat fmt;
        fmt.SetAlignment(align);
        fmt.SetLineAlignment(StringAlignmentCenter);
        fmt.SetTrimming(wrap ? StringTrimmingEllipsisWord : StringTrimmingEllipsisCharacter);
        if (!wrap) fmt.SetFormatFlags(StringFormatFlagsNoWrap);
        SolidBrush b(c);
        g.DrawString(s.c_str(), -1, font, r, &fmt, &b);
    }

    REAL TextWidth(Graphics& g, const std::wstring& s, const Font* font) {
        RectF box;
        g.MeasureString(s.c_str(), -1, font, PointF(0, 0), &box);
        return box.Width;
    }

    void Chevron(Graphics& g, REAL cx, REAL cy, int dir, const Color& c) {
        RPen pen(c, S(1.8f));
        const REAL d = S(3.2f) * dir;
        PointF pts[] = { { cx - d * 0.5f, cy - S(4.5f) }, { cx + d * 0.5f, cy }, { cx - d * 0.5f, cy + S(4.5f) } };
        g.DrawLines(&pen, pts, 3);
    }

    void PlayIcon(Graphics& g, int kind, REAL cx, REAL cy, REAL r, const Color& c) { // 0 назад, 1 пауза, 2 играть, 3 вперёд
        SolidBrush b(c);
        if (kind == 1) {
            g.FillRectangle(&b, cx - r * 0.75f, cy - r, r * 0.5f, r * 2);
            g.FillRectangle(&b, cx + r * 0.25f, cy - r, r * 0.5f, r * 2);
        } else if (kind == 2) {
            PointF tri[] = { { cx - r * 0.6f, cy - r }, { cx + r, cy }, { cx - r * 0.6f, cy + r } };
            g.FillPolygon(&b, tri, 3);
        } else {
            const REAL s = kind == 3 ? 1.0f : -1.0f;
            for (int i = 0; i < 2; ++i) {
                const REAL o = (i == 0 ? -r * 0.9f : 0.0f) * s;
                PointF tri[] = { { cx + o, cy - r * 0.8f }, { cx + o + r * 0.9f * s, cy }, { cx + o, cy + r * 0.8f } };
                g.FillPolygon(&b, tri, 3);
            }
        }
    }

    void DropShape(Graphics& g, REAL cx, REAL cy, REAL r, const Color& c, bool fill) {
        GraphicsPath p;
        p.AddBezier(cx, cy - r, cx + r * 0.2f, cy - r * 0.5f, cx + r * 0.8f, cy, cx + r * 0.8f, cy + r * 0.35f);
        p.AddArc(cx - r * 0.8f, cy - r * 0.45f, r * 1.6f, r * 1.6f, 0, 180);
        p.AddBezier(cx - r * 0.8f, cy + r * 0.35f, cx - r * 0.8f, cy, cx - r * 0.2f, cy - r * 0.5f, cx, cy - r);
        p.CloseFigure();
        if (fill) { SolidBrush b(c); g.FillPath(&b, &p); }
        else { RPen pen(c, S(1.6f)); g.DrawPath(&pen, &p); }
    }

    // Значки модулей в нижней панели
    // Мордочка-эмодзи без контура головы: глазки с бликом, ротик и румянец. Моргает, смотрит за мышкой,
    // под музыку поёт «о», рядом с мышкой — радостная ^ ^ с улыбкой
    void EyesBody(Graphics& g, const View& v, REAL cx, REAL cy, float a);
    void Eyes(Graphics& g, const View& v, REAL cx, REAL cy, float a) { // мордочка чуть крупнее — масштаб вокруг её центра
        const GraphicsState state = g.Save();
        g.TranslateTransform(cx, cy);
        g.ScaleTransform(1.0f, 1.0f);
        g.TranslateTransform(-cx, -cy);
        EyesBody(g, v, cx, cy, a);
        g.Restore(state);
    }
    // Глазки как у котика-маскота Nusa: два белых вертикальных овала без зрачков, с мягким свечением.
    // Сами глазки плавно смотрят за мышкой и моргают
    void EyesBody(Graphics& g, const View& v, REAL cx, REAL cy, float a) {
        const float t = v.t;
        const float blinkK = std::max(v.blink, v.sleepy * 0.6f);
        const float breath = std::sin(t * 1.7f);   // еле заметно «дышат»
        const REAL bob = (v.track.playing ? std::sin(t * 6.2f) * S(0.8f) : 0.0f) + std::sin(t * 1.1f) * S(0.45f) + v.sleepy * S(1.2f);
        const REAL dx = v.lookX * S(3.2f), dy = v.lookY * S(2.2f) + bob;
        const REAL rx = S(3.4f), ry0 = S(5.4f), ry = ry0 * (1.0f - 0.9f * blinkK), gap = S(g_eyeShape == 0 ? 14.0f : 16.0f);
        const int shape = g_eyeShape;
        for (int i = 0; i < 2; ++i) {
            const REAL ex = cx + (i == 0 ? -gap * 0.5f : gap * 0.5f) + dx, ey = cy + dy, h = std::max(ry, S(0.9f));
            for (int k = 3; k >= 1; --k) { // мягкий ореол любимого цвета
                SolidBrush glow(Grad(0.4f, a * 0.10f));
                const REAL e = S(1.6f) * k;
                g.FillEllipse(&glow, ex - rx - e, ey - h - e, (rx + e) * 2, (h + e) * 2);
            }
            SolidBrush white(Rgb(255, 255, 255, a));
            const REAL r = S(5.6f), sq = h / std::max(ry0, S(0.1f)); // sq < 1 — глаз моргает: форма сплющивается
            const GraphicsState st = g.Save();
            g.TranslateTransform(ex, ey);
            g.ScaleTransform(1.0f + 0.03f * breath + 0.06f * blinkK, std::max(0.12f, static_cast<float>(sq)) * (1.0f - 0.025f * breath)); // при моргании чуть шире — как будто веки
            switch (shape) {
            case 1: { // сердечко
                GraphicsPath p;
                p.AddBezier(PointF(0, r * 0.95f), PointF(-r * 1.3f, r * 0.1f), PointF(-r * 0.9f, -r * 1.0f), PointF(0, -r * 0.35f));
                p.AddBezier(PointF(0, -r * 0.35f), PointF(r * 0.9f, -r * 1.0f), PointF(r * 1.3f, r * 0.1f), PointF(0, r * 0.95f));
                g.FillPath(&white, &p);
                break;
            }
            case 2: { // звёздочка
                PointF pts[10];
                for (int k = 0; k < 10; ++k) { const float ang = -1.5708f + k * 3.14159f / 5.0f; const REAL rr = k % 2 ? r * 0.45f : r; pts[k] = PointF(std::cos(ang) * rr, std::sin(ang) * rr); }
                g.FillPolygon(&white, pts, 10);
                break;
            }
            case 3: { // стрелочка ^
                RPen pen(Rgb(255, 255, 255, a), S(2.4f));
                PointF p[] = { { -r * 0.8f, r * 0.35f }, { 0, -r * 0.5f }, { r * 0.8f, r * 0.35f } };
                g.DrawLines(&pen, p, 3);
                break;
            }
            case 4: { // плюсик
                RPen pen(Rgb(255, 255, 255, a), S(2.4f));
                g.DrawLine(&pen, -r * 0.7f, 0.0f, r * 0.7f, 0.0f);
                g.DrawLine(&pen, 0.0f, -r * 0.7f, 0.0f, r * 0.7f);
                break;
            }
            case 5: { // искра
                PointF p[] = { { 0, -r }, { r * 0.22f, -r * 0.22f }, { r, 0 }, { r * 0.22f, r * 0.22f }, { 0, r }, { -r * 0.22f, r * 0.22f }, { -r, 0 }, { -r * 0.22f, -r * 0.22f } };
                g.FillPolygon(&white, p, 8);
                break;
            }
            default: g.FillEllipse(&white, -rx, -ry0, rx * 2, ry0 * 2); break;
            }
            g.Restore(st);
        }
    }

    void TabIcon(Graphics& g, int page, REAL cx, REAL cy, REAL r, const Color& c) {
        RPen pen(c, S(1.6f));
        SolidBrush b(c);
        switch (page) {
        case kMusic:
            g.FillEllipse(&b, cx - r * 0.75f, cy + r * 0.2f, r * 0.7f, r * 0.55f);
            g.DrawLine(&pen, cx - r * 0.08f, cy + r * 0.45f, cx - r * 0.08f, cy - r * 0.8f);
            g.DrawLine(&pen, cx - r * 0.08f, cy - r * 0.8f, cx + r * 0.6f, cy - r * 0.45f);
            break;
        case kShelf: {
            PointF tray[] = { { cx - r * 0.8f, cy }, { cx - r * 0.8f, cy + r * 0.65f }, { cx + r * 0.8f, cy + r * 0.65f }, { cx + r * 0.8f, cy } };
            g.DrawLines(&pen, tray, 4);
            g.DrawLine(&pen, cx, cy - r * 0.8f, cx, cy + r * 0.15f);
            PointF head[] = { { cx - r * 0.35f, cy - r * 0.2f }, { cx, cy + r * 0.15f }, { cx + r * 0.35f, cy - r * 0.2f } };
            g.DrawLines(&pen, head, 3);
            break;
        }
        case kWeather:
            g.DrawEllipse(&pen, cx - r * 0.75f, cy - r * 0.15f, r * 0.8f, r * 0.7f);
            g.DrawEllipse(&pen, cx - r * 0.3f, cy - r * 0.6f, r * 1.0f, r * 1.0f);
            break;
        case kTimers: { // колокольчик
            GraphicsPath p;
            p.AddArc(cx - r * 0.55f, cy - r * 0.75f, r * 1.1f, r * 1.1f, 180, 180);
            p.AddLine(cx + r * 0.55f, cy - r * 0.2f, cx + r * 0.75f, cy + r * 0.45f);
            p.AddLine(cx - r * 0.75f, cy + r * 0.45f, cx - r * 0.55f, cy - r * 0.2f);
            p.CloseFigure();
            g.DrawPath(&pen, &p);
            g.FillEllipse(&b, cx - r * 0.18f, cy + r * 0.55f, r * 0.36f, r * 0.36f);
            break;
        }
        case kStopwatch:
            g.DrawEllipse(&pen, cx - r * 0.7f, cy - r * 0.5f, r * 1.4f, r * 1.4f);
            g.DrawLine(&pen, cx, cy - r * 0.95f, cx, cy - r * 0.6f);
            g.DrawLine(&pen, cx - r * 0.25f, cy - r * 0.95f, cx + r * 0.25f, cy - r * 0.95f);
            g.DrawLine(&pen, cx, cy + r * 0.2f, cx + r * 0.3f, cy - r * 0.1f);
            break;
        case kPomodoro:
            g.DrawEllipse(&pen, cx - r * 0.75f, cy - r * 0.45f, r * 1.5f, r * 1.3f);
            g.DrawLine(&pen, cx, cy - r * 0.45f, cx - r * 0.1f, cy - r * 0.9f);
            g.DrawLine(&pen, cx, cy - r * 0.45f, cx + r * 0.4f, cy - r * 0.7f);
            break;
        case kWater: DropShape(g, cx, cy - r * 0.05f, r * 0.9f, c, false); break;
        default: { // шестерёнка
            g.DrawEllipse(&pen, cx - r * 0.45f, cy - r * 0.45f, r * 0.9f, r * 0.9f);
            for (int i = 0; i < 8; ++i) {
                const REAL a = i * 3.14159265f / 4;
                g.DrawLine(&pen, cx + std::cos(a) * r * 0.62f, cy + std::sin(a) * r * 0.62f, cx + std::cos(a) * r * 0.85f, cy + std::sin(a) * r * 0.85f);
            }
            break;
        }
        }
    }

    void DrawFaded(Graphics& g, Image* img, const RectF& r, float a) {
        ColorMatrix m = { { { 1, 0, 0, 0, 0 }, { 0, 1, 0, 0, 0 }, { 0, 0, 1, 0, 0 }, { 0, 0, 0, Clamp01(a), 0 }, { 0, 0, 0, 0, 1 } } };
        ImageAttributes at;
        at.SetColorMatrix(&m);
        g.DrawImage(img, r, 0, 0, static_cast<REAL>(img->GetWidth()), static_cast<REAL>(img->GetHeight()), UnitPixel, &at);
    }

    std::wstring Clock2(double sec) {
        const int s = static_cast<int>(std::max(0.0, sec));
        wchar_t b[16];
        swprintf(b, 16, L"%d:%02d", s / 60, s % 60);
        return b;
    }

    std::wstring SwText(ULONGLONG ms, bool tenths) {
        const ULONGLONG s = ms / 1000;
        wchar_t b[32];
        if (s >= 3600) swprintf(b, 32, L"%llu:%02llu:%02llu", s / 3600, s / 60 % 60, s % 60);
        else if (tenths) swprintf(b, 32, L"%02llu:%02llu.%llu", s / 60, s % 60, ms / 100 % 10);
        else swprintf(b, 32, L"%02llu:%02llu", s / 60, s % 60);
        return b;
    }

    std::wstring Temp(float t) {
        wchar_t b[16];
        const int v = static_cast<int>(std::lround(t));
        swprintf(b, 16, v > 0 ? L"+%d°" : L"%d°", v);
        return b;
    }

    void AddHit(std::vector<Hit>* hits, RectF r, int id) { if (hits) hits->push_back({ r, id }); }

    void Bar(Graphics& g, REAL x0, REAL x1, REAL y, float k, float a, REAL thick) {
        RPen rail(Gray(48, a), thick);
        g.DrawLine(&rail, x0, y, x1, y);
        if (k > 0.005f) {
            LinearGradientBrush b(PointF(x0 - 1, 0), PointF(x1 + 1, 0), Grad(0, a), Grad(1, a));
            RPen fill(&b, thick);
            g.DrawLine(&fill, x0, y, x0 + (x1 - x0) * Clamp01(k), y);
        }
    }

    void Pill(Graphics& g, RectF r, const std::wstring& text, bool accent, bool hot, float a) {
        GraphicsPath p;
        Rounded(p, r.X, r.Y, r.Width, r.Height, r.Height * 0.5f);
        if (accent) {
            LinearGradientBrush b(PointF(r.X, 0), PointF(r.GetRight(), 0), Grad(0, a * (hot ? 1.0f : 0.85f)), Grad(1, a * (hot ? 1.0f : 0.85f)));
            g.FillPath(&b, &p);
        } else {
            SolidBrush b(Gray(hot ? 70 : 44, a));
            g.FillPath(&b, &p);
        }
        Text1(g, text, g_fonts.tiny.get(), r, Gray(245, a), StringAlignmentCenter);
    }

    // ---- «Музыка»
    void MusicPage(Graphics& g, const View& v, RectF area, float a, int hover, std::vector<Hit>* hits) {
        const auto& t = v.track;
        const REAL art = S(50), ax = area.X, ay = area.Y + S(4);
        for (int i = 3; i >= 1; --i) { // мягкий отсвет обложки, ярче, когда играет
            GraphicsPath gp;
            const REAL e = S(3.0f) * i;
            Rounded(gp, ax - e, ay - e + S(3), art + e * 2, art + e * 2, S(14) + e);
            SolidBrush gb(Grad(0.5f, a * (t.playing ? 0.07f : 0.035f) * (4 - i)));
            g.FillPath(&gb, &gp);
        }
        { // обложка: градиент логотипа, нота или живой эквалайзер
            GraphicsPath p;
            Rounded(p, ax, ay, art, art, S(14));
            LinearGradientBrush b(PointF(ax, ay), PointF(ax + art, ay + art), Grad(0, a), Grad(1, a));
            g.FillPath(&b, &p);
            if (t.playing) {
                for (int i = 0; i < 4; ++i) {
                    const float lv = 0.35f + 0.65f * (0.5f + 0.5f * std::sin(v.t * (5.0f + i) + i * 1.3f));
                    const REAL hh = S(18) * lv, x = ax + S(13) + i * S(8);
                    RPen pen(Rgb(255, 255, 255, a * 0.92f), S(3.2f));
                    g.DrawLine(&pen, x, ay + art * 0.5f - hh * 0.5f, x, ay + art * 0.5f + hh * 0.5f);
                }
            } else Text1(g, L"♫", g_fonts.note.get(), RectF(ax, ay - S(1), art, art), Rgb(255, 255, 255, a * 0.92f), StringAlignmentCenter);
        }
        // кнопки — справа, на уровне середины обложки; «играть» — круг цвета логотипа
        const REAL cy = ay + art * 0.5f, right = area.GetRight();
        const REAL bx[3] = { right - S(78), right - S(44), right - S(10) };
        for (int i = 0; i < 3; ++i) {
            const int id = kPrev + i;
            const REAL r = i == 1 ? S(15) : S(11);
            const RectF hit(bx[i] - r, cy - r, 2 * r, 2 * r);
            if (i == 1) {
                LinearGradientBrush pb(PointF(hit.X, hit.Y), PointF(hit.GetRight(), hit.GetBottom()), Grad(0, a * (hover == id ? 1.0f : 0.9f)), Grad(1, a * (hover == id ? 1.0f : 0.9f)));
                g.FillEllipse(&pb, hit);
            } else if (hover == id) {
                SolidBrush c(Gray(44, a));
                g.FillEllipse(&c, hit);
            }
            PlayIcon(g, i == 0 ? 0 : i == 2 ? 3 : (t.playing ? 1 : 2), bx[i] + (i == 1 && !t.playing ? S(1) : 0), cy, i == 1 ? S(6) : S(5), i == 1 ? Rgb(255, 255, 255, a) : Gray(225, a * (t.present ? 1.0f : 0.45f)));
            AddHit(hits, hit, id);
        }
        // название и исполнитель — по левому краю, до кнопок
        const REAL tx = ax + art + S(14), tw = bx[0] - S(18) - tx;
        const std::wstring title = t.present && !t.title.empty() ? Text::ToWide(t.title) : Tw("Ничего не играет");
        const std::wstring who = t.present ? Text::ToWide(t.artist.empty() ? t.app : t.artist) : Tw("включи музыку в любом плеере");
        Text1(g, title, g_fonts.title.get(), RectF(tx, ay + S(2), tw, S(20)), Gray(245, a));
        Text1(g, who, g_fonts.tiny.get(), RectF(tx, ay + S(21), tw, S(16)), Gray(150, a));
        // тонкая полоса времени — под текстом, время мелко по краям
        const bool known = t.present && t.duration > 1;
        const REAL py = ay + art - S(5);
        if (known) {
            const std::wstring a0 = Clock2(v.position), a1 = Clock2(t.duration);
            const REAL w0 = TextWidth(g, a0, g_fonts.tiny.get()), w1 = TextWidth(g, a1, g_fonts.tiny.get());
            Text1(g, a0, g_fonts.tiny.get(), RectF(tx, py - S(8), w0, S(16)), Gray(140, a));
            Text1(g, a1, g_fonts.tiny.get(), RectF(right - w1, py - S(8), w1, S(16)), Gray(140, a), StringAlignmentFar);
            Bar(g, tx + w0 + S(6), right - w1 - S(6), py, static_cast<float>(v.position / t.duration), a, S(3));
        } else Bar(g, tx, right, py, 0.0f, a * 0.6f, S(3));
    }

    float g_lyricScroll = 0; // плавная прокрутка строк: дробный номер строки по центру

    int LyricIndex(double pos) { // вызывать под замком g_lyrics.mutex
        int cur = 0;
        if (g_lyrics.state == 2)
            for (int i = 0; i < static_cast<int>(g_lyrics.lines.size()); ++i) if (g_lyrics.lines[i].at <= pos + 0.25) cur = i;
        return cur;
    }

    // Колонка со словами справа от плеера: текущая строка крупно по центру, соседние мельче и тусклее,
    // строки плавно уезжают вверх
    void LyricsBlock(Graphics& g, const View& v, RectF area, float a) {
        std::lock_guard lock(g_lyrics.mutex);
        const auto& L = g_lyrics.lines;
        const auto status = [&](const char* ru) { Text1(g, Tw(ru), g_fonts.tiny.get(), area, Gray(150, a), StringAlignmentCenter, true); };
        if (!v.track.present) return status("Включи музыку — здесь появятся слова");
        if (g_lyrics.state <= 1) return status("Ищу текст песни…");
        if (g_lyrics.state == 4 || L.empty()) return status("Текст этой песни не нашёлся");
        const GraphicsState saved = g.Save();
        g.SetClip(area);
        const REAL lineH = S(22), cy = area.Y + area.Height * 0.5f, cx = area.X + area.Width * 0.5f;
        const float scroll = g_lyrics.state == 2 ? g_lyricScroll : 1.0f;
        const int first = std::max(0, static_cast<int>(std::floor(scroll)) - 3), last = std::min(static_cast<int>(L.size()) - 1, static_cast<int>(std::ceil(scroll)) + 3);
        for (int i = first; i <= last; ++i) {
            const float d = std::fabs(i - scroll);
            const float focus = g_lyrics.state == 2 ? Clamp01(1.0f - d) : 0.0f;  // 1 — строка, которая звучит
            const float fade = a * Clamp01(1.0f - (d - 1.0f) * 0.8f);              // дальше соседних — тает к краям
            if (fade <= 0.01f) continue;
            const std::wstring line = L[i].text.empty() ? L"\u266A" : L[i].text;
            const REAL fit = std::min(1.0f, (area.Width - S(4)) / std::max(S(1), TextWidth(g, line, g_fonts.lyric.get()))); // \u0434\u043B\u0438\u043D\u043D\u0430\u044F \u0441\u0442\u0440\u043E\u043A\u0430 \u0443\u043C\u0435\u043D\u044C\u0448\u0430\u0435\u0442\u0441\u044F, \u0430 \u043D\u0435 \u043E\u0431\u0440\u0435\u0437\u0430\u0435\u0442\u0441\u044F
            const REAL scale = (0.76f + 0.24f * focus) * std::max(0.62f, fit);
            const GraphicsState st = g.Save();
            g.TranslateTransform(cx, cy + (i - scroll) * lineH);
            g.ScaleTransform(scale, scale);
            const REAL w = area.Width / scale, h = lineH * 1.3f / scale;
            Text1(g, line, g_fonts.lyric.get(), RectF(-w * 0.5f, -h * 0.5f, w, h), Gray(130 + static_cast<int>(122 * focus), fade * (0.5f + 0.5f * focus)), StringAlignmentCenter);
            g.Restore(st);
        }
        g.Restore(saved);
    }

    // ---- «Полка»
    void TrayIcon(Graphics& g, REAL cx, REAL cy, const Color& c) {
        RPen pen(c, S(2.2f));
        g.DrawLine(&pen, cx, cy - S(10), cx, cy + S(1));
        PointF head[] = { { cx - S(5), cy - S(4) }, { cx, cy + S(1) }, { cx + S(5), cy - S(4) } };
        g.DrawLines(&pen, head, 3);
        PointF tray[] = { { cx - S(11), cy }, { cx - S(11), cy + S(8) }, { cx + S(11), cy + S(8) }, { cx + S(11), cy } };
        g.DrawLines(&pen, tray, 4);
    }

    void ShelfPage(Graphics& g, RectF area, float a, int hover, bool dragOver, std::vector<Hit>* hits) {
        const bool empty = true; // полки больше нет: это зона удаления
        if (empty || dragOver) { // пунктирная рамка «брось сюда»
            GraphicsPath p;
            Rounded(p, area.X, area.Y, area.Width, area.Height, S(14));
            if (dragOver) {
                SolidBrush fill(Grad(0.3f, a * 0.12f));
                g.FillPath(&fill, &p);
                LinearGradientBrush b(PointF(area.X, 0), PointF(area.GetRight(), 0), Grad(0, a), Grad(1, a));
                Pen pen(&b, S(1.6f));
                pen.SetDashStyle(DashStyleDash);
                g.DrawPath(&pen, &p);
            } else {
                Pen pen(Gray(70, a), S(1.4f));
                pen.SetDashStyle(DashStyleDash);
                g.DrawPath(&pen, &p);
            }
        }
        if (empty) {
            const REAL cx = area.X + area.Width * 0.5f;
            TrayIcon(g, cx, area.Y + S(17), Gray(225, a));
            Text1(g, Tw(dragOver ? "Отпусти — удалю навсегда" : "Перетащи сюда — удалю навсегда"), g_fonts.label.get(), RectF(area.X, area.Y + S(28), area.Width, S(20)), Gray(240, a), StringAlignmentCenter);
            Text1(g, Tw("Файлы и папки удаляются насовсем, мимо корзины"), g_fonts.tiny.get(), RectF(area.X + S(8), area.Y + S(46), area.Width - S(16), S(16)), Gray(140, a), StringAlignmentCenter);
            return;
        }
        const int n = static_cast<int>(g_shelf.size());
        const REAL gap = S(8), tileW = std::min(S(78), (area.Width - gap * (n - 1)) / n), tileH = area.Height;
        const REAL startX = area.X + (area.Width - (tileW * n + gap * (n - 1))) * 0.5f;
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        for (int i = 0; i < n; ++i) {
            const auto& it = g_shelf[i];
            const RectF tile(startX + i * (tileW + gap), area.Y, tileW, tileH);
            const bool hot = hover == kItem + i || hover == kRemove + i;
            if (hot) {
                GraphicsPath p;
                Rounded(p, tile.X, tile.Y, tile.Width, tile.Height, S(12));
                SolidBrush b(Gray(34, a));
                g.FillPath(&b, &p);
            }
            const REAL icon = std::min(S(38), tileW - S(10));
            RectF box(tile.X + (tileW - icon) * 0.5f, tile.Y + S(5), icon, icon);
            if (it.thumb && it.thumb->bmp) {
                const REAL iw = static_cast<REAL>(it.thumb->bmp->GetWidth()), ih = static_cast<REAL>(it.thumb->bmp->GetHeight());
                const REAL k = std::min(icon / iw, icon / ih);
                DrawFaded(g, it.thumb->bmp.get(), RectF(box.X + (icon - iw * k) * 0.5f, box.Y + (icon - ih * k) * 0.5f, iw * k, ih * k), a);
            } else {
                GraphicsPath p;
                Rounded(p, box.X + icon * 0.15f, box.Y, icon * 0.7f, icon, S(6));
                SolidBrush b(Gray(70, a));
                g.FillPath(&b, &p);
            }
            Text1(g, it.name, g_fonts.tiny.get(), RectF(tile.X + S(3), box.GetBottom() + S(3), tileW - S(6), S(15)), Gray(225, a), StringAlignmentCenter);
            AddHit(hits, tile, kItem + i);
            if (hot) { // видимая кнопка «убрать с полки»
                const REAL r = S(8), cx = tile.GetRight() - S(9), cy = tile.Y + S(9);
                SolidBrush b(Gray(hover == kRemove + i ? 90 : 60, a));
                g.FillEllipse(&b, cx - r, cy - r, 2 * r, 2 * r);
                RPen pen(Gray(240, a), S(1.5f));
                g.DrawLine(&pen, cx - S(3), cy - S(3), cx + S(3), cy + S(3));
                g.DrawLine(&pen, cx + S(3), cy - S(3), cx - S(3), cy + S(3));
                AddHit(hits, RectF(cx - r, cy - r, 2 * r, 2 * r), kRemove + i);
            }
        }
    }

    // ---- «Погода»
    void SkyIcon(Graphics& g, Weather::Sky sky, bool night, REAL cx, REAL cy, REAL r, float a) {
        using Weather::Sky;
        if (sky == Sky::Clear || sky == Sky::PartlyCloudy) {
            const REAL sr = sky == Sky::Clear ? r * 0.62f : r * 0.46f;
            const REAL sx = sky == Sky::Clear ? cx : cx - r * 0.32f, sy = sky == Sky::Clear ? cy : cy - r * 0.3f;
            if (night) {
                SolidBrush moon(Rgb(226, 222, 255, a));
                g.FillEllipse(&moon, sx - sr, sy - sr, 2 * sr, 2 * sr);
                SolidBrush cut(Color(255, 0, 0, 0)); // фон чёрный — вырезаем серп
                g.FillEllipse(&cut, sx - sr * 0.45f, sy - sr * 1.25f, 2 * sr, 2 * sr);
            } else {
                LinearGradientBrush b(PointF(sx - sr, sy - sr), PointF(sx + sr, sy + sr), Rgb(255, 214, 90, a), Rgb(255, 146, 64, a));
                g.FillEllipse(&b, sx - sr, sy - sr, 2 * sr, 2 * sr);
            }
        }
        if (sky != Sky::Clear) {
            const bool dark = sky == Sky::Rain || sky == Sky::Thunder;
            SolidBrush c(Gray(dark ? 165 : 222, a));
            const REAL by = cy + r * 0.05f;
            g.FillEllipse(&c, cx - r * 0.85f, by - r * 0.15f, r * 0.9f, r * 0.62f);
            g.FillEllipse(&c, cx - r * 0.42f, by - r * 0.55f, r * 0.92f, r * 0.92f);
            g.FillEllipse(&c, cx + r * 0.05f, by - r * 0.28f, r * 0.8f, r * 0.75f);
            g.FillRectangle(&c, cx - r * 0.55f, by + r * 0.05f, r * 1.1f, r * 0.42f);
            const REAL ly = by + r * 0.62f;
            if (sky == Sky::Rain || sky == Sky::Drizzle || sky == Sky::Thunder) {
                RPen drop(Rgb(120, 170, 255, a), S(1.6f));
                for (int i = 0; i < 3; ++i) g.DrawLine(&drop, cx - r * 0.4f + i * r * 0.4f, ly, cx - r * 0.5f + i * r * 0.4f, ly + r * 0.3f);
            } else if (sky == Sky::Snow) {
                SolidBrush flake(Gray(250, a));
                for (int i = 0; i < 3; ++i) g.FillEllipse(&flake, cx - r * 0.45f + i * r * 0.4f, ly, r * 0.16f, r * 0.16f);
            } else if (sky == Sky::Fog) {
                RPen fog(Gray(200, a), S(1.6f));
                g.DrawLine(&fog, cx - r * 0.6f, ly + r * 0.05f, cx + r * 0.6f, ly + r * 0.05f);
                g.DrawLine(&fog, cx - r * 0.4f, ly + r * 0.25f, cx + r * 0.5f, ly + r * 0.25f);
            }
            if (sky == Sky::Thunder) {
                SolidBrush bolt(Rgb(255, 210, 80, a));
                PointF z[] = { { cx + r * 0.05f, ly - r * 0.05f }, { cx - r * 0.15f, ly + r * 0.3f }, { cx + r * 0.02f, ly + r * 0.3f }, { cx - r * 0.1f, ly + r * 0.6f }, { cx + r * 0.22f, ly + r * 0.18f }, { cx + r * 0.05f, ly + r * 0.18f } };
                g.FillPolygon(&bolt, z, 6);
            }
        }
    }

    void WeatherPage(Graphics& g, const Weather::Today& w, RectF area, float a) {
        if (!w.ready) { Text1(g, Tw("Погода ещё загружается"), g_fonts.label.get(), area, Gray(200, a), StringAlignmentCenter); return; }
        const REAL mid = area.Y + area.Height * 0.5f;
        SkyIcon(g, w.sky, w.night, area.X + S(20), mid - S(2), S(20), a);
        const std::wstring t = Temp(w.temperature);
        const REAL tx = area.X + S(48), tw = TextWidth(g, t, g_fonts.big.get());
        Text1(g, t, g_fonts.big.get(), RectF(tx, mid - S(22), tw + S(4), S(40)), Gray(250, a));
        const int n = std::max(0, std::min<int>(4, static_cast<int>(w.next.size()) - 1));
        const REAL colW = S(32), right = area.GetRight();
        const REAL ix = tx + tw + S(10), iw = std::max(S(40), right - n * colW - S(8) - ix); // не наезжать на часы справа
        Text1(g, Text::ToWide(w.description), g_fonts.label.get(), RectF(ix, mid - S(22), iw, S(18)), Gray(235, a));
        wchar_t line[96];
        swprintf(line, 96, L"%ls %ls · %ls %ls", Tw("днём").c_str(), Temp(w.high).c_str(), Tw("ночью").c_str(), Temp(w.low).c_str());
        Text1(g, line, g_fonts.tiny.get(), RectF(ix, mid - S(4), iw, S(15)), Gray(165, a));
        Text1(g, Text::ToWide(Lang::IsEnglish() ? Lang::Latin(w.city) : w.city), g_fonts.tiny.get(), RectF(ix, mid + S(10), iw, S(15)), Gray(130, a));
        for (int i = 0; i < n; ++i) {
            const auto& hv = w.next[i + 1];
            const REAL x = right - (n - i) * colW;
            wchar_t hh[8];
            swprintf(hh, 8, L"%02d", hv.hour);
            Text1(g, hh, g_fonts.tiny.get(), RectF(x, mid - S(28), colW, S(14)), Gray(140, a), StringAlignmentCenter);
            SkyIcon(g, hv.sky, hv.night, x + colW * 0.5f, mid - S(2), S(8.5f), a);
            Text1(g, Temp(hv.temperature), g_fonts.tiny.get(), RectF(x, mid + S(12), colW, S(14)), Gray(225, a), StringAlignmentCenter);
        }
    }

    // ---- «Напоминания»
    void TimersPage(Graphics& g, const std::vector<Skills::Countdown>& timers, RectF area, float a, int hover, std::vector<Hit>* hits) {
        if (timers.empty()) {
            Text1(g, Tw("Нет напоминаний и таймеров"), g_fonts.label.get(), RectF(area.X, area.Y + S(8), area.Width, S(22)), Gray(235, a), StringAlignmentCenter);
            Text1(g, Tw("Скажи: «Аня, напомни через 10 минут…»"), g_fonts.tiny.get(), RectF(area.X, area.Y + S(32), area.Width, S(16)), Gray(140, a), StringAlignmentCenter);
            return;
        }
        const Clock now = NowMs();
        const int n = std::min<int>(3, static_cast<int>(timers.size()));
        const REAL rowH = area.Height / 3;
        for (int i = 0; i < n; ++i) {
            const auto& t = timers[i];
            const REAL y = area.Y + rowH * i + (3 - n) * rowH * 0.5f;
            const ULONGLONG left = t.end > now ? t.end - now : 0;
            const REAL xw = S(26); // крестик справа — снять таймер
            Text1(g, t.about.empty() ? Tw("Таймер") : Text::ToWide(t.about), g_fonts.text.get(), RectF(area.X, y, area.Width - S(70) - xw, rowH - S(5)), Gray(235, a));
            Text1(g, SwText(left, false), g_fonts.label.get(), RectF(area.GetRight() - S(70) - xw, y, S(70), rowH - S(5)), Gray(250, a), StringAlignmentFar);
            const RectF xr(area.GetRight() - xw + S(4), y + (rowH - S(22)) * 0.5f - S(2), S(22), S(22));
            { const bool on = hover == kTimerX + i; SolidBrush b(Gray(on ? 90 : 50, a)); g.FillEllipse(&b, xr); Pen pen(Gray(on ? 250 : 190, a), S(1.6f));
              const REAL cx = xr.X + xr.Width / 2, cy = xr.Y + xr.Height / 2, d = S(4); g.DrawLine(&pen, cx - d, cy - d, cx + d, cy + d); g.DrawLine(&pen, cx + d, cy - d, cx - d, cy + d); }
            AddHit(hits, xr, kTimerX + i);
            Bar(g, area.X, area.GetRight() - xw, y + rowH - S(4), t.length ? static_cast<float>(left) / static_cast<float>(t.length) : 0.0f, a, S(3));
        }
    }

    // ---- «Секундомер»
    void StopwatchPage(Graphics& g, RectF area, float a, int hover, std::vector<Hit>* hits) {
        const ULONGLONG ms = g_sw.Elapsed();
        // циферблат: 60 делений (каждое пятое длиннее), дуга текущей минуты цветом логотипа, плавная стрелка
        const REAL dial = std::min(area.Height, S(74)), dr = dial * 0.5f, dcx = area.X + dr, dcy = area.Y + area.Height * 0.5f;
        {
            SolidBrush face(Gray(18, a));
            g.FillEllipse(&face, dcx - dr, dcy - dr, dial, dial);
            for (int i = 0; i < 60; ++i) {
                const float ang = i * 6.0f * 3.14159265f / 180.0f;
                const bool major = i % 5 == 0;
                const REAL r1 = dr - S(3), r0 = r1 - (major ? S(6) : S(3));
                RPen tick(Gray(major ? 120 : 60, a), major ? S(1.6f) : S(1.0f));
                g.DrawLine(&tick, dcx + std::sin(ang) * r0, dcy - std::cos(ang) * r0, dcx + std::sin(ang) * r1, dcy - std::cos(ang) * r1);
            }
            // показанная доля минуты догоняет настоящую: на сбросе стрелка плавно уезжает к нулю, а не прыгает
            static float s_shown = 0;
            static ULONGLONG s_last = GetTickCount64();
            const ULONGLONG nowTick = GetTickCount64();
            const float fdt = std::min(0.05f, (nowTick - s_last) / 1000.0f);
            s_last = nowTick;
            const float target = static_cast<float>(ms % 60000) / 60000.0f;
            if (g_sw.running && target >= s_shown - 0.5f) s_shown = target; // идёт — точно по времени (и переход через минуту)
            else s_shown += (target - s_shown) * (1.0f - std::exp(-fdt * 7.0f));
            const float pulse = g_sw.running ? 1.0f : 0.75f + 0.25f * std::sin(static_cast<float>(nowTick % 100000) / 1000.0f * 2.2f); // на паузе — «дышит»
            const float sweep = s_shown * 360.0f;
            if (sweep > 0.5f) {
                const REAL ar = dr - S(12);
                LinearGradientBrush gb(PointF(dcx - ar, dcy - ar), PointF(dcx + ar, dcy + ar), Grad(0, a * 0.9f * pulse), Grad(1, a * 0.9f * pulse));
                RPen arc(&gb, S(2.6f));
                g.DrawArc(&arc, dcx - ar, dcy - ar, ar * 2, ar * 2, -90.0f, sweep);
            }
            const float hand = s_shown * 2.0f * 3.14159265f;
            const REAL hl = dr - S(9);
            LinearGradientBrush hb(PointF(dcx, dcy), PointF(dcx + std::sin(hand) * hl, dcy - std::cos(hand) * hl), Grad(0, a), Grad(1, a));
            RPen hp(&hb, S(2.2f));
            g.DrawLine(&hp, dcx - std::sin(hand) * S(5), dcy + std::cos(hand) * S(5), dcx + std::sin(hand) * hl, dcy - std::cos(hand) * hl);
            if (g_sw.running) // светящийся кончик стрелки
                for (int k = 3; k >= 1; --k) {
                    SolidBrush glow(Grad(1, a * 0.12f));
                    const REAL gr = S(1.6f) * k + S(1);
                    g.FillEllipse(&glow, dcx + std::sin(hand) * hl - gr, dcy - std::cos(hand) * hl - gr, gr * 2, gr * 2);
                }
            SolidBrush hub(Gray(240, a));
            g.FillEllipse(&hub, dcx - S(2.6f), dcy - S(2.6f), S(5.2f), S(5.2f));
        }
        area.X += dial + S(14);
        area.Width -= dial + S(14);
        Text1(g, SwText(ms, true), g_fonts.big.get(), RectF(area.X, area.Y, area.Width - S(104), area.Height * 0.66f), Gray(g_sw.running ? 250 : 215, a));
        std::wstring laps;
        const int count = static_cast<int>(g_sw.laps.size());
        for (int i = std::max(0, count - 2); i < count; ++i) {
            if (!laps.empty()) laps += L"   ";
            laps += Tw("круг") + L" " + std::to_wstring(i + 1) + L" · " + SwText(g_sw.laps[i], true);
        }
        Text1(g, laps.empty() ? Tw("Круги появятся здесь") : laps, g_fonts.tiny.get(), RectF(area.X, area.Y + area.Height * 0.62f, area.Width - S(104), area.Height * 0.38f), Gray(150, a));
        const REAL bw = S(94), bh = S(19), gap = (area.Height - 3 * bh) / 2, bx = area.GetRight() - bw;
        const struct { int id; const char* ru; bool accent; } buttons[] = {
            { kSwStart, g_sw.running ? "Пауза" : (ms ? "Продолжить" : "Старт"), true }, { kSwLap, "Круг", false }, { kSwReset, "Сброс", false } };
        for (int i = 0; i < 3; ++i) {
            const RectF r(bx, area.Y + i * (bh + gap), bw, bh);
            Pill(g, r, Tw(buttons[i].ru), buttons[i].accent, hover == buttons[i].id, a);
            AddHit(hits, r, buttons[i].id);
        }
    }

    // ---- «Помодоро»
    void PomodoroPage(Graphics& g, RectF area, float a, int hover, std::vector<Hit>* hits) {
        const REAL split = area.X + area.Width * 0.5f;
        wchar_t head[96];
        if (g_pomo.phase == 0) swprintf(head, 96, L"%ls · %d %ls 4", Tw("Фокус").c_str(), g_pomo.session, Tw("из").c_str());
        else swprintf(head, 96, L"%ls", Tw(g_pomo.phase == 1 ? "Короткий перерыв" : "Длинный перерыв").c_str());
        Text1(g, head, g_fonts.tiny.get(), RectF(area.X, area.Y - S(2), split - area.X, S(15)), Gray(150, a));
        const ULONGLONG left = g_pomo.Left(), len = Pomodoro::Len(g_pomo.phase);
        const std::wstring big = SwText(left, false);
        const REAL bigW = TextWidth(g, big, g_fonts.big.get());
        Text1(g, big, g_fonts.big.get(), RectF(area.X - S(2), area.Y + S(10), bigW + S(4), S(36)), Gray(250, a));
        const char* status = g_pomo.running ? "идёт" : left == len ? "готов" : "пауза";
        Text1(g, Tw(status), g_fonts.tiny.get(), RectF(area.X + bigW + S(8), area.Y + S(24), split - area.X - bigW - S(12), S(16)), g_pomo.running ? Grad(0.4f, a) : Gray(150, a)); // мелко, у нижнего края цифр
        Bar(g, area.X, split - S(14), area.GetBottom() - S(4), 1.0f - static_cast<float>(left) / static_cast<float>(len), a, S(3));
        // правая половина
        Pen sep(Gray(40, a), S(1));
        g.DrawLine(&sep, split, area.Y, split, area.GetBottom());
        const REAL rx = split + S(14), rw = area.GetRight() - rx;
        const RectF start(area.GetRight() - S(28) - S(84), area.Y, S(84), S(21));
        Pill(g, start, Tw(g_pomo.running ? "Пауза" : (left == len ? "Старт" : "Продолжить")), true, hover == kPomoStart, a);
        AddHit(hits, start, kPomoStart);
        const RectF reset(area.GetRight() - S(22), area.Y, S(21), S(21));
        SolidBrush rb(Gray(hover == kPomoReset ? 70 : 44, a));
        g.FillEllipse(&rb, reset);
        { // стрелка «сначала»
            RPen pen(Gray(235, a), S(1.5f));
            const REAL c = reset.X + reset.Width * 0.5f, m = reset.Y + reset.Height * 0.5f, r = S(5);
            g.DrawArc(&pen, c - r, m - r, 2 * r, 2 * r, -60, 290);
            g.DrawLine(&pen, c + r * 0.5f, m - r * 0.87f, c + r * 0.5f + S(3), m - r * 0.87f - S(1));
        }
        AddHit(hits, reset, kPomoReset);
        // 4 фокуса и длинный перерыв
        const REAL dy = area.Y + S(38), segW = (rw - S(4) * 4) / 5;
        for (int i = 0; i < 5; ++i) {
            const REAL x = rx + i * (segW + S(4));
            const bool done = i < g_pomo.session - 1 || (i == 4 && g_pomo.phase == 2);
            const bool cur = (i == g_pomo.session - 1 && g_pomo.phase != 2) || (i == 4 && g_pomo.phase == 2);
            RPen pen(done ? Grad(i / 4.0f, a) : cur ? Gray(150, a) : Gray(55, a), S(5));
            g.DrawLine(&pen, x + S(3), dy, x + segW - S(3), dy);
            Text1(g, i < 4 ? std::to_wstring(i + 1) : Tw("отдых"), g_fonts.tiny.get(), RectF(x - S(6), dy + S(6), segW + S(12), S(15)), Gray(cur ? 230 : 130, a), StringAlignmentCenter); // «длинный» не влезал — обрезался в «длин…»
        }
    }

    // ---- «Вода»
    void WaterPage(Graphics& g, RectF area, float a, int hover, std::vector<Hit>* hits) {
        const float k = static_cast<float>(g_water.ml) / g_water.goal;
        wchar_t big[32], goal[32], line[96];
        swprintf(big, 32, L"%.2f", g_water.ml / 1000.0);
        swprintf(goal, 32, L"/ %.1f %ls", g_water.goal / 1000.0, Tw("л").c_str());
        const REAL bigW = TextWidth(g, big, g_fonts.big.get());
        Text1(g, big, g_fonts.big.get(), RectF(area.X - S(2), area.Y - S(4), bigW + S(4), S(36)), Gray(250, a));
        Text1(g, goal, g_fonts.tiny.get(), RectF(area.X + bigW + S(2), area.Y + S(8), S(80), S(18)), Gray(150, a));
        const REAL leftW = area.Width - S(80);
        swprintf(line, 96, L"%ls · %d%%", Tw("Сегодня").c_str(), static_cast<int>(std::lround(k * 100)));
        Text1(g, line, g_fonts.tiny.get(), RectF(area.X, area.Y + S(28), leftW, S(14)), Gray(150, a));
        Bar(g, area.X, area.X + leftW - S(10), area.Y + S(44), k, a, S(3.5f));
        const RectF drink(area.X, area.GetBottom() - S(19), S(86), S(19));
        Pill(g, drink, Tw("+ стакан"), true, hover == kDrink, a);
        AddHit(hits, drink, kDrink);
        const RectF undo(drink.GetRight() + S(6), drink.Y, S(70), S(19));
        Pill(g, undo, Tw("отменить"), false, hover == kUndo, a);
        AddHit(hits, undo, kUndo);
        const RectF bell(undo.GetRight() + S(6), drink.Y, S(104), S(19));
        Pill(g, bell, Tw(g_water.bell ? "напоминать: да" : "напоминать: нет"), false, hover == kBell, a);
        AddHit(hits, bell, kBell);
        // кольцо справа
        const REAL r = std::min(area.Height * 0.5f - S(4), S(28)), cx = area.GetRight() - r - S(4), cy = area.Y + area.Height * 0.5f;
        Pen rail(Gray(45, a), S(5));
        g.DrawEllipse(&rail, cx - r, cy - r, 2 * r, 2 * r);
        if (k > 0.005f) {
            LinearGradientBrush b(PointF(cx - r, cy - r), PointF(cx + r, cy + r), Rgb(96, 165, 250, a), Grad(0, a));
            RPen arc(&b, S(5));
            g.DrawArc(&arc, cx - r, cy - r, 2 * r, 2 * r, -90, 360 * Clamp01(k));
        }
        Text1(g, std::to_wstring(g_water.cups), g_fonts.title.get(), RectF(cx - r, cy - r * 0.6f, 2 * r, r), Gray(250, a), StringAlignmentCenter);
        DropShape(g, cx, cy + r * 0.42f, S(4.5f), Rgb(96, 165, 250, a), true);
    }

    // ---- настройки островка прямо в нём
    struct Chip { const char* ru; const char* key; const char* def; };
    const Chip kChips[] = {
        { "Музыка", "notch_music", "1" }, { "Удаление", "notch_shelf", "1" }, { "Погода", "notch_weather", "1" }, { "Напоминания", "notch_timers", "1" },
        { "Секундомер", "notch_stopwatch", "1" }, { "Фокусировка", "notch_pomodoro", "1" },
        { "Текст песен", "notch_lyrics", "0" }, { "Раскладка", "notch_layout", "1" }, { "Зарядка", "notch_power", "1" },
        { "Волна музыки", "notch_wave", "1" }, { "Крупнее", "notch_size", "0" },
    };

    void ConfigPage(Graphics& g, RectF area, float a, int hover, std::vector<Hit>* hits) {
        const int n = static_cast<int>(std::size(kChips)), cols = 3, rows = (n + cols - 1) / cols;
        const REAL cellW = area.Width / cols, rowH = std::min(S(18), area.Height / rows);
        for (int i = 0; i < n; ++i) {
            const REAL x = area.X + (i % cols) * cellW, y = area.Y + (i / cols) * rowH;
            const RectF cell(x, y, cellW - S(8), rowH);
            const bool on = Opt(kChips[i].key, kChips[i].def), hot = hover == kChip + i;
            if (hot) { GraphicsPath hp; Rounded(hp, cell.X - S(4), cell.Y, cell.Width + S(6), cell.Height, S(6)); SolidBrush hb(Gray(26, a)); g.FillPath(&hb, &hp); }
            // переключатель: дорожка и шарик
            const REAL tw = S(20), th = S(11), tx = cell.X, ty = y + (rowH - th) * 0.5f;
            GraphicsPath track;
            Rounded(track, tx, ty, tw, th, th * 0.5f);
            if (on) { LinearGradientBrush b(PointF(tx, 0), PointF(tx + tw, 0), Grad(0, a), Grad(1, a)); g.FillPath(&b, &track); }
            else { Pen p(Gray(110, a), S(1)); g.DrawPath(&p, &track); }
            const REAL kr = th * 0.32f, kx = on ? tx + tw - th * 0.5f : tx + th * 0.5f;
            SolidBrush knob(on ? Rgb(255, 255, 255, a) : Gray(150, a));
            g.FillEllipse(&knob, kx - kr, ty + th * 0.5f - kr, kr * 2, kr * 2);
            Text1(g, Tw(kChips[i].ru), g_fonts.tiny.get(), RectF(tx + tw + S(7), y, cell.Width - tw - S(7), rowH), Gray(on ? 235 : 150, a));
            AddHit(hits, cell, kChip + i);
        }
    }

    // ---- нижняя панель модулей
    void TabBar(Graphics& g, REAL cx, REAL x0, REAL h, float a, int hover, std::vector<Hit>* hits, bool lyricsButton) {
        (void)x0;
        std::vector<int> pages;
        for (int p = 0; p < kPageCount; ++p) if (g_pageOn[p]) pages.push_back(p);
        const bool lyr = lyricsButton && g_page == kMusic && g_optLyrics;
        const REAL step = S(28), cy = h - S(kTabH) * 0.5f - S(1), pad = S(6), sepW = S(10);
        const int count = static_cast<int>(pages.size()) + 1 + (lyr ? 1 : 0);
        const REAL barW = pad * 2 + step * count + sepW * (lyr ? 2 : 1), bx = cx - barW * 0.5f;
        GraphicsPath p;
        Rounded(p, bx, cy - S(14), barW, S(28), S(14));
        SolidBrush bg(Gray(22, a));
        g.FillPath(&bg, &p);
        auto item = [&](int id, int icon, bool cur, REAL x) {
            if (cur) { // текущая — кружок любимого цвета с переходом
                LinearGradientBrush on(PointF(x - S(11), cy - S(11)), PointF(x + S(11), cy + S(11)), Grad(0, a), Grad(1, a));
                g.FillEllipse(&on, x - S(11), cy - S(11), S(22), S(22));
            } else if (hover == id) { SolidBrush hb(Gray(38, a)); g.FillEllipse(&hb, x - S(11), cy - S(11), S(22), S(22)); }
            TabIcon(g, icon, x, cy, S(6.5f), cur ? Gray(255, a) : Gray(hover == id ? 225 : 150, a));
            AddHit(hits, RectF(x - step * 0.5f, cy - S(14), step, S(28)), id);
        };
        auto separator = [&](REAL x) { Pen sep(Gray(56, a), S(1)); g.DrawLine(&sep, x, cy - S(7), x, cy + S(7)); };
        REAL x = bx + pad + step * 0.5f;
        if (lyr) { // «слова песни» — первым в ряду, кавычка
            if (g_lyricsOpen) { SolidBrush on(Gray(52, a)); g.FillEllipse(&on, x - S(11), cy - S(11), S(22), S(22)); }
            else if (hover == kLyrics) { SolidBrush hb(Gray(38, a)); g.FillEllipse(&hb, x - S(11), cy - S(11), S(22), S(22)); }
            Text1(g, L"”", g_fonts.label.get(), RectF(x - S(10), cy - S(9), S(20), S(20)), g_lyricsOpen ? Gray(255, a) : Gray(hover == kLyrics ? 225 : 150, a), StringAlignmentCenter);
            AddHit(hits, RectF(x - step * 0.5f, cy - S(14), step, S(28)), kLyrics);
            separator(x + step * 0.5f + sepW * 0.5f);
            x += step + sepW;
        }
        for (int page : pages) { item(kTab + page, page, g_page == page, x); x += step; }
        separator(x - step * 0.5f + sepW * 0.5f);
        x += sepW;
        item(kGear, -1, g_page == kConfig, x);
    }

    void ToastIconDraw(Graphics& g, const Toast& t, REAL cx, REAL cy, float a) {
        switch (t.icon) {
        case kIconLayout: {
            GraphicsPath p;
            Rounded(p, cx - S(14), cy - S(9), S(28), S(18), S(6));
            LinearGradientBrush b(PointF(cx - S(14), 0), PointF(cx + S(14), 0), Grad(0, a), Grad(1, a));
            g.FillPath(&b, &p);
            Text1(g, t.badge, g_fonts.tiny.get(), RectF(cx - S(14), cy - S(9), S(28), S(18)), Gray(255, a), StringAlignmentCenter);
            break;
        }
        case kIconTomato: {
            SolidBrush red(Rgb(244, 88, 88, a));
            g.FillEllipse(&red, cx - S(7), cy - S(5), S(14), S(13));
            RPen leaf(Rgb(110, 210, 120, a), S(2));
            g.DrawLine(&leaf, cx, cy - S(5), cx + S(4), cy - S(8));
            break;
        }
        case kIconDrop: DropShape(g, cx, cy, S(8), Rgb(96, 165, 250, a), true); break;
        case kIconPower: {
            SolidBrush y(Rgb(255, 210, 80, a));
            PointF z[] = { { cx + S(2), cy - S(9) }, { cx - S(5), cy + S(1) }, { cx, cy + S(1) }, { cx - S(2), cy + S(9) }, { cx + S(5), cy - S(1) }, { cx, cy - S(1) } };
            g.FillPolygon(&y, z, 6);
            break;
        }
        default: TabIcon(g, kShelf, cx, cy, S(8), Grad(0.5f, a)); break;
        }
    }

    REAL ToastWidth(const Toast& t) {
        g_fonts.Ensure();
        static Bitmap probe(1, 1, PixelFormat32bppPARGB);
        Graphics g(&probe);
        return std::clamp(TextWidth(g, t.text, g_fonts.label.get()) + S(74), S(170), S(380));
    }

    void Draw(Surface& s, const View& v, int hover, bool dragOver, std::vector<Hit>* hits) {
        if (hits) hits->clear();
        Bitmap bitmap(s.width, s.height, s.width * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(s.bits));
        Graphics g(&bitmap);
        g.Clear(Color(0, 0, 0, 0));
        if (v.h < 0.5f || v.w < 2) return;
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g_fonts.Ensure();
        RefreshColors();

        const REAL w = v.w, h = v.h;
        const REAL cx = g_edge == 2 ? w * 0.5f : g_edge == 3 ? s.width - w * 0.5f : s.width * 0.5f;
        const REAL x0 = cx - w * 0.5f, x1 = cx + w * 0.5f, mid = h * 0.5f;
        g_offY = g_edge == 1 ? s.height - h : Side() ? std::round((s.height - h) * 0.5f) : 0.0f;
        g.TranslateTransform(0, g_offY);
        const bool voice = v.mode == Mode::Listening || v.mode == Mode::Thinking || v.mode == Mode::Speaking;
        const bool panel = v.mode == Mode::Panel;
        // содержимое проявляется, когда плашка уже выдвинулась
        const float k = panel ? Clamp01((h - S(80)) / S(45)) * Clamp01((w - S(260)) / S(140)) : Clamp01((h - S(26)) / S(14));
        GraphicsPath path;
        EdgeShape(path, cx, w, h);

        if ((voice || panel || (v.mode == Mode::Toast && h > S(34))) && h > S(20)) { // у всплывашки свечение — только когда она раскрылась, иначе виден один цветной комок // мягкое свечение снизу, с переходом цвета
            const float pulse = v.mode == Mode::Listening ? 0.6f + 0.4f * v.levels.back() : v.mode == Mode::Speaking ? 0.6f + 0.4f * v.mouth
                : panel ? 0.45f : 0.55f + 0.25f * std::sin(v.t * 3.0f);
            const float base = Clamp01((h - S(20)) / S(20));
            for (int i = 7; i >= 1; --i) {
                const float a = base * pulse * 0.075f * (8 - i) / 7.0f;
                LinearGradientBrush b(PointF(x0 - 1, 0), PointF(x1 + 1, 0), Grad(0, a), Grad(1, a));
                Pen pen(&b, S(2.4f) * i);
                pen.SetLineJoin(LineJoinRound);
                g.DrawPath(&pen, &path);
            }
        }
        SolidBrush black(Gray(0, 1.0f)); // цвет островка — фон приложения
        g.FillPath(&black, &path);
        { // живая кромка: тонкая линия, цвет медленно перетекает по кругу (любимый цвет ↔ розовый); в покое — едва заметна
            const float open = Clamp01((h - S(44)) / S(40));
            const float rim = 0.22f + 0.5f * open;
            const REAL ang = v.t * 0.6f, r = std::max(w, h);
            const PointF c0(cx + std::cos(ang) * r, h * 0.5f + std::sin(ang) * r), c1(cx - std::cos(ang) * r, h * 0.5f - std::sin(ang) * r);
            LinearGradientBrush rb(c0, c1, Grad(0, rim), Grad(1, rim * 0.6f));
            Pen edge(&rb, S(1.3f));
            edge.SetLineJoin(LineJoinRound);
            g.DrawPath(&edge, &path);
        }
        if (voice) AddHit(hits, RectF(x0, 0, w, h), kVoice);

        if (v.mode == Mode::Idle || (v.mode == Mode::Hidden && h > S(12))) { // маленькая чёлка: часы (или секундомер / помодоро) и волна музыки
            const float a = Clamp01((h - S(20)) / S(12));
            if (Side()) { // вертикальная таблетка у бока: мордочка, ниже часы «12 / 04»
                const float sa = Clamp01((w - S(20)) / S(12));
                if (v.eyes) Eyes(g, v, cx, S(30), sa);
                wchar_t hh[4], mm[4];
                swprintf(hh, 4, L"%02d", v.local.wHour);
                swprintf(mm, 4, L"%02d", v.local.wMinute);
                const REAL top = v.eyes ? S(48) : h * 0.5f - S(24);
                Text1(g, hh, g_fonts.idle.get(), RectF(x0, top, w, S(24)), Gray(240, sa), StringAlignmentCenter);
                Text1(g, mm, g_fonts.idle.get(), RectF(x0, top + S(22), w, S(24)), Gray(170, sa), StringAlignmentCenter);
                return;
            }
            // время всегда ровно по центру; точка секундомера слева от него, волна — у правого края
            wchar_t clock[16];
            swprintf(clock, 16, L"%02d:%02d", v.local.wHour, v.local.wMinute);
            if (g_sw.running || g_pomo.running) {
                const REAL r = S(4.5f);
                SolidBrush dot(g_pomo.running && !g_sw.running ? Rgb(244, 88, 88, a) : Grad(0.5f, a));
                const std::wstring t = SwText(g_sw.running ? g_sw.Elapsed() : g_pomo.Left(), false);
                const REAL tw = TextWidth(g, t, g_fonts.idle.get());
                g.FillEllipse(&dot, cx - tw * 0.5f - S(8) - r, h * 0.5f - r, 2 * r, 2 * r);
                Text1(g, t, g_fonts.idle.get(), RectF(x0, 0, w, h - S(1)), Gray(240, a), StringAlignmentCenter);
            } else Text1(g, clock, g_fonts.idle.get(), RectF(x0, 0, w, h - S(1)), Gray(240, a), StringAlignmentCenter);
            if (v.eyes && a > 0) Eyes(g, v, x0 + S(30), h * 0.5f, a);
            if (v.wave && a > 0) {
                for (int i = 0; i < 4; ++i) {
                    // по нотам: каждый столбик — своя полоса частот (слева бас, справа высокие); звук ещё не слушается — мягкая заглушка
                    const float lv = v.bandsLive ? 0.16f + 0.84f * v.bands[i] : 0.3f + 0.25f * (0.5f + 0.5f * std::sin(v.t * (3.0f + i * 0.7f) + i * 1.9f));
                    const REAL bh = S(17) * lv, bx = x1 - S(17) - (3 - i) * S(8);
                    RPen pen(Grad(i / 3.0f, a), S(3.4f));
                    g.DrawLine(&pen, bx, h * 0.5f - bh * 0.5f, bx, h * 0.5f + bh * 0.5f);
                }
            }
            return;
        }
        if (v.mode == Mode::Toast) {
            const float a = Clamp01((h - S(30)) / S(12));
            ToastIconDraw(g, g_toast, x0 + S(30), mid, a);
            Text1(g, g_toast.text, g_fonts.label.get(), RectF(x0 + S(52), 0, w - S(66), h), Gray(245, a));
            return;
        }
        if (k <= 0) return;

        if (panel) {
            // шапка: название страницы слева, справа — время и короткая дата (без обрезанного «Воскресен…»)
            const float pa = k * Ease(g_pageT);
            const REAL hy = S(10), hh = S(22), left = x0 + S(22), rightEdge = x1 - S(22);
            static const char* kTitles[kPageCount + 1] = { "Музыка", "Удаление", "Погода", "Напоминания", "Секундомер", "Фокусировка", "Вода", "Островок" };
            const std::wstring title = Tw(kTitles[g_page]);
            const REAL titleEnd = left + TextWidth(g, title, g_fonts.label.get()) + S(10);
            Text1(g, title, g_fonts.label.get(), RectF(left, hy, S(150), hh), Gray(240, k));
            const ULONGLONG now = GetTickCount64();
            REAL right = rightEdge;
            if (false && g_page == kShelf && !g_shelf.empty()) {
                const std::wstring clear = Tw("Очистить");
                const REAL cw = TextWidth(g, clear, g_fonts.tiny.get());
                const RectF r(rightEdge - cw, hy, cw, hh);
                Text1(g, clear, g_fonts.tiny.get(), r, Gray(hover == kClear ? 235 : 140, k), StringAlignmentFar);
                AddHit(hits, r, kClear);
                right = r.X - S(14);
            }
            if (!g_flash.empty() && now < g_flashUntil) {
                Text1(g, g_flash, g_fonts.tiny.get(), RectF(titleEnd, hy, right - titleEnd, hh), Grad(0.4f, k), StringAlignmentFar);
            } else {
                const wchar_t* locale = Lang::IsEnglish() ? L"en-US" : L"ru-RU";
                wchar_t date[48] = {}, clock[16];
                GetDateFormatEx(locale, 0, &v.local, L"ddd, d MMM", date, 48, nullptr); // «вс, 15 нояб.»
                swprintf(clock, 16, L"%02d:%02d", v.local.wHour, v.local.wMinute);
                const REAL cw = TextWidth(g, clock, g_fonts.label.get());
                Text1(g, clock, g_fonts.label.get(), RectF(right - cw, hy, cw + S(2), hh), Gray(250, k), StringAlignmentFar);
                Text1(g, date, g_fonts.tiny.get(), RectF(titleEnd, hy, std::max(S(1), right - cw - S(8) - titleEnd), hh), Gray(150, k), StringAlignmentFar);
            }

            const REAL contentH = S(kPanelH) - S(40) - S(kTabH);
            // стрелок по бокам больше нет: страницы листают значки снизу и колесо мыши — места для содержимого больше

            const REAL slide = (1.0f - Ease(g_pageT)) * S(18) * g_pageDir;
            const bool lyrics = g_page == kMusic && g_lyricsOpen && g_optLyrics;
            const REAL mainW = lyrics ? std::min(w - S(44), S(kPanelW - kLyricsNarrow) - S(44)) : w - S(44); // со словами плеер сужается, слова — справа
            const RectF area(x0 + S(22) + slide, S(40), mainW, contentH);
            switch (g_page) {
            case kMusic:
                MusicPage(g, v, area, pa, hover, hits);
                if (lyrics) {
                    const REAL lx0 = area.GetRight() + S(22), lx1 = x1 - S(22), lw = lx1 - lx0;
                    if (lw > S(40)) {
                        const float la = pa * Clamp01((lw - S(60)) / S(120));
                        Pen sep(Gray(38, la), S(1));
                        g.DrawLine(&sep, lx0 - S(11), area.Y + S(4), lx0 - S(11), area.GetBottom() - S(4));
                        { // строки не залезают на шапку — обрезаем по области под ней
                            const RectF box(lx0, area.Y + S(2), lw, area.Height - S(2));
                            g.SetClip(box);
                            LyricsBlock(g, v, box, la);
                            g.ResetClip();
                        }
                    }
                }
                break;
            case kShelf: ShelfPage(g, area, pa, hover, dragOver, hits); break;
            case kWeather: WeatherPage(g, v.weather, area, pa); break;
            case kTimers: TimersPage(g, v.timers, area, pa, hover, hits); break;
            case kStopwatch: StopwatchPage(g, area, pa, hover, hits); break;
            case kPomodoro: PomodoroPage(g, area, pa, hover, hits); break;
            case kWater: WaterPage(g, area, pa, hover, hits); break;
            case kConfig: ConfigPage(g, area, pa, hover, hits); break;
            default: break;
            }
            TabBar(g, cx, x0, h, k, hover, hits, true); // значок слов песни — всегда на месте
            return;
        }

        if (v.mode == Mode::Listening) {
            // точка «микрофон» и волна из настоящей громкости микрофона
            const float level = v.levels.back();
            const REAL mr = S(6) * (1.0f + 0.35f * level);
            SolidBrush dot(Grad(0.2f, k));
            g.FillEllipse(&dot, x0 + S(24) - mr, mid - mr, 2 * mr, 2 * mr);
            const REAL left = x0 + S(46), right = x1 - S(22);
            const int n = static_cast<int>(v.levels.size());
            const REAL step = (right - left) / (n - 1);
            for (int i = 0; i < n; ++i) {
                const float lv = std::sqrt(Clamp01(v.levels[i] * 2.5f));
                const float wobble = 0.85f + 0.15f * std::sin(v.t * 9.0f + i * 0.6f);
                const REAL a = S(1.5f) + (mid - S(10)) * lv * wobble;
                RPen pen(Grad(static_cast<float>(i) / (n - 1), k), S(3));
                const REAL x = left + step * i;
                g.DrawLine(&pen, x, mid - a, x, mid + a);
            }
        } else if (v.mode == Mode::Thinking) {
            for (int i = 0; i < 3; ++i) { // три «дышащие» точки
                const float p = 0.5f + 0.5f * std::sin(v.t * 6.0f - i * 0.9f);
                const REAL r = S(3.5f) + S(1.5f) * p;
                SolidBrush dot(Grad(i * 0.5f, k * (0.4f + 0.6f * p)));
                const REAL dx = x0 + S(28) + i * S(15);
                g.FillEllipse(&dot, dx - r, mid - r, 2 * r, 2 * r);
            }
            Text1(g, Tw("Думаю…"), g_fonts.label.get(), RectF(x0 + S(80), 0, x1 - x0 - S(96), h), Gray(190, k * 0.85f));
        } else if (v.mode == Mode::Speaking) {
            for (int i = 0; i < 4; ++i) { // маленький эквалайзер от «рта» голоса
                const float lv = Clamp01(v.mouth * (0.7f + 0.3f * std::sin(v.t * 11.0f + i * 1.7f)) + 0.12f);
                const REAL a = S(2) + S(10) * lv;
                RPen pen(Grad(i / 3.0f, k), S(3));
                const REAL x = x0 + S(22) + i * S(7);
                g.DrawLine(&pen, x, mid - a, x, mid + a);
            }
            Text1(g, v.text, g_fonts.text.get(), RectF(x0 + S(58), S(8), x1 - x0 - S(78), h - S(16)), Gray(245, k * 0.92f), StringAlignmentNear, true);
        }
    }

    // Высота плашки под текст ответа: от одной до трёх строк
    float SpeakingHeight(const std::wstring& text) {
        if (text.empty()) return S(48);
        g_fonts.Ensure();
        static Bitmap probe(1, 1, PixelFormat32bppPARGB);
        Graphics g(&probe);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        RectF box;
        g.MeasureString(text.c_str(), -1, g_fonts.text.get(), RectF(0, 0, S(460 - 78), S(1000)), nullptr, &box);
        const float line = g_fonts.text->GetHeight(&g);
        return std::clamp(box.Height + S(18), S(48), line * 3 + S(18));
    }

    void SavePng(Surface& s, const std::string& path) {
        CLSID png;
        CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &png);
        Bitmap bitmap(s.width, s.height, s.width * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(s.bits));
        bitmap.Save(Text::ToWide(path).c_str(), &png);
    }

    void SaveDump(const std::string& folder) {
        Surface s;
        s.Resize(static_cast<int>(S(kWinW)), static_cast<int>(S(kWinH)));
        View v;
        v.t = 1.3f;
        GetLocalTime(&v.local);
        for (size_t i = 0; i < v.levels.size(); ++i) v.levels[i] = 0.08f + 0.25f * (0.5f + 0.5f * std::sin(i * 0.45f));
        v.mouth = 0.6f;
        v.text = Tw("Сейчас в Ташкенте +14 и ясно. Вечером похолодает до +6, зонт не понадобится.");
        v.track = MediaNow::Snapshot();
        if (!v.track.present) { v.track.present = true; v.track.playing = true; v.track.title = "Пример песни"; v.track.artist = "Исполнитель"; v.track.position = 74; v.track.duration = 203; }
        v.position = v.track.position;
        const auto shot = [&](Mode mode, float w, float h, int page, bool drag, int hover, const char* name) {
            const int savedPage = g_page;
            g_page = page;
            g_pageT = 1;
            v.mode = mode; v.w = S(w); v.h = h;
            Draw(s, v, hover, drag, nullptr);
            SavePng(s, folder + "\\notch_" + name + ".png");
            g_page = savedPage;
        };
        shot(Mode::Idle, kIdleW, S(kIdleH), 0, false, 0, "idle");
        v.eyes = true; v.lookX = 0.6f;
        shot(Mode::Idle, kIdleW, S(kIdleH), 0, false, 0, "idle_eyes");
        v.nearK = 1;
        shot(Mode::Idle, kIdleW, S(kIdleH), 0, false, 0, "idle_eyes_happy");
        v.nearK = 0; v.lookX = -0.8f; v.lookY = 0.6f;
        shot(Mode::Idle, kIdleW, S(kIdleH), 0, false, 0, "idle_eyes_look");
        g_edge = 2;
        shot(Mode::Idle, kSideIdleW, S(kSideIdleH), 0, false, 0, "idle_side");
        shot(Mode::Panel, kPanelW, S(kPanelH), kMusic, false, 0, "music_side");
        g_edge = 0;
        v.eyes = false; v.lookX = v.lookY = 0;
        v.wave = true;
        shot(Mode::Idle, kIdleW + kWaveW, S(kIdleH), 0, false, 0, "idle_wave");
        v.wave = false;
        shot(Mode::Listening, 320, S(48), 0, false, 0, "listening");
        shot(Mode::Thinking, 236, S(48), 0, false, 0, "thinking");
        shot(Mode::Speaking, 460, SpeakingHeight(v.text), 0, false, 0, "speaking");
        const Toast savedToast = g_toast;
        g_toast = { L"Русский", L"RU", kIconLayout, 0 };
        shot(Mode::Toast, ToastWidth(g_toast) / S(1), S(48), 0, false, 0, "toast_layout");
        g_toast = savedToast;
        shot(Mode::Panel, kPanelW, S(kPanelH), kMusic, false, kPlay, "music");
        {
            std::lock_guard lock(g_lyrics.mutex);
            g_lyrics.state = 2;
            g_lyrics.lines = { { 0, L"Первая строчка песни" }, { 4, L"Вторая строчка" }, { 8, L"Сейчас звучит вот эта строчка" }, { 1e6, L"А потом будет эта" }, { 2e6, L"И ещё одна" } };
        }
        const bool savedOpen = g_lyricsOpen, savedOpt = g_optLyrics;
        g_lyricsOpen = g_optLyrics = true;
        { std::lock_guard lock(g_lyrics.mutex); g_lyricScroll = static_cast<float>(LyricIndex(v.position)) - 0.35f; } // посередине перехода
        shot(Mode::Panel, kPanelW + kLyricsW - kLyricsNarrow, S(kPanelH), kMusic, false, 0, "music_lyrics");
        g_lyricsOpen = savedOpen;
        g_optLyrics = savedOpt;
        {
            std::lock_guard lock(g_lyrics.mutex);
            g_lyrics.key.clear();
            g_lyrics.state = 0;
            g_lyrics.lines.clear();
        }
        std::vector<ShelfItem> saved;
        saved.swap(g_shelf);
        shot(Mode::Panel, kPanelW, S(kPanelH), kShelf, false, 0, "shelf_empty");
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        AddToShelf(exe);
        AddToShelf(Text::ToWide(folder + "\\notch_music.png"));
        AddToShelf(Text::ToWide(folder));
        for (auto& it : g_shelf) it.thumb = LoadThumb(it.path, static_cast<int>(S(48)));
        shot(Mode::Panel, kPanelW, S(kPanelH), kShelf, false, kItem + 1, "shelf_files");
        g_shelf.swap(saved);
        v.weather = Weather::Snapshot();
        shot(Mode::Panel, kPanelW, S(kPanelH), kWeather, false, 0, "weather");
        v.timers = Skills::Countdowns();
        if (v.timers.empty()) v.timers = { { "выключить чайник", NowMs() + 252000, 600000 }, { "", NowMs() + 1500000, 1800000 } };
        shot(Mode::Panel, kPanelW, S(kPanelH), kTimers, false, 0, "timers");
        const Stopwatch savedSw = g_sw;
        g_sw.running = false; g_sw.total = 83400; g_sw.laps = { 41300, 83400 };
        shot(Mode::Panel, kPanelW, S(kPanelH), kStopwatch, false, kSwStart, "stopwatch");
        g_sw = savedSw;
        const Pomodoro savedPomo = g_pomo;
        g_pomo.session = 2; g_pomo.left = 14 * 60000 + 12000;
        shot(Mode::Panel, kPanelW, S(kPanelH), kPomodoro, false, 0, "pomodoro");
        g_pomo = savedPomo;
        const Water savedWater = g_water;
        g_water.ml = 750; g_water.cups = 3;
        shot(Mode::Panel, kPanelW, S(kPanelH), kWater, false, 0, "water");
        g_water = savedWater;
        shot(Mode::Panel, kPanelW, S(kPanelH), kConfig, false, 0, "config");
    }

    // ---------------------------------------------------------------- окно и цикл
    std::atomic<bool> g_optionsDirty{ true };
}
void Notch::Reload() { g_optionsDirty = true; } // настройки поменялись — островок перечитает их в следующем кадре
namespace {

    void Activate(int id) {
        if (id == kVoice) { Brain::ToggleListening(); return; } // как кнопка «говорить»: перебить или слушать
        if (id >= kPrev && id <= kNext) {
            MediaNow::Control(id == kPrev ? MediaNow::Action::Previous : id == kPlay ? MediaNow::Action::PlayPause : MediaNow::Action::Next);
            return;
        }
        if (id == kArrowL || id == kArrowR) { Turn(id == kArrowL ? -1 : 1); return; }
        if (id >= kTab && id < kTab + kPageCount) { SetPage(id - kTab); return; }
        if (id == kGear) { IslandSettings::Open(); return; } // все настройки островка — в отдельном окне
        if (id == kLyrics) {
            if (!g_optLyrics) { // включаем по нажатию: сама попросила
                Config::SetValue("overlay", "notch_lyrics", "1");
                g_optLyrics = true;
                g_lyricsOpen = true;
                Flash("Слова песен берутся с lrclib.net");
            } else g_lyricsOpen = !(g_lyricsOpen && g_page == kMusic);
            SetPage(kMusic);
            return;
        }
        if (id >= kChip && id < kChip + static_cast<int>(std::size(kChips))) {
            const Chip& c = kChips[id - kChip];
            Config::SetValue("overlay", c.key, Opt(c.key, c.def) ? "0" : "1");
            g_optionsDirty = true;
            return;
        }
        if (id == kSwStart) {
            if (g_sw.running) { g_sw.total = g_sw.Elapsed(); g_sw.running = false; }
            else { g_sw.since = GetTickCount64(); g_sw.running = true; }
            return;
        }
        if (id == kSwLap) { if (g_sw.Elapsed()) g_sw.laps.push_back(g_sw.Elapsed()); return; }
        if (id == kSwReset) { g_sw = {}; return; }
        if (id == kPomoStart) { g_pomo.Toggle(); return; }
        if (id == kPomoReset) { g_pomo.Reset(); return; }
        if (id == kDrink) { g_water.Drink(250); return; }
        if (id == kUndo) { g_water.Drink(-250); return; }
        if (id == kBell) { g_water.bell = !g_water.bell; Config::SetValue("overlay", "water_bell", g_water.bell ? "1" : "0"); return; }
        if (id == kClear) { g_shelf.clear(); SaveShelf(); return; }
        if (id >= kTimerX && id < kTimerX + 3) { // крестик у таймера
            const auto list = Skills::Countdowns();
            const size_t i = id - kTimerX;
            if (i < list.size()) { Skills::CancelCountdown(list[i].end); Flash("Таймер выключен"); }
            return;
        }
        if (id >= kRemove && id < kRemove + kShelfMax) {
            const size_t i = id - kRemove;
            if (i < g_shelf.size()) { g_shelf.erase(g_shelf.begin() + i); SaveShelf(); }
            return;
        }
        if (id >= kItem && id < kItem + kShelfMax) {
            const size_t i = id - kItem;
            if (i < g_shelf.size()) ShellExecuteW(nullptr, L"open", g_shelf[i].path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    void CopyItem(size_t i) {
        if (i >= g_shelf.size()) return;
        if (IDataObject* data = FileData(g_shelf[i].path)) {
            if (SUCCEEDED(OleSetClipboard(data))) { OleFlushClipboard(); Flash("Скопировано — вставь куда нужно"); }
            data->Release();
        }
    }

    void DragOut(size_t i) { // тащим файл с полки в проводник, мессенджер, браузер…
        if (i >= g_shelf.size()) return;
        const std::wstring path = g_shelf[i].path;
        IDataObject* data = FileData(path);
        if (!data) return;
        DWORD effect = DROPEFFECT_NONE;
        SHDoDragDrop(nullptr, data, nullptr, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
        data->Release();
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) { // файл перенесли — с полки убираем
            std::erase_if(g_shelf, [&](const ShelfItem& it) { return it.path == path; });
            SaveShelf();
        }
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        const float x = static_cast<float>(static_cast<short>(LOWORD(lParam))), y = static_cast<float>(static_cast<short>(HIWORD(lParam)));
        switch (message) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE; // фокус остаётся у игры или браузера
        case WM_SETCURSOR: {
            POINT at{};
            GetCursorPos(&at);
            ScreenToClient(hwnd, &at);
            SetCursor(LoadCursorW(nullptr, HitAt(static_cast<float>(at.x), static_cast<float>(at.y)) ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        case WM_LBUTTONDOWN:
            g_pressed = HitAt(x, y);
            g_pressAt = { static_cast<int>(x), static_cast<int>(y) };
            SetCapture(hwnd);
            return 0;
        case WM_MOUSEMOVE:
            if (g_pressed >= kItem && g_pressed < kItem + kShelfMax && (wParam & MK_LBUTTON)) {
                const int dx = std::abs(static_cast<int>(x) - g_pressAt.x), dy = std::abs(static_cast<int>(y) - g_pressAt.y);
                if (dx > GetSystemMetrics(SM_CXDRAG) || dy > GetSystemMetrics(SM_CYDRAG)) {
                    const size_t i = g_pressed - kItem;
                    g_pressed = kNone;
                    ReleaseCapture();
                    DragOut(i);
                }
            }
            return 0;
        case WM_LBUTTONUP: {
            ReleaseCapture();
            const int id = HitAt(x, y);
            if (id != kNone && id == g_pressed) Activate(id);
            g_pressed = kNone;
            return 0;
        }
        case WM_RBUTTONUP: {
            const int id = HitAt(x, y);
            if (id >= kItem && id < kItem + kShelfMax) CopyItem(id - kItem);
            return 0;
        }
        case WM_MOUSEWHEEL:
            Turn(GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? -1 : 1);
            return 0;
        default: return DefWindowProcW(hwnd, message, wParam, lParam);
        }
    }

    bool FullscreenApp() { // игра или видео на весь экран — островок не мешает
        QUERY_USER_NOTIFICATION_STATE st{};
        if (FAILED(SHQueryUserNotificationState(&st))) return false;
        return st == QUNS_BUSY || st == QUNS_RUNNING_D3D_FULL_SCREEN || st == QUNS_PRESENTATION_MODE;
    }

    // Раскладка активного окна: «Русский», «RU»
    LANGID CurrentLayout() {
        const HWND fg = GetForegroundWindow();
        return LOWORD(reinterpret_cast<UINT_PTR>(GetKeyboardLayout(GetWindowThreadProcessId(fg, nullptr))));
    }
    void LayoutToast(LANGID lang) {
        wchar_t loc[LOCALE_NAME_MAX_LENGTH] = {}, name[96] = {}, iso[16] = {};
        if (!LCIDToLocaleName(MAKELCID(lang, SORT_DEFAULT), loc, LOCALE_NAME_MAX_LENGTH, 0)) return;
        GetLocaleInfoEx(loc, LOCALE_SNATIVELANGUAGENAME, name, 96);
        GetLocaleInfoEx(loc, LOCALE_SISO639LANGNAME, iso, 16);
        CharUpperBuffW(name, 1);
        CharUpperBuffW(iso, static_cast<DWORD>(wcslen(iso)));
        ShowToast(name, kIconLayout, iso, 1800);
    }

    void Loop() {
        OleInitialize(nullptr); // перетаскивание файлов и буфер обмена
        ULONG_PTR token = 0;
        GdiplusStartupInput input;
        GdiplusStartup(&token, &input, nullptr);
        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ZaviriaNotch";
        RegisterClassW(&wc);
        Surface s;
        // WS_EX_TOOLWINDOW — нет в панели задач и Alt-Tab; NOACTIVATE — не забирает фокус;
        // TRANSPARENT (в покое) — мышь проходит насквозь даже через саму полоску.
        s.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
            L"ZaviriaNotch", L"ZAVIRIA: островок Ани", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        RegisterDragDrop(s.hwnd, &g_drop);
        LoadShelf();
        g_water.Load();

        Spring sw, sh;
        sw.x = sw.to = S(kIdleW);
        bool shown = false, clickable = false, enabled = true, idleBar = true, fullscreen = false, panel = false;
        bool optLayout = true, optPower = true, optWave = true, optEyes = true;
        ULONGLONG lastOptions = 0, lastTopmost = 0, lastBusy = 0, lastText = 0, lastWant = 0, topSince = 0, lmbSince = 0, lastTrack = 0;
        ULONGLONG lastLayoutCheck = 0, lastPower = 0, posAt = 0, lastWeather = 0;
        float pressY = 0; // где зажали кнопку мыши: файл к чёлке «тащат» только снизу
        double lastPos = -1;
        LANGID layout = 0;
        int acLine = -1;
        ULONGLONG last = GetTickCount64();
        const ULONGLONG start = last;
        // время кадра — по точному таймеру: у GetTickCount шаг ~16 мс, кадры выходили то короче, то длиннее (рывки)
        LARGE_INTEGER qpcFreq{}, qpcStart{}, qpcLast{};
        QueryPerformanceFrequency(&qpcFreq);
        QueryPerformanceCounter(&qpcStart);
        qpcLast = qpcStart;
        View v;
        Mode held = Mode::Idle, visual = Mode::Idle;
        float speakH = S(48);

        while (g_app.running) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            {
                std::lock_guard lock(g_dumpMutex);
                if (!g_dumpFolder.empty()) { SaveDump(g_dumpFolder); g_dumpFolder.clear(); }
            }
            const ULONGLONG now = GetTickCount64();
            if (now - lastOptions > 1000 || g_optionsDirty.exchange(false)) {
                enabled = !EcoMode::Active() && Opt("notch");
                idleBar = Opt("notch_idle");
                { // размер ползунком: 70…150 %, 100 % — как задумано (старая «крупнее» = 115 %)
                    const int pct = std::clamp(std::atoi(Config::Value("overlay", "notch_scale", Opt("notch_size", "0") ? "115" : "100").c_str()), 70, 150);
                    g_scale = 0.93f * pct / 100.0f;
                }
                static const char* kKeys[kPageCount] = { "notch_music", "notch_shelf", "notch_weather", "notch_timers", "notch_stopwatch", "notch_pomodoro", "notch_water" };
                for (int i = 0; i < kPageCount; ++i) g_pageOn[i] = Opt(kKeys[i]);
                g_pageOn[kWater] = false; // «Вода» убрана по просьбе владелицы
                if (g_page < kPageCount && !g_pageOn[g_page]) g_page = StepPage(g_page, 1);
                optLayout = Opt("notch_layout");
                optPower = Opt("notch_power");
                optWave = Opt("notch_wave");
                optEyes = Opt("notch_eyes");
                g_optLyrics = Opt("notch_lyrics", "0");
                g_edge = std::clamp(std::atoi(Config::Value("overlay", "notch_edge", "0").c_str()), 0, 3);
                g_eyeShape = std::clamp(std::atoi(Config::Value("overlay", "notch_eye_shape", "0").c_str()), 0, 5);
                g_along = std::clamp(std::atoi(Config::Value("overlay", "notch_along", "500").c_str()) / 1000.0f, 0.0f, 1.0f);
                fullscreen = FullscreenApp();
                lastOptions = now;
            }
            LARGE_INTEGER qpcNow{};
            QueryPerformanceCounter(&qpcNow);
            float dt = std::min(static_cast<float>(static_cast<double>(qpcNow.QuadPart - qpcLast.QuadPart) / qpcFreq.QuadPart), 0.05f);
            qpcLast = qpcNow;
            last = now;
            g_pomo.Tick();
            g_water.Tick();

            // всплывашки: раскладка и зарядка
            if (enabled && optLayout && now - lastLayoutCheck > 120) {
                const LANGID cur = CurrentLayout();
                if (cur && layout && cur != layout && !fullscreen) LayoutToast(cur);
                if (cur) layout = cur;
                lastLayoutCheck = now;
            }
            if (enabled && optPower && now - lastPower > 2000) {
                SYSTEM_POWER_STATUS ps{};
                if (GetSystemPowerStatus(&ps) && ps.BatteryFlag != 128 && ps.ACLineStatus != 255) {
                    if (acLine != -1 && ps.ACLineStatus != acLine) {
                        wchar_t b[96];
                        swprintf(b, 96, L"%ls · %d%%", Tw(ps.ACLineStatus ? "Зарядка подключена" : "Зарядка отключена").c_str(), ps.BatteryLifePercent);
                        ShowToast(b, kIconPower, L"", 2200);
                    }
                    acLine = ps.ACLineStatus;
                }
                lastPower = now;
            }

            MONITORINFO info{ sizeof(info) };
            GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &info);
            const RECT mon = info.rcMonitor;
            s.Resize(static_cast<int>(S(kWinW)), static_cast<int>(S(kWinH)));
            int winX = 0, winY = 0;
            {
                const float mw = static_cast<float>(mon.right - mon.left), mh = static_cast<float>(mon.bottom - mon.top);
                if (!Side()) { // вдоль верха/низа: середина островка там, куда поставили, но целиком на экране
                    const float half = S(kPanelW) * 0.5f + S(kEar);
                    const float c = std::clamp(mon.left + g_along * mw, mon.left + half, mon.right - half);
                    winX = static_cast<int>(std::lround(c - s.width * 0.5f));
                    winY = g_edge == 0 ? mon.top : mon.bottom - s.height;
                } else {
                    const float half = S(kPanelH) * 0.5f + S(kEar) + S(30);
                    const float c = std::clamp(mon.top + g_along * mh, mon.top + half, mon.bottom - half);
                    winY = static_cast<int>(std::lround(c - s.height * 0.5f));
                    winX = g_edge == 2 ? mon.left : mon.right - s.width;
                }
            }

            // голос Ани — только читаем
            Mode voice = Mode::Idle;
            if (g_app.speaking) voice = Mode::Speaking;
            else if (g_app.thinking || g_app.transcribing) voice = Mode::Thinking;
            else if (g_app.capturing || g_app.pttArmed) voice = Mode::Listening;
            if (voice != Mode::Idle) { lastBusy = now; held = voice; }
            else if (now - lastBusy < 450) voice = held; // промежутки между фазами не схлопывают островок

            // мышь: навели на маленькую чёлку — раскрываем; тащат файл к верху — сразу на «Полку»
            POINT c{};
            GetCursorPos(&c);
            const float lx = static_cast<float>(c.x - winX), ly = static_cast<float>(c.y - winY);
            // depth — как далеко от края экрана, half — насколько в сторону от середины островка вдоль края
            const float depthPos = g_edge == 0 ? ly : g_edge == 1 ? s.height - ly : g_edge == 2 ? lx : s.width - lx;
            const float half = std::fabs(Side() ? ly - s.height * 0.5f : lx - s.width * 0.5f);
            const float extAlong = Side() ? sh.x : sw.x, extDepth = Side() ? sw.x : sh.x; // размер островка вдоль края и вглубь
            const bool lmb = (GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000) != 0;
            if (lmb && !lmbSince) pressY = depthPos;
            lmbSince = lmb ? (lmbSince ? lmbSince : now) : 0;
            // развёрнутое окно ZAVIRIA впереди — его меню прямо под чёлкой, не закрываем его
            bool ownMaximized = false;
            if (const HWND fg = GetForegroundWindow(); fg && fg != s.hwnd && IsZoomed(fg)) {
                DWORD pid = 0;
                GetWindowThreadProcessId(fg, &pid);
                ownMaximized = pid == GetCurrentProcessId() && MonitorFromWindow(fg, MONITOR_DEFAULTTONULL) == MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
            }
            const bool atTop = depthPos >= 0 && depthPos <= std::max(extDepth, S(3)) + S(2) && half <= std::max(extAlong, Side() ? S(kSideIdleH) : S(kIdleW)) * 0.5f + S(6);
            topSince = atTop && !lmb ? (topSince ? topSince : now) : 0;
            // Под чёлкой может быть что-то нужное (верхняя панель самой ZAVIRIA, вкладки браузера).
            // Над окном ZAVIRIA раскрываемся только от самого края экрана, над остальными — после паузы.
            bool overOwn = false;
            if (topSince && !panel) {
                const HWND under = GetAncestor(WindowFromPoint(c), GA_ROOT);
                DWORD pid = 0;
                if (under && under != s.hwnd) GetWindowThreadProcessId(under, &pid);
                overOwn = pid == GetCurrentProcessId();
            }
            const bool edge = depthPos <= S(2);
            const ULONGLONG dwell = edge ? 110 : 260;
            const bool overShape = shown && half <= extAlong * 0.5f + S(kEar) && depthPos >= 0 && depthPos <= extDepth + S(8);
            const bool dragIn = g_pageOn[kShelf] && lmb && lmbSince && pressY > S(90) && now - lmbSince > 150 && depthPos <= S(30) && half <= (Side() ? S(kPanelH) : S(kPanelW)) * 0.5f && !g_pressed;
            const bool want = enabled && !fullscreen && AnyPage() && (g_dragOver || (panel && (overShape || g_pressed)) || (topSince && now - topSince > dwell && (edge || !overOwn)) || dragIn);
            if (dragIn && !panel) SetPage(kShelf);
            if (want) lastWant = now;
            panel = want || (panel && now - lastWant < 380);
            g_panelOpen = panel;

            const bool toast = !g_toast.text.empty() && now < g_toast.until;
            Mode mode = voice != Mode::Idle && !g_dragOver ? voice : panel ? Mode::Panel : toast ? Mode::Toast : Mode::Idle;
            if (!enabled) mode = Mode::Hidden;
            else if (mode == Mode::Idle && (!idleBar || fullscreen || ownMaximized)) mode = Mode::Hidden;
            // при сворачивании содержимое не исчезает разом, а тает вместе с плашкой
            if (mode != Mode::Idle && mode != Mode::Hidden) visual = mode;
            else if (visual != Mode::Idle && visual != Mode::Hidden && sh.x > S(kIdleH) + S(6)) {}
            else visual = mode;
            v.mode = visual;

            if (mode == Mode::Speaking && now - lastText > 150) {
                std::string caption;
                bool live = false;
                Caption::Get(caption, live);
                std::wstring text = Text::ToWide(caption);
                if (text != v.text) { v.text = std::move(text); speakH = SpeakingHeight(v.text); }
                lastText = now;
            }
            const bool needMedia = mode == Mode::Panel || (mode == Mode::Idle && optWave);
            if (needMedia && now - lastTrack > (mode == Mode::Panel ? 250u : 700u)) {
                v.track = MediaNow::Snapshot();
                if (v.track.position != lastPos) { lastPos = v.track.position; posAt = now; }
                if (mode == Mode::Panel) {
                    v.timers = Skills::Countdowns();
                    if (now - lastWeather > 5000 || !v.weather.ready) { v.weather = Weather::Snapshot(); lastWeather = now; }
                    if (g_optLyrics && g_lyricsOpen && v.track.present && !v.track.title.empty()) FetchLyrics(v.track);
                }
                lastTrack = now;
            }
            v.position = v.track.playing ? std::min(v.track.duration > 0 ? v.track.duration : 1e9, v.track.position + (now - posAt) / 1000.0) : v.track.position;
            const bool idleWave = optWave && v.track.present && v.track.playing && mode == Mode::Idle;
            v.eyes = optEyes && mode == Mode::Idle;
            if (v.eyes) { // куда смотреть: на мышку, плавно; близко к островку — радуются
                const float icx = g_edge == 2 ? sw.x * 0.5f : g_edge == 3 ? s.width - sw.x * 0.5f : s.width * 0.5f;
                const float icy = (g_edge == 1 ? s.height - sh.x : Side() ? (s.height - sh.x) * 0.5f : 0.0f) + sh.x * 0.5f;
                const float ddx = lx - icx, ddy = ly - icy;
                const float k = 1.0f - std::exp(-dt * 8.0f);
                v.lookX += (std::clamp(ddx / 350.0f, -1.0f, 1.0f) - v.lookX) * k;
                v.lookY += (std::clamp(ddy / 250.0f, -1.0f, 1.0f) - v.lookY) * k;
                const float dist = std::sqrt(ddx * ddx + ddy * ddy);
                v.nearK += ((dist < S(130) ? 1.0f : 0.0f) - v.nearK) * k;
                // «живость»: моргают неровно (иногда дважды), без мышки оглядываются, долго без дела — засыпают
                static double nextBlink = 2.0, blinkAt = -1, lastMove = 0, nextGlance = 0;
                static int extraBlinks = 0;
                static float glanceX = 0, glanceY = 0, lastLx = 0, lastLy = 0;
                static unsigned seed = 0x9E3779B9u;
                auto rnd = [] { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; };
                const double t = v.t;
                if (std::fabs(lx - lastLx) + std::fabs(ly - lastLy) > 2) { // мышку двинули
                    if (v.sleepy > 0.5f) { nextBlink = t; extraBlinks = 1; } // проснулись — хлоп-хлоп
                    lastMove = t;
                    lastLx = lx; lastLy = ly;
                }
                const double idle = t - lastMove;
                v.sleepy += ((idle > 45.0 ? 1.0f : 0.0f) - v.sleepy) * (1.0f - std::exp(-dt * (idle > 45.0 ? 0.8f : 6.0f)));
                if (blinkAt < 0 && t >= nextBlink) blinkAt = t;
                if (blinkAt >= 0) {
                    const float p = static_cast<float>((t - blinkAt) / 0.21); // закрываются быстро, открываются мягче
                    v.blink = p < 0.36f ? std::pow(p / 0.36f, 2.0f) : 1.0f - std::pow(Clamp01((p - 0.36f) / 0.64f), 0.7f);
                    if (p >= 1.0f) {
                        v.blink = 0;
                        blinkAt = -1;
                        if (extraBlinks > 0) { --extraBlinks; nextBlink = t + 0.09; }
                        else {
                            nextBlink = t + (v.sleepy > 0.5f ? 4.0 + rnd() * 5.0 : 1.8 + rnd() * 4.2);
                            if (rnd() < 0.2f) extraBlinks = 1; // следующий раз — двойное
                        }
                    }
                }
                if (idle > 2.5 && v.sleepy < 0.5f) { // мышка стоит — сами поглядывают по сторонам
                    if (t >= nextGlance) {
                        const bool center = rnd() < 0.3f;
                        glanceX = center ? 0.0f : rnd() * 1.6f - 0.8f;
                        glanceY = center ? 0.0f : rnd() * 0.9f - 0.3f;
                        nextGlance = t + 1.4 + rnd() * 2.8;
                        if (rnd() < 0.35f && blinkAt < 0) nextBlink = t; // перевела взгляд — моргнула
                    }
                    const float kk = 1.0f - std::exp(-dt * 13.0f); // взгляд — быстрым рывком, как у живых
                    v.lookX += (glanceX - v.lookX) * kk;
                    v.lookY += (glanceY - v.lookY) * kk;
                } else nextGlance = t + 0.8;
            }
            v.wave = idleWave;
            AudioBands::Want(idleWave); // столбики волны — по настоящему звуку, только пока волна видна
            v.bandsLive = idleWave && AudioBands::Get(v.bands, dt);
            if (mode == Mode::Panel && g_page == kShelf) // миниатюры — по одной за кадр
                for (auto& it : g_shelf) if (!it.tried) { it.tried = true; it.thumb = LoadThumb(it.path, static_cast<int>(S(48))); break; }
            const int count = static_cast<int>(v.levels.size()), head = g_app.levelIndex.load();
            for (int i = 0; i < count; ++i) v.levels[i] = g_app.levelHistory[(head - count + i) & (AppState::kLevelHistory - 1)].load();
            v.levels.back() = std::max(v.levels.back(), g_app.micLevel.load());
            v.mouth = g_app.mouth;

            switch (mode) {
            case Mode::Hidden: if (Side()) { sw.to = 0; sh.to = S(kSideIdleH); } else { sw.to = S(kIdleW); sh.to = 0; } break;
            case Mode::Idle: // маленькая чёлка с часами — продолжение рамки; у бока — вертикальная, вплотную к краю
                if (Side()) { sw.to = S(kSideIdleW); sh.to = S(optEyes ? kSideIdleH : kSideIdleH - 30); }
                else { sw.to = S(kIdleW + (idleWave ? kWaveW : 0)); sh.to = S(kIdleH); }
                break;
            case Mode::Toast: sw.to = ToastWidth(g_toast); sh.to = S(48); break;
            case Mode::Panel: sw.to = S(kPanelW + (g_page == kMusic && g_lyricsOpen && g_optLyrics ? kLyricsW - kLyricsNarrow : 0)); sh.to = S(kPanelH); break;
            case Mode::Listening: sw.to = S(320); sh.to = S(48); break;
            case Mode::Thinking: sw.to = S(236); sh.to = S(48); break;
            case Mode::Speaking: sw.to = S(460); sh.to = speakH; break;
            }
            g_pageT = std::min(1.0f, g_pageT + dt * 2.4f); // смена вкладки чуть дольше — мягче
            { // слова песни: строка по центру плавно догоняет звучащую, при перемотке — прыжок
                std::lock_guard lock(g_lyrics.mutex);
                const float target = static_cast<float>(LyricIndex(v.position));
                static float s_lyricV = 0;
                if (std::fabs(target - g_lyricScroll) > 4) { g_lyricScroll = target; s_lyricV = 0; }
                else for (float left = dt; left > 0; left -= 1.0f / 240.0f) { // критически затухающая пружина: без скачка скорости в начале
                    const float h = std::min(left, 1.0f / 240.0f), k = 30.0f;
                    s_lyricV += (k * (target - g_lyricScroll) - 2.0f * std::sqrt(k) * s_lyricV) * h;
                    g_lyricScroll += s_lyricV * h;
                }
            }
            sw.stiff = sh.stiff = mode == Mode::Toast || visual == Mode::Toast ? 2.6f : 1.0f;
            while (dt > 0) { // мелкие шаги — пружина устойчива при любой частоте кадров
                const float step = std::min(dt, 1.0f / 240.0f);
                sw.Step(step);
                sh.Step(step);
                dt -= step;
            }

            const bool active = mode == Mode::Panel || mode == Mode::Listening || mode == Mode::Thinking || mode == Mode::Speaking;
            const bool animating = active || mode == Mode::Toast || !sw.Settled() || !sh.Settled();
            if (active != clickable) { // ловим мышь только когда островок раскрыт
                LONG_PTR ex = GetWindowLongPtrW(s.hwnd, GWL_EXSTYLE);
                ex = active ? ex & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT) : ex | WS_EX_TRANSPARENT;
                SetWindowLongPtrW(s.hwnd, GWL_EXSTYLE, ex);
                clickable = active;
            }
            g_hover = active ? HitAt(lx, ly) : kNone;

            const bool gone = mode == Mode::Hidden && sh.Settled() && sw.Settled() && (Side() ? sw.x < 0.5f : sh.x < 0.5f);
            SYSTEMTIME st{};
            GetLocalTime(&st);
            const bool idleTick = st.wMinute != v.local.wMinute || ((g_sw.running || g_pomo.running) && st.wSecond != v.local.wSecond);
            if (gone) {
                if (shown) { ShowWindow(s.hwnd, SW_HIDE); shown = false; }
            } else if (animating || !shown || idleTick || idleWave || v.eyes) { // в покое — раз в минуту / секунду; волна и глазки — ~30 кадров
                v.w = std::max(0.0f, sw.x);
                v.h = std::clamp(sh.x, 0.0f, static_cast<float>(s.height));
                v.t = static_cast<float>(static_cast<double>(qpcNow.QuadPart - qpcStart.QuadPart) / qpcFreq.QuadPart);
                v.local = st;
                Draw(s, v, g_hover, g_dragOver, &g_hits);
                s.Present(winX, winY); // прижат к выбранному краю экрана
                if (!shown) { ShowWindow(s.hwnd, SW_SHOWNOACTIVATE); shown = true; }
            }
            if (shown && now - lastTopmost > 2000) { // полноэкранные окна иногда перекрывают — возвращаемся наверх
                SetWindowPos(s.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                lastTopmost = now;
            }
            if (animating && shown) { if (FAILED(DwmFlush())) Sleep(16); } // кадр в такт монитору
            else MsgWaitForMultipleObjects(0, nullptr, FALSE, 30, QS_ALLINPUT); // в покое почти не грузит процессор
        }
        ShowWindow(s.hwnd, SW_HIDE);
        RevokeDragDrop(s.hwnd);
        OleUninitialize();
    }
}

void Notch::Start() { std::thread(Loop).detach(); }

void Notch::Dump(const std::string& folder) {
    std::lock_guard lock(g_dumpMutex);
    g_dumpFolder = folder;
}
