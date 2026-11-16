#include "PieMenu.h"

#include "Launcher.h"
#include "Lang.h"
#include "Log.h"
#include "Settings.h"
#include "TextUtils.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellscalingapi.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>

#include "../third_party/nlohmann/json.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#pragma comment(lib, "Shcore.lib")

using namespace Gdiplus;
using PieMenu::Node;

namespace {
    constexpr ULONG_PTR kMarker = 0x5A415649;  // «ZAVI»: свои нажатия хук пропускает
    constexpr UINT kHoldMs = 280;               // столько держать среднюю кнопку
    constexpr UINT kOpenMsg = WM_APP + 1, kClickMsg = WM_APP + 2, kReleaseMsg = WM_APP + 3, kPickMsg = WM_APP + 4,
                   kCloseMsg = WM_APP + 5, kPreviewMsg = WM_APP + 6, kBackMsg = WM_APP + 7, kDownMsg = WM_APP + 8;
    constexpr float kPi = 3.14159265f;
    constexpr int kCenter = -2;                  // «наведено на середину»

    // ---------------------------------------------------------------- дерево пунктов (pie.json)
    std::mutex g_treeMutex;
    Node g_root;
    bool g_loaded = false, g_haveFile = false;

    std::wstring TreeFile() { return Text::ExecutableDirectory() + L"\\pie.json"; }

    nlohmann::json ToJson(const Node& n) {
        nlohmann::json j = { { "kind", n.kind }, { "name", n.name } };
        if (!n.target.empty()) j["target"] = n.target;
        if (n.kind == "folder") {
            j["items"] = nlohmann::json::array();
            for (const auto& c : n.items) j["items"].push_back(ToJson(c));
        }
        return j;
    }
    Node FromJson(const nlohmann::json& j, int depth = 0) {
        Node n;
        n.kind = j.value("kind", "app");
        n.name = j.value("name", "");
        n.target = j.value("target", "");
        if (depth < 32 && j.contains("items") && j["items"].is_array())
            for (const auto& c : j["items"]) n.items.push_back(FromJson(c, depth + 1));
        return n;
    }

    void LoadLocked() {
        g_loaded = true;
        g_root = Node{ "folder", "root" };
        std::ifstream in(TreeFile(), std::ios::binary);
        if (in) {
            try { g_root = FromJson(nlohmann::json::parse(in)); g_root.kind = "folder"; g_haveFile = true; return; }
            catch (...) { Log::Write("круговое меню: pie.json не читается — начинаю заново"); }
        }
        // прошлый вариант хранил список в anya.ini: [pie] apps через «|»
        const std::string list = Config::Value("pie", "apps", "");
        size_t from = 0;
        while (from < list.size()) {
            const size_t bar = list.find('|', from);
            const std::string name = Text::Trim(list.substr(from, bar == std::string::npos ? std::string::npos : bar - from));
            if (!name.empty()) g_root.items.push_back(Node{ "app", name });
            if (bar == std::string::npos) break;
            from = bar + 1;
        }
        g_haveFile = !g_root.items.empty();
    }

    Node RootCopy() {
        std::lock_guard lock(g_treeMutex);
        if (!g_loaded) LoadLocked();
        static bool s_guessed = false;
        if (!g_haveFile && g_root.items.empty() && !s_guessed && !Launcher::AppNames().empty()) // своего набора ещё нет — частые программы, какие нашлись в «Пуске» (один раз)
            if ((s_guessed = true))
            for (const char* guess : { "Проводник", "Steam", "Discord", "Telegram", "Яндекс Музыка", "Google Chrome", "Microsoft Edge", "Spotify" }) {
                const auto r = Launcher::Find(guess);
                if (r.found && std::none_of(g_root.items.begin(), g_root.items.end(), [&](const Node& n) { return n.name == r.name; })) g_root.items.push_back(Node{ "app", r.name });
                if (g_root.items.size() >= 6) break;
            }
        return g_root;
    }

    const Node* Level(const Node& root, const std::vector<int>& path) {
        const Node* n = &root;
        for (int i : path) {
            if (i < 0 || i >= static_cast<int>(n->items.size()) || n->items[i].kind != "folder") return nullptr;
            n = &n->items[i];
        }
        return n;
    }

    // ---------------------------------------------------------------- значки программ
    struct Icon { std::vector<uint32_t> px; std::unique_ptr<Bitmap> bmp; };
    using IconCache = std::map<std::string, std::unique_ptr<Icon>>;

    std::unique_ptr<Icon> LoadIcon(const Node& n, int size) {
        const std::wstring target = n.kind == "app" ? Launcher::TargetOf(n.name) : n.kind == "path" ? Text::ToWide(n.target) : L"";
        if (target.empty()) return nullptr;
        IShellItemImageFactory* factory = nullptr;
        if (FAILED(SHCreateItemFromParsingName(target.c_str(), nullptr, IID_PPV_ARGS(&factory)))) return nullptr;
        HBITMAP hb = nullptr;
        const HRESULT hr = factory->GetImage(SIZE{ size, size }, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hb);
        factory->Release();
        if (FAILED(hr) || !hb) return nullptr;
        BITMAP bm{};
        GetObjectW(hb, sizeof(bm), &bm);
        const int w = bm.bmWidth, h = std::abs(bm.bmHeight);
        if (w <= 0 || h <= 0) { DeleteObject(hb); return nullptr; }
        auto icon = std::make_unique<Icon>();
        icon->px.resize(static_cast<size_t>(w) * h);
        BITMAPINFO bi{};
        bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
        HDC dc = GetDC(nullptr);
        GetDIBits(dc, hb, 0, h, icon->px.data(), &bi, DIB_RGB_COLORS);
        ReleaseDC(nullptr, dc);
        DeleteObject(hb);
        if (std::none_of(icon->px.begin(), icon->px.end(), [](uint32_t p) { return (p >> 24) != 0; }))
            for (auto& p : icon->px) p |= 0xFF000000;
        icon->bmp = std::make_unique<Bitmap>(w, h, w * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(icon->px.data()));
        return icon;
    }
    Icon* IconFor(IconCache& cache, const Node& n, int size) {
        const std::string key = n.kind + "|" + n.name + "|" + n.target;
        auto it = cache.find(key);
        if (it != cache.end() && it->second) return it->second.get();
        // не нашёлся — пробуем снова, но не чаще раза в 4 с: при запуске список «Пуска» ещё собирается, и значков сначала нет
        thread_local std::map<std::string, ULONGLONG> failedAt;
        const ULONGLONG now = GetTickCount64();
        if (it != cache.end() && now - failedAt[key] < 4000) return nullptr;
        auto icon = LoadIcon(n, size);
        if (!icon) failedAt[key] = now;
        Icon* raw = icon.get();
        cache[key] = std::move(icon);
        return raw;
    }

