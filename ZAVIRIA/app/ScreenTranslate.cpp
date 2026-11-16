#ifndef NOMINMAX
#define NOMINMAX // min/max из windows.h ломают std::min и заголовки WinRT
#endif
#include "ScreenTranslate.h"

#include "Lang.h"
#include "LocalAi.h"
#include "Log.h"
#include "Screen.h"
#include "Settings.h"
#include "TextUtils.h"

#include <windows.h>
#include <dwmapi.h>
#undef GetCurrentTime // мешает заголовкам WinRT
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "windowsapp")
#pragma comment(lib, "dwmapi")

namespace {
    constexpr UINT kHotkeyId = 0x5A54;
    constexpr UINT kReloadMsg = WM_APP + 1, kRunMsg = WM_APP + 2, kResultMsg = WM_APP + 3;

    const ScreenTranslate::Language kLanguages[] = {
        { "ru", "Русский", "Russian" }, { "en", "English", "English" }, { "uk", "Українська", "Ukrainian" }, { "be", "Беларуская", "Belarusian" },
        { "kk", "Қазақша", "Kazakh" }, { "de", "Deutsch", "German" }, { "fr", "Français", "French" }, { "es", "Español", "Spanish" },
        { "it", "Italiano", "Italian" }, { "pt", "Português", "Portuguese" }, { "pl", "Polski", "Polish" }, { "cs", "Čeština", "Czech" },
        { "tr", "Türkçe", "Turkish" }, { "ja", "日本語", "Japanese" }, { "ko", "한국어", "Korean" }, { "zh", "中文", "Chinese (Simplified)" },
        { "ar", "العربية", "Arabic" }, { "hi", "हिन्दी", "Hindi" }, { "uz", "Oʻzbekcha", "Uzbek" }, { "ka", "ქართული", "Georgian" },
        { "hy", "Հայերեն", "Armenian" }, { "az", "Azərbaycanca", "Azerbaijani" }, { "nl", "Nederlands", "Dutch" }, { "sv", "Svenska", "Swedish" },
        { "fi", "Suomi", "Finnish" }, { "el", "Ελληνικά", "Greek" }, { "he", "עברית", "Hebrew" }, { "vi", "Tiếng Việt", "Vietnamese" },
        { "th", "ไทย", "Thai" }, { "id", "Bahasa Indonesia", "Indonesian" },
    };

    HWND g_msgWnd = nullptr, g_selectWnd = nullptr, g_resultWnd = nullptr;
    // снимок экрана на момент нажатия — выделяем по нему (экран «замирает», как у ножниц Windows)
    std::vector<uint32_t> g_shot, g_dim;
    int g_vx = 0, g_vy = 0, g_vw = 0, g_vh = 0;
    POINT g_from{}, g_to{};
    bool g_dragging = false;
    std::mutex g_resultMutex;
    std::wstring g_resultText;
    RECT g_resultNear{};

    std::wstring HotkeyText() { return Text::ToWide(Config::Value("translate", "hotkey", "4|k|84")); }

    void Register() {
        UnregisterHotKey(g_msgWnd, kHotkeyId);
        int mods = 4, vk = 'T';
        char kind = 'k';
        if (std::sscanf(Config::Value("translate", "hotkey", "4|k|84").c_str(), "%d|%c|%d", &mods, &kind, &vk) != 3 || kind != 'k') { mods = 4; vk = 'T'; }
        UINT m = MOD_NOREPEAT;
        if (mods & 1) m |= MOD_CONTROL;
        if (mods & 2) m |= MOD_ALT;
        if (mods & 4) m |= MOD_SHIFT;
        if (mods & 8) m |= MOD_WIN;
        if (Config::Value("translate", "enabled", "1") == "1" && !RegisterHotKey(g_msgWnd, kHotkeyId, m, static_cast<UINT>(vk)))
            Log::Write("перевод экрана: сочетание занято другой программой");
    }

