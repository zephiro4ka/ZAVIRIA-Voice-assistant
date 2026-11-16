#include "WeatherWidget.h"

#include "../ImGui/imgui_internal.h"
#include "Lang.h"
#include "Theme.h"
#include "Weather.h"
#include "TextUtils.h"
#include "Widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using Theme::Px;
using Weather::Sky;

namespace {
    constexpr float kPi = 3.14159265f;
    float g_open = 0.0f; // 0 — маленькая, 1 — раскрытая

    // размеры (до DPI)
    constexpr float kPad = 14, kHeader = 42, kLine = 19;
    constexpr float kMiniCell = 54, kMiniCellHeight = 80;

    ImU32 Fade(ImU32 color, float alpha) {
        const float a = static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f);
        return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
    }

    std::string Temp(float t) {
        char text[16];
        sprintf_s(text, "%d°", static_cast<int>(std::lround(t)));
        return text;
    }

    void TextCentered(ImDrawList* dl, ImFont* font, float centerX, float y, ImU32 color, const std::string& text) {
        const float w = Widgets::TextSize(font, text.c_str()).x;
        Widgets::Text(dl, font, ImVec2(centerX - w * 0.5f, y), color, text.c_str());
    }

    // ------------------------------------------------------------ значки погоды (рисуем векторно)
    // на светлой плашке бледные облака и голубой пропадают — там те же цвета, но гуще
    ImU32 Tone(ImU32 dark, ImU32 light) { return Theme::Light() ? light : dark; }
    ImU32 CloudColor() { return Tone(IM_COL32(214, 208, 236, 255), IM_COL32(150, 138, 196, 255)); }
    ImU32 RainBlue() { return Tone(IM_COL32(125, 185, 255, 255), IM_COL32(40, 112, 220, 255)); }

    void Sun(ImDrawList* dl, ImVec2 c, float s, float a) {
        const ImU32 gold = Fade(IM_COL32(255, 204, 102, 255), a);
        const float r = s * 0.2f;
        dl->AddCircleFilled(c, r * 1.9f, Fade(IM_COL32(255, 204, 102, 40), a), 24); // мягкое свечение
        dl->AddCircleFilled(c, r, gold, 24);
        for (int i = 0; i < 8; ++i) {
            const float angle = i * kPi / 4.0f;
            const ImVec2 dir(std::cos(angle), std::sin(angle));
            dl->AddLine(ImVec2(c.x + dir.x * r * 1.45f, c.y + dir.y * r * 1.45f), ImVec2(c.x + dir.x * r * 2.0f, c.y + dir.y * r * 2.0f), gold, s * 0.055f);
        }
    }

    void Moon(ImDrawList* dl, ImVec2 c, float s, float a) {
        // серп = круг минус сдвинутый круг: внешняя дуга между точками пересечения, затем внутренняя обратно
        const float r = s * 0.27f, inner = r * 0.82f;
        const float phi = std::atan2(-0.28f, 0.48f), d = r * std::sqrt(0.48f * 0.48f + 0.28f * 0.28f);
        const ImVec2 c1(c.x + std::cos(phi) * d, c.y + std::sin(phi) * d);
        const float alpha = std::acos((r * r + d * d - inner * inner) / (2 * r * d));         // точки пересечения на внешнем круге
        const float beta = std::acos((inner * inner + d * d - r * r) / (2 * inner * d));      // и на внутреннем
        constexpr int kSteps = 28;
        ImVec2 points[kSteps * 2 + 2];
        int n = 0;
        for (int i = 0; i <= kSteps; ++i) {
            const float t = phi + alpha + (2 * kPi - 2 * alpha) * i / kSteps;
            points[n++] = ImVec2(c.x + std::cos(t) * r, c.y + std::sin(t) * r);
        }
        for (int i = 0; i <= kSteps; ++i) {
            const float t = phi + kPi + beta - 2 * beta * i / kSteps;
            points[n++] = ImVec2(c1.x + std::cos(t) * inner, c1.y + std::sin(t) * inner);
        }
        const bool cozy = Theme::CozyAtmosphere(); // в «Домашнем уюте» луна сливочная, а не сиреневая
        dl->AddCircleFilled(c, r * 1.6f, Fade(cozy ? IM_COL32(240, 210, 170, 30) : IM_COL32(200, 190, 255, 30), a), 24);
        dl->AddConcavePolyFilled(points, n, Fade(cozy ? Tone(IM_COL32(246, 230, 205, 255), IM_COL32(176, 130, 92, 255)) : Tone(IM_COL32(226, 220, 255, 255), IM_COL32(150, 132, 222, 255)), a));
    }

    void Cloud(ImDrawList* dl, ImVec2 c, float s, float a, ImU32 color = 0) {
        const ImU32 col = Fade(color ? color : CloudColor(), a);
        dl->AddCircleFilled(ImVec2(c.x - s * 0.17f, c.y + s * 0.04f), s * 0.16f, col, 24);
        dl->AddCircleFilled(ImVec2(c.x + s * 0.03f, c.y - s * 0.07f), s * 0.21f, col, 24);
        dl->AddCircleFilled(ImVec2(c.x + s * 0.21f, c.y + s * 0.06f), s * 0.14f, col, 24);
        dl->AddRectFilled(ImVec2(c.x - s * 0.3f, c.y + s * 0.04f), ImVec2(c.x + s * 0.33f, c.y + s * 0.2f), col, s * 0.08f);
    }

    void DrawSky(ImDrawList* dl, Sky sky, bool night, ImVec2 c, float s, float a) {
        auto celestial = [&](ImVec2 p, float size) { night ? Moon(dl, p, size, a) : Sun(dl, p, size, a); };
        const ImVec2 up(c.x, c.y - s * 0.1f); // туча повыше, под ней капли
        switch (sky) {
        case Sky::Clear: celestial(c, s); break;
        case Sky::PartlyCloudy:
            celestial(ImVec2(c.x - s * 0.14f, c.y - s * 0.14f), s * 0.8f);
            Cloud(dl, ImVec2(c.x + s * 0.06f, c.y + s * 0.06f), s * 0.82f, a);
            break;
        case Sky::Cloudy:
            Cloud(dl, ImVec2(c.x + s * 0.1f, c.y - s * 0.08f), s * 0.7f, a, Tone(IM_COL32(150, 142, 180, 255), IM_COL32(110, 98, 160, 255))); // дальняя, темнее
            Cloud(dl, ImVec2(c.x - s * 0.04f, c.y + s * 0.04f), s * 0.9f, a);
            break;
        case Sky::Fog:
            for (int i = 0; i < 3; ++i) {
                const float y = c.y + (i - 1) * s * 0.2f, half = s * (i == 1 ? 0.34f : 0.26f);
                dl->AddLine(ImVec2(c.x - half, y), ImVec2(c.x + half, y), Fade(CloudColor(), a * 0.86f), s * 0.08f);
            }
            break;
        case Sky::Drizzle:
        case Sky::Rain: {
            Cloud(dl, up, s, a);
            const int drops = sky == Sky::Rain ? 3 : 2;
            const ImU32 blue = Fade(RainBlue(), a);
            for (int i = 0; i < drops; ++i) {
                const float x = c.x + (i - (drops - 1) * 0.5f) * s * 0.2f, y = c.y + s * 0.22f;
                dl->AddLine(ImVec2(x + s * 0.04f, y), ImVec2(x - s * 0.04f, y + s * (sky == Sky::Rain ? 0.2f : 0.12f)), blue, s * 0.06f);
            }
            break;
        }
        case Sky::Snow:
            Cloud(dl, up, s, a);
            for (int i = 0; i < 3; ++i)
                dl->AddCircleFilled(ImVec2(c.x + (i - 1) * s * 0.2f, c.y + s * (i == 1 ? 0.36f : 0.28f)), s * 0.045f, Fade(Theme::Overlay(255), a), 12);
            break;
        case Sky::Thunder: {
            Cloud(dl, up, s, a, Tone(IM_COL32(170, 160, 200, 255), IM_COL32(120, 108, 170, 255)));
            const ImVec2 bolt[] = { { c.x + s * 0.04f, c.y + s * 0.12f }, { c.x - s * 0.08f, c.y + s * 0.3f }, { c.x + s * 0.03f, c.y + s * 0.3f }, { c.x - s * 0.06f, c.y + s * 0.46f } };
            dl->AddPolyline(bolt, 4, Fade(IM_COL32(255, 214, 90, 255), a), 0, s * 0.06f);
            break;
        }
        }
    }

    // ------------------------------------------------------------ мелкие значки (линиями, цветом текста)
    void Pin(ImDrawList* dl, ImVec2 c, float s, ImU32 col) { // капля-метка места
        const float r = s * 0.3f;
        dl->PathArcTo(ImVec2(c.x, c.y - s * 0.1f), r, kPi * 0.82f, kPi * 2.18f, 16);
        dl->PathLineTo(ImVec2(c.x, c.y + s * 0.42f));
        dl->PathStroke(col, ImDrawFlags_Closed, Px(1.5f));
        dl->AddCircleFilled(ImVec2(c.x, c.y - s * 0.1f), s * 0.1f, col, 10);
    }

    void SunEdge(ImDrawList* dl, ImVec2 c, float s, bool rise, ImU32 col) { // солнце на горизонте и стрелка
        const float r = s * 0.3f, th = Px(1.4f);
        const ImVec2 base(c.x, c.y + s * 0.2f);
        dl->AddLine(ImVec2(c.x - s * 0.48f, base.y), ImVec2(c.x + s * 0.48f, base.y), col, th);
        dl->PathArcTo(base, r, kPi, kPi * 2.0f, 12);
        dl->PathStroke(col, 0, th);
        const float tip = rise ? base.y - s * 0.62f : base.y - s * 0.38f, tail = rise ? base.y - s * 0.38f : base.y - s * 0.62f;
        const float wing = rise ? s * 0.12f : -s * 0.12f;
        dl->AddLine(ImVec2(c.x, tail), ImVec2(c.x, tip), col, th);
        dl->AddLine(ImVec2(c.x - s * 0.12f, tip + wing), ImVec2(c.x, tip), col, th);
        dl->AddLine(ImVec2(c.x + s * 0.12f, tip + wing), ImVec2(c.x, tip), col, th);
    }

    void WindIcon(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
        const float th = Px(1.5f);
        const float ys[] = { -0.22f, 0.0f, 0.22f }, ends[] = { 0.2f, 0.4f, 0.1f };
        for (int i = 0; i < 3; ++i) {
            const float y = c.y + ys[i] * s;
            dl->AddLine(ImVec2(c.x - s * 0.45f, y), ImVec2(c.x + ends[i] * s, y), col, th);
            if (i != 2) { // завиток на конце
                dl->PathArcTo(ImVec2(c.x + ends[i] * s, y - s * 0.09f), s * 0.09f, kPi * 0.5f, -kPi * 0.9f, 8);
                dl->PathStroke(col, 0, th);
            }
        }
    }

    void DropIcon(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
        const float r = s * 0.28f;
        const ImVec2 o(c.x, c.y + s * 0.12f);
        dl->PathArcTo(o, r, -kPi * 0.2f, kPi * 1.2f, 14);
        dl->PathLineTo(ImVec2(c.x, c.y - s * 0.44f));
        dl->PathStroke(col, ImDrawFlags_Closed, Px(1.5f));
    }

    void UmbrellaIcon(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
        const float th = Px(1.5f), r = s * 0.44f;
        dl->PathArcTo(ImVec2(c.x, c.y), r, kPi, kPi * 2.0f, 14);
        dl->PathStroke(col, ImDrawFlags_Closed, th);
        dl->AddLine(ImVec2(c.x, c.y), ImVec2(c.x, c.y + s * 0.38f), col, th);
        dl->PathArcTo(ImVec2(c.x - s * 0.08f, c.y + s * 0.38f), s * 0.08f, 0.0f, kPi, 6);
        dl->PathStroke(col, 0, th);
    }

    // ------------------------------------------------------------ текст
    // не влезает — обрезаем с многоточием
    std::string Fit(ImFont* font, std::string text, float width) {
        if (Widgets::TextSize(font, text.c_str()).x <= width) return text;
        while (!text.empty()) {
            size_t cut = text.size() - 1;
            while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut; // целая буква UTF-8
            text.erase(cut);
            if (Widgets::TextSize(font, (text + "…").c_str()).x <= width) return text + "…";
        }
        return text;
    }

    std::vector<std::string> Wrap(ImFont* font, const std::string& text, float width, int maxLines) {
        std::vector<std::string> lines;
        std::string line;
        size_t pos = 0;
        while (pos < text.size()) {
            size_t space = text.find(' ', pos);
            if (space == std::string::npos) space = text.size();
            const std::string word = text.substr(pos, space - pos);
            const std::string candidate = line.empty() ? word : line + " " + word;
            if (!line.empty() && Widgets::TextSize(font, candidate.c_str()).x > width) { lines.push_back(line); line = word; }
            else line = candidate;
            pos = space + 1;
        }
        if (!line.empty()) lines.push_back(line);
        if (static_cast<int>(lines.size()) > maxLines) {
            lines.resize(maxLines);
            lines.back() = Fit(font, lines.back() + "…", width);
        }
        return lines;
    }

    std::string Decimal(float v) { // «2,4» по-русски, «2.4» по-английски
        char text[16];
        sprintf_s(text, "%.1f", v);
        std::string s = text;
        if (!Lang::IsEnglish()) std::replace(s.begin(), s.end(), '.', ',');
        return s;
    }

    const char* Compass(int degrees) {
        static const char* kSides[] = { "С", "СВ", "В", "ЮВ", "Ю", "ЮЗ", "З", "СЗ" };
        const int d = (degrees % 360 + 360) % 360;
        return Lang::Tr(kSides[(d * 2 + 45) / 90 % 8]);
    }

    std::string Description(const Weather::Today& w) { return Text::Capitalize(Lang::Tr(w.description.c_str())); }

    // цвет полоски температур: холод голубой, середина — любимый цвет, жара тёплая
    Theme::Color TempColor(float t) {
        const Theme::Color cold{ 110, 175, 255 }, warm{ 255, 168, 92 };
        const Theme::Color mid = Theme::Accent().Mix({ 255, 255, 255 }, 0.15f);
        return t < 12 ? cold.Mix(mid, std::clamp((t + 4) / 16.0f, 0.0f, 1.0f)) : mid.Mix(warm, std::clamp((t - 12) / 16.0f, 0.0f, 1.0f));
    }

    // ------------------------------------------------------------ маленький вид
    // Шапка: значок, температура, город; под ней — описание
    float DrawHeader(ImDrawList* dl, const Weather::Today& w, ImVec2 p, float width, float a) {
        DrawSky(dl, w.sky, w.night, ImVec2(p.x + Px(20), p.y + Px(21)), Px(40), a);
        const std::string temp = Temp(w.temperature);
        Widgets::Text(dl, Theme::hero, ImVec2(p.x + Px(46), p.y + Px(2)), Fade(Theme::kInk, a), temp.c_str());
        const float tempEnd = Px(46) + Widgets::TextSize(Theme::hero, temp.c_str()).x + Px(10);
        const std::string city = Fit(Theme::semibold, Lang::IsEnglish() ? Lang::Latin(w.city) : w.city, width - tempEnd);
        const float cityWidth = Widgets::TextSize(Theme::semibold, city.c_str()).x;
        Widgets::Text(dl, Theme::semibold, ImVec2(p.x + width - cityWidth, p.y + Px(4)), Fade(Theme::kInkMuted, a), city.c_str());
        const std::string sky = Fit(Theme::semibold, Description(w), width);
        Widgets::Text(dl, Theme::semibold, ImVec2(p.x, p.y + Px(kHeader)), Fade(Theme::kInkSoft, a), sky.c_str());
        return Px(kHeader + kLine);
    }

    void DrawHour(ImDrawList* dl, const Weather::HourView& h, ImVec2 p, ImVec2 size, bool current, bool past, bool rain, float a) {
        const float cx = p.x + size.x * 0.5f;
        if (current) Theme::Pick(dl, p, ImVec2(p.x + size.x, p.y + size.y), Px(12), a);
        const float dim = past ? 0.45f : 1.0f;
        char label[16];
        sprintf_s(label, "%02d:00", h.hour);
        const std::string text = current ? (Widgets::TextSize(Theme::semibold, Lang::Tr("сейчас")).x <= size.x - Px(4) ? std::string(Lang::Tr("сейчас")) : std::string(Lang::Tr("сейч.") )) : label; // узкая ячейка — коротко, без «…»
        TextCentered(dl, Theme::semibold, cx, p.y + Px(6), Fade(current ? Theme::kInk : Theme::kInkMuted, a * dim), Fit(Theme::semibold, text, size.x - Px(4)));
        const float tempY = p.y + size.y - Px(rain ? 42.0f : 26.0f);
        DrawSky(dl, h.sky, h.night, ImVec2(cx, (p.y + Px(6) + Theme::semibold->LegacySize + tempY) * 0.5f), Px(26), a * dim);
        TextCentered(dl, Theme::body, cx, tempY, Fade(Theme::kInk, a * dim), Temp(h.temperature));
        if (rain && h.rainChance >= 20) {
            char chance[16];
            sprintf_s(chance, "%d%%", h.rainChance);
            TextCentered(dl, Theme::semibold, cx, tempY + Theme::body->LegacySize + Px(2), Fade(RainBlue(), a * dim), chance);
        }
    }

    ImVec2 MiniSize() { return ImVec2(Px(kMiniCell * 4 + kPad * 2), Px(kPad * 2 + kHeader + kLine + 8 + kMiniCellHeight)); }

    void DrawMini(ImDrawList* dl, const Weather::Today& w, ImVec2 p, float a) {
        const float width = MiniSize().x - Px(kPad * 2);
        const float y = p.y + DrawHeader(dl, w, p, width, a) + Px(8);
        for (size_t i = 0; i < w.around.size(); ++i) {
            const auto& h = w.around[i];
            DrawHour(dl, h, ImVec2(p.x + i * Px(kMiniCell), y), ImVec2(Px(kMiniCell - 4), Px(kMiniCellHeight)), h.hour == w.currentHour,
                h.hour == (w.currentHour + 23) % 24, false, a);
        }
    }

    // ------------------------------------------------------------ раскрытый вид
    // Сверху вниз: город и солнце, сейчас, совет, три плитки, лента часов, дни.
    // Не хватает высоты окна — дней меньше, совсем тесно — без ленты часов
    constexpr float kFullWidth = 380, kFullPad = 16, kTile = 52, kHourCell = 92, kDayRow = 30, kGap = 10;
    constexpr int kHours = 6;

    struct FullLayout {
        float width = 0, height = 0;
        float top = 0, hero = 0;     // строка города, блок «сейчас»
        std::vector<std::string> advice;
        bool hours = true;
        int days = 0;
    };

    float DayRow() { return std::max(Px(kDayRow), Theme::semibold->LegacySize + Px(12)); }

    FullLayout Layout(const Weather::Today& w, float maxHeight) {
        FullLayout l;
        l.width = Px(kFullWidth);
        const float inner = l.width - Px(kFullPad * 2);
        l.top = std::max(Px(18), Theme::semibold->LegacySize + Px(4));
        l.hero = std::max(Px(56), Theme::hero->LegacySize + Px(14));
        if (!w.advice.empty()) l.advice = Wrap(Theme::body, Lang::Tr(w.advice.c_str()), inner, 2);
        float h = Px(kFullPad * 2) + l.top + Px(8) + l.hero + Px(kGap) + Px(kTile);
        if (!l.advice.empty()) h += l.advice.size() * Theme::body->LegacySize * 1.3f + Px(kGap);
        const float hours = Px(kGap + kHourCell);
        l.hours = h + hours <= maxHeight && !w.next.empty();
        if (l.hours) h += hours;
        const int fit = static_cast<int>((maxHeight - h - Px(kGap)) / DayRow());
        l.days = std::clamp(fit, 0, static_cast<int>(w.week.size()));
        if (l.days > 0) h += Px(kGap) + l.days * DayRow();
        l.height = h;
        return l;
    }

    void DrawTile(ImDrawList* dl, ImVec2 a, ImVec2 b, int icon, const char* caption, const std::string& value, float alpha) {
        Theme::Plate(dl, a, b, Fade(Theme::Surface(), 0.7f * alpha), Px(12));
        const ImU32 muted = Fade(Theme::kInkMuted, alpha);
        const float s = Px(13), pad = Px(10), cy = a.y + Px(8) + Theme::semibold->LegacySize * 0.5f;
        const ImVec2 ic(a.x + pad + s * 0.5f, cy);
        if (icon == 0) WindIcon(dl, ic, s, muted);
        else if (icon == 1) DropIcon(dl, ic, s, muted);
        else UmbrellaIcon(dl, ic, s, muted);
        const float textX = a.x + pad + s + Px(6), room = b.x - textX - pad;
        Widgets::Text(dl, Theme::semibold, ImVec2(textX, cy - Theme::semibold->LegacySize * 0.5f), muted, Fit(Theme::semibold, Lang::Tr(caption), room).c_str());
        Widgets::Text(dl, Theme::title, ImVec2(a.x + pad, b.y - Theme::title->LegacySize - Px(7)), Fade(Theme::kInk, alpha),
                      Fit(Theme::title, value, b.x - a.x - pad * 2).c_str());
    }

    void DrawFull(ImDrawList* dl, const Weather::Today& w, const FullLayout& l, ImVec2 p, float a) {
        const float width = l.width - Px(kFullPad * 2);
        const ImU32 ink = Fade(Theme::kInk, a), soft = Fade(Theme::kInkSoft, a), muted = Fade(Theme::kInkMuted, a);
        const float lineH = Theme::semibold->LegacySize;
        float y = p.y;

        // город слева, восход и закат справа
        {
            const float cy = y + l.top * 0.5f, s = Px(15);
            float right = p.x + width;
            auto sunTime = [&](const std::string& time, bool rise) {
                if (time.empty()) return;
                right -= Widgets::TextSize(Theme::semibold, time.c_str()).x;
                Widgets::Text(dl, Theme::semibold, ImVec2(right, cy - lineH * 0.5f), muted, time.c_str());
                right -= Px(5) + s * 0.5f;
                SunEdge(dl, ImVec2(right, cy), s, rise, muted);
                right -= s * 0.5f + Px(12);
            };
            sunTime(w.sunset, false);
            sunTime(w.sunrise, true);
            Pin(dl, ImVec2(p.x + s * 0.4f, cy), s, soft);
            const std::string city = Fit(Theme::semibold, Lang::IsEnglish() ? Lang::Latin(w.city) : w.city, right - p.x - s - Px(6));
            Widgets::Text(dl, Theme::semibold, ImVec2(p.x + s + Px(4), cy - lineH * 0.5f), soft, city.c_str());
            y += l.top + Px(8);
        }

        // сейчас: значок, температура, описание и «ощущается»
        {
            const float icon = Px(54);
            DrawSky(dl, w.sky, w.night, ImVec2(p.x + icon * 0.5f, y + l.hero * 0.5f), icon, a);
            const std::string temp = Temp(w.temperature);
            const float tx = p.x + icon + Px(8), tw = Widgets::TextSize(Theme::hero, temp.c_str()).x;
            Widgets::Text(dl, Theme::hero, ImVec2(tx, y + (l.hero - Theme::hero->LegacySize) * 0.5f), ink, temp.c_str());
            const float cx = tx + tw + Px(14), room = p.x + width - cx;
            const float block = Theme::title->LegacySize + Px(4) + lineH, top = y + (l.hero - block) * 0.5f;
            Widgets::Text(dl, Theme::title, ImVec2(cx, top), ink, Fit(Theme::title, Description(w), room).c_str());
            const float low = w.week.empty() ? w.low : w.week.front().low, high = w.week.empty() ? w.high : w.week.front().high;
            char line[160];
            sprintf_s(line, Lang::Tr("ощущается как %s · %s…%s"), Temp(w.feelsLike).c_str(), Temp(low).c_str(), Temp(high).c_str());
            Widgets::Text(dl, Theme::semibold, ImVec2(cx, top + Theme::title->LegacySize + Px(4)), muted, Fit(Theme::semibold, line, room).c_str());
            y += l.hero + Px(kGap);
        }

        // совет
        for (const auto& text : l.advice) {
            Widgets::Text(dl, Theme::body, ImVec2(p.x, y), soft, text.c_str());
            y += Theme::body->LegacySize * 1.3f;
        }
        if (!l.advice.empty()) y += Px(kGap);

        // три плитки
        {
            const float gap = Px(8), tile = (width - gap * 2) / 3.0f;
            char text[64];
            sprintf_s(text, "%d %s %s", static_cast<int>(std::lround(w.wind)), Lang::Tr("км/ч"), Compass(w.windFrom));
            DrawTile(dl, ImVec2(p.x, y), ImVec2(p.x + tile, y + Px(kTile)), 0, "ветер", text, a);
            sprintf_s(text, "%d%%", w.humidity);
            DrawTile(dl, ImVec2(p.x + tile + gap, y), ImVec2(p.x + tile * 2 + gap, y + Px(kTile)), 1, "влажность", text, a);
            const std::string rain = Decimal(w.rainToday) + " " + Lang::Tr("мм");
            DrawTile(dl, ImVec2(p.x + (tile + gap) * 2, y), ImVec2(p.x + width, y + Px(kTile)), 2, "осадки", rain, a);
            y += Px(kTile);
        }

        // лента часов: сейчас и дальше
        if (l.hours) {
            y += Px(kGap);
            const float cell = width / kHours;
            for (int i = 0; i < kHours && i < static_cast<int>(w.next.size()); ++i)
                DrawHour(dl, w.next[i], ImVec2(p.x + i * cell, y), ImVec2(cell - Px(4), Px(kHourCell)), i == 0, false, true, a);
            y += Px(kHourCell);
        }

        // дни: имя, значок, минимум, полоска на шкале недели, максимум, осадки
        if (l.days > 0) {
            y += Px(kGap);
            static const char* kDays[] = { "Вс", "Пн", "Вт", "Ср", "Чт", "Пт", "Сб" };
            float weekLow = 1e9f, weekHigh = -1e9f;
            bool anyRain = false;
            for (int i = 0; i < l.days; ++i) {
                weekLow = std::min(weekLow, w.week[i].low), weekHigh = std::max(weekHigh, w.week[i].high);
                anyRain |= w.week[i].rain >= 0.1f;
            }
            const float span = std::max(weekHigh - weekLow, 1.0f), row = DayRow();
            const float nameW = std::max(Widgets::TextSize(Theme::semibold, Lang::Tr("Сегодня")).x, Px(30)) + Px(10);
            const float tempW = Widgets::TextSize(Theme::semibold, "-00°").x;
            const float rainW = anyRain ? Widgets::TextSize(Theme::semibold, (Decimal(88.8f) + " " + Lang::Tr("мм")).c_str()).x + Px(10) : 0.0f;
            const float pad = Px(8), iconX = p.x + pad + nameW + Px(11);
            const float lowRight = iconX + Px(20) + tempW;
            const float barA = lowRight + Px(8), barB = p.x + width - pad - rainW - tempW - Px(8);
            for (int i = 0; i < l.days; ++i) {
                const Weather::DayView& d = w.week[i];
                const float top = y + i * row, cy = top + row * 0.5f, ty = cy - lineH * 0.5f;
                if (i == 0) Theme::Pick(dl, ImVec2(p.x, top + Px(1)), ImVec2(p.x + width, top + row - Px(1)), Px(10), a, true);
                Widgets::Text(dl, Theme::semibold, ImVec2(p.x + pad, ty), i == 0 ? ink : soft, Lang::Tr(i == 0 ? "Сегодня" : kDays[d.weekday]));
                DrawSky(dl, d.sky, false, ImVec2(iconX, cy), Px(22), a);
                const std::string lo = Temp(d.low), hi = Temp(d.high);
                Widgets::Text(dl, Theme::semibold, ImVec2(lowRight - Widgets::TextSize(Theme::semibold, lo.c_str()).x, ty), muted, lo.c_str());
                Widgets::Text(dl, Theme::semibold, ImVec2(barB + Px(8), ty), ink, hi.c_str());
                if (barB - barA > Px(20)) {
                    const float bh = Px(5);
                    dl->AddRectFilled(ImVec2(barA, cy - bh * 0.5f), ImVec2(barB, cy + bh * 0.5f), Fade(Theme::Overlay(28), a), bh);
                    const float x0 = barA + (barB - barA) * (d.low - weekLow) / span;
                    const float x1 = std::max(x0 + bh, barA + (barB - barA) * (d.high - weekLow) / span);
                    const int start = dl->VtxBuffer.Size;
                    dl->AddRectFilled(ImVec2(x0, cy - bh * 0.5f), ImVec2(x1, cy + bh * 0.5f), IM_COL32_WHITE, bh);
                    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, start, dl->VtxBuffer.Size, ImVec2(x0, cy), ImVec2(x1, cy),
                                                                  TempColor(d.low).WithAlpha(a), TempColor(d.high).WithAlpha(a));
                    if (i == 0) { // где мы сейчас на сегодняшней полоске
                        const float now = std::clamp(barA + (barB - barA) * (w.temperature - weekLow) / span, x0, x1);
                        dl->AddCircleFilled(ImVec2(now, cy), bh * 0.95f, ink, 14);
                        dl->AddCircle(ImVec2(now, cy), bh * 0.95f, Fade(Theme::SurfaceSolid(), a), 14, Px(1.5f));
                    }
                }
                if (d.rain >= 0.1f) {
                    const std::string rain = Decimal(d.rain) + " " + Lang::Tr("мм");
                    const float rw = Widgets::TextSize(Theme::semibold, rain.c_str()).x;
                    Widgets::Text(dl, Theme::semibold, ImVec2(p.x + width - pad - rw, ty), Fade(RainBlue(), a), rain.c_str());
                }
            }
        }
    }
}