    // ---------------------------------------------------------------- рисование
    float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
    float EaseOut(float t) { t = Clamp01(t); return 1.0f - std::pow(1.0f - t, 4.0f); } // quart: мягкий разгон и долгое «дотягивание»
    float EaseBack(float t) { t = Clamp01(t); const float c = 0.7f; /* лёгкий, едва заметный перелёт */ return 1.0f + (c + 1.0f) * std::pow(t - 1.0f, 3.0f) + c * std::pow(t - 1.0f, 2.0f); }

    float Bubble(float s) { return 25.0f * s; }   // радиус кружка
    float Hub(float s) { return 17.0f * s; }      // середина: закрыть / назад
    float RingFor(size_t n, float s) { return (std::max)(78.0f * s, n * Bubble(s) * 2.35f / (2.0f * kPi)); } // больше пунктов — шире круг
    float Extent(size_t n, float s) { return RingFor(n, s) + Bubble(s) * 1.25f + 36.0f * s; } // половина стороны окна

    PointF ItemPos(int i, size_t n, REAL cx, REAL cy, float radius) {
        const float a = -kPi / 2 + i * 2.0f * kPi / static_cast<float>(n ? n : 1);
        return PointF(cx + std::cos(a) * radius, cy + std::sin(a) * radius);
    }

    Color Lerp(const Color& a, const Color& b, float t) {
        t = Clamp01(t);
        auto m = [&](BYTE x, BYTE y) { return static_cast<BYTE>(x + (y - x) * t); };
        return Color(m(a.GetA(), b.GetA()), m(a.GetR(), b.GetR()), m(a.GetG(), b.GetG()), m(a.GetB(), b.GetB()));
    }
    Color Fade(const Color& c, float a) { return Color(static_cast<BYTE>(c.GetA() * Clamp01(a)), c.GetR(), c.GetG(), c.GetB()); }
    const Color kLight(226, 250, 249, 252), kDark(238, 22, 20, 27), kInkDark(255, 24, 22, 30), kInkLight(255, 250, 250, 252);

    struct RPen : Pen {
        RPen(const Color& c, REAL w) : Pen(c, w) { SetStartCap(LineCapRound); SetEndCap(LineCapRound); SetLineJoin(LineJoinRound); }
    };

    void Label(Graphics& g, const std::wstring& text, REAL size, const RectF& box, const Color& color, bool shadow) {
        thread_local std::unique_ptr<FontFamily> family;
        thread_local std::map<int, std::unique_ptr<Font>> fonts;
        if (!family) family = std::make_unique<FontFamily>(L"Segoe UI Semibold");
        auto& slot = fonts[static_cast<int>(size * 4)];
        if (!slot) slot = std::make_unique<Font>(family.get(), size, FontStyleRegular, UnitPixel);
        Font& font = *slot;
        StringFormat f;
        f.SetAlignment(StringAlignmentCenter);
        f.SetLineAlignment(StringAlignmentCenter);
        f.SetTrimming(StringTrimmingEllipsisCharacter);
        if (shadow) { // подпись висит прямо над чем угодно — мягкая тень, чтобы читалась на любом фоне
            for (int k = 0; k < 2; ++k) {
                SolidBrush sb(Color(static_cast<BYTE>(color.GetA() * (k ? 0.45f : 0.25f)), 0, 0, 0));
                RectF b = box;
                b.Offset(0, size * (k ? 0.06f : 0.12f));
                g.DrawString(text.c_str(), -1, &font, b, &f, &sb);
            }
        }
        SolidBrush b(color);
        g.DrawString(text.c_str(), -1, &font, box, &f, &b);
    }

    // Простые значки для папки, сайта, файла и середины — тонкими линиями
    void Glyph(Graphics& g, const std::string& kind, PointF p, REAL r, const Color& c) {
        RPen pen(c, r * 0.09f);
        const REAL u = r * 0.42f;
        if (kind == "folder") {
            GraphicsPath path;
            path.AddLine(p.X - u, p.Y - u * 0.55f, p.X - u * 0.25f, p.Y - u * 0.55f);
            path.AddLine(p.X - u * 0.25f, p.Y - u * 0.55f, p.X - u * 0.05f, p.Y - u * 0.3f);
            path.AddLine(p.X - u * 0.05f, p.Y - u * 0.3f, p.X + u, p.Y - u * 0.3f);
            path.AddLine(p.X + u, p.Y - u * 0.3f, p.X + u, p.Y + u * 0.7f);
            path.AddLine(p.X + u, p.Y + u * 0.7f, p.X - u, p.Y + u * 0.7f);
            path.CloseFigure();
            g.DrawPath(&pen, &path);
        } else if (kind == "site") {
            g.DrawEllipse(&pen, p.X - u, p.Y - u, u * 2, u * 2);
            g.DrawEllipse(&pen, p.X - u * 0.42f, p.Y - u, u * 0.84f, u * 2);
            g.DrawLine(&pen, p.X - u, p.Y, p.X + u, p.Y);
        } else if (kind == "path") {
            PointF doc[] = { { p.X - u * 0.7f, p.Y - u }, { p.X + u * 0.25f, p.Y - u }, { p.X + u * 0.7f, p.Y - u * 0.55f }, { p.X + u * 0.7f, p.Y + u }, { p.X - u * 0.7f, p.Y + u } };
            g.DrawPolygon(&pen, doc, 5);
        } else if (kind == "close") {
            const REAL d = u * 0.55f;
            g.DrawLine(&pen, p.X - d, p.Y - d, p.X + d, p.Y + d);
            g.DrawLine(&pen, p.X + d, p.Y - d, p.X - d, p.Y + d);
        } else if (kind == "back") {
            const REAL d = u * 0.6f;
            g.DrawLine(&pen, p.X - d, p.Y, p.X + d, p.Y);
            g.DrawLine(&pen, p.X - d, p.Y, p.X - d * 0.2f, p.Y - d * 0.75f);
            g.DrawLine(&pen, p.X - d, p.Y, p.X - d * 0.2f, p.Y + d * 0.75f);
        }
    }