    // ---------------------------------------------------------------- распознавание и перевод
    std::string Ocr(const std::vector<uint32_t>& px, int w, int h) {
        using namespace winrt;
        using namespace winrt::Windows::Graphics::Imaging;
        using namespace winrt::Windows::Media::Ocr;
        try {
            try { init_apartment(apartment_type::multi_threaded); } catch (...) {}
            auto engine = OcrEngine::TryCreateFromUserProfileLanguages();
            if (!engine) return {};
            // мелкий текст OCR читает плохо — увеличиваем вдвое (но не больше, чем движок принимает)
            const int scale = (h < 400 && w * 2 <= static_cast<int>(OcrEngine::MaxImageDimension()) && h * 2 <= static_cast<int>(OcrEngine::MaxImageDimension())) ? 2 : 1;
            const int W = w * scale, H = h * scale;
            winrt::Windows::Storage::Streams::Buffer buffer(static_cast<uint32_t>(W * H * 4));
            buffer.Length(buffer.Capacity());
            auto* out = reinterpret_cast<uint32_t*>(buffer.data());
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) out[static_cast<size_t>(y) * W + x] = px[static_cast<size_t>(y / scale) * w + x / scale] | 0xFF000000;
            SoftwareBitmap bmp(BitmapPixelFormat::Bgra8, W, H, BitmapAlphaMode::Premultiplied);
            bmp.CopyFromBuffer(buffer);
            const auto result = engine.RecognizeAsync(bmp).get();
            std::wstring text;
            for (const auto& line : result.Lines()) text += std::wstring(line.Text()) + L"\n";
            return Text::Trim(Text::ToUtf8(text));
        } catch (...) {
            return {};
        }
    }

    const ScreenTranslate::Language& Target() {
        const std::string code = Config::Value("translate", "lang", Lang::IsEnglish() ? "en" : "ru");
        for (const auto& l : kLanguages) if (code == l.code) return l;
        return kLanguages[0];
    }

    void ShowResult(const std::wstring& text) {
        { std::lock_guard lock(g_resultMutex); g_resultText = text; }
        PostMessageW(g_msgWnd, kResultMsg, 0, 0);
    }

    void Translate(std::vector<uint32_t> px, int w, int h) {
        std::thread([px = std::move(px), w, h] {
            const auto& to = Target();
            std::string text = Ocr(px, w, h);
            std::string answer;
            if (text.size() >= 2) {
                Log::Write("перевод экрана: распознано " + std::to_string(text.size()) + " байт текста");
                answer = Ai::Quiet(std::string("Translate the following text into ") + to.english +
                                   ". Reply with the translation only, keep line breaks, no comments or quotes. If it is already in " + to.english + ", return it as is.\n\n" + text, "", false);
            } else if (Ai::CanSee()) { // Windows не нашла текст (другой алфавит, стилизованный шрифт) — смотрим «глазами» нейросети
                std::vector<uint8_t> bgr(static_cast<size_t>(w) * h * 4);
                std::memcpy(bgr.data(), px.data(), bgr.size());
                const auto jpeg = Screen::EncodeJpeg(bgr.data(), w, h, 90);
                answer = Ai::Quiet(std::string("Read all text in this image and translate it into ") + to.english +
                                   ". Reply with the translation only, keep line breaks. If there is no text, reply: —", Screen::Base64(jpeg), false);
            }
            answer = Text::Trim(answer);
            if (answer.empty() || answer == "—") ShowResult(Text::ToWide(text.empty() ? Lang::Tr("Текст не нашёлся. Выдели область поточнее.") : Lang::Tr("Нейросеть не ответила — перевести не получилось.")));
            else ShowResult(Text::ToWide(answer));
        }).detach();
    }

    // ---------------------------------------------------------------- выделение области
    RECT Selection() {
        RECT r{ std::min(g_from.x, g_to.x), std::min(g_from.y, g_to.y), std::max(g_from.x, g_to.x), std::max(g_from.y, g_to.y) };
        return r;
    }

    void PaintSelect(HDC dc) {
        // буфер: затемнённый снимок, внутри выделения — как есть, по краю — фиолетовая рамка
        std::vector<uint32_t> frame = g_dim;
        const RECT r = Selection();
        if (g_dragging)
            for (int y = std::max(0L, r.top); y < std::min<long>(g_vh, r.bottom); ++y)
                std::memcpy(&frame[static_cast<size_t>(y) * g_vw + std::max(0L, r.left)], &g_shot[static_cast<size_t>(y) * g_vw + std::max(0L, r.left)],
                            sizeof(uint32_t) * std::max(0L, std::min<long>(g_vw, r.right) - std::max(0L, r.left)));
        BITMAPINFO bi{};
        bi.bmiHeader = { sizeof(BITMAPINFOHEADER), g_vw, -g_vh, 1, 32, BI_RGB };
        SetDIBitsToDevice(dc, 0, 0, g_vw, g_vh, 0, 0, 0, g_vh, frame.data(), &bi, DIB_RGB_COLORS);
        if (g_dragging) {
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(170, 120, 255));
            HGDIOBJ old = SelectObject(dc, pen);
            SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, r.left - 1, r.top - 1, r.right + 1, r.bottom + 1);
            SelectObject(dc, old);
            DeleteObject(pen);
        } else { // подсказка сверху
            const std::wstring hint = Text::ToWide(Lang::Tr("Выдели мышкой текст, который перевести · Esc — отмена"));
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(240, 236, 248));
            HFONT font = CreateFontW(-22, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            HGDIOBJ old = SelectObject(dc, font);
            // по центру монитора, где мышка (не на стыке двух экранов), на тёмной плашке
            POINT cur{};
            GetCursorPos(&cur);
            MONITORINFO mi{ sizeof(mi) };
            GetMonitorInfoW(MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST), &mi);
            const int mx = mi.rcMonitor.left - g_vx, mw = mi.rcMonitor.right - mi.rcMonitor.left, my = mi.rcMonitor.top - g_vy;
            RECT size{ 0, 0, 0, 0 };
            DrawTextW(dc, hint.c_str(), -1, &size, DT_SINGLELINE | DT_CALCRECT);
            const int pw = size.right + 40, ph = size.bottom + 18, px = mx + (mw - pw) / 2, py = my + 36;
            HBRUSH plate = CreateSolidBrush(RGB(28, 20, 40));
            HPEN edge = CreatePen(PS_SOLID, 1, RGB(110, 80, 170));
            HGDIOBJ oldBrush = SelectObject(dc, plate), oldPen = SelectObject(dc, edge);
            RoundRect(dc, px, py, px + pw, py + ph, ph, ph);
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(plate);
            DeleteObject(edge);
            RECT t{ px, py, px + pw, py + ph };
            DrawTextW(dc, hint.c_str(), -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, old);
            DeleteObject(font);
        }
    }

    LRESULT CALLBACK SelectProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            PaintSelect(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, IDC_CROSS)); return TRUE;
        case WM_LBUTTONDOWN:
            g_from = g_to = POINT{ static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)) };
            g_dragging = true;
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE:
            if (g_dragging) { g_to = POINT{ static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)) }; InvalidateRect(hwnd, nullptr, FALSE); }
            return 0;
        case WM_LBUTTONUP: {
            if (!g_dragging) return 0;
            g_dragging = false;
            ReleaseCapture();
            const RECT r = Selection();
            DestroyWindow(hwnd);
            g_selectWnd = nullptr;
            const int w = r.right - r.left, h = r.bottom - r.top;
            if (w < 8 || h < 8) return 0; // просто щёлкнули — ничего
            std::vector<uint32_t> crop(static_cast<size_t>(w) * h);
            for (int y = 0; y < h; ++y) std::memcpy(&crop[static_cast<size_t>(y) * w], &g_shot[static_cast<size_t>(r.top + y) * g_vw + r.left], sizeof(uint32_t) * w);
            g_resultNear = RECT{ r.left + g_vx, r.top + g_vy, r.right + g_vx, r.bottom + g_vy };
            ShowResult(Text::ToWide(Lang::Tr("перевожу…")));
            Translate(std::move(crop), w, h);
            return 0;
        }
        case WM_RBUTTONDOWN:
        case WM_KEYDOWN:
            if (msg == WM_RBUTTONDOWN || wp == VK_ESCAPE) { DestroyWindow(hwnd); g_selectWnd = nullptr; }
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void BeginSelect() {
        if (g_selectWnd) return;
        if (g_resultWnd) { DestroyWindow(g_resultWnd); g_resultWnd = nullptr; }
        g_vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        g_vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        g_vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        g_vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        g_shot.assign(static_cast<size_t>(g_vw) * g_vh, 0);
        HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
        BITMAPINFO bi{};
        bi.bmiHeader = { sizeof(BITMAPINFOHEADER), g_vw, -g_vh, 1, 32, BI_RGB };
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old = SelectObject(mem, bmp);
        BitBlt(mem, 0, 0, g_vw, g_vh, screen, g_vx, g_vy, SRCCOPY | CAPTUREBLT);
        std::memcpy(g_shot.data(), bits, g_shot.size() * 4);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        g_dim.resize(g_shot.size());
        for (size_t i = 0; i < g_shot.size(); ++i) { // затемнить с лёгким фиолетовым оттенком
            const uint32_t p = g_shot[i];
            const uint32_t b = (p & 255) * 45 / 100 + 14, g = ((p >> 8) & 255) * 42 / 100 + 6, r = ((p >> 16) & 255) * 45 / 100 + 12;
            g_dim[i] = (std::min(r, 255u) << 16) | (std::min(g, 255u) << 8) | std::min(b, 255u);
        }
        g_dragging = false;
        g_selectWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"ZaviriaTranslateSelect", L"", WS_POPUP, g_vx, g_vy, g_vw, g_vh, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ShowWindow(g_selectWnd, SW_SHOW);
        SetForegroundWindow(g_selectWnd);
        SetFocus(g_selectWnd);
    }

    // ---------------------------------------------------------------- окошко с переводом
    LRESULT CALLBACK ResultProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH bg = CreateSolidBrush(RGB(30, 22, 42));
            FillRect(dc, &rc, bg);
            DeleteObject(bg);
            SetBkMode(dc, TRANSPARENT);
            HFONT head = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            HFONT body = CreateFontW(-18, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            HGDIOBJ old = SelectObject(dc, head);
            SetTextColor(dc, RGB(170, 150, 210));
            const std::wstring title = Text::ToWide(std::string(Lang::Tr("Перевод")) + " → " + Target().name + " · " + Lang::Tr("щёлкни — скопировать"));
            RECT t{ 16, 10, rc.right - 16, 32 };
            DrawTextW(dc, title.c_str(), -1, &t, DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, body);
            SetTextColor(dc, RGB(244, 240, 250));
            std::wstring text;
            { std::lock_guard lock(g_resultMutex); text = g_resultText; }
            RECT b{ 16, 36, rc.right - 16, rc.bottom - 12 };
            DrawTextW(dc, text.c_str(), -1, &b, DT_WORDBREAK | DT_NOPREFIX);
            SelectObject(dc, old);
            DeleteObject(head);
            DeleteObject(body);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONUP: { // скопировать и закрыть
            std::wstring text;
            { std::lock_guard lock(g_resultMutex); text = g_resultText; }
            if (OpenClipboard(hwnd)) {
                EmptyClipboard();
                if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t))) {
                    std::memcpy(GlobalLock(mem), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
                    GlobalUnlock(mem);
                    SetClipboardData(CF_UNICODETEXT, mem);
                }
                CloseClipboard();
            }
            DestroyWindow(hwnd);
            g_resultWnd = nullptr;
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) { DestroyWindow(hwnd); g_resultWnd = nullptr; }
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) { DestroyWindow(hwnd); g_resultWnd = nullptr; } // щёлкнули в другое место — закрыть
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void UpdateResult() {
        std::wstring text;
        { std::lock_guard lock(g_resultMutex); text = g_resultText; }
        // размер по тексту: ширина до 560, высота — сколько нужно
        HDC dc = GetDC(nullptr);
        HFONT body = CreateFontW(-18, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HGDIOBJ old = SelectObject(dc, body);
        RECT calc{ 0, 0, 528, 0 };
        DrawTextW(dc, text.c_str(), -1, &calc, DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
        SelectObject(dc, old);
        DeleteObject(body);
        ReleaseDC(nullptr, dc);
        const int w = std::max(300L, calc.right + 32), h = std::min(700L, calc.bottom + 52);
        // под выделением; не влезает — над ним; не за краем монитора
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromRect(&g_resultNear, MONITOR_DEFAULTTONEAREST), &mi);
        int x = std::clamp(static_cast<int>(g_resultNear.left), static_cast<int>(mi.rcWork.left) + 8, static_cast<int>(mi.rcWork.right) - w - 8);
        int y = g_resultNear.bottom + 10;
        if (y + h > mi.rcWork.bottom - 8) y = std::max(static_cast<int>(mi.rcWork.top) + 8, static_cast<int>(g_resultNear.top) - h - 10);
        if (!g_resultWnd) {
            g_resultWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"ZaviriaTranslateResult", L"ZAVIRIA", WS_POPUP, x, y, w, h, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            const DWORD round = 2; // DWMWCP_ROUND — скруглённые углы Windows 11
            DwmSetWindowAttribute(g_resultWnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
            ShowWindow(g_resultWnd, SW_SHOW);
            SetForegroundWindow(g_resultWnd);
        } else SetWindowPos(g_resultWnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
        InvalidateRect(g_resultWnd, nullptr, TRUE);
    }

    LRESULT CALLBACK MsgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_HOTKEY: if (wp == kHotkeyId) BeginSelect(); return 0;
        case kRunMsg: BeginSelect(); return 0;
        case kReloadMsg: Register(); return 0;
        case kResultMsg: UpdateResult(); return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void Loop() {
        const HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSW wc{};
        wc.hInstance = inst;
        wc.lpfnWndProc = MsgProc;
        wc.lpszClassName = L"ZaviriaTranslate";
        RegisterClassW(&wc);
        wc.lpfnWndProc = SelectProc;
        wc.lpszClassName = L"ZaviriaTranslateSelect";
        RegisterClassW(&wc);
        wc.lpfnWndProc = ResultProc;
        wc.lpszClassName = L"ZaviriaTranslateResult";
        wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
        RegisterClassW(&wc);
        g_msgWnd = CreateWindowExW(0, L"ZaviriaTranslate", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
        Register();
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

void ScreenTranslate::Start() { std::thread(Loop).detach(); }
void ScreenTranslate::Reload() { if (g_msgWnd) PostMessageW(g_msgWnd, kReloadMsg, 0, 0); }
void ScreenTranslate::Run() { if (g_msgWnd) PostMessageW(g_msgWnd, kRunMsg, 0, 0); }
const ScreenTranslate::Language* ScreenTranslate::Languages(int& count) {
    count = static_cast<int>(std::size(kLanguages));
    return kLanguages;
}