float WeatherWidget::MiniWidth() { return MiniSize().x; }

namespace { ImVec2 g_boundsMin, g_boundsMax; }
void WeatherWidget::Bounds(ImVec2& min, ImVec2& max) { min = g_boundsMin, max = g_boundsMax; }

void WeatherWidget::Draw(ImDrawList* dl, float left, float centerY, float top, float bottom, bool hover, float dt) {
    const Weather::Today w = Weather::Snapshot();
    const float screenBottom = ImGui::GetIO().DisplaySize.y - Px(12);
    const FullLayout layout = Layout(w, screenBottom - top);
    const ImVec2 mini = MiniSize(), full(layout.width, std::max(layout.height, mini.y));
    const float ease = g_open * g_open * (3.0f - 2.0f * g_open); // плавный старт и финиш
    ImVec2 size(mini.x + (full.x - mini.x) * ease, mini.y + (full.y - mini.y) * ease);
    if (!w.ready) size = ImVec2(mini.x, Px(52));

    // по центру сферы, но не залезая на шапку и поле ввода (раскрытой можно ниже — она поверх)
    float y = centerY - size.y * 0.5f;
    y = std::max(top, std::min(y, (g_open > 0.01f ? screenBottom : bottom) - size.y));
    const ImVec2 min(left, y), max(left + size.x, y + size.y);
    g_boundsMin = min, g_boundsMax = ImVec2(left + mini.x, y + (w.ready ? mini.y : size.y));

    const bool hovered = hover && w.ready && ImGui::IsMouseHoveringRect(min, max, false);
    g_open += ((hovered ? 1.0f : 0.0f) - g_open) * Theme::Ease(dt, 6.2f);

    Theme::Plate(dl, min, max, Theme::Surface(0.82f + 0.18f * ease), Px(20), ease);

    dl->PushClipRect(min, max, true);
    if (!w.ready) {
        Widgets::Text(dl, Theme::semibold, ImVec2(min.x + Px(kPad), min.y + (size.y - Theme::semibold->LegacySize) * 0.5f), Theme::kInkMuted, "погода загружается…");
    } else if (ease < 0.5f) {
        DrawMini(dl, w, ImVec2(min.x + Px(kPad), min.y + Px(kPad)), 1.0f - ease * 2.0f);
    } else {
        DrawFull(dl, w, layout, ImVec2(min.x + Px(kFullPad), min.y + Px(kFullPad)), ease * 2.0f - 1.0f);
    }
    dl->PopClipRect();
}