    struct Frame {
        const std::vector<Node>* items = nullptr;
        const std::vector<float>* hoverT = nullptr;
        float centerT = 0;       // середина под мышкой
        int lit = -1, picked = -1;
        float openT = 1, closeT = 0;
        bool nested = false;     // внутри папки — в середине «назад»
        bool label = true;
    };

    // Как на её примере: никакой подложки и рамок — только светлые кружки с мягкой тенью; выбранный — тёмный
    void DrawLevel(Graphics& g, REAL cx, REAL cy, float s, IconCache& icons, const Frame& f) {
        const auto& items = *f.items;
        const size_t n = items.size();
        const REAL ring = RingFor(n, s);
        const bool closing = f.closeT > 0;
        const float shown = EaseOut(f.openT * 1.6f) * (1.0f - EaseOut(f.closeT));

        for (int i = 0; i < static_cast<int>(n); ++i) {
            const float local = Clamp01((f.openT - i * 0.04f) / 0.8f);
            const float h = f.hoverT ? (*f.hoverT)[i] : 0.0f;
            float radius = ring * EaseBack(local), scale = (0.55f + 0.45f * EaseOut(local)) * (1.0f + 0.1f * h), alpha = EaseOut(local * 1.5f);
            if (closing) {
                if (i == f.picked) { scale *= 1.0f + 0.25f * EaseOut(f.closeT); alpha *= 1.0f - EaseOut(f.closeT); }
                else { radius *= 1.0f - 0.35f * EaseOut(f.closeT); alpha *= 1.0f - EaseOut(f.closeT * 1.5f); }
            }
            if (alpha <= 0.01f) continue;
            const PointF p = ItemPos(i, n, cx, cy, radius);
            const REAL r = Bubble(s) * scale;
            for (int k = 2; k >= 1; --k) { // мягкая тень под кружком
                SolidBrush sh(Color(static_cast<BYTE>((k == 1 ? 34 : 14) * alpha), 0, 0, 0));
                const REAL sr = r + s * (k == 1 ? 1.5f : 5.0f);
                g.FillEllipse(&sh, p.X - sr, p.Y - sr + s * 3.0f, sr * 2, sr * 2);
            }
            SolidBrush fill(Fade(Lerp(kLight, kDark, h), alpha));
            g.FillEllipse(&fill, p.X - r, p.Y - r, r * 2, r * 2);
            const Node& node = items[i];
            Icon* icon = node.kind == "app" || node.kind == "path" ? IconFor(icons, node, static_cast<int>(64 * s)) : nullptr;
            if (icon && icon->bmp) {
                const REAL is = r * 1.15f;
                ImageAttributes ia;
                ColorMatrix cm = { { { 1, 0, 0, 0, 0 }, { 0, 1, 0, 0, 0 }, { 0, 0, 1, 0, 0 }, { 0, 0, 0, alpha, 0 }, { 0, 0, 0, 0, 1 } } };
                ia.SetColorMatrix(&cm);
                g.DrawImage(icon->bmp.get(), RectF(p.X - is * 0.5f, p.Y - is * 0.5f, is, is), 0, 0, static_cast<REAL>(icon->bmp->GetWidth()),
                            static_cast<REAL>(icon->bmp->GetHeight()), UnitPixel, &ia);
            } else if (node.kind == "app") {
                Label(g, Text::ToWide(node.name).substr(0, 1), r * 0.85f, RectF(p.X - r, p.Y - r, r * 2, r * 2), Fade(Lerp(kInkDark, kInkLight, h), alpha), false);
            } else Glyph(g, node.kind, p, r, Fade(Lerp(kInkDark, kInkLight, h), alpha));
        }

        // середина: маленький полупрозрачный кружок с «×» (или «←» в папке)
        const REAL hub = Hub(s) * (0.8f + 0.2f * EaseOut(f.openT * 1.4f)) * (1.0f + 0.08f * f.centerT);
        SolidBrush hubFill(Fade(Lerp(Color(130, 250, 249, 252), kDark, f.centerT), shown));
        g.FillEllipse(&hubFill, cx - hub, cy - hub, hub * 2, hub * 2);
        Glyph(g, f.nested ? "back" : "close", PointF(cx, cy), hub * 1.45f, Fade(Lerp(kInkDark, kInkLight, f.centerT), shown * 0.9f));

        // подпись выбранного — над кругом, без плашки
        if (f.label && f.lit >= 0 && f.lit < static_cast<int>(n)) {
            const float la = shown * (f.hoverT ? (*f.hoverT)[f.lit] : 1.0f);
            const REAL top = cy - ring - Bubble(s) - s * 24.0f;
            Label(g, Text::ToWide(items[f.lit].name), s * 13.0f, RectF(cx - ring - Bubble(s), top - s * 12.0f, (ring + Bubble(s)) * 2, s * 24.0f), Fade(kInkLight, la), true);
        } else if (f.label && n == 0) {
            Label(g, Text::ToWide(Lang::Tr("Добавь программы в настройках")), s * 14.0f, RectF(cx - s * 160, cy - Hub(s) - s * 40, s * 320, s * 24), Fade(kInkLight, shown), true);
        }
    }

