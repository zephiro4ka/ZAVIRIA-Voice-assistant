#include "Overlay.h"
#include "EcoMode.h"

#include "Lang.h"
#include "Platform.h"
#include "Log.h"
#include "MediaNow.h"
#include "Settings.h"
#include "State.h"
#include "Skills.h"
#include "SystemMonitor.h"
#include "TextUtils.h"
#include "Theme.h"
#include "Weather.h"
#include "../ImGui/imgui_internal.h"

#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

namespace {
    std::atomic<bool> g_dirty{ true };
    std::mutex g_dumpMutex;
    std::string g_dumpFolder;

    struct Options { bool res = false, pinned = false, island = false, pages[3] = { true, true, true }; int opacity = 75, place = 0, x = -1, y = -1, w = 0, h = 0; } g_opt;

    float S(float v) { return v * Theme::dpi; }

    Color Argb(int a, const Theme::Color& c) { return Color(static_cast<BYTE>(std::clamp(a, 0, 255)), static_cast<BYTE>(c.r), static_cast<BYTE>(c.g), static_cast<BYTE>(c.b)); }

    void RoundedPath(GraphicsPath& path, REAL x, REAL y, REAL w, REAL h, REAL r) {
        const REAL d = r * 2;
        path.AddArc(x, y, d, d, 180, 90);
        path.AddArc(x + w - d, y, d, d, 270, 90);
        path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
        path.AddArc(x, y + h - d, d, d, 90, 90);
        path.CloseFigure();
    }

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
        void Present(int x, int y, int alpha) const {
            POINT source{ 0, 0 }, target{ x, y };
            SIZE size{ width, height };
            BLENDFUNCTION blend{ AC_SRC_OVER, 0, static_cast<BYTE>(std::clamp(alpha, 0, 255)), AC_SRC_ALPHA };
            UpdateLayeredWindow(hwnd, nullptr, &target, &size, dc, &source, 0, &blend, ULW_ALPHA);
        }
    };

    Surface g_res, g_island;
    float g_islandT = 0, g_islandE = 0, g_resAlpha = 1;
    struct Resize { int edges = 0; POINT start{}; RECT rect{}; } g_resize; // 1 слева, 2 справа, 4 сверху, 8 снизу
    ULONGLONG g_resDrawn = 0, g_islandLeft = 0;
    bool g_resShown = false;
    std::atomic<bool> g_islandShown{ false }; // читает и Notch

    // ---------------------------------------------------------------- окно нагрузки
    constexpr float kResW = 224, kHeader = 30, kRow = 27, kGraph = 34, kMinW = 190, kMinRow = 27;

    void Text2(Graphics& g, const WCHAR* text, REAL x, REAL y, REAL size, const Color& color, bool bold, REAL maxWidth = 0) {
        std::wstring translated;
        if (Lang::IsEnglish()) { translated = Text::ToWide(Lang::Tr(Text::ToUtf8(text).c_str())); text = translated.c_str(); }
        FontFamily family(L"Segoe UI");
        Font font(&family, size, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
        SolidBrush brush(color);
        StringFormat format;
        format.SetTrimming(StringTrimmingEllipsisCharacter);
        format.SetFormatFlags(StringFormatFlagsNoWrap);
        g.DrawString(text, -1, &font, RectF(x, y, maxWidth > 0 ? maxWidth : 2000, size * 1.5f), &format, &brush);
    }

    void Graph(Graphics& g, REAL x, REAL y, REAL w, REAL h, const std::array<float, SystemMonitor::kHistory>& data, const Theme::Color& color) {
        SolidBrush back(Color(50, 255, 255, 255));
        g.FillRectangle(&back, x, y, w, h);
        PointF points[SystemMonitor::kHistory + 2];
        for (int i = 0; i < SystemMonitor::kHistory; ++i)
            points[i] = PointF(x + w * i / (SystemMonitor::kHistory - 1), y + h - h * std::clamp(data[i], 0.0f, 100.0f) / 100.0f);
        Pen pen(Argb(255, color), S(1.4f));
        g.DrawLines(&pen, points, SystemMonitor::kHistory);
        points[SystemMonitor::kHistory] = PointF(x + w, y + h);
        points[SystemMonitor::kHistory + 1] = PointF(x, y + h);
        SolidBrush fill(Argb(70, color));
        g.FillPolygon(&fill, points, SystemMonitor::kHistory + 2);
    }

    int ResHeight(const SystemMonitor::Stats& s) {
        const int rows = 4 + (s.hasGpu ? 1 : 0) + (s.hasIgpu ? 1 : 0);
        return static_cast<int>(S(kHeader + kRow * rows + kGraph + 10));
    }

    Color Gdi(ImU32 c, int alpha = -1) { // цвет ImGui (ABGR) → GDI+
        return Color(static_cast<BYTE>(alpha >= 0 ? alpha : (c >> 24) & 0xFF), static_cast<BYTE>(c & 0xFF), static_cast<BYTE>((c >> 8) & 0xFF), static_cast<BYTE>((c >> 16) & 0xFF));
    }

    // Окно нагрузки (вариант «строки с полосками»): название слева, цифры справа, под ними тонкая полоска
    // в цветах логотипа; внизу — плавный график процессора за минуту. Цвета — из темы приложения.
    void DrawRes(Graphics& g, int W, int H, const SystemMonitor::Stats& s) {
        g.Clear(Color(0, 0, 0, 0));
        GraphicsPath panel;
        RoundedPath(panel, 0.5f, 0.5f, W - 1.0f, H - 1.0f, S(18));
        SolidBrush bg(Gdi(Theme::Surface(1.0f), 236));
        g.FillPath(&bg, &panel);
        Pen edge(Gdi(Theme::kInk, 26), 1.0f);
        g.DrawPath(&edge, &panel);

        const Color ink = Gdi(Theme::kInk), soft = Gdi(Theme::kInkMuted);
        const Theme::Color accent = Theme::Accent(), pink = Theme::Accent().Mix({ 244, 114, 182 }, 0.6f);
        Text2(g, L"Нагрузка", S(12), S(7), S(13), ink, true);
        // кнопки: закрепить и закрыть
        const REAL by = S(15);
        Pen button(soft, S(1.6f));
        const REAL pinX = W - S(40), closeX = W - S(18);
        if (g_opt.pinned) { SolidBrush on(Argb(255, accent)); g.FillEllipse(&on, pinX - S(5), by - S(5), S(10), S(10)); }
        else g.DrawEllipse(&button, pinX - S(5), by - S(5), S(10), S(10));
        g.DrawLine(&button, closeX - S(4.5f), by - S(4.5f), closeX + S(4.5f), by + S(4.5f));
        g.DrawLine(&button, closeX + S(4.5f), by - S(4.5f), closeX - S(4.5f), by + S(4.5f));

        const REAL padX = S(12), inner = W - padX * 2;
        const int rows = 4 + (s.hasGpu ? 1 : 0) + (s.hasIgpu ? 1 : 0);
        const REAL rowH = S(kRow), k = 1.0f; // высота окна — по содержимому, строки плотные
        REAL y = S(kHeader);
        wchar_t line[96];
        FontFamily family(L"Segoe UI");
        Font valueFont(&family, S(11.5f) * k, FontStyleRegular, UnitPixel);
        auto row = [&](const WCHAR* name, const WCHAR* value, const WCHAR* shortValue, float fraction, const Color& valueColor) {
            // цифры справа — измеряем; не влезают рядом с названием — показываем короткие (только проценты)
            std::wstring v = value;
            RectF box, nameBox;
            Font nameFont(&family, S(12.5f) * k, FontStyleBold, UnitPixel);
            g.MeasureString(name, -1, &nameFont, PointF(0, 0), &nameBox);
            g.MeasureString(v.c_str(), -1, &valueFont, PointF(0, 0), &box);
            if (nameBox.Width + box.Width + S(8) > inner) { v = shortValue; g.MeasureString(v.c_str(), -1, &valueFont, PointF(0, 0), &box); }
            const REAL vw = std::min(box.Width, inner * 0.62f);
            Text2(g, name, padX, y, S(12.5f) * k, ink, true, inner - vw - S(8));
            SolidBrush vb(valueColor);
            StringFormat right;
            right.SetAlignment(StringAlignmentFar);
            right.SetTrimming(StringTrimmingEllipsisCharacter);
            right.SetFormatFlags(StringFormatFlagsNoWrap);
            g.DrawString(v.c_str(), -1, &valueFont, RectF(W - padX - vw - S(2), y + S(1), vw + S(2), S(16) * k), &right, &vb);
            const REAL barY = y + S(18), barH = S(4);
            GraphicsPath track;
            RoundedPath(track, padX, barY, inner, barH, barH / 2);
            SolidBrush trackBrush(Gdi(Theme::kInk, 22));
            g.FillPath(&trackBrush, &track);
            const REAL fill = std::clamp(fraction, 0.0f, 1.0f) * inner;
            if (fill > barH) {
                GraphicsPath bar;
                RoundedPath(bar, padX, barY, fill, barH, barH / 2);
                LinearGradientBrush grad(PointF(padX, barY), PointF(padX + inner, barY), Argb(255, accent), Argb(255, pink));
                g.FillPath(&grad, &bar);
            }
            y += rowH;
        };
        const Color hot = Gdi(IM_COL32(248, 113, 113, 255));
        auto temp = [&](float t) { return t >= 80 ? hot : soft; };
        swprintf_s(line, L"%d%% · %.1f ГГц%s", static_cast<int>(s.cpu), s.cpuFreqGhz, s.cpuTemp > 0 ? (L" · " + std::to_wstring(static_cast<int>(s.cpuTemp)) + L"°").c_str() : L"");
        wchar_t brief[32];
        swprintf_s(brief, L"%d%%", static_cast<int>(s.cpu));
        row(L"Процессор", line, brief, s.cpu / 100.0f, temp(s.cpuTemp));
        swprintf_s(line, L"%.1f / %.1f ГБ", s.ramUsedGb, s.ramTotalGb);
        swprintf_s(brief, L"%d%%", static_cast<int>(s.ram));
        row(L"Память", line, brief, s.ram / 100.0f, soft);
        if (s.hasGpu) {
            swprintf_s(line, L"%d%%%s", static_cast<int>(s.gpu), s.gpuTemp > 0 ? (L" · " + std::to_wstring(static_cast<int>(s.gpuTemp)) + L"°").c_str() : L""); // видеопамять — в «Процессах», тут коротко
            swprintf_s(brief, L"%d%%", static_cast<int>(s.gpu));
            row(L"Видеокарта", line, brief, s.gpu / 100.0f, temp(s.gpuTemp));
        }
        if (s.hasIgpu) {
            swprintf_s(line, L"%d%%", static_cast<int>(s.igpu));
            row(L"Встроенная", line, line, s.igpu / 100.0f, soft);
        }
        swprintf_s(line, L"%d%% · %.1f МБ/с", static_cast<int>(s.diskActive), s.diskReadMBs);
        swprintf_s(brief, L"%d%%", static_cast<int>(s.diskActive));
        row(L"Диск", line, brief, s.diskActive / 100.0f, soft);
        swprintf_s(line, L"↓ %.0f  ↑ %.0f КБ/с", s.netDownKbps, s.netUpKbps);
        row(L"Сеть", line, line, std::clamp((s.netDownKbps + s.netUpKbps) / 10240.0f, 0.0f, 1.0f), soft); // полная полоска — 10 МБ/с

        // график процессора за минуту: плавная линия и прозрачная заливка
        const REAL gx = padX, gy = y + S(2), gw = inner, gh = std::max(S(14), static_cast<REAL>(H) - gy - S(10));
        PointF points[SystemMonitor::kHistory + 2];
        for (int i = 0; i < SystemMonitor::kHistory; ++i)
            points[i] = PointF(gx + gw * i / (SystemMonitor::kHistory - 1), gy + gh - gh * std::clamp(s.cpuHistory[i], 0.0f, 100.0f) / 100.0f);
        points[SystemMonitor::kHistory] = PointF(gx + gw, gy + gh);
        points[SystemMonitor::kHistory + 1] = PointF(gx, gy + gh);
        LinearGradientBrush area(PointF(0, gy), PointF(0, gy + gh), Argb(70, accent), Argb(0, accent));
        g.FillPolygon(&area, points, SystemMonitor::kHistory + 2);
        Pen line2(Argb(255, accent), S(1.6f));
        line2.SetLineJoin(LineJoinRound);
        g.DrawCurve(&line2, points, SystemMonitor::kHistory, 0.4f);
    }

    // ---------------------------------------------------------------- островок
    // Страницы: «Сейчас играет», «Время и погода», «Ближайшее» (таймеры Ани).
    // Листаются стрелками, колесом и точками внизу. Плашку рисует сама тема (Theme::Plate),
    // поэтому тёмная и светлая тема, «капли» и «стикеры» здесь такие же, как в окне.
    constexpr float kIslandW = 360, kIslandH = 172, kPillW = 250, kPillH = 44, kMargin = 16; // поле вокруг плашки — под тень и наклон
    enum Page { kMusic, kTime, kNext, kPageCount };

    // Theme::Plate и Theme::Pick пишут треугольники в ImDrawList; здесь свой список и перевод его в картинку окна.
    // Рисуем вдвое крупнее и сжимаем — тонкие линии и края выходят гладкими.
    struct Raster {
        ImDrawListSharedData shared;
        ImDrawList list;
        std::vector<float> acc;

        Raster() : list(&shared) {
            shared.SetCircleTessellationMaxError(0.3f);
            shared.CurveTessellationMaxError = 1.25f;
            shared.InitialDrawFlags = ImDrawFlags_AAFill; // линии без текстуры шрифта: её здесь нет
        }
        ImDrawList* Begin() {
            list._ResetForNewFrame();
            list._SetPixelDensity(1.0f);
            list.PushClipRect(ImVec2(-10000, -10000), ImVec2(10000, 10000));
            return &list;
        }

        static bool Owns(float dx, float dy) { return dy > 0 || (dy == 0 && dx > 0); } // общий край достаётся одному из двух треугольников

        void Triangle(const ImDrawVert& va, const ImDrawVert& vb0, const ImDrawVert& vc0, int bw, int bh) {
            constexpr float k = 2.0f;
            const ImDrawVert* vb = &vb0; const ImDrawVert* vc = &vc0;
            float ax = va.pos.x * k, ay = va.pos.y * k, bx = vb->pos.x * k, by = vb->pos.y * k, cx = vc->pos.x * k, cy = vc->pos.y * k;
            float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
            if (std::fabs(area) < 1e-6f) return;
            if (area < 0) { std::swap(vb, vc); std::swap(bx, cx); std::swap(by, cy); area = -area; }
            const int y0 = std::max(0, static_cast<int>(std::floor(std::min({ ay, by, cy })))), y1 = std::min(bh - 1, static_cast<int>(std::ceil(std::max({ ay, by, cy }))));
            const int xa = std::max(0, static_cast<int>(std::floor(std::min({ ax, bx, cx })))), xb = std::min(bw - 1, static_cast<int>(std::ceil(std::max({ ax, bx, cx }))));
            if (y0 > y1 || xa > xb) return;
            auto unpack = [](ImU32 c, float out[4]) { for (int i = 0; i < 4; ++i) out[i] = ((c >> (8 * i)) & 0xFF) / 255.0f; };
            float ca[4], cb[4], cc[4];
            unpack(va.col, ca); unpack(vb->col, cb); unpack(vc->col, cc);
            // края b→c, c→a, a→b: вес вершин a, b, c
            const float ex[3] = { cx - bx, ax - cx, bx - ax }, ey[3] = { cy - by, ay - cy, by - ay };
            const float ox[3] = { bx, cx, ax }, oy[3] = { by, cy, ay };
            const bool own[3] = { Owns(ex[0], ey[0]), Owns(ex[1], ey[1]), Owns(ex[2], ey[2]) };
            for (int y = y0; y <= y1; ++y) {
                const float py = y + 0.5f;
                float lo = static_cast<float>(xa), hi = static_cast<float>(xb);
                bool empty = false;
                for (int e = 0; e < 3 && !empty; ++e) { // где по этой строке край «за нами»: w(px) = base - ey * px
                    const float base = ex[e] * (py - oy[e]) + ey[e] * ox[e], slope = -ey[e];
                    if (slope == 0) { if (base < 0) empty = true; continue; }
                    const float edge = -base / slope - 0.5f;
                    if (slope > 0) lo = std::max(lo, std::floor(edge)); else hi = std::min(hi, std::ceil(edge));
                }
                if (empty || lo > hi) continue;
                float* row = acc.data() + static_cast<size_t>(y) * bw * 4;
                for (int x = static_cast<int>(lo); x <= static_cast<int>(hi); ++x) {
                    const float px = x + 0.5f;
                    float w[3];
                    bool in = true;
                    for (int e = 0; e < 3; ++e) {
                        w[e] = ex[e] * (py - oy[e]) - ey[e] * (px - ox[e]);
                        if (w[e] < 0 || (w[e] == 0 && !own[e])) { in = false; break; }
                    }
                    if (!in) continue;
                    const float l0 = w[0] / area, l1 = w[1] / area, l2 = 1.0f - l0 - l1;
                    float col[4];
                    for (int i = 0; i < 4; ++i) col[i] = ca[i] * l0 + cb[i] * l1 + cc[i] * l2;
                    const float alpha = std::clamp(col[3], 0.0f, 1.0f);
                    if (alpha <= 0.0f) continue;
                    float* d = row + x * 4;
                    for (int i = 0; i < 3; ++i) d[i] = col[i] * alpha + d[i] * (1 - alpha);
                    d[3] = alpha + d[3] * (1 - alpha);
                }
            }
        }

        // в out — готовые пиксели окна (BGRA с умноженной прозрачностью)
        void Paint(std::vector<uint32_t>& out, int w, int h) {
            const int bw = w * 2, bh = h * 2;
            acc.assign(static_cast<size_t>(bw) * bh * 4, 0.0f);
            const ImDrawVert* v = list.VtxBuffer.Data;
            const ImDrawIdx* idx = list.IdxBuffer.Data;
            for (int i = 0; i + 2 < list.IdxBuffer.Size; i += 3) Triangle(v[idx[i]], v[idx[i + 1]], v[idx[i + 2]], bw, bh);
            out.assign(static_cast<size_t>(w) * h, 0);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    float sum[4] = {};
                    for (int sy = 0; sy < 2; ++sy)
                        for (int sx = 0; sx < 2; ++sx) {
                            const float* p = acc.data() + (static_cast<size_t>(y * 2 + sy) * bw + x * 2 + sx) * 4;
                            for (int i = 0; i < 4; ++i) sum[i] += p[i];
                        }
                    auto byte = [](float f) { return static_cast<uint32_t>(std::clamp(f * 0.25f * 255.0f + 0.5f, 0.0f, 255.0f)); };
                    out[static_cast<size_t>(y) * w + x] = (byte(sum[3]) << 24) | (byte(sum[0]) << 16) | (byte(sum[1]) << 8) | byte(sum[2]);
                }
        }
    };

    struct IslandData {
        MediaNow::Track track;
        Weather::Today weather;
        std::vector<Skills::Countdown> timers;
        Clock now = 0;          // NowMs() — для таймеров
        SYSTEMTIME local{};
    };

    // Что сейчас на экране: страница, листание, наведение. Трогается только из потока оверлея.
    struct IslandState {
        int page = kTime, old = -1, hover = 0, wheel = 0;
        float slide = 0;                  // 1 → 0: новая страница въезжает справа, -1 → 0 — слева
        std::vector<int> ready;           // страницы, которые сейчас есть
    } g_is;

    struct PlateKey {
        int w = 0, h = 0, style = 0, hover = 0, glow = 0;
        bool light = false, pick = false;
        ImU32 fill = 0, accent = 0;
        bool operator==(const PlateKey&) const = default;
    };
    Raster* g_raster = nullptr;
    PlateKey g_plateKey;
    std::vector<uint32_t> g_plate;

    Color FromIm(ImU32 c) { return Color(static_cast<BYTE>(c >> 24), static_cast<BYTE>(c & 0xFF), static_cast<BYTE>((c >> 8) & 0xFF), static_cast<BYTE>((c >> 16) & 0xFF)); }
    Color Fade(const Color& c, float a) { return Color(static_cast<BYTE>(c.GetA() * std::clamp(a, 0.0f, 1.0f)), c.GetR(), c.GetG(), c.GetB()); }

    struct Inks { Color ink, soft, muted, accent, accentInk; };
    Inks ThemeInks() {
        const bool paper = Theme::Paper();
        const auto accent = paper ? Theme::Accent() : Theme::Accent().Mix({ 255, 255, 255 }, 0.35f);
        const auto accentInk = paper ? Theme::Accent().Mix({ 0, 0, 0 }, 0.2f) : Theme::Accent().Mix({ 255, 255, 255 }, 0.55f);
        return { FromIm(Theme::kInk), FromIm(Theme::kInkSoft), FromIm(Theme::kInkMuted), Argb(255, accent), Argb(255, accentInk) };
    }

    std::wstring Tw(const char* ru) { return Text::ToWide(Lang::IsEnglish() ? Lang::Tr(ru) : ru); }

    void Label(Graphics& g, const std::wstring& text, REAL x, REAL y, REAL w, REAL size, const Color& color, bool bold, int align = 0) {
        if (w < 4 || color.GetA() == 0) return;
        FontFamily family(L"Segoe UI");
        Font font(&family, size, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
        SolidBrush brush(color);
        StringFormat format;
        format.SetTrimming(StringTrimmingEllipsisCharacter);
        format.SetFormatFlags(StringFormatFlagsNoWrap);
        format.SetAlignment(align == 1 ? StringAlignmentCenter : align == 2 ? StringAlignmentFar : StringAlignmentNear);
        g.DrawString(text.c_str(), -1, &font, RectF(x, y, w, size * 1.45f), &format, &brush);
    }

    void Icon(Graphics& g, int kind, REAL cx, REAL cy, REAL r, const Color& color) { // 0 назад, 1 пауза, 2 играть, 3 вперёд
        SolidBrush brush(color);
        if (kind == 1) { g.FillRectangle(&brush, cx - r * 0.7f, cy - r, r * 0.5f, r * 2); g.FillRectangle(&brush, cx + r * 0.2f, cy - r, r * 0.5f, r * 2); return; }
        if (kind == 2) { PointF p[3] = { {cx - r * 0.7f, cy - r}, {cx + r, cy}, {cx - r * 0.7f, cy + r} }; g.FillPolygon(&brush, p, 3); return; }
        const REAL side = kind == 3 ? 1.0f : -1.0f;
        PointF p[3] = { {cx - side * r * 0.8f, cy - r}, {cx + side * r * 0.5f, cy}, {cx - side * r * 0.8f, cy + r} };
        g.FillPolygon(&brush, p, 3);
        g.FillRectangle(&brush, cx + (side > 0 ? r * 0.5f : -r * 0.75f), cy - r, r * 0.25f, r * 2);
    }

    // Волна пройденной части: бежит, пока играет звук, на паузе замирает
    void Wave(Graphics& g, REAL x, REAL y, REAL w, REAL amplitude, const Color& color, REAL thick, bool moving) {
        if (w < 2) return;
        const float phase = moving ? static_cast<float>(GetTickCount64() % 100000) / 1000.0f * 5.0f : 0.0f;
        const int steps = std::clamp(static_cast<int>(w), 2, 500);
        std::vector<PointF> points(steps + 1);
        for (int i = 0; i <= steps; ++i) {
            const REAL px = x + w * i / steps, edge = std::min(px - x, x + w - px) / S(14); // к концам волна плавно сходит на нет
            const REAL fade = std::clamp(edge, 0.0f, 1.0f);
            points[i] = PointF(px, y + std::sin((px - x) * 0.06f / Theme::dpi - phase) * amplitude * fade * fade * (3 - 2 * fade));
        }
        Pen pen(color, thick);
        pen.SetLineJoin(LineJoinRound);
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        g.DrawLines(&pen, points.data(), steps + 1);
    }

    std::wstring Clock2(double seconds) {
        wchar_t t[16];
        swprintf_s(t, L"%d:%02d", static_cast<int>(seconds) / 60, static_cast<int>(seconds) % 60);
        return t;
    }
    std::wstring Countdown(long long s) { // «4:12», «1:04:12»
        s = std::max(0LL, s);
        wchar_t t[24];
        if (s >= 3600) swprintf_s(t, L"%lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60);
        else swprintf_s(t, L"%lld:%02lld", s / 60, s % 60);
        return t;
    }
    std::wstring Span(long long s) { // «8 ч 14 мин», «4 мин 12 с»
        s = std::max(0LL, s);
        const bool en = Lang::IsEnglish();
        const long long h = s / 3600, m = s / 60 % 60, sec = s % 60;
        wchar_t t[48];
        if (h && m) swprintf_s(t, en ? L"%lld h %lld min" : L"%lld ч %lld мин", h, m);
        else if (h) swprintf_s(t, en ? L"%lld h" : L"%lld ч", h);
        else if (m && sec && m < 10) swprintf_s(t, en ? L"%lld min %lld s" : L"%lld мин %lld с", m, sec);
        else if (m) swprintf_s(t, en ? L"%lld min" : L"%lld мин", m);
        else swprintf_s(t, en ? L"%lld s" : L"%lld с", sec);
        return t;
    }
    std::wstring In(const char* what, long long s) { return Tw(what) + L" " + Span(s); }

    std::wstring DateLine(const SYSTEMTIME& t, bool withTime) {
        static const wchar_t* kRuDays[] = { L"вс", L"пн", L"вт", L"ср", L"чт", L"пт", L"сб" };
        static const wchar_t* kEnDays[] = { L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat" };
        static const wchar_t* kRuMonths[] = { L"января", L"февраля", L"марта", L"апреля", L"мая", L"июня", L"июля", L"августа", L"сентября", L"октября", L"ноября", L"декабря" };
        static const wchar_t* kEnMonths[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
        const bool en = Lang::IsEnglish();
        const int day = std::clamp<int>(t.wDayOfWeek, 0, 6), month = std::clamp<int>(t.wMonth, 1, 12) - 1;
        wchar_t line[64];
        swprintf_s(line, L"%s, %d %s", en ? kEnDays[day] : kRuDays[day], t.wDay, en ? kEnMonths[month] : kRuMonths[month]);
        std::wstring out = line;
        if (withTime) { swprintf_s(line, L"   %02d:%02d", t.wHour, t.wMinute); out += line; }
        return out;
    }

    int Minutes(const std::string& hhmm) { // «07:12» → 432; -1 — не разобрать
        int h = 0, m = 0;
        return std::sscanf(hhmm.c_str(), "%d:%d", &h, &m) == 2 ? h * 60 + m : -1;
    }

    // Где солнце: днём доля пути от восхода к закату, ночью — от заката к восходу
    struct SunPath { bool known = false, day = true; float f = 0; long long left = 0; }; // left — секунд до заката/восхода
    SunPath Sun(const IslandData& d) {
        SunPath s;
        const int rise = Minutes(d.weather.sunrise), set = Minutes(d.weather.sunset);
        if (!d.weather.ready || rise < 0 || set <= rise) return s;
        const float now = d.local.wHour * 60.0f + d.local.wMinute + d.local.wSecond / 60.0f;
        s.known = true;
        if (now >= rise && now < set) {
            s.f = (now - rise) / (set - rise);
            s.left = static_cast<long long>((set - now) * 60);
        } else {
            const float night = 1440.0f - (set - rise), since = now >= set ? now - set : now + 1440 - set;
            s.day = false;
            s.f = std::clamp(since / night, 0.0f, 1.0f);
            s.left = static_cast<long long>((night - since) * 60);
        }
        return s;
    }

    PointF Arrow(int side, REAL W, REAL H) { return PointF(side ? W - S(17) : S(17), (S(32) + H - S(24)) * 0.5f); }
    REAL DotsY(REAL H) { return H - S(13); }
    REAL DotX(int i, int n, REAL W) { return W * 0.5f + (i - (n - 1) * 0.5f) * S(16); }
    PointF MusicButton(int index, REAL W, REAL H, float e) { // 0 назад, 1 пауза, 2 вперёд
        if (e > 0.5f) return PointF(W * 0.5f + (index - 1) * S(52), S(124));
        return PointF(W - S(82) + index * S(30), H * 0.5f);
    }

    // Что под мышкой: 1/2 — стрелки, 3..5 — кнопки музыки, 10+i — точки
    int HitTest(float x, float y, float W, float H, float e) {
        if (e > 0.5f) {
            if (g_is.ready.size() > 1) {
                for (int side = 0; side < 2; ++side)
                    if (const PointF a = Arrow(side, W, H); std::hypot(x - a.X, y - a.Y) < S(16)) return 1 + side;
                for (int i = 0; i < static_cast<int>(g_is.ready.size()); ++i)
                    if (std::fabs(x - DotX(i, static_cast<int>(g_is.ready.size()), W)) < S(8) && std::fabs(y - DotsY(H)) < S(9)) return 10 + i;
            }
            if (g_is.page == kMusic)
                for (int i = 0; i < 3; ++i)
                    if (const PointF c = MusicButton(i, W, H, e); std::hypot(x - c.X, y - c.Y) < (i == 1 ? S(22) : S(18))) return 3 + i;
        } else if (g_is.page == kMusic) {
            for (int i = 0; i < 3; ++i)
                if (const PointF c = MusicButton(i, W, H, e); std::hypot(x - c.X, y - c.Y) < S(14)) return 3 + i;
        }
        return 0;
    }

    // Подложка: плашка темы и мягкие пятна выбора — перерисовывается, только когда что-то из этого поменялось
    void BuildPlate(int surfaceW, int surfaceH, REAL W, REAL H, float e, int hover, bool pickNext) {
        PlateKey key;
        key.w = surfaceW; key.h = surfaceH; key.style = Theme::Style(); key.hover = hover; key.pick = pickNext;
        key.glow = static_cast<int>(e * 20); key.light = Theme::Paper();
        key.fill = (Theme::SurfaceSolid() & ~IM_COL32_A_MASK) | (246u << IM_COL32_A_SHIFT);
        key.accent = Theme::Accent().WithAlpha(1.0f);
        if (key == g_plateKey && g_plate.size() == static_cast<size_t>(surfaceW) * surfaceH) return;
        g_plateKey = key;
        if (!g_raster) g_raster = new Raster();
        ImDrawList* dl = g_raster->Begin();
        const float m = S(kMargin);
        const ImVec2 a(m, m), b(m + W, m + H);
        Theme::Plate(dl, a, b, key.fill, std::min(S(18 + 8 * e), H * 0.5f), 0.1f + 0.4f * e);
        auto spot = [&](PointF c, REAL r, bool horizontal) { Theme::Pick(dl, ImVec2(m + c.X - r, m + c.Y - r), ImVec2(m + c.X + r, m + c.Y + r), r, 1.0f, horizontal); };
        if (hover == 1 || hover == 2) spot(Arrow(hover - 1, W, H), S(14), false);
        if (hover >= 3 && hover <= 5) spot(MusicButton(hover - 3, W, H, e), e > 0.5f ? S(hover == 4 ? 22.0f : 18.0f) : S(14), false);
        if (hover >= 10) spot(PointF(DotX(hover - 10, static_cast<int>(g_is.ready.size()), W), DotsY(H)), S(9), false);
        if (pickNext) Theme::Pick(dl, ImVec2(m + S(28), m + S(30)), ImVec2(m + W - S(28), m + S(90)), S(14), 1.0f, true);
        g_raster->Paint(g_plate, surfaceW, surfaceH);
    }

    const char* PageName(int page) { return page == kMusic ? "Сейчас играет" : page == kTime ? "Время и погода" : "Ближайшее"; }

    void Header(Graphics& g, int page, REAL W, REAL dx, float a, const IslandData& d, const Inks& c) {
        std::wstring place = page == kMusic ? Text::ToWide(d.track.app) : page == kTime ? Text::ToWide(Lang::IsEnglish() ? Lang::Latin(d.weather.city) : d.weather.city) : Tw("Аня");
        std::wstring title = Tw(PageName(page));
        if (!place.empty()) title += L"  ·  " + place;
        Label(g, title, S(20) + dx, S(10), W * 0.56f - S(20), S(12), Fade(c.accentInk, a), true);
        Label(g, DateLine(d.local, page != kTime), W * 0.56f + dx, S(10), W * 0.44f - S(20), S(12), Fade(c.soft, a), false, 2);
    }

    void MusicPage(Graphics& g, REAL W, REAL H, REAL dx, float a, const IslandData& d, const Inks& c) {
        const auto& t = d.track;
        const REAL x = S(38) + dx, w = W - S(76);
        Label(g, Text::ToWide(t.title.empty() ? Lang::Tr("Сейчас играет") : t.title), x, S(32), w, S(17), Fade(c.ink, a), true);
        Label(g, Text::ToWide(t.artist.empty() ? t.app : t.artist), x, S(56), w, S(13), Fade(c.soft, a), false);
        if (t.duration > 1) {
            const REAL k = static_cast<REAL>(std::clamp(t.position / t.duration, 0.0, 1.0)), barX = x + S(36), barW = w - S(72), barY = S(91);
            Pen track(Fade(c.muted, a * 0.45f), S(1.4f));
            g.DrawLine(&track, barX + barW * k, barY, barX + barW, barY);
            Wave(g, barX, barY, barW * k, S(3.5f), Fade(c.accent, a), S(2.2f), t.playing);
            Label(g, Clock2(t.position), x, barY - S(9), S(36), S(11), Fade(c.soft, a), false);
            Label(g, Clock2(t.duration), x + w - S(36), barY - S(9), S(36), S(11), Fade(c.soft, a), false, 2);
        }
        for (int i = 0; i < 3; ++i) {
            PointF p = MusicButton(i, W, H, 1.0f);
            p.X += dx;
            if (i == 1) {
                SolidBrush circle(Fade(c.accent, a * 0.9f));
                g.FillEllipse(&circle, p.X - S(17), p.Y - S(17), S(34), S(34));
                Icon(g, t.playing ? 1 : 2, p.X, p.Y, S(8), Fade(Color(255, 255, 255, 255), a));
            } else Icon(g, i == 0 ? 0 : 3, p.X, p.Y, S(8), Fade(c.ink, a));
        }
    }

    // Дуга солнца: днём точка идёт по дуге от восхода к закату, ночью — по ложбинке под горизонтом
    void SunArc(Graphics& g, REAL x, REAL y, REAL w, REAL h, const SunPath& sun, const IslandData& d, float a, const Inks& c) {
        const REAL horizon = y + h, cx = x + w * 0.5f, dip = h * 0.34f;
        Pen line(Fade(c.muted, a * 0.7f), S(1.2f));
        REAL dash[2] = { 3, 3 };
        line.SetDashPattern(dash, 2);
        g.DrawLine(&line, x - S(4), horizon, x + w + S(4), horizon);
        Pen arc(Fade(c.muted, a * (sun.day ? 0.8f : 0.45f)), S(1.6f));
        g.DrawArc(&arc, x, y, w, h * 2, 180, 180);
        Pen low(Fade(c.muted, a * (sun.day ? 0.3f : 0.7f)), S(1.2f));
        low.SetDashPattern(dash, 2);
        g.DrawArc(&low, x, horizon - dip, w, dip * 2, 0, 180);
        const float pi = 3.14159265f;
        PointF dot;
        if (sun.day) {
            Pen done(Fade(c.accent, a), S(2.6f));
            done.SetStartCap(LineCapRound);
            done.SetEndCap(LineCapRound);
            if (sun.f > 0.01f) g.DrawArc(&done, x, y, w, h * 2, 180, 180 * sun.f);
            dot = PointF(cx - std::cos(pi * sun.f) * w * 0.5f, horizon - std::sin(pi * sun.f) * h);
            SolidBrush halo(Fade(c.accent, a * 0.25f)), core(Fade(c.accent, a));
            g.FillEllipse(&halo, dot.X - S(10), dot.Y - S(10), S(20), S(20));
            g.FillEllipse(&core, dot.X - S(5.5f), dot.Y - S(5.5f), S(11), S(11));
        } else {
            dot = PointF(cx + std::cos(pi * sun.f) * w * 0.5f, horizon + std::sin(pi * sun.f) * dip);
            SolidBrush moon(Fade(c.soft, a));
            g.FillEllipse(&moon, dot.X - S(5), dot.Y - S(5), S(10), S(10));
        }
        Label(g, Text::ToWide(d.weather.sunrise), x - S(10), horizon + S(5), w * 0.5f, S(11), Fade(c.soft, a), false);
        Label(g, Text::ToWide(d.weather.sunset), x + w * 0.5f + S(10), horizon + S(5), w * 0.5f, S(11), Fade(c.soft, a), false, 2);
    }

    void TimePage(Graphics& g, REAL W, REAL, REAL dx, float a, const IslandData& d, const Inks& c) {
        wchar_t clock[8];
        swprintf_s(clock, L"%02d:%02d", d.local.wHour, d.local.wMinute);
        const REAL x = S(38) + dx, arcW = S(112), arcX = W - S(40) - arcW + dx, leftW = arcX - S(14) - x;
        Label(g, clock, x - S(2), S(26), leftW + S(4), S(40), Fade(c.ink, a), true);
        const auto& w = d.weather;
        if (!w.ready) {
            Label(g, Tw("Погода ещё загружается"), x, S(88), W - S(76), S(13), Fade(c.soft, a), false);
            return;
        }
        wchar_t temp[24];
        swprintf_s(temp, L"%+.0f°", std::round(w.temperature) == 0 ? 0.0f : w.temperature);
        const std::wstring sky = Text::ToWide(Lang::IsEnglish() ? Lang::Tr(w.description.c_str()) : w.description);
        Label(g, temp, x, S(84), S(52), S(15), Fade(c.ink, a), true);
        Label(g, sky, x + S(48), S(85), leftW - S(48), S(13), Fade(c.soft, a), false);
        const SunPath sun = Sun(d);
        if (!sun.known) return;
        Label(g, In(sun.day ? "закат через" : "восход через", sun.left), x, S(108), leftW, S(12), Fade(c.soft, a), false);
        SunArc(g, arcX, S(42), arcW, S(52), sun, d, a, c);
    }

    void NextPage(Graphics& g, REAL W, REAL, REAL dx, float a, const IslandData& d, const Inks& c) {
        if (d.timers.empty()) return;
        auto left = [&](const Skills::Countdown& t) { return static_cast<long long>((t.end > d.now ? t.end - d.now + 999 : 0) / 1000); };
        auto name = [&](const Skills::Countdown& t) { return t.about.empty() ? Tw("Таймер") : Text::ToWide(t.about); };
        const auto& first = d.timers.front();
        const REAL x = S(38) + dx, ringR = S(24), ringX = W - S(40) - ringR + dx, ringY = S(60), textW = ringX - ringR - S(12) - x;
        Label(g, name(first), x, S(36), textW, S(17), Fade(c.ink, a), true);
        Label(g, In("через", left(first)), x, S(60), textW, S(13), Fade(c.soft, a), false);
        Pen back(Fade(c.muted, a * 0.4f), S(3.5f));
        g.DrawEllipse(&back, ringX - ringR, ringY - ringR, ringR * 2, ringR * 2);
        const float share = first.length ? std::clamp(static_cast<float>(left(first) * 1000.0) / first.length, 0.0f, 1.0f) : 0.0f;
        if (share > 0.005f) {
            Pen ring(Fade(c.accent, a), S(3.5f));
            ring.SetStartCap(LineCapRound);
            ring.SetEndCap(LineCapRound);
            g.DrawArc(&ring, ringX - ringR, ringY - ringR, ringR * 2, ringR * 2, -90, 360 * share);
        }
        Label(g, Countdown(left(first)), ringX - ringR - S(6), ringY - S(9), ringR * 2 + S(12), S(12), Fade(c.ink, a), true, 1);
        const int more = static_cast<int>(d.timers.size()) - 1;
        for (int i = 1; i <= std::min(more, 2); ++i) {
            const REAL y = S(96) + (i - 1) * S(20);
            if (i == 2 && more > 2) {
                Label(g, Lang::IsEnglish() ? L"and " + std::to_wstring(more - 1) + L" more" : L"и ещё " + std::to_wstring(more - 1), x, y, W - S(76), S(12), Fade(c.muted, a), false);
                break;
            }
            const auto& t = d.timers[i];
            Label(g, L"•  " + name(t), x, y, W - S(76) - S(70), S(12), Fade(c.soft, a), false);
            Label(g, Countdown(left(t)), x + W - S(76) - S(70), y, S(70), S(12), Fade(c.soft, a), false, 2);
        }
    }

    void Content(Graphics& g, int page, REAL W, REAL H, REAL dx, float a, const IslandData& d, const Inks& c) {
        if (a <= 0.01f || page < 0) return;
        Header(g, page, W, dx, a, d, c);
        if (page == kMusic) MusicPage(g, W, H, dx, a, d, c);
        else if (page == kTime) TimePage(g, W, H, dx, a, d, c);
        else NextPage(g, W, H, dx, a, d, c);
    }

    void PillContent(Graphics& g, int page, REAL W, REAL H, float a, const IslandData& d, const Inks& c) {
        const REAL y = H * 0.5f - S(10);
        if (page == kMusic) {
            Label(g, Text::ToWide(d.track.title.empty() ? Lang::Tr("Сейчас играет") : d.track.title), S(18), y, W - S(124), S(13), Fade(c.ink, a), true);
            for (int i = 0; i < 3; ++i) {
                const PointF p = MusicButton(i, W, H, 0.0f);
                if (i == 1) {
                    SolidBrush circle(Fade(c.accent, a * 0.9f));
                    g.FillEllipse(&circle, p.X - S(13), p.Y - S(13), S(26), S(26));
                    Icon(g, d.track.playing ? 1 : 2, p.X, p.Y, S(6), Fade(Color(255, 255, 255, 255), a));
                } else Icon(g, i == 0 ? 0 : 3, p.X, p.Y, S(6), Fade(c.ink, a));
            }
        } else if (page == kTime) {
            wchar_t clock[8];
            swprintf_s(clock, L"%02d:%02d", d.local.wHour, d.local.wMinute);
            Label(g, clock, S(20), y - S(1), S(56), S(15), Fade(c.ink, a), true);
            std::wstring rest = DateLine(d.local, false);
            if (d.weather.ready) {
                wchar_t temp[24];
                swprintf_s(temp, L"%+.0f°   ", std::round(d.weather.temperature) == 0 ? 0.0f : d.weather.temperature);
                rest = temp + rest;
            }
            Label(g, rest, S(78), y + S(1), W - S(96), S(13), Fade(c.soft, a), false);
        } else if (!d.timers.empty()) {
            const auto& t = d.timers.front();
            const long long left = static_cast<long long>((t.end > d.now ? t.end - d.now + 999 : 0) / 1000);
            Label(g, t.about.empty() ? Tw("Таймер") : Text::ToWide(t.about), S(20), y, W - S(110), S(13), Fade(c.ink, a), true);
            Label(g, Countdown(left), W - S(92), y, S(72), S(13), Fade(c.accentInk, a), true, 2);
        }
    }

    void Chevron(Graphics& g, PointF at, int side, const Color& color) {
        Pen pen(color, S(2));
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        pen.SetLineJoin(LineJoinRound);
        const REAL k = side ? 1.0f : -1.0f;
        PointF p[3] = { { at.X - k * S(3), at.Y - S(6) }, { at.X + k * S(3), at.Y }, { at.X - k * S(3), at.Y + S(6) } };
        g.DrawLines(&pen, p, 3);
    }

    // W, H — размер плашки; холст уже сдвинут на поле вокруг неё
    void DrawIsland(Graphics& g, REAL W, REAL H, const IslandData& d, float e, int page, int old, float slide) {
        const Inks c = ThemeInks();
        g.SetClip(RectF(0, 0, W, H));
        const float pillShow = 1.0f - std::clamp(e * 2.2f, 0.0f, 1.0f), big = std::clamp(e * 2.2f - 1.2f, 0.0f, 1.0f);
        if (pillShow > 0.01f) PillContent(g, page, W, H, pillShow, d, c);
        if (big <= 0.01f) return;
        const REAL shift = S(44);
        const int dir = slide >= 0 ? 1 : -1;
        const float t = std::fabs(slide), ease = t * t * (3 - 2 * t);
        Content(g, page, W, H, shift * ease * dir, big * (1.0f - ease), d, c);
        if (old >= 0 && t > 0.01f) Content(g, old, W, H, shift * (ease - 1.0f) * dir, big * ease, d, c);
        const int n = static_cast<int>(g_is.ready.size());
        if (n < 2) return;
        for (int side = 0; side < 2; ++side) Chevron(g, Arrow(side, W, H), side, Fade(c.soft, big));
        int at = 0;
        for (int i = 0; i < n; ++i) if (g_is.ready[i] == page) at = i;
        for (int i = 0; i < n; ++i) {
            const REAL x = DotX(i, n, W), y = DotsY(H);
            if (i == at) {
                GraphicsPath pill;
                RoundedPath(pill, x - S(7), y - S(3), S(14), S(6), S(3));
                SolidBrush on(Fade(c.accent, big));
                g.FillPath(&on, &pill);
            } else {
                SolidBrush off(Fade(c.muted, big * 0.8f));
                g.FillEllipse(&off, x - S(3), y - S(3), S(6), S(6));
            }
        }
    }

    // Нарисовать островок в поверхность: подложка темы, сверху текст и значки
    void RenderIsland(Surface& surface, REAL W, REAL H, const IslandData& d, float e, int page, int old, float slide, int hover, bool pickNext) {
        BuildPlate(surface.width, surface.height, W, H, e, hover, pickNext);
        std::memcpy(surface.bits, g_plate.data(), g_plate.size() * 4);
        Bitmap bitmap(surface.width, surface.height, surface.width * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(surface.bits));
        Graphics g(&bitmap);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.TranslateTransform(S(kMargin), S(kMargin));
        DrawIsland(g, W, H, d, e, page, old, slide);
    }

    void ReadyPages(const IslandData& d) {
        g_is.ready.clear();
        for (int p = 0; p < kPageCount; ++p) {
            if (!g_opt.pages[p]) continue;
            if (p == kMusic && !d.track.present) continue;
            if (p == kNext && d.timers.empty()) continue;
            g_is.ready.push_back(p);
        }
    }

    void SetPage(int page, int dir) {
        if (page == g_is.page) return;
        g_is.old = g_is.page;
        g_is.page = page;
        g_is.slide = static_cast<float>(dir);
    }

    void Turn(int dir) { // на страницу вперёд или назад по кругу
        const int n = static_cast<int>(g_is.ready.size());
        if (n < 2) return;
        int at = 0;
        for (int i = 0; i < n; ++i) if (g_is.ready[i] == g_is.page) at = i;
        SetPage(g_is.ready[(at + dir + n) % n], dir);
    }

    void IslandClick(int hit) {
        if (hit == 1 || hit == 2) Turn(hit == 1 ? -1 : 1);
        else if (hit >= 3 && hit <= 5) MediaNow::Control(hit == 3 ? MediaNow::Action::Previous : hit == 4 ? MediaNow::Action::PlayPause : MediaNow::Action::Next);
        else if (hit >= 10 && hit - 10 < static_cast<int>(g_is.ready.size())) {
            int at = 0;
            for (int i = 0; i < static_cast<int>(g_is.ready.size()); ++i) if (g_is.ready[i] == g_is.page) at = i;
            SetPage(g_is.ready[hit - 10], hit - 10 > at ? 1 : -1);
        }
    }

    void Render(Surface& surface, const std::function<void(Graphics&)>& paint) {
        Bitmap bitmap(surface.width, surface.height, surface.width * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(surface.bits));
        Graphics g(&bitmap);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        paint(g);
    }

    void ReadOptions() {
        const bool eco = EcoMode::Active(); // режим экономии прячет оба оверлея
        g_opt.res = !eco && Config::Value("overlay", "res", "0") == "1";
        g_opt.pinned = Config::Value("overlay", "pinned", "0") == "1";
        g_opt.island = false; (void)eco; // плавающий островок убран: остался только островок, прилипший к краю (Notch)
        g_opt.place = std::clamp(std::atoi(Config::Value("overlay", "place", "0").c_str()), 0, 3);
        if (g_opt.place == 0 && Config::Value("overlay", "notch", "1") == "1") g_opt.island = false; // сверху по центру теперь чёлка Ани (Notch)
        g_opt.pages[0] = Config::Value("overlay", "page_music", "1") == "1";
        g_opt.pages[1] = Config::Value("overlay", "page_time", "1") == "1";
        g_opt.pages[2] = Config::Value("overlay", "page_next", "1") == "1";
        g_opt.opacity = std::clamp(std::atoi(Config::Value("overlay", "opacity", "85").c_str()), 20, 100);
        g_opt.place = std::clamp(std::atoi(Config::Value("overlay", "place", "0").c_str()), 0, 3);
        g_opt.x = std::atoi(Config::Value("overlay", "x", "-1").c_str());
        g_opt.y = std::atoi(Config::Value("overlay", "y", "-1").c_str());
        g_opt.w = std::atoi(Config::Value("overlay", "w", "0").c_str());
        g_opt.h = std::atoi(Config::Value("overlay", "h", "0").c_str());
        LONG_PTR ex = GetWindowLongPtrW(g_res.hwnd, GWL_EXSTYLE);
        ex = g_opt.pinned ? ex | WS_EX_TRANSPARENT : ex & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT); // закреплённое окно не ловит мышь
        SetWindowLongPtrW(g_res.hwnd, GWL_EXSTYLE, ex);
        g_resDrawn = 0;
    }

    RECT ResRect() {
        RECT r{};
        GetWindowRect(g_res.hwnd, &r);
        return r;
    }

    int EdgesAt(HWND hwnd, LPARAM lParam) { // у какого края мышь (только у незакреплённого окна)
        RECT r{};
        GetClientRect(hwnd, &r);
        const int x = static_cast<short>(LOWORD(lParam)), y = static_cast<short>(HIWORD(lParam)), m = static_cast<int>(S(7));
        return (x < m ? 1 : 0) | (x >= r.right - m ? 2 : 0) | (y < m ? 4 : 0) | (y >= r.bottom - m ? 8 : 0);
    }

    void SaveResRect() {
        const RECT r = ResRect();
        g_opt.x = r.left; g_opt.y = r.top; g_opt.w = r.right - r.left; g_opt.h = r.bottom - r.top;
        Config::SetValue("overlay", "x", std::to_string(g_opt.x));
        Config::SetValue("overlay", "y", std::to_string(g_opt.y));
        Config::SetValue("overlay", "w", std::to_string(g_opt.w));
        Config::SetValue("overlay", "h", std::to_string(g_opt.h));
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_LBUTTONDOWN:
            if (hwnd == g_res.hwnd) {
                if (const int edges = EdgesAt(hwnd, lParam)) { // тянем за край — меняем размер
                    g_resize.edges = edges;
                    GetCursorPos(&g_resize.start);
                    g_resize.rect = ResRect();
                    SetCapture(hwnd);
                    return 0;
                }
                const float x = static_cast<float>(static_cast<short>(LOWORD(lParam))), y = static_cast<float>(static_cast<short>(HIWORD(lParam)));
                if (y < S(kHeader) && x > g_res.width - S(36)) { Config::SetValue("overlay", "res", "0"); g_dirty = true; return 0; }
                if (y < S(kHeader) && x > g_res.width - S(66)) { Config::SetValue("overlay", "pinned", "1"); g_dirty = true; return 0; }
                ReleaseCapture();
                SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                return 0;
            }
            break;
        case WM_SETCURSOR:
            if (hwnd == g_res.hwnd) {
                POINT at{};
                GetCursorPos(&at);
                ScreenToClient(hwnd, &at);
                const int e = g_resize.edges ? g_resize.edges : EdgesAt(hwnd, MAKELPARAM(at.x, at.y));
                if (e) {
                    const bool nwse = e == 5 || e == 10, nesw = e == 6 || e == 9;
                    SetCursor(LoadCursorW(nullptr, nwse ? IDC_SIZENWSE : nesw ? IDC_SIZENESW : (e & 3) ? IDC_SIZEWE : IDC_SIZENS));
                    return TRUE;
                }
            }
            break;
        case WM_MOUSEMOVE:
            if (hwnd == g_res.hwnd && g_resize.edges) {
                POINT now{};
                GetCursorPos(&now);
                const int dx = now.x - g_resize.start.x, dy = now.y - g_resize.start.y;
                RECT r = g_resize.rect;
                const auto stats = SystemMonitor::Snapshot();
                const int minW = static_cast<int>(S(kMinW)), minH = static_cast<int>(S(kHeader + kMinRow * (4 + (stats.hasGpu ? 1 : 0) + (stats.hasIgpu ? 1 : 0)) + kGraph + 8));
                if (g_resize.edges & 1) r.left = std::min(r.left + dx, r.right - minW);
                if (g_resize.edges & 2) r.right = std::max(r.right + dx, r.left + minW);
                if (g_resize.edges & 4) r.top = std::min(r.top + dy, r.bottom - minH);
                if (g_resize.edges & 8) r.bottom = std::max(r.bottom + dy, r.top + minH);
                g_opt.x = r.left; g_opt.y = r.top; g_opt.w = r.right - r.left; g_opt.h = r.bottom - r.top;
                g_resDrawn = 0;
                return 0;
            }
            break;
        case WM_EXITSIZEMOVE:
            if (hwnd == g_res.hwnd) SaveResRect();
            return 0;
        case WM_LBUTTONUP:
            if (hwnd == g_res.hwnd && g_resize.edges) {
                g_resize.edges = 0;
                ReleaseCapture();
                SaveResRect();
                return 0;
            }
            if (hwnd == g_island.hwnd) {
                const float m = S(kMargin), x = static_cast<short>(LOWORD(lParam)) - m, y = static_cast<short>(HIWORD(lParam)) - m;
                IslandClick(HitTest(x, y, g_island.width - 2 * m, g_island.height - 2 * m, g_islandE * g_islandE * (3.0f - 2.0f * g_islandE)));
                return 0;
            }
            break;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            if (hwnd == g_island.hwnd) { // колесо листает страницы
                g_is.wheel += GET_WHEEL_DELTA_WPARAM(wParam) * (message == WM_MOUSEHWHEEL ? -1 : 1);
                while (g_is.wheel >= WHEEL_DELTA) { Turn(-1); g_is.wheel -= WHEEL_DELTA; }
                while (g_is.wheel <= -WHEEL_DELTA) { Turn(1); g_is.wheel += WHEEL_DELTA; }
                return 0;
            }
            break;
        default: break;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    HWND MakeWindow(const wchar_t* title) {
        return CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"ZaviriaOverlay", title, WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    }

    void UpdateRes(ULONGLONG now, POINT cursor) {
        if (!g_opt.res) {
            if (g_resShown) { ShowWindow(g_res.hwnd, SW_HIDE); g_resShown = false; }
            return;
        }
        const auto stats = SystemMonitor::Snapshot();
        if (!stats.ready) return;
        const int minH = static_cast<int>(S(kHeader + kMinRow * (4 + (stats.hasGpu ? 1 : 0) + (stats.hasIgpu ? 1 : 0)) + kGraph + 8)); // размер, сохранённый под старое окно, — график должен влезать
        const int W = g_opt.w > 0 ? std::max(g_opt.w, static_cast<int>(S(kMinW))) : static_cast<int>(S(kResW)), H = ResHeight(stats); (void)minH; // высота — по содержимому (компактно), ширину можно тянуть
        const bool redraw = now - g_resDrawn > 1000 || g_res.width != W || g_res.height != H;
        if (redraw) {
            g_res.Resize(W, H);
            Render(g_res, [&](Graphics& g) { DrawRes(g, W, H, stats); });
            g_resDrawn = now;
        }
        int x = g_opt.x, y = g_opt.y;
        if (x < -30000 || (x == -1 && y == -1)) {
            RECT work{};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
            x = work.right - W - static_cast<int>(S(16));
            y = work.top + static_cast<int>(S(16));
        } else { // окно стало шире или выше прежнего сохранённого — не даём ему уехать за край своего монитора
            MONITORINFO mi{ sizeof(mi) };
            const RECT r{ x, y, x + W, y + H };
            if (GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi)) {
                x = std::clamp(x, static_cast<int>(mi.rcWork.left), std::max(static_cast<int>(mi.rcWork.left), static_cast<int>(mi.rcWork.right) - W));
                y = std::clamp(y, static_cast<int>(mi.rcWork.top), std::max(static_cast<int>(mi.rcWork.top), static_cast<int>(mi.rcWork.bottom) - H));
            }
        }
        // закреплённое окно под мышкой становится почти прозрачным, чтобы не мешать
        const bool over = g_opt.pinned && cursor.x >= x && cursor.x < x + W && cursor.y >= y && cursor.y < y + H;
        g_resAlpha += ((over ? 0.15f : 1.0f) - g_resAlpha) * 0.18f;
        g_res.Present(x, y, static_cast<int>(255 * g_resAlpha * g_opt.opacity / 100.0f));
        if (!g_resShown) { ShowWindow(g_res.hwnd, SW_SHOWNOACTIVATE); g_resShown = true; }
    }

    void UpdateIsland(ULONGLONG now, POINT cursor) {
        static IslandData d;
        static ULONGLONG s_read = 0, s_weather = 0;
        if (now - s_read > 200) { // плееры и таймеры опрашиваются в фоне — здесь только снимок
            d.track = MediaNow::Snapshot();
            d.timers = Skills::Countdowns();
            s_read = now;
        }
        if (now - s_weather > 5000) { d.weather = Weather::Snapshot(); s_weather = now; }
        d.now = NowMs();
        GetLocalTime(&d.local);
        ReadyPages(d);

        MONITORINFO info{ sizeof(info) };
        GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &info);
        const RECT mon = info.rcMonitor;
        static ULONGLONG s_last = now;
        const float dt = std::min(static_cast<float>(now - s_last) / 1000.0f, 0.05f);
        s_last = now;
        const float smooth = g_islandE * g_islandE * (3.0f - 2.0f * g_islandE);
        const int W = static_cast<int>(S(kPillW + (kIslandW - kPillW) * smooth)), H = static_cast<int>(S(kPillH + (kIslandH - kPillH) * smooth));
        const int gap = static_cast<int>(S(10)), m = static_cast<int>(S(kMargin));
        const bool bottom = g_opt.place == 3;
        const int restX = g_opt.place == 1 ? mon.left + gap : g_opt.place == 2 ? mon.right - W - gap : (mon.left + mon.right - W) / 2;
        const int shownY = bottom ? info.rcWork.bottom - H - gap : mon.top + gap, hiddenY = bottom ? mon.bottom + m + 4 : mon.top - H - m - 4;

        const bool any = g_opt.island && !g_is.ready.empty();
        const bool atEdge = (bottom ? cursor.y >= mon.bottom - 2 : cursor.y <= mon.top + 1) && cursor.x >= restX - W / 3 && cursor.x <= restX + W + W / 3;
        const bool onIsland = g_islandT > 0.4f && cursor.x >= restX && cursor.x <= restX + W && cursor.y >= shownY && cursor.y <= shownY + H;
        const bool near2 = g_islandT > 0.05f && cursor.x >= restX - gap * 3 && cursor.x <= restX + W + gap * 3 && (bottom ? cursor.y >= shownY - gap * 3 : cursor.y <= shownY + H + gap * 3);
        bool want = any && (atEdge || near2);
        if (want) g_islandLeft = now;
        else if (any && now - g_islandLeft < 500 && g_islandT > 0.05f) want = true; // мышь успеет вернуться

        if (g_islandT <= 0.0f && want) { // только появился: сначала то, что важнее сейчас
            auto has = [](int p) { return std::find(g_is.ready.begin(), g_is.ready.end(), p) != g_is.ready.end(); };
            g_is.page = has(kMusic) && d.track.playing ? kMusic : has(kNext) ? kNext : has(kTime) ? kTime : g_is.ready.front();
            g_is.old = -1;
            g_is.slide = 0;
        } else if (any && std::find(g_is.ready.begin(), g_is.ready.end(), g_is.page) == g_is.ready.end()) {
            SetPage(g_is.ready.front(), 1); // страница пропала: музыку закрыли или таймер кончился
        }
        g_is.slide *= std::exp(-dt * 8.0f);
        if (std::fabs(g_is.slide) < 0.01f) { g_is.slide = 0; g_is.old = -1; }

        g_islandT += ((want ? 1.0f : 0.0f) - g_islandT) * (1.0f - std::exp(-dt * 5.5f)); // медленнее и мягче
        g_islandE += ((onIsland ? 1.0f : 0.0f) - g_islandE) * (1.0f - std::exp(-dt * 5.0f));
        if (g_islandT < 0.01f && !want) {
            g_islandT = 0;
            g_islandE = 0;
            if (g_islandShown) { ShowWindow(g_island.hwnd, SW_HIDE); g_islandShown = false; }
            return;
        }
        const float ease = g_islandT * g_islandT * (3.0f - 2.0f * g_islandT);
        const int y = hiddenY + static_cast<int>((shownY - hiddenY) * ease);
        g_is.hover = onIsland && g_is.slide == 0 ? HitTest(static_cast<float>(cursor.x - restX), static_cast<float>(cursor.y - y), static_cast<float>(W), static_cast<float>(H), smooth) : 0;
        g_island.Resize(W + 2 * m, H + 2 * m);
        RenderIsland(g_island, static_cast<REAL>(W), static_cast<REAL>(H), d, smooth, g_is.page, g_is.old, g_is.slide, g_is.hover,
                     g_is.page == kNext && g_is.slide == 0 && smooth > 0.9f);
        g_island.Present(restX - m, y - m, static_cast<int>(255 * std::min(1.0f, g_islandT * 1.6f)));
        if (!g_islandShown) { ShowWindow(g_island.hwnd, SW_SHOWNOACTIVATE); g_islandShown = true; }
    }

    void SavePng(Surface& s, const std::string& path) {
        CLSID png;
        CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &png);
        Bitmap bitmap(s.width, s.height, s.width * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(s.bits));
        bitmap.Save(Text::ToWide(path).c_str(), &png);
    }

    void SaveDump(const std::string& folder) {
        SystemMonitor::Stats stats = SystemMonitor::Snapshot();
        Surface res;
        res.Resize(static_cast<int>(S(kResW)), ResHeight(stats));
        Render(res, [&](Graphics& g) { DrawRes(g, res.width, res.height, stats); });
        SavePng(res, folder + "\\overlay_res.png");

        // островок: каждая страница раскрытой и свёрнутой; чего нет — показываем на примере
        IslandData d;
        d.track = MediaNow::Snapshot();
        d.weather = Weather::Snapshot();
        d.timers = Skills::Countdowns();
        d.now = NowMs();
        GetLocalTime(&d.local);
        if (!d.track.present) { d.track.present = true; d.track.title = "Пример песни"; d.track.artist = "Исполнитель"; d.track.app = "Spotify"; d.track.playing = true; d.track.position = 40; d.track.duration = 120; }
        if (d.timers.empty()) d.timers = { { "выключить чайник", d.now + 252000, 600000 }, { "", d.now + 1500000, 1800000 } };
        const IslandState saved = g_is;
        g_is.ready = { kMusic, kTime, kNext };
        static const char* kNames[] = { "music", "time", "next" };
        for (int p = 0; p < kPageCount; ++p) {
            g_is.page = p;
            Surface big, pill;
            big.Resize(static_cast<int>(S(kIslandW) + 2 * S(kMargin)), static_cast<int>(S(kIslandH) + 2 * S(kMargin)));
            RenderIsland(big, S(kIslandW), S(kIslandH), d, 1.0f, p, -1, 0, 0, p == kNext);
            SavePng(big, folder + "\\overlay_island_" + kNames[p] + ".png");
            pill.Resize(static_cast<int>(S(kPillW) + 2 * S(kMargin)), static_cast<int>(S(kPillH) + 2 * S(kMargin)));
            RenderIsland(pill, S(kPillW), S(kPillH), d, 0.0f, p, -1, 0, 0, false);
            SavePng(pill, folder + "\\overlay_pill_" + kNames[p] + ".png");
        }
        g_is = saved;
        g_plateKey = {};
    }

    void Loop() {
        ULONG_PTR token = 0;
        GdiplusStartupInput input;
        GdiplusStartup(&token, &input, nullptr);
        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ZaviriaOverlay";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        g_res.hwnd = MakeWindow(L"ZAVIRIA: нагрузка");
        g_island.hwnd = MakeWindow(L"ZAVIRIA: сейчас играет");

        while (g_app.running) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            if (g_dirty.exchange(false)) ReadOptions();
            {
                std::lock_guard lock(g_dumpMutex);
                if (!g_dumpFolder.empty()) { SaveDump(g_dumpFolder); g_dumpFolder.clear(); }
            }
            POINT cursor{};
            GetCursorPos(&cursor);
            const ULONGLONG now = GetTickCount64();
            UpdateRes(now, cursor);
            UpdateIsland(now, cursor);
            MsgWaitForMultipleObjects(0, nullptr, FALSE, g_islandShown ? 16 : 40, QS_ALLINPUT);
        }
        ShowWindow(g_res.hwnd, SW_HIDE);
        ShowWindow(g_island.hwnd, SW_HIDE);
    }
}

void Overlay::Start() { std::thread(Loop).detach(); }
void Overlay::Reload() { g_dirty = true; }
bool Overlay::IslandShown() { return g_islandShown; }

void Overlay::Dump(const std::string& folder) {
    std::lock_guard lock(g_dumpMutex);
    g_dumpFolder = folder;
}
