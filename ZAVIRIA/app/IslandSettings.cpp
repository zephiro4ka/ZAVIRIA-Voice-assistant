#define NOMINMAX
#include "IslandSettings.h"

#include "Graphics.h"
#include "Lang.h"
#include "Settings.h"
#include "TextUtils.h"
#include "Theme.h"
#include "Widgets.h"
#include "Visuals.h"
#include "../resource.h"
#include "../ImGui/imgui.h"
#include "../ImGui/imgui_internal.h"
#include "../ImGui/imgui_impl_dx11.h"
#include "../ImGui/imgui_impl_win32.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <dwmapi.h>
#include <shellapi.h>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

using Theme::Px;

namespace {
    constexpr const wchar_t* kClass = L"AnyaIslandSettings";

    // настройку записали — окну Ани сказать, чтобы островок перечитал её сразу
    void Changed() {
        if (const HWND main = FindWindowW(L"AnyaWindow", nullptr)) PostMessageW(main, IslandSettings::kChangedMessage, 0, 0);
    }
    void Set(const char* key, const std::string& value) { Config::SetValue("overlay", key, value); Changed(); }
    std::string Get(const char* key, const char* def) { return Config::Value("overlay", key, def); }

    // Группа как у Nusa: подпись над ней, серый скруглённый островок, строки через тонкие линии
    struct Group {
        ImDrawList* dl; ImVec2 start; float width; int rows = 0;
        Group(ImDrawList* d, const char* caption, float w) : dl(d), width(w) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (caption) { Widgets::Text(dl, Theme::semibold, ImVec2(p.x + Px(6), p.y), Theme::kInkMuted, caption); ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + Theme::semibold->LegacySize + Px(8))); }
            start = ImGui::GetCursorScreenPos();
            dl->ChannelsSplit(2);
            dl->ChannelsSetCurrent(1);
        }
        void Divider() {
            if (rows++ == 0) return;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddLine(ImVec2(p.x + Px(16), p.y), ImVec2(p.x + width - Px(16), p.y), Theme::Overlay(14), Px(1));
        }
        ~Group() {
            const ImVec2 end(start.x + width, ImGui::GetCursorScreenPos().y);
            dl->ChannelsSetCurrent(0);
            dl->AddRectFilled(start, end, Theme::Overlay(Theme::Paper() ? 12 : 9), Px(14));
            dl->AddRect(start, end, Theme::Overlay(14), Px(14), 0, Px(1));
            dl->ChannelsMerge();
            ImGui::SetCursorScreenPos(ImVec2(start.x, end.y + Px(18)));
        }
    };

    // строка: название, под ним серое пояснение, справа переключатель
    void ToggleRow(Group& gr, const char* title, const char* hint, const char* key, const char* def) {
        gr.Divider();
        ImDrawList* dl = gr.dl;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = hint ? Px(60) : Px(48);
        const float ty = p.y + (h - Theme::title->LegacySize - (hint ? Theme::mono->LegacySize + Px(4) : 0.0f)) * 0.5f;
        Widgets::Text(dl, Theme::title, ImVec2(p.x + Px(16), ty), Theme::kInk, title);
        if (hint) Widgets::Text(dl, Theme::mono, ImVec2(p.x + Px(16), ty + Theme::title->LegacySize + Px(4)), Theme::kInkMuted, hint);
        bool on = Get(key, def) == "1";
        ImGui::PushID(key);
        if (Widgets::Toggle("##t", ImVec2(p.x + gr.width - Px(16) - Px(34), p.y + (h - Px(18)) * 0.5f), &on, Theme::Accent().WithAlpha(1.0f))) Set(key, on ? "1" : "0");
        ImGui::PopID();
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    }

    // переключатель-сегменты (как «Size» у Nusa)
    void Segments(Group& gr, const char* title, const char* hint, const char* key, const char* const* names, int count, const char* def) {
        gr.Divider();
        ImDrawList* dl = gr.dl;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        Widgets::Text(dl, Theme::title, ImVec2(p.x + Px(16), p.y + Px(12)), Theme::kInk, title);
        float y = p.y + Px(12) + Theme::title->LegacySize + Px(4);
        if (hint) { Widgets::Text(dl, Theme::mono, ImVec2(p.x + Px(16), y), Theme::kInkMuted, hint); y += Theme::mono->LegacySize + Px(10); }
        const int cur = std::clamp(std::atoi(Get(key, def).c_str()), 0, count - 1);
        const ImVec2 a(p.x + Px(14), y), b(p.x + gr.width - Px(14), y + Px(30));
        dl->AddRectFilled(a, b, Theme::Overlay(12), Px(8));
        const float segW = (b.x - a.x) / count;
        for (int i = 0; i < count; ++i) {
            const ImVec2 s0(a.x + segW * i + Px(2), a.y + Px(2)), s1(a.x + segW * (i + 1) - Px(2), b.y - Px(2));
            ImGui::SetCursorScreenPos(s0);
            ImGui::PushID(key); ImGui::PushID(i);
            if (ImGui::InvisibleButton("##seg", ImVec2(s1.x - s0.x, s1.y - s0.y))) Set(key, std::to_string(i));
            const bool hot = ImGui::IsItemHovered();
            ImGui::PopID(); ImGui::PopID();
            if (i == cur) dl->AddRectFilled(s0, s1, Theme::Overlay(36), Px(6));
            else if (hot) dl->AddRectFilled(s0, s1, Theme::Overlay(16), Px(6));
            const char* name = Lang::Tr(names[i]);
            const ImVec2 t = Widgets::TextSize(Theme::semibold, name);
            Widgets::Text(dl, Theme::semibold, ImVec2((s0.x + s1.x - t.x) * 0.5f, (s0.y + s1.y - t.y) * 0.5f), i == cur ? Theme::kInk : Theme::kInkSoft, name);
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x, b.y + Px(14)));
    }

    // Мини-экран: островок тянут к любому краю и в любое место вдоль края, он едет по настоящему экрану сразу
    void PlacePicker(ImDrawList* dl, float w) {
        MONITORINFO info{ sizeof(info) };
        GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &info);
        const float aspect = static_cast<float>(info.rcMonitor.right - info.rcMonitor.left) / std::max(1.0f, static_cast<float>(info.rcMonitor.bottom - info.rcMonitor.top));
        const float sw = std::min(w - Px(20), Px(360)), sh = sw / aspect;
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const ImVec2 a(start.x + (w - sw) * 0.5f, start.y + Px(10)), b(a.x + sw, a.y + sh);
        dl->AddRectFilled(ImVec2(a.x - Px(7), a.y - Px(7)), ImVec2(b.x + Px(7), b.y + Px(7)), Theme::Overlay(26), Px(12));
        dl->AddRectFilledMultiColor(a, b, Theme::Background(0).WithAlpha(1.0f), Theme::Background(0).Mix(Theme::Accent(), 0.12f).WithAlpha(1.0f),
            Theme::Background(1).WithAlpha(1.0f), Theme::Background(1).WithAlpha(1.0f));
        dl->AddRectFilled(ImVec2((a.x + b.x) * 0.5f - Px(24), b.y + Px(7)), ImVec2((a.x + b.x) * 0.5f + Px(24), b.y + Px(24)), Theme::Overlay(20));
        dl->AddRectFilled(ImVec2((a.x + b.x) * 0.5f - Px(56), b.y + Px(24)), ImVec2((a.x + b.x) * 0.5f + Px(56), b.y + Px(30)), Theme::Overlay(26), Px(3));

        static int s_edge = 0;
        static float s_along = 0.5f;
        static bool s_drag = false;
        if (!s_drag) {
            s_edge = std::clamp(std::atoi(Get("notch_edge", "0").c_str()), 0, 3);
            s_along = std::clamp(std::atoi(Get("notch_along", "500").c_str()) / 1000.0f, 0.0f, 1.0f);
        }
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("##place", ImVec2(sw, sh));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemActive()) {
            s_drag = true;
            const ImVec2 m = ImGui::GetIO().MousePos;
            const float fx = std::clamp((m.x - a.x) / sw, 0.0f, 1.0f), fy = std::clamp((m.y - a.y) / sh, 0.0f, 1.0f);
            const float d[4] = { fy, 1.0f - fy, fx * sw / sh, (1.0f - fx) * sw / sh };
            s_edge = static_cast<int>(std::min_element(d, d + 4) - d);
            s_along = s_edge < 2 ? fx : fy;
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            static int s_savedEdge = -1, s_savedAlong = -1;
            static double s_savedAt = 0;
            const int along = static_cast<int>(std::lround(s_along * 1000));
            if ((s_edge != s_savedEdge || std::abs(along - s_savedAlong) >= 3) && ImGui::GetTime() - s_savedAt > 0.04) {
                Config::SetValue("overlay", "notch_edge", std::to_string(s_edge));
                Set("notch_along", std::to_string(along));
                s_savedEdge = s_edge; s_savedAlong = along; s_savedAt = ImGui::GetTime();
            }
        } else if (s_drag) {
            s_drag = false;
            Config::SetValue("overlay", "notch_edge", std::to_string(s_edge));
            Set("notch_along", std::to_string(static_cast<int>(std::lround(s_along * 1000))));
        }
        if (hovered && !s_drag) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const float thick = Px(11);
        ImVec2 p0, p1;
        if (s_edge < 2) {
            const float len = sw * 0.26f, c = std::clamp(a.x + s_along * sw, a.x + len * 0.5f, b.x - len * 0.5f);
            p0 = ImVec2(c - len * 0.5f, s_edge == 0 ? a.y : b.y - thick);
            p1 = ImVec2(c + len * 0.5f, s_edge == 0 ? a.y + thick : b.y);
        } else {
            const float l = sh * 0.3f, c = std::clamp(a.y + s_along * sh, a.y + l * 0.5f, b.y - l * 0.5f);
            p0 = ImVec2(s_edge == 2 ? a.x : b.x - thick, c - l * 0.5f);
            p1 = ImVec2(s_edge == 2 ? a.x + thick : b.x, c + l * 0.5f);
        }
        const ImDrawFlags corners = s_edge == 0 ? ImDrawFlags_RoundCornersBottom : s_edge == 1 ? ImDrawFlags_RoundCornersTop : s_edge == 2 ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersLeft;
        if (s_drag || hovered) dl->AddRectFilled(ImVec2(p0.x - Px(4), p0.y - Px(4)), ImVec2(p1.x + Px(4), p1.y + Px(4)), Theme::Accent().WithAlpha(0.25f), Px(9), corners);
        dl->AddRectFilled(p0, p1, Theme::Accent().Mix({ 20, 14, 30 }, 0.4f).WithAlpha(1.0f), Px(6), corners);
        ImGui::SetCursorScreenPos(ImVec2(start.x, b.y + Px(42)));
    }

    // Форма глазок: плитки с примером
    void EyeShapes(ImDrawList* dl, float w) {
        static const char* kNames[] = { "овалы", "сердечки", "звёздочки", "стрелочки", "плюсики", "искры" };
        constexpr int kCount = 6;
        const int current = std::clamp(std::atoi(Get("notch_eye_shape", "0").c_str()), 0, kCount - 1);
        const float gap = Px(10), tile = std::min(Px(78), (w - gap * 5) / 6.0f); // все шесть форм в один ряд
        const int perRow = kCount; // ширина плитки уже посчитана так, чтобы влезли все шесть
        const ImVec2 start = ImGui::GetCursorScreenPos();
        for (int i = 0; i < kCount; ++i) {
            const ImVec2 a(start.x + (i % perRow) * (tile + gap), start.y + (i / perRow) * (tile + gap + Px(18)));
            const ImVec2 b(a.x + tile, a.y + tile * 0.62f);
            ImGui::SetCursorScreenPos(a);
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##shape", ImVec2(tile, tile * 0.62f + Px(18)))) Set("notch_eye_shape", std::to_string(i));
            const bool hot = ImGui::IsItemHovered();
            ImGui::PopID();
            dl->AddRectFilled(a, b, IM_COL32(16, 14, 20, 255), Px(12));
            if (i == current) dl->AddRect(a, b, Theme::Accent().WithAlpha(1.0f), Px(12), 0, Px(2));
            else if (hot) dl->AddRect(a, b, Theme::Overlay(60), Px(12), 0, Px(1));
            // пример: два глазка этой формы
            const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            for (int e = 0; e < 2; ++e) {
                const ImVec2 o(c.x + (e ? Px(9) : -Px(9)), c.y);
                const ImU32 col = IM_COL32(255, 255, 255, 255);
                const float r = Px(6);
                switch (i) {
                case 1: { // сердечко
                    dl->AddCircleFilled(ImVec2(o.x - r * 0.45f, o.y - r * 0.25f), r * 0.55f, col, 16);
                    dl->AddCircleFilled(ImVec2(o.x + r * 0.45f, o.y - r * 0.25f), r * 0.55f, col, 16);
                    dl->AddTriangleFilled(ImVec2(o.x - r * 0.98f, o.y - r * 0.05f), ImVec2(o.x + r * 0.98f, o.y - r * 0.05f), ImVec2(o.x, o.y + r * 0.95f), col);
                    break;
                }
                case 2: { // звёздочка
                    ImVec2 pts[10];
                    for (int k = 0; k < 10; ++k) { const float ang = -1.5708f + k * 3.14159f / 5.0f, rr = k % 2 ? r * 0.45f : r; pts[k] = ImVec2(o.x + std::cos(ang) * rr, o.y + std::sin(ang) * rr); }
                    dl->AddConcavePolyFilled(pts, 10, col);
                    break;
                }
                case 3: { // стрелочка ^
                    const ImVec2 p[] = { { o.x - r * 0.8f, o.y + r * 0.35f }, { o.x, o.y - r * 0.5f }, { o.x + r * 0.8f, o.y + r * 0.35f } };
                    dl->AddPolyline(p, 3, col, 0, Px(2.4f));
                    break;
                }
                case 4: // плюсик
                    dl->AddLine(ImVec2(o.x - r * 0.7f, o.y), ImVec2(o.x + r * 0.7f, o.y), col, Px(2.4f));
                    dl->AddLine(ImVec2(o.x, o.y - r * 0.7f), ImVec2(o.x, o.y + r * 0.7f), col, Px(2.4f));
                    break;
                case 5: { // искра — четыре луча
                    const ImVec2 p[] = { { o.x, o.y - r }, { o.x + r * 0.22f, o.y - r * 0.22f }, { o.x + r, o.y }, { o.x + r * 0.22f, o.y + r * 0.22f },
                                         { o.x, o.y + r }, { o.x - r * 0.22f, o.y + r * 0.22f }, { o.x - r, o.y }, { o.x - r * 0.22f, o.y - r * 0.22f } };
                    dl->AddConcavePolyFilled(p, 8, col);
                    break;
                }
                default: dl->AddEllipseFilled(o, ImVec2(r * 0.55f, r * 0.9f), col, 0.0f, 20); break;
                }
            }
            const char* name = Lang::Tr(kNames[i]);
            const float tw = Widgets::TextSize(Theme::semibold, name).x;
            Widgets::Text(dl, Theme::semibold, ImVec2(a.x + (tile - tw) * 0.5f, b.y + Px(4)), i == current ? Theme::kInk : Theme::kInkMuted, name);
        }
        const int rows = (kCount + perRow - 1) / perRow;
        ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + rows * (tile * 0.62f + gap + Px(18)) + Px(6)));
    }

    struct Section { const char* name; const char* emoji; const char* about; Theme::Color color; };
    const Section kSections[] = {
        { "Общее", "\u2699", "часы, волна и размер", { 148, 140, 170 } },
        { "Расположение", "\u26f3", "к какому краю прилипает", { 99, 102, 241 } },
        { "Глазки", "\u2728", "моргают и следят за мышкой", { 192, 92, 246 } },
        { "Музыка и тексты", "\u23ef", "плеер и слова песен", { 244, 72, 120 } },
        { "Страницы", "\u2b50", "что есть в островке", { 56, 152, 246 } },
        { "Всплывашки", "\u26a1", "короткие подсказки", { 250, 166, 40 } },
    };
    int g_section = 0;
    ImFont* g_emoji = nullptr; // Segoe UI Emoji

    // плитка раздела: переход цвета, мягкий блик, эмодзи по центру
    void Tile(ImDrawList* dl, ImVec2 c, float r, const Section& sec, float glow) {
        if (glow > 0.01f) for (int k = 3; k >= 1; --k) dl->AddRectFilled(ImVec2(c.x - r - Px(2) * k, c.y - r - Px(2) * k), ImVec2(c.x + r + Px(2) * k, c.y + r + Px(2) * k), sec.color.WithAlpha(0.08f * glow), r * 0.55f + Px(2) * k);
        const ImVec2 a(c.x - r, c.y - r), b(c.x + r, c.y + r);
        const int v0 = dl->VtxBuffer.Size;
        dl->AddRectFilled(a, b, sec.color.WithAlpha(1.0f), r * 0.55f);
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, a, b, sec.color.Mix({ 255, 255, 255 }, 0.3f).WithAlpha(1.0f), sec.color.Mix({ 0, 0, 0 }, 0.2f).WithAlpha(1.0f));
        dl->AddRect(a, b, IM_COL32(255, 255, 255, 40), r * 0.55f, 0, Px(1));
        if (g_emoji) {
            const float fs = r * 1.15f;
            const ImVec2 t = g_emoji->CalcTextSizeA(fs, FLT_MAX, 0.0f, sec.emoji);
            dl->AddText(g_emoji, fs, ImVec2(c.x - t.x * 0.5f, c.y - t.y * 0.5f), IM_COL32(255, 255, 255, 255), sec.emoji);
        }
    }

    void DrawSidebar(ImDrawList* dl, ImVec2 a, ImVec2 b) {
        dl->AddRectFilled(a, b, Theme::Background(1).Mix({ 0, 0, 0 }, Theme::Light() ? 0.04f : 0.25f).WithAlpha(1.0f));
        { // шапка меню: маленький логотип-«островок» с глазками
            const ImVec2 c(a.x + Px(32), a.y + Px(30));
            dl->AddRectFilled(ImVec2(c.x - Px(18), c.y - Px(10)), ImVec2(c.x + Px(18), c.y + Px(10)), IM_COL32(14, 12, 18, 255), Px(10));
            dl->AddEllipseFilled(ImVec2(c.x - Px(5), c.y), ImVec2(Px(2.2f), Px(3.6f)), IM_COL32(255, 255, 255, 255));
            dl->AddEllipseFilled(ImVec2(c.x + Px(5), c.y), ImVec2(Px(2.2f), Px(3.6f)), IM_COL32(255, 255, 255, 255));
            Widgets::Text(dl, Theme::title, ImVec2(c.x + Px(28), c.y - Theme::title->LegacySize * 0.55f), Theme::kInk, "Островок");
        }
        float y = a.y + Px(64);
        for (int i = 0; i < static_cast<int>(std::size(kSections)); ++i) {
            const ImVec2 r0(a.x + Px(10), y), r1(b.x - Px(10), y + Px(38));
            ImGui::SetCursorScreenPos(r0);
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##sec", ImVec2(r1.x - r0.x, r1.y - r0.y))) g_section = i;
            const bool hot = ImGui::IsItemHovered();
            ImGui::PopID();
            if (i == g_section) { // выбранный: подложка оттенка раздела и полоска слева
                dl->AddRectFilled(r0, r1, kSections[i].color.WithAlpha(0.16f), Px(10));
                dl->AddRectFilled(ImVec2(r0.x, r0.y + Px(9)), ImVec2(r0.x + Px(3), r1.y - Px(9)), kSections[i].color.WithAlpha(1.0f), Px(2));
            } else if (hot) dl->AddRectFilled(r0, r1, Theme::Overlay(10), Px(10));
            const ImVec2 ic(r0.x + Px(22), (r0.y + r1.y) * 0.5f);
            Tile(dl, ic, Px(13), kSections[i], i == g_section ? 1.0f : 0.0f);
            Widgets::Text(dl, Theme::semibold, ImVec2(r0.x + Px(44), ic.y - Theme::semibold->LegacySize * 0.55f), i == g_section ? Theme::kInk : Theme::kInkSoft, kSections[i].name);
            y += Px(42);
        }
    }

    void DrawSection(ImDrawList* dl, float w) {
        switch (g_section) {
        case 0: {
            { Group g(dl, "Островок", w);
              ToggleRow(g, "Часы в покое", "маленький островок с часами, когда Аня молчит", "notch_idle", "1");
              ToggleRow(g, "Волна музыки", "рядом с часами, когда играет музыка", "notch_wave", "1"); }
            { Group g(dl, "Размер", w); g.Divider();
              const ImVec2 p = ImGui::GetCursorScreenPos();
              Widgets::Text(dl, Theme::title, ImVec2(p.x + Px(16), p.y + Px(12)), Theme::kInk, "Размер островка");
              Widgets::Text(dl, Theme::mono, ImVec2(p.x + Px(16), p.y + Px(14) + Theme::title->LegacySize), Theme::kInkMuted, "островок на экране меняется сразу");
              static int s_scale = -1;
              if (s_scale < 0) s_scale = std::clamp(std::atoi(Get("notch_scale", Get("notch_size", "0") == "1" ? "115" : "100").c_str()), 70, 150);
              ImGui::SetCursorScreenPos(ImVec2(p.x + Px(16), p.y + Px(22) + Theme::title->LegacySize + Theme::mono->LegacySize));
              static double s_saved = 0;
              if (Widgets::Slider("##scale", &s_scale, 70, 150, "%d%%", w - Px(32)) && ImGui::GetTime() - s_saved > 0.05) { Set("notch_scale", std::to_string(s_scale)); s_saved = ImGui::GetTime(); }
              if (ImGui::IsItemDeactivated()) Set("notch_scale", std::to_string(s_scale));
              ImGui::SetCursorScreenPos(ImVec2(p.x, ImGui::GetItemRectMax().y + Px(16))); }
            break;
        }
        case 1: { Group g(dl, "Край и место", w); g.Divider(); ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x + Px(10), ImGui::GetCursorScreenPos().y));
                  PlacePicker(dl, w - Px(20)); ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x - Px(10), ImGui::GetCursorScreenPos().y - Px(18)));
                  Widgets::Text(dl, Theme::mono, ImVec2(ImGui::GetCursorScreenPos().x + Px(16), ImGui::GetCursorScreenPos().y), Theme::kInkMuted, "перетащи к любому краю — островок на экране едет следом");
                  ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y + Theme::mono->LegacySize + Px(16))); break; }
        case 2: {
            { Group g(dl, nullptr, w); ToggleRow(g, "Показывать глазки", "следят за мышкой и моргают", "notch_eyes", "1"); }
            { Group g(dl, "Форма", w); g.Divider(); ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x + Px(16), ImGui::GetCursorScreenPos().y + Px(14)));
              EyeShapes(dl, w - Px(32)); ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x - Px(16), ImGui::GetCursorScreenPos().y)); }
            break;
        }
        case 3: { Group g(dl, nullptr, w);
                  ToggleRow(g, "Музыка", "трек, кнопки и полоса времени", "notch_music", "1");
                  ToggleRow(g, "Текст песен", "слова под музыку", "notch_lyrics", "0"); break; }
        case 4: { Group g(dl, nullptr, w);
                  ToggleRow(g, "Удаление", "перетащи файл в островок — удалится насовсем, мимо корзины", "notch_shelf", "1");
                  ToggleRow(g, "Погода", nullptr, "notch_weather", "1");
                  ToggleRow(g, "Напоминания", nullptr, "notch_timers", "1");
                  ToggleRow(g, "Секундомер", nullptr, "notch_stopwatch", "1");
                  ToggleRow(g, "Фокусировка", nullptr, "notch_pomodoro", "1");
                  ToggleRow(g, "Вода", nullptr, "notch_water", "1"); break; }
        default: { Group g(dl, nullptr, w);
                   ToggleRow(g, "Раскладка", "показать язык при переключении", "notch_layout", "1");
                   ToggleRow(g, "Зарядка", "подключили или отключили зарядку", "notch_power", "1"); break; }
        }
    }

    void DrawPage(ImVec2 size) {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(size);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::Background(1).Mix(Theme::Background(0), 0.5f).WithAlpha(1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("##island", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float side = Px(220);
        DrawSidebar(dl, ImVec2(0, 0), ImVec2(side, size.y));
        // справа — раздел со своей прокруткой
        ImGui::SetCursorScreenPos(ImVec2(side, 0));
        ImGui::BeginChild("##section", ImVec2(size.x - side, size.y), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
        Widgets::SmoothScroll();
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        const float pad = Px(26), w = ImGui::GetWindowWidth() - pad * 2 - Px(8);
        ImGui::SetCursorPos(ImVec2(pad, Px(18)));
        const ImVec2 t = ImGui::GetCursorScreenPos();
        { // заголовок раздела: крупная плитка с эмодзи, название и короткое пояснение; сзади мягкое пятно цвета раздела
            const Section& sec = kSections[g_section];
            const float r = Px(24);
            const ImVec2 c(t.x + r, t.y + r + Px(4));
            const float rad[3] = { 0.0f, Px(90), Px(180) };
            const ImU32 col[3] = { sec.color.WithAlpha(0.16f), sec.color.WithAlpha(0.05f), sec.color.WithAlpha(0.0f) };
            Visuals::RadialGradient(cdl, c, rad, col, 3, 48);
            Tile(cdl, c, r, sec, 0.6f);
            Widgets::Text(cdl, Theme::hero, ImVec2(t.x + r * 2 + Px(16), c.y - Theme::hero->LegacySize * 0.62f), Theme::kInk, sec.name);
            Widgets::Text(cdl, Theme::mono, ImVec2(t.x + r * 2 + Px(18), c.y + Theme::hero->LegacySize * 0.42f), Theme::kInkMuted, sec.about);
        }
        ImGui::SetCursorScreenPos(ImVec2(t.x, t.y + Px(80)));
        DrawSection(cdl, w);
        ImGui::Dummy(ImVec2(0, Px(8)));
        ImGui::EndChild();
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor();
    }

    LRESULT WINAPI WndProc(HWND window, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (ImGui_ImplWin32_WndProcHandler(window, msg, wParam, lParam)) return TRUE;
        switch (msg) {
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) Graphics::RequestResize(LOWORD(lParam), HIWORD(lParam));
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(window, msg, wParam, lParam);
    }
}

void IslandSettings::Open() {
    if (const HWND open = FindWindowW(kClass, nullptr)) { ShowWindow(open, SW_RESTORE); SetForegroundWindow(open); return; }
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    ShellExecuteW(nullptr, L"open", path, kFlag, nullptr, SW_SHOWNORMAL);
}

int IslandSettings::Run(HINSTANCE instance) {
    if (const HWND open = FindWindowW(kClass, nullptr)) { ShowWindow(open, SW_RESTORE); SetForegroundWindow(open); return 0; }
    Config::Load();
    Lang::Reload();
    WNDCLASSEXW wc{ sizeof(wc), CS_CLASSDC, WndProc, 0, 0, instance, LoadIconW(instance, MAKEINTRESOURCEW(IDI_ZAVIRIA)), LoadCursorW(nullptr, IDC_ARROW),
                    nullptr, nullptr, kClass, LoadIconW(instance, MAKEINTRESOURCEW(IDI_SMALL)) };
    RegisterClassExW(&wc);
    const float scale = GetDpiForSystem() / 96.0f;
    const int width = static_cast<int>(820 * scale), height = static_cast<int>(680 * scale);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const HWND window = CreateWindowExW(0, kClass, Text::ToWide(Lang::Tr("ZAVIRIA — островок")).c_str(), WS_OVERLAPPEDWINDOW,
        work.left + (work.right - work.left - width) / 2, work.top + (work.bottom - work.top - height) / 2, width, height, nullptr, nullptr, instance, nullptr);
    if (!window || !Graphics::Create(window)) return 1;
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(window, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    Theme::Init(GetDpiForWindow(window) / 96.0f);
    g_emoji = ImGui::GetIO().Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguiemj.ttf", Theme::Px(26));
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(Graphics::Device(), Graphics::Context());

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;
        if (IsIconic(window)) { Sleep(30); continue; }
        Graphics::ApplyPendingResize();
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        Theme::Apply(Config::Get());
        DrawPage(ImGui::GetIO().DisplaySize);
        ImGui::Render();
        const Theme::Color bg = Theme::Background(1);
        Graphics::Clear(bg.r / 255.0f, bg.g / 255.0f, bg.b / 255.0f, 1.0f);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        Graphics::Present();
    }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    Graphics::Destroy();
    return 0;
}