    // ---------------------------------------------------------------- само меню (свой поток)
    enum class St { Idle, Pending, Open, Closing };
    std::atomic<St> g_state{ St::Idle }; // хуки мыши — в своём потоке, рисование — в своём: кадр меню не задерживает курсор
    HWND g_hwnd = nullptr;
    HHOOK g_mouseHook = nullptr, g_keyHook = nullptr;
    POINT g_down{};
    bool g_eatLeftUp = false, g_eatRightUp = false, g_eatMiddleUp = false;
    std::atomic<bool> g_enabled{ true }, g_fullscreen{ false };
    std::atomic<bool> g_fromDrag{ false }; // открыли рывком мыши — отпускание выбирает сразу
    std::atomic<bool> g_treeDirty{ true };  // пункты поменяли — заранее загрузить значки
    // сочетание, которым открывается меню (см. PieMenu.h)
    // несколько сочетаний ([pie] trigger через «;»), любое открывает меню
    constexpr int kMaxTrig = 8;
    std::atomic<int> g_tMods[kMaxTrig], g_tCode[kMaxTrig], g_tCount{ 1 };
    std::atomic<char> g_tKind[kMaxTrig];
    bool MouseTrigger(int button, int mods) { for (int i = 0; i < g_tCount; ++i) if (g_tKind[i] == 'm' && g_tCode[i] == button && (mods < 0 || g_tMods[i] == mods)) return true; return false; }
    bool KeyTrigger(int vk, int mods) { for (int i = 0; i < g_tCount; ++i) if (g_tKind[i] == 'k' && g_tCode[i] == vk && (mods < 0 || g_tMods[i] == mods)) return true; return false; }
    std::atomic<int> g_heldButton{ 0 };   // какая кнопка мыши сейчас держит меню (1…5)
    std::atomic<bool> g_keyOpened{ false }; // открыли клавишей — отпускание клавиши выбирает
    std::atomic<bool> g_tapOpen{ false };   // [pie] tap_open: короткое нажатие (касание тачпада тремя пальцами) тоже открывает меню
    std::atomic<ULONGLONG> g_downTick{ 0 };
    bool g_eatUp[6] = {};                 // отпускание этих кнопок мыши проглотить (нажатие уже съели)
    std::atomic<bool> g_capture{ false };
    std::mutex g_captureMutex;
    std::string g_captured;
    bool g_capturedReady = false;

    int Mods() {
        int m = 0;
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) m |= 1;
        if (GetAsyncKeyState(VK_MENU) & 0x8000) m |= 2;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) m |= 4;
        if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) m |= 8;
        return m;
    }
    bool IsModifierKey(DWORD vk) {
        return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
               vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LWIN || vk == VK_RWIN;
    }
    // кнопка мыши из сообщения хука: 1 левая, 2 правая, 3 колёсико, 4/5 боковые; down — нажатие или отпускание
    int ButtonOf(WPARAM wp, const MSLLHOOKSTRUCT* m, bool& down) {
        down = wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN || wp == WM_XBUTTONDOWN;
        if (wp == WM_LBUTTONDOWN || wp == WM_LBUTTONUP) return 1;
        if (wp == WM_RBUTTONDOWN || wp == WM_RBUTTONUP) return 2;
        if (wp == WM_MBUTTONDOWN || wp == WM_MBUTTONUP) return 3;
        if (wp == WM_XBUTTONDOWN || wp == WM_XBUTTONUP) return HIWORD(m->mouseData) == XBUTTON1 ? 4 : 5;
        return 0;
    }
    void ParseTrigger(const std::string& list) {
        int n = 0;
        size_t from = 0;
        while (from <= list.size() && n < kMaxTrig) {
            const size_t semi = list.find(';', from);
            const std::string t = list.substr(from, semi == std::string::npos ? std::string::npos : semi - from);
            int mods = 0, code = 3;
            char kind = 'm';
            if (std::sscanf(t.c_str(), "%d|%c|%d", &mods, &kind, &code) == 3 && (kind == 'm' || kind == 'k') && !(kind == 'm' && (code < 1 || code > 5)) &&
                !(kind == 'm' && code == 1 && mods == 0)) { // просто левая кнопка — нельзя: ей кликают всё
                g_tMods[n] = mods; g_tKind[n] = kind; g_tCode[n] = code;
                ++n;
            }
            if (semi == std::string::npos) break;
            from = semi + 1;
        }
        if (n == 0) { g_tMods[0] = 0; g_tKind[0] = 'm'; g_tCode[0] = 3; n = 1; }
        g_tCount = n;
    }

    IconCache g_icons;              // значки для меню (у картинки в настройках — свои: другой поток)
    Node g_tree;                    // копия дерева на время показа
    std::vector<int> g_path;        // в какой папке сейчас
    std::vector<Node> g_items;      // пункты этой папки
    std::vector<float> g_hoverT;
    float g_centerT = 0;
    int g_hover = -1, g_picked = -1;
    POINT g_center{};
    int g_winX = 0, g_winY = 0, g_w = 0, g_h = 0;
    float g_s = 1.0f;
    double g_openAt = 0, g_closeAt = 0, g_lastFrame = 0;
    HDC g_dc = nullptr;
    HBITMAP g_bitmap = nullptr;
    void* g_bits = nullptr;
    std::string g_dumpFile;         // DevProbe: сохранить кадр
    int g_dumpHover = -1;

    double Now() {
        static LARGE_INTEGER f{};
        if (!f.QuadPart) QueryPerformanceFrequency(&f);
        LARGE_INTEGER c{};
        QueryPerformanceCounter(&c);
        return static_cast<double>(c.QuadPart) / f.QuadPart;
    }

    void Resize(int w, int h) {
        if (w == g_w && h == g_h && g_bits) return;
        if (g_bitmap) DeleteObject(g_bitmap);
        if (!g_dc) g_dc = CreateCompatibleDC(nullptr);
        BITMAPINFO info{};
        info.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
        g_bitmap = CreateDIBSection(g_dc, &info, DIB_RGB_COLORS, &g_bits, nullptr, 0);
        SelectObject(g_dc, g_bitmap);
        g_w = w;
        g_h = h;
    }

    int HoverAt(POINT cursor) {
        const float dx = static_cast<float>(cursor.x - g_center.x), dy = static_cast<float>(cursor.y - g_center.y);
        if (std::sqrt(dx * dx + dy * dy) < Hub(g_s) * 1.25f) return kCenter;
        if (g_items.empty()) return -1;
        const float step = 2.0f * kPi / g_items.size();
        float a = std::atan2(dy, dx) + kPi / 2 + step / 2; // по направлению: от верха по часовой
        while (a < 0) a += 2 * kPi;
        return static_cast<int>(a / step) % static_cast<int>(g_items.size());
    }

    void SaveDump() {
        std::vector<uint8_t> out(static_cast<size_t>(g_w) * g_h * 3);
        const auto* px = static_cast<const uint32_t*>(g_bits);
        for (int y = 0; y < g_h; ++y)
            for (int x = 0; x < g_w; ++x) { // поверх «неба», как на её примере, — видно прозрачность
                const uint32_t p = px[static_cast<size_t>(y) * g_w + x];
                const int a = p >> 24, bg[3] = { 225 - y * 60 / g_h, 190 - y * 40 / g_h, 150 };
                uint8_t* o = &out[(static_cast<size_t>(g_h - 1 - y) * g_w + x) * 3];
                for (int c = 0; c < 3; ++c) o[c] = static_cast<uint8_t>((std::min)(255, static_cast<int>((p >> (8 * c)) & 255) + bg[c] * (255 - a) / 255));
            }
        BITMAPFILEHEADER fh{ 0x4D42 };
        BITMAPINFOHEADER ih{ sizeof(ih), g_w, g_h, 1, 24, BI_RGB };
        const int stride = (g_w * 3 + 3) & ~3;
        fh.bfOffBits = sizeof(fh) + sizeof(ih);
        fh.bfSize = fh.bfOffBits + stride * g_h;
        if (FILE* f = _wfopen(Text::ToWide(g_dumpFile).c_str(), L"wb")) {
            fwrite(&fh, sizeof(fh), 1, f);
            fwrite(&ih, sizeof(ih), 1, f);
            const uint8_t pad[3] = {};
            for (int y = 0; y < g_h; ++y) { fwrite(&out[static_cast<size_t>(y) * g_w * 3], 1, g_w * 3, f); fwrite(pad, 1, stride - g_w * 3, f); }
            fclose(f);
        }
        g_dumpFile.clear();
        g_dumpHover = -1;
    }

    void Render() {
        const double now = Now();
        const float dt = static_cast<float>((std::min)(0.05, now - g_lastFrame));
        g_lastFrame = now;
        const bool closing = g_state == St::Closing;
        POINT cursor{};
        GetCursorPos(&cursor);
        if (!closing) g_hover = !g_dumpFile.empty() && g_dumpHover >= 0 ? g_dumpHover : HoverAt(cursor);
        const float k = 1.0f - std::exp(-dt * 11.0f); // подсветка перетекает мягче
        for (size_t i = 0; i < g_hoverT.size(); ++i) g_hoverT[i] += ((static_cast<int>(i) == (closing ? g_picked : g_hover) ? 1.0f : 0.0f) - g_hoverT[i]) * k;
        g_centerT += ((!closing && g_hover == kCenter ? 1.0f : 0.0f) - g_centerT) * k;

        Bitmap canvas(g_w, g_h, g_w * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(g_bits));
        Graphics g(&canvas);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.Clear(Color(0, 0, 0, 0));
        Frame f;
        f.items = &g_items;
        f.hoverT = &g_hoverT;
        f.centerT = g_centerT;
        f.lit = closing ? g_picked : g_hover;
        f.picked = g_picked;
        f.openT = static_cast<float>((now - g_openAt) / 0.55);
        f.closeT = closing ? static_cast<float>((now - g_closeAt) / 0.32) : 0.0f;
        f.nested = !g_path.empty();
        DrawLevel(g, g_w * 0.5f, g_h * 0.5f, g_s, g_icons, f);
        g.Flush();
        if (!g_dumpFile.empty() && now - g_openAt > 0.8) SaveDump();
        POINT source{ 0, 0 }, target{ g_winX, g_winY };
        SIZE size{ g_w, g_h };
        BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(g_hwnd, nullptr, &target, &size, g_dc, &source, 0, &blend, ULW_ALPHA);
    }

    // Показать папку по пути g_path: пункты заново вылетают из середины
    void ShowLevel() {
        const Node* level = Level(g_tree, g_path);
        if (!level) { g_path.clear(); level = &g_tree; }
        g_items = level->items;
        g_hoverT.assign(g_items.size(), 0.0f);
        g_hover = g_picked = -1;
        const int half = static_cast<int>(Extent(g_items.size(), g_s));
        Resize(half * 2, half * 2);
        g_winX = g_center.x - half;
        g_winY = g_center.y - half;
        g_openAt = Now();
    }

    void OpenAt(POINT pt) {
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        UINT dx = 96, dy = 96;
        GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
        g_s = dx / 96.0f;
        g_tree = RootCopy();
        g_path.clear();
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(mon, &mi);
        const int half = static_cast<int>(Extent(g_tree.items.size(), g_s)) - static_cast<int>(30 * g_s);
        g_center.x = std::clamp(pt.x, mi.rcWork.left + half, mi.rcWork.right - half); // круг целиком на экране
        g_center.y = std::clamp(pt.y, mi.rcWork.top + half, mi.rcWork.bottom - half);
        if (g_center.x != pt.x || g_center.y != pt.y) SetCursorPos(g_center.x, g_center.y);
        g_centerT = 0;
        g_state = St::Open;
        g_lastFrame = Now();
        ShowLevel();
        Render();
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    void Launch(const Node& n) {
        std::thread([n] {
            if (n.kind == "app") Launcher::Open(n.name);
            else if (n.kind == "site") {
                std::string url = Text::Trim(n.target);
                if (url.find("://") == std::string::npos) url = "https://" + url;
                ShellExecuteW(nullptr, L"open", Text::ToWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            } else if (n.kind == "path") ShellExecuteW(nullptr, L"open", Text::ToWide(n.target).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }).detach();
        Log::Write("круговое меню: " + n.name);
    }

    void Close(int picked) {
        if (g_state != St::Open) return;
        g_picked = picked;
        g_state = St::Closing;
        g_closeAt = Now();
        if (picked >= 0 && picked < static_cast<int>(g_items.size())) Launch(g_items[picked]);
    }

    void Back() { // из папки — на круг выше; с верхнего — закрыть
        if (g_path.empty()) { Close(-1); return; }
        g_path.pop_back();
        ShowLevel();
    }

    void Activate(int i) {
        if (i == kCenter) { Back(); return; }
        if (i < 0 || i >= static_cast<int>(g_items.size())) return;
        if (g_items[i].kind == "folder") { // папка — следующий круг на том же месте
            g_path.push_back(i);
            g_fromDrag = false;
            ShowLevel();
        } else Close(i);
    }

    // Хуки мыши и клавиатуры — в своём потоке, который только ждёт сообщений: Windows ждёт ответа хука на каждое
    // движение мыши, и если бы он стоял в потоке рисования, курсор подтормаживал бы на каждом кадре меню
    LRESULT CALLBACK MouseHook(int code, WPARAM wp, LPARAM lp);
    LRESULT CALLBACK KeyHook(int code, WPARAM wp, LPARAM lp);
    void HookLoop() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, GetModuleHandleW(nullptr), 0);
        g_keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyHook, GetModuleHandleW(nullptr), 0);
        if (!g_mouseHook) Log::Write("круговое меню: не удалось поставить хук мыши");
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
        if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
        if (g_keyHook) UnhookWindowsHookEx(g_keyHook);
    }

    // Заранее: дерево и значки всех пунктов (на всех уровнях) — чтобы меню открывалось сразу, без загрузки
    void PrewarmNode(const Node& n, int size) {
        for (const auto& c : n.items) {
            if (c.kind == "app" || c.kind == "path") IconFor(g_icons, c, size);
            if (c.kind == "folder") PrewarmNode(c, size);
        }
    }
    void Prewarm() {
        POINT p{};
        GetCursorPos(&p);
        UINT dx = 96, dy = 96;
        GetDpiForMonitor(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &dx, &dy);
        PrewarmNode(RootCopy(), static_cast<int>(64 * dx / 96.0f));
    }

    void Refresh() {
        g_enabled = Config::Value("pie", "enabled", "1") == "1";
        ParseTrigger(Config::Value("pie", "trigger", "0|m|3"));
        g_tapOpen = Config::Value("pie", "tap_open", "0") == "1";
        QUERY_USER_NOTIFICATION_STATE q{};
        g_fullscreen = SUCCEEDED(SHQueryUserNotificationState(&q)) && (q == QUNS_RUNNING_D3D_FULL_SCREEN || q == QUNS_BUSY || q == QUNS_PRESENTATION_MODE);
    }

    LRESULT CALLBACK MouseHook(int code, WPARAM wp, LPARAM lp) {
        if (code != HC_ACTION) return CallNextHookEx(nullptr, code, wp, lp);
        const auto* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        if (m->dwExtraInfo == kMarker) return CallNextHookEx(nullptr, code, wp, lp);
        bool down = false;
        const int button = ButtonOf(wp, m, down);
        if (g_capture && button) { // записываем сочетание: кнопка мыши (левая — только с клавишей)
            if (down && (button != 1 || Mods())) {
                std::lock_guard lock(g_captureMutex);
                g_captured = std::to_string(Mods()) + "|m|" + std::to_string(button);
                g_capturedReady = true;
                g_capture = false;
                g_eatUp[button] = true;
                return 1;
            }
            if (down) return CallNextHookEx(nullptr, code, wp, lp);
        }
        switch (g_state.load()) {
        case St::Idle:
            if (down && button && MouseTrigger(button, Mods()) && g_enabled && !g_fullscreen) { // в игре на весь экран — кнопки игре
                g_state = St::Pending;
                g_heldButton = button;
                g_keyOpened = false;
                g_down = m->pt;
                g_downTick = GetTickCount64();
                PostMessageW(g_hwnd, kDownMsg, 0, 0); // таймер ставит поток меню
                return 1;
            }
            break;
        case St::Pending:
            if (!down && button && button == g_heldButton) {
                if (g_tapOpen) { // короткое нажатие открывает меню — оно остаётся, выбор щелчком
                    g_heldButton = 0;
                    g_fromDrag = false;
                    g_state = St::Open;
                    PostMessageW(g_hwnd, kOpenMsg, 0, 0);
                    return 1;
                }
                g_state = St::Idle; // коротко нажали — обычный щелчок этой кнопкой
                PostMessageW(g_hwnd, kClickMsg, static_cast<WPARAM>(button), 0);
                return 1;
            }
            // рванули мышью с зажатой кнопкой — открыть сразу; но не на быстром касании, пока мышь и так едет (тачпад)
            if (wp == WM_MOUSEMOVE && GetTickCount64() - g_downTick > 150 && (std::abs(m->pt.x - g_down.x) > 40 || std::abs(m->pt.y - g_down.y) > 40)) {
                g_fromDrag = true;
                g_state = St::Open;
                PostMessageW(g_hwnd, kOpenMsg, 0, 0);
            }
            break;
        case St::Open:
            if (!down && button && button == g_heldButton) { g_heldButton = 0; PostMessageW(g_hwnd, kReleaseMsg, 0, 0); return 1; }
            if (down && button) {
                g_eatUp[button] = true;
                if (MouseTrigger(button, -1)) PostMessageW(g_hwnd, kCloseMsg, 0, 0); // ещё раз кнопкой-сочетанием — закрыть
                else if (button == 1) PostMessageW(g_hwnd, kPickMsg, 0, 0);
                else if (button == 2) PostMessageW(g_hwnd, kBackMsg, 0, 0);
                else PostMessageW(g_hwnd, kCloseMsg, 0, 0);
                return 1;
            }
            if (wp == WM_MOUSEWHEEL || wp == WM_MOUSEHWHEEL) return 1;
            break;
        case St::Closing:
            break;
        }
        if (!down && button && g_eatUp[button]) { g_eatUp[button] = false; return 1; }
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    LRESULT CALLBACK KeyHook(int code, WPARAM wp, LPARAM lp) {
        if (code != HC_ACTION) return CallNextHookEx(nullptr, code, wp, lp);
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        if (k->dwExtraInfo == kMarker || (k->flags & LLKHF_INJECTED)) return CallNextHookEx(nullptr, code, wp, lp);
        const bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN, up = wp == WM_KEYUP || wp == WM_SYSKEYUP;
        const DWORD vk = k->vkCode;
        if (g_capture && down && !IsModifierKey(vk)) { // записываем сочетание клавиш; Esc — отмена
            std::lock_guard lock(g_captureMutex);
            g_captured = vk == VK_ESCAPE ? std::string() : std::to_string(Mods()) + "|k|" + std::to_string(vk);
            g_capturedReady = true;
            g_capture = false;
            return 1;
        }
        const bool keyTrigger = KeyTrigger(static_cast<int>(vk), -1);
        if (keyTrigger && down && g_state == St::Idle && KeyTrigger(static_cast<int>(vk), Mods()) && g_enabled && !g_fullscreen) {
            POINT p{};
            GetCursorPos(&p);
            g_down = p;
            g_keyOpened = true;
            g_fromDrag = false;   // отпустили клавишу посередине — меню остаётся, можно щёлкнуть
            g_state = St::Open;
            PostMessageW(g_hwnd, kOpenMsg, 0, 0);
            return 1;
        }
        static DWORD s_eatKeyUp = 0;
        if (up && s_eatKeyUp == vk) { s_eatKeyUp = 0; return 1; }
        if (keyTrigger && down && !g_keyOpened && g_state == St::Open) { // меню открыто, нажали сочетание ещё раз — убрать
            s_eatKeyUp = vk;
            PostMessageW(g_hwnd, kCloseMsg, 0, 0);
            return 1;
        }
        if (keyTrigger && g_keyOpened && (g_state == St::Open || g_state == St::Closing)) { // держим клавишу: повторы глотаем, отпустили — выбор
            if (up) { g_keyOpened = false; PostMessageW(g_hwnd, kReleaseMsg, 0, 0); }
            return 1;
        }
        if (g_state == St::Open && down) {
            if (vk == VK_ESCAPE) { PostMessageW(g_hwnd, kCloseMsg, 0, 0); return 1; }
            if (vk == VK_BACK) { PostMessageW(g_hwnd, kBackMsg, 0, 0); return 1; }
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_TIMER:
            if (wp == 1) {
                KillTimer(hwnd, 1);
                if (g_state == St::Pending) { g_fromDrag = false; OpenAt(g_down); }
            } else if (wp == 2) { Refresh(); if (g_state == St::Idle && g_treeDirty.exchange(false)) Prewarm(); }
            return 0;
        case kDownMsg:
            if (g_state == St::Pending) SetTimer(hwnd, 1, kHoldMs, nullptr);
            return 0;
        case kOpenMsg:
            KillTimer(hwnd, 1);
            OpenAt(g_down);
            return 0;
        case kPreviewMsg:
            if (lp) { std::unique_ptr<std::string> file(reinterpret_cast<std::string*>(lp)); g_dumpFile = *file; g_dumpHover = static_cast<int>(wp) - 1; }
            if (g_state == St::Idle) {
                POINT p{};
                GetCursorPos(&p);
                g_fromDrag = false;
                OpenAt(p);
            }
            return 0;
        case kClickMsg: {
            KillTimer(hwnd, 1);
            INPUT in[2] = {};
            in[0].type = in[1].type = INPUT_MOUSE;
            const int b = static_cast<int>(wp);
            in[0].mi.dwFlags = b == 1 ? MOUSEEVENTF_LEFTDOWN : b == 2 ? MOUSEEVENTF_RIGHTDOWN : b >= 4 ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_MIDDLEDOWN;
            in[1].mi.dwFlags = b == 1 ? MOUSEEVENTF_LEFTUP : b == 2 ? MOUSEEVENTF_RIGHTUP : b >= 4 ? MOUSEEVENTF_XUP : MOUSEEVENTF_MIDDLEUP;
            if (b >= 4) in[0].mi.mouseData = in[1].mi.mouseData = b == 4 ? XBUTTON1 : XBUTTON2;
            in[0].mi.dwExtraInfo = in[1].mi.dwExtraInfo = kMarker;
            SendInput(2, in, sizeof(INPUT));
            return 0;
        }
        case kReleaseMsg:
            // отпустили над пунктом — выбрать; в середине сразу после зажатия — меню остаётся, можно щёлкать
            if (g_state != St::Open) return 0;
            if (g_hover >= 0) Activate(g_hover);
            // отпустили почти сразу и ничего не выбрали (касание тачпада тремя пальцами, быстрый рывок) — меню остаётся, выбираешь щелчком
            else if (g_fromDrag && Now() - g_openAt > 0.45) Close(-1);
            return 0;
        case kPickMsg:
            if (g_state == St::Open) Activate(g_hover);
            return 0;
        case kBackMsg:
            if (g_state == St::Open) Back();
            return 0;
        case kCloseMsg:
            Close(-1);
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void Loop() {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ULONG_PTR token = 0;
        GdiplusStartupInput input;
        GdiplusStartup(&token, &input, nullptr);
        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ZaviriaPieMenu";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        g_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, wc.lpszClassName, L"",
                                 WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
        Refresh();
        SetTimer(g_hwnd, 2, 2000, nullptr); // раз в 2 с: включено ли меню и не идёт ли игра на весь экран
        Prewarm();
        g_treeDirty = false;
        std::thread(HookLoop).detach();
        MSG msg{};
        for (;;) {
            if (g_state == St::Open || g_state == St::Closing) { // анимация: кадр в такт монитору, сообщения — между кадрами
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    if (msg.message == WM_QUIT) return;
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                if (g_state == St::Closing && Now() - g_closeAt > 0.36) {
                    ShowWindow(g_hwnd, SW_HIDE);
                    g_state = St::Idle;
                    continue;
                }
                if (g_state == St::Open || g_state == St::Closing) Render();
                if (FAILED(DwmFlush())) Sleep(16);
            } else {
                if (GetMessageW(&msg, nullptr, 0, 0) <= 0) break;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
}

void PieMenu::Start() { std::thread(Loop).detach(); }

Node PieMenu::Root() { return RootCopy(); }

void PieMenu::SetRoot(const Node& root) {
    std::lock_guard lock(g_treeMutex);
    g_root = root;
    g_root.kind = "folder";
    g_loaded = g_haveFile = true;
    g_treeDirty = true;
    std::ofstream out(TreeFile(), std::ios::binary | std::ios::trunc);
    out << ToJson(g_root).dump(1);
}

void PieMenu::Preview() { if (g_hwnd) PostMessageW(g_hwnd, kPreviewMsg, 0, 0); }

void PieMenu::Dump(const std::string& file, int hover) {
    if (g_hwnd) PostMessageW(g_hwnd, kPreviewMsg, static_cast<WPARAM>(hover + 1), reinterpret_cast<LPARAM>(new std::string(file)));
}

std::vector<unsigned char> PieMenu::RenderPicture(const std::vector<int>& path, int size) {
    static IconCache s_icons; // значки для картинки в настройках — в потоке окна
    const Node root = RootCopy();
    const Node* level = Level(root, path);
    if (!level) level = &root;
    const float s = size / (2.0f * Extent(level->items.size(), 1.0f));
    std::vector<unsigned char> px(static_cast<size_t>(size) * size * 4, 0);
    {
        Bitmap canvas(size, size, size * 4, PixelFormat32bppPARGB, px.data());
        Graphics g(&canvas);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        Frame f;
        f.items = &level->items;
        f.nested = !path.empty();
        f.label = false;
        DrawLevel(g, size * 0.5f, size * 0.5f, s, s_icons, f);
    }
    for (size_t i = 0; i < px.size(); i += 4) { // ImGui ждёт обычную (не умноженную) прозрачность
        const unsigned a = px[i + 3];
        if (a && a < 255) for (int c = 0; c < 3; ++c) px[i + c] = static_cast<unsigned char>((std::min)(255u, px[i + c] * 255u / a));
    }
    return px;
}

std::string PieMenu::TriggerName(const std::string& t) {
    int mods = 0, code = 3;
    char kind = 'm';
    if (std::sscanf(t.c_str(), "%d|%c|%d", &mods, &kind, &code) != 3) { mods = 0; kind = 'm'; code = 3; }
    std::string out;
    if (mods & 1) out += "Ctrl + ";
    if (mods & 2) out += "Alt + ";
    if (mods & 4) out += "Shift + ";
    if (mods & 8) out += "Win + ";
    if (kind == 'm') {
        static const char* kNames[] = { "", "левая кнопка мыши", "правая кнопка мыши", "колёсико мыши", "боковая кнопка мыши 1", "боковая кнопка мыши 2" };
        out += Lang::Tr(kNames[std::clamp(code, 1, 5)]);
    } else {
        wchar_t name[64] = {};
        UINT scan = MapVirtualKeyW(static_cast<UINT>(code), MAPVK_VK_TO_VSC);
        const bool ext = code == VK_INSERT || code == VK_DELETE || code == VK_HOME || code == VK_END || code == VK_PRIOR || code == VK_NEXT ||
                         code == VK_LEFT || code == VK_RIGHT || code == VK_UP || code == VK_DOWN;
        if (GetKeyNameTextW(static_cast<LONG>((scan << 16) | (ext ? (1 << 24) : 0)), name, 64) > 0) out += Text::ToUtf8(name);
        else out += "#" + std::to_string(code);
    }
    return out;
}

std::atomic<int> g_captureOwner{ 0 };
void PieMenu::StartCapture(int owner) {
    std::lock_guard lock(g_captureMutex);
    g_capturedReady = false;
    g_captureOwner = owner;
    g_capture = true;
}
bool PieMenu::Capturing(int owner) { return g_capture.load() && g_captureOwner == owner; }
bool PieMenu::TakeCaptured(std::string& trigger, int owner) {
    std::lock_guard lock(g_captureMutex);
    if (!g_capturedReady || g_captureOwner != owner) return false;
    g_capturedReady = false;
    trigger = g_captured;
    return true;
}
void PieMenu::ReloadTrigger() { ParseTrigger(Config::Value("pie", "trigger", "0|m|3")); g_tapOpen = Config::Value("pie", "tap_open", "0") == "1"; }
