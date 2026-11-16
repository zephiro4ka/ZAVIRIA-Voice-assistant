#include "HomeWidgets.h"

#include "Lang.h"
#include "MediaNow.h"
#include "ProcessMonitor.h"
#include "Settings.h"
#include "Rates.h"
#include "SystemMonitor.h"
#include "TextUtils.h"
#include "Theme.h"
#include "Widgets.h"

#include "HomeLayout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <atomic>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

using Theme::Px;
using Widgets::Icon;

namespace {
    constexpr float kWidth = 270, kPad = 16, kRadius = 20;

    float Approach(float& value, float target, float dt, float speed = 6.0f) {
        value += (target - value) * std::min(1.0f, dt * speed);
        return value;
    }

    ImU32 Fade(ImU32 color, float alpha) {
        const float a = static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f);
        return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
    }

    // Строка, обрезанная по ширине с «…»
    std::string Fit(ImFont* font, const std::string& text, float width) {
        if (Widgets::TextSize(font, text.c_str()).x <= width) return text;
        std::wstring w = Text::ToWide(text);
        while (!w.empty() && Widgets::TextSize(font, (Text::ToUtf8(w) + "…").c_str()).x > width) w.pop_back();
        return Text::ToUtf8(w) + "…";
    }

    ImU32 TempColor(float t) {
        if (t < 60) return IM_COL32(110, 231, 183, 255);
        if (t < 80) return IM_COL32(250, 204, 21, 255);
        return IM_COL32(251, 113, 133, 255);
    }

    // Карточка: фон, рамка, при наведении — чуть приподнимается и светлеет. Возвращает сдвиг (подъём)
    float Card(ImDrawList* dl, ImVec2 min, ImVec2 max, float alpha, float& hover) {
        Approach(hover, ImGui::IsMouseHoveringRect(min, max, false) ? 1.0f : 0.0f, ImGui::GetIO().DeltaTime, 12.0f);
        const float lift = Px(2) * hover;
        min.y -= lift, max.y -= lift;
        Theme::Plate(dl, min, max, Fade(Theme::Surface(0.86f + 0.08f * hover), alpha), Px(kRadius), hover);
        return lift;
    }

    // ------------------------------------------------------------ «Система»
    struct Meter { float value = 0, temp = 0; };
    Meter g_cpu, g_gpu, g_ram, g_vram, g_disk;
    float g_sysHover = 0, g_mediaHover = 0;

    void MeterRow(ImDrawList* dl, ImVec2 pos, float width, const char* label, const std::string& value, float fraction, float temp, float alpha) {
        Widgets::Text(dl, Theme::semibold, pos, Fade(Theme::kInkSoft, alpha), label);
        float right = pos.x + width;
        if (temp > 0) { // температура — цветной «бейдж» справа
            char t[16];
            snprintf(t, sizeof(t), "%d°", static_cast<int>(std::lround(temp)));
            const float tw = Widgets::TextSize(Theme::semibold, t).x;
            Widgets::Text(dl, Theme::semibold, ImVec2(right - tw, pos.y), Fade(TempColor(temp), alpha), t);
            right -= tw + Px(10);
        }
        const float vw = Widgets::TextSize(Theme::semibold, value.c_str()).x;
        Widgets::Text(dl, Theme::semibold, ImVec2(right - vw, pos.y), Fade(Theme::kInk, alpha), value.c_str());

        const float barY = pos.y + Theme::semibold->LegacySize + Px(6), barH = Px(5);
        dl->AddRectFilled(ImVec2(pos.x, barY), ImVec2(pos.x + width, barY + barH), Theme::Overlay(static_cast<int>(16 * alpha)), barH);
        const float fill = std::clamp(fraction, 0.0f, 1.0f) * width;
        if (fill > 1) {
            const Theme::Color a = Theme::Accent(), b = Theme::Accent().Mix({ 244, 114, 182 }, 0.6f);
            dl->AddRectFilledMultiColor(ImVec2(pos.x, barY), ImVec2(pos.x + fill, barY + barH), a.WithAlpha(alpha), b.WithAlpha(alpha), b.WithAlpha(alpha), a.WithAlpha(alpha));
        }
    }

    void Sparkline(ImDrawList* dl, ImVec2 min, ImVec2 max, const std::array<float, SystemMonitor::kHistory>& data, ImU32 color, float alpha) {
        ImVec2 points[SystemMonitor::kHistory];
        for (int i = 0; i < SystemMonitor::kHistory; ++i) {
            const float x = min.x + (max.x - min.x) * i / (SystemMonitor::kHistory - 1);
            points[i] = ImVec2(x, max.y - (max.y - min.y) * std::clamp(data[i] / 100.0f, 0.0f, 1.0f));
        }
        // заливка под линией: столбики сверху вниз — прозрачнее
        for (int i = 0; i + 1 < SystemMonitor::kHistory; ++i)
            dl->AddQuadFilled(points[i], points[i + 1], ImVec2(points[i + 1].x, max.y), ImVec2(points[i].x, max.y), Fade(color, 0.12f * alpha));
        dl->AddPolyline(points, SystemMonitor::kHistory, Fade(color, alpha), 0, Px(1.5f));
    }

    constexpr float kPi = 3.14159265f;
    // Мелкая подпись (10 px) — под кольцами три колонки по ~75 px, обычный шрифт туда не влезает
    float SmallSize() { return Px(10.5f); }
    ImVec2 SmallTextSize(const char* text) { return Theme::mono->CalcTextSizeA(SmallSize(), FLT_MAX, 0.0f, text); }
    void SmallText(ImDrawList* dl, ImVec2 pos, ImU32 color, const char* text) { dl->AddText(Theme::mono, SmallSize(), pos, color, text); }
    // Кольцо загрузки: дорожка, дуга в цветах логотипа (от акцента к розовому), число в центре, подпись снизу
    void Ring(ImDrawList* dl, ImVec2 c, float r, float fraction, const char* value, const char* label, const char* sub, ImU32 subColor, float alpha) {
        const float thick = Px(6);
        dl->PathArcTo(c, r, 0, kPi * 2, 48);
        dl->PathStroke(Theme::Overlay(static_cast<int>(18 * alpha)), ImDrawFlags_Closed, thick);
        const float f = std::clamp(fraction, 0.0f, 1.0f);
        if (f > 0.005f) {
            const Theme::Color a = Theme::Accent(), b = Theme::Accent().Mix({ 244, 114, 182 }, 0.6f);
            const int segments = std::max(2, static_cast<int>(48 * f));
            const float start = -kPi * 0.5f, sweep = kPi * 2 * f;
            for (int i = 0; i < segments; ++i) { // по кусочку — цвет плавно переходит вдоль дуги
                const float t0 = static_cast<float>(i) / segments, t1 = static_cast<float>(i + 1) / segments;
                dl->PathArcTo(c, r, start + sweep * t0, start + sweep * t1 + 0.01f, 3);
                dl->PathStroke(a.Mix(b, (t0 + t1) * 0.5f).WithAlpha(alpha), 0, thick);
            }
            const ImVec2 cap0(c.x, c.y - r), cap1(c.x + std::cos(start + sweep) * r, c.y + std::sin(start + sweep) * r);
            dl->AddCircleFilled(cap0, thick * 0.5f, a.WithAlpha(alpha), 12); // скруглённые концы
            dl->AddCircleFilled(cap1, thick * 0.5f, b.Mix(a, 1.0f - f).WithAlpha(alpha), 12);
        }
        const ImVec2 vs = Widgets::TextSize(Theme::semibold, value);
        Widgets::Text(dl, Theme::semibold, ImVec2(c.x - vs.x * 0.5f, c.y - vs.y * 0.5f), Fade(Theme::kInk, alpha), value);
        const ImVec2 ls = Widgets::TextSize(Theme::semibold, label);
        Widgets::Text(dl, Theme::semibold, ImVec2(c.x - ls.x * 0.5f, c.y + r + Px(8)), Fade(Theme::kInkSoft, alpha), label);
        if (sub && *sub) {
            const ImVec2 ss = SmallTextSize(sub);
            SmallText(dl, ImVec2(c.x - ss.x * 0.5f, c.y + r + Px(8) + ls.y + Px(2)), Fade(subColor, alpha), sub);
        }
    }

    // «Система» (вариант «кольца»): процессор, память и видеокарта (или диск) кольцами, внизу — сеть и диск одной строкой
    float DrawSystem(ImDrawList* dl, ImVec2 pos, float dt, float alpha) {
        const auto s = SystemMonitor::Snapshot();
        const float width = Px(kWidth), inner = width - Px(kPad) * 2;
        const float ringR = std::min(Px(30), (inner / 3 - Px(10)) * 0.5f);
        const float graphH = Px(42); // под кольцами — график процессора и видеокарты за минуту
        const float height = Px(kPad) * 2 + Px(26) + ringR * 2 + Px(48) + Px(14) + graphH + Px(8);
        const float lift = Card(dl, pos, ImVec2(pos.x + width, pos.y + height), alpha, g_sysHover);
        ImVec2 p(pos.x + Px(kPad), pos.y + Px(kPad) - lift);

        // заголовок с «живым» огоньком
        Widgets::DrawIcon(dl, Icon::Grid, ImVec2(p.x + Px(7), p.y + Px(8)), Px(14), Fade(Theme::kInkSoft, alpha));
        Widgets::Text(dl, Theme::semibold, ImVec2(p.x + Px(22), p.y), Fade(Theme::kInk, alpha), "Система");
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f);
        dl->AddCircleFilled(ImVec2(p.x + inner - Px(4), p.y + Px(8)), Px(3.5f), Fade(IM_COL32(110, 231, 183, 255), alpha * (0.5f + 0.5f * pulse)), 12);
        p.y += Px(26);

        if (!s.ready) {
            Widgets::Text(dl, Theme::semibold, p, Fade(Theme::kInkMuted, alpha), "собираю данные…");
            return height;
        }
        char value[24], sub[32];
        const float cy = p.y + ringR + Px(4);
        const float step = inner / 3;
        auto tempColor = [](float t) { return t > 0 ? TempColor(t) : Theme::kInkMuted; };

        const float cpu = Approach(g_cpu.value, s.cpu, dt);
        snprintf(value, sizeof(value), "%d%%", static_cast<int>(std::lround(cpu)));
        if (s.cpuTemp > 0) snprintf(sub, sizeof(sub), "%d°", static_cast<int>(std::lround(s.cpuTemp)));
        else snprintf(sub, sizeof(sub), "%.1f ГГц", s.cpuFreqGhz);
        Ring(dl, ImVec2(p.x + step * 0.5f, cy), ringR, cpu / 100.0f, value, "Процессор", sub, tempColor(s.cpuTemp), alpha);

        const float ram = Approach(g_ram.value, s.ram / 100.0f, dt);
        snprintf(value, sizeof(value), "%d%%", static_cast<int>(std::lround(ram * 100)));
        snprintf(sub, sizeof(sub), "%.0f/%.0f ГБ", s.ramUsedGb, s.ramTotalGb);
        Ring(dl, ImVec2(p.x + step * 1.5f, cy), ringR, ram, value, "Память", sub, Theme::kInkMuted, alpha);

        if (s.hasGpu) {
            const float gpu = Approach(g_gpu.value, s.gpu, dt);
            snprintf(value, sizeof(value), "%d%%", static_cast<int>(std::lround(gpu)));
            if (s.gpuTemp > 0) snprintf(sub, sizeof(sub), "%d°", static_cast<int>(std::lround(s.gpuTemp)));
            else snprintf(sub, sizeof(sub), "%.0f/%.0f ГБ", s.vramUsedGb, s.vramTotalGb);
            Ring(dl, ImVec2(p.x + step * 2.5f, cy), ringR, gpu / 100.0f, value, "Видеокарта", sub, tempColor(s.gpuTemp), alpha);
        } else {
            const float disk = Approach(g_disk.value, s.diskTotalGb > 0 ? 1.0f - s.diskFreeGb / s.diskTotalGb : 0, dt);
            snprintf(value, sizeof(value), "%d%%", static_cast<int>(std::lround(disk * 100)));
            snprintf(sub, sizeof(sub), "%.0f ГБ своб.", s.diskFreeGb);
            Ring(dl, ImVec2(p.x + step * 2.5f, cy), ringR, disk, value, "Диск C", sub, Theme::kInkMuted, alpha);
        }
        p.y = cy + ringR + Px(48);

        { // график за минуту: процессор — цвет темы, видеокарта — розовее
            const ImVec2 gMin(p.x, p.y), gMax(p.x + inner, p.y + graphH);
            dl->AddRectFilled(gMin, gMax, Theme::Overlay(static_cast<int>(8 * alpha)), Px(8));
            if (s.hasGpu) Sparkline(dl, gMin, gMax, s.gpuHistory, Theme::Accent().Mix({ 244, 114, 182 }, 0.6f).WithAlpha(0.9f), alpha);
            Sparkline(dl, gMin, gMax, s.cpuHistory, Theme::Accent().WithAlpha(1.0f), alpha);
            p.y += graphH + Px(8);
        }
        auto speed = [](float kbps, char* out, size_t size) {
            if (kbps >= 1024) snprintf(out, size, "%.1f МБ/с", kbps / 1024);
            else snprintf(out, size, "%.0f КБ/с", kbps);
        };
        char down[24], up[24], text[96];
        speed(s.netDownKbps, down, sizeof(down));
        speed(s.netUpKbps, up, sizeof(up));
        snprintf(text, sizeof(text), "сеть ↓ %s ↑ %s", down, up);
        SmallText(dl, p, Fade(Theme::kInkMuted, alpha), text);
        if (s.hasGpu) snprintf(text, sizeof(text), "C: %.0f ГБ", s.diskFreeGb);
        else snprintf(text, sizeof(text), "диск %d%%", static_cast<int>(s.diskActive));
        const ImVec2 ts = SmallTextSize(text);
        SmallText(dl, ImVec2(p.x + inner - ts.x, p.y), Fade(Theme::kInkMuted, alpha), text);
        return height;
    }

    // ------------------------------------------------------------ «Сейчас играет»
    float g_progress = 0, g_bars[4] = {}, g_mediaShow = 0;

    float DrawMedia(ImDrawList* dl, ImVec2 pos, float dt, float alpha) {
        const auto t = MediaNow::Snapshot();
        Approach(g_mediaShow, t.present ? 1.0f : 0.0f, dt, 8.0f);
        if (g_mediaShow < 0.02f) return 0;
        alpha *= g_mediaShow;
        const float width = Px(kWidth), inner = width - Px(kPad) * 2, height = Px(126);
        const float lift = Card(dl, pos, ImVec2(pos.x + width, pos.y + height), alpha, g_mediaHover);
        ImVec2 p(pos.x + Px(kPad), pos.y + Px(kPad) - lift);

        // эквалайзер: прыгает, когда играет
        const float time = static_cast<float>(ImGui::GetTime());
        for (int i = 0; i < 4; ++i) {
            const float target = t.playing ? 0.35f + 0.65f * std::fabs(std::sin(time * (3.1f + i * 1.3f) + i)) : 0.2f;
            Approach(g_bars[i], target, dt, 10.0f);
            const float h = Px(12) * g_bars[i];
            dl->AddRectFilled(ImVec2(p.x + i * Px(4), p.y + Px(13) - h), ImVec2(p.x + i * Px(4) + Px(2.5f), p.y + Px(13)), Fade(Theme::Accent().WithAlpha(1.0f), alpha), Px(1));
        }
        const std::string source = t.app + (t.playing ? " · играет" : " · пауза");
        Widgets::Text(dl, Theme::semibold, ImVec2(p.x + Px(22), p.y), Fade(Theme::kInkMuted, alpha), Fit(Theme::semibold, source, inner - Px(22)).c_str());
        p.y += Px(22);
        Widgets::Text(dl, Theme::body, p, Fade(Theme::kInk, alpha), Fit(Theme::body, t.title.empty() ? "без названия" : t.title, inner).c_str());
        p.y += Theme::body->LegacySize + Px(2);
        if (!t.artist.empty()) Widgets::Text(dl, Theme::semibold, p, Fade(Theme::kInkSoft, alpha), Fit(Theme::semibold, t.artist, inner).c_str());
        p.y += Theme::semibold->LegacySize + Px(10);

        // полоска прогресса и кнопки
        const float barY = pos.y + height - Px(kPad) - Px(30) - lift;
        if (t.duration > 0) {
            Approach(g_progress, static_cast<float>(t.position / t.duration), dt, 4.0f);
            const float w = inner - Px(110);
            dl->AddRectFilled(ImVec2(p.x, barY + Px(13)), ImVec2(p.x + w, barY + Px(16)), Theme::Overlay(static_cast<int>(20 * alpha)), Px(2));
            dl->AddRectFilled(ImVec2(p.x, barY + Px(13)), ImVec2(p.x + w * std::clamp(g_progress, 0.0f, 1.0f), barY + Px(16)), Fade(Theme::Accent().WithAlpha(1.0f), alpha), Px(2));
        }
        const float size = Px(30);
        float x = pos.x + width - Px(kPad) - size;
        if (Widgets::IconButton("##mediaNext", ImVec2(x, barY), size, Icon::Next)) MediaNow::Control(MediaNow::Action::Next);
        x -= size + Px(6);
        if (Widgets::IconButton("##mediaPlay", ImVec2(x, barY), size, t.playing ? Icon::Pause : Icon::Play)) MediaNow::Control(MediaNow::Action::PlayPause);
        x -= size + Px(6);
        if (Widgets::IconButton("##mediaPrev", ImVec2(x, barY), size, Icon::Previous)) MediaNow::Control(MediaNow::Action::Previous);
        return height;
    }
}

namespace {
    // ------------------------------------------------------------ «Курсы валют»
    float g_ratesHover = 0;

    std::string Money(double value) {
        char text[32];
        if (value < 100) { snprintf(text, sizeof(text), "%.2f", value); return text; }
        snprintf(text, sizeof(text), "%.0f", value);
        std::string out = text;
        for (int at = static_cast<int>(out.size()) - 3; at > 0; at -= 3) out.insert(at, " ");
        return out;
    }

    const char* UnitName(const std::string& base) { return base == "UZS" ? " сум" : base == "RUB" ? " руб" : base == "KZT" ? " тг" : ""; }

    float RatesHeight() { return Px(kPad) * 2 + Px(26) + Px(27) * 3; }

    float DrawRates(ImDrawList* dl, ImVec2 pos, float alpha) {
        const auto data = Rates::Snapshot();
        const float width = Px(kWidth), inner = width - Px(kPad) * 2, height = RatesHeight();
        const float lift = Card(dl, pos, ImVec2(pos.x + width, pos.y + height), alpha, g_ratesHover);
        ImVec2 p(pos.x + Px(kPad), pos.y + Px(kPad) - lift);
        Widgets::Text(dl, Theme::semibold, p, Fade(Theme::kInk, alpha), "Курсы валют");
        if (!data.base.empty()) {
            const float w = Widgets::TextSize(Theme::semibold, data.base.c_str()).x;
            Widgets::Text(dl, Theme::semibold, ImVec2(p.x + inner - w, p.y), Fade(Theme::kInkMuted, alpha), data.base.c_str());
        }
        p.y += Px(26);
        if (!data.ready) {
            Widgets::Text(dl, Theme::semibold, p, Fade(Theme::kInkMuted, alpha), "загружаю…");
            return height;
        }
        for (const auto& item : data.items) {
            Widgets::Text(dl, Theme::semibold, p, Fade(Theme::kInkSoft, alpha), item.code.c_str());
            const std::string value = Money(item.value) + UnitName(data.base);
            const float vw = Widgets::TextSize(Theme::semibold, value.c_str()).x;
            Widgets::Text(dl, Theme::semibold, ImVec2(p.x + inner - vw, p.y), Fade(Theme::kInk, alpha), value.c_str());
            if (item.change != 0) {
                char change[32];
                snprintf(change, sizeof(change), "%+.2f", item.change);
                Widgets::Text(dl, Theme::semibold, ImVec2(p.x + Px(48), p.y), Fade(item.change > 0 ? IM_COL32(110, 231, 183, 255) : IM_COL32(251, 113, 133, 255), alpha), change);
            }
            p.y += Px(27);
        }
        return height;
    }
}

float HomeWidgets::Width() { return Px(kWidth); }

namespace { ImVec2 g_boundsMin, g_boundsMax; }
void HomeWidgets::Bounds(ImVec2& min, ImVec2& max) { min = g_boundsMin, max = g_boundsMax; }

namespace {
    bool On(const char* key, const char* fallback) { return Config::Value("widgets", key, fallback) == "1"; }

    float g_clockHover = 0, g_calendarHover = 0, g_notesHover = 0;
    constexpr float kClockH = 92, kCalendarH = 258, kNotesH = 168;

    std::tm Now() {
        const std::time_t t = std::time(nullptr);
        std::tm out{};
        localtime_s(&out, &t);
        return out;
    }

    void DrawClock(ImDrawList* dl, ImVec2 pos, float alpha) {
        static const char* kDays[] = { "воскресенье", "понедельник", "вторник", "среда", "четверг", "пятница", "суббота" };
        static const char* kMonths[] = { "января", "февраля", "марта", "апреля", "мая", "июня", "июля", "августа", "сентября", "октября", "ноября", "декабря" };
        const std::tm now = Now();
        const float lift = Card(dl, pos, ImVec2(pos.x + Px(kWidth), pos.y + Px(kClockH)), alpha, g_clockHover);
        char time[8];
        snprintf(time, sizeof(time), "%02d:%02d", now.tm_hour, now.tm_min);
        Widgets::Text(dl, Theme::hero, ImVec2(pos.x + Px(kPad), pos.y + Px(10) - lift), Fade(Theme::kInk, alpha), time);
        const std::string date = Lang::IsEnglish() ? std::string(Lang::Tr(kMonths[now.tm_mon])) + " " + std::to_string(now.tm_mday) + ", " + Lang::Tr(kDays[now.tm_wday])
                                                   : std::to_string(now.tm_mday) + " " + kMonths[now.tm_mon] + ", " + kDays[now.tm_wday];
        Widgets::Text(dl, Theme::semibold, ImVec2(pos.x + Px(kPad), pos.y + Px(kClockH) - Px(30) - lift), Fade(Theme::kInkMuted, alpha), date.c_str());
    }

    void DrawCalendar(ImDrawList* dl, ImVec2 pos, float alpha) {
        static const char* kNames[] = { "Январь", "Февраль", "Март", "Апрель", "Май", "Июнь", "Июль", "Август", "Сентябрь", "Октябрь", "Ноябрь", "Декабрь" };
        static const char* kHead[] = { "Пн", "Вт", "Ср", "Чт", "Пт", "Сб", "Вс" };
        const std::tm now = Now();
        const float lift = Card(dl, pos, ImVec2(pos.x + Px(kWidth), pos.y + Px(kCalendarH)), alpha, g_calendarHover);
        const float inner = Px(kWidth) - Px(kPad) * 2, cell = inner / 7.0f;
        ImVec2 p(pos.x + Px(kPad), pos.y + Px(kPad) - lift);
        Widgets::Text(dl, Theme::semibold, p, Fade(Theme::kInk, alpha), kNames[now.tm_mon]);
        p.y += Px(28);
        const auto centered = [&](const char* text, float column, float y, ImU32 color) {
            Widgets::Text(dl, Theme::semibold, ImVec2(p.x + cell * column + (cell - Widgets::TextSize(Theme::semibold, text).x) * 0.5f, y), color, text);
        };
        for (int i = 0; i < 7; ++i) centered(kHead[i], static_cast<float>(i), p.y, Fade(Theme::kInkMuted, alpha));
        p.y += Px(27);
        std::tm first = now;
        first.tm_mday = 1;
        std::mktime(&first);
        std::tm last = now;
        last.tm_mon += 1;
        last.tm_mday = 0;
        std::mktime(&last);
        int column = (first.tm_wday + 6) % 7;
        float y = p.y;
        for (int day = 1; day <= last.tm_mday; ++day) {
            const bool today = day == now.tm_mday;
            if (today) {
                const ImVec2 c(p.x + cell * column + cell * 0.5f, y + Theme::semibold->LegacySize * 0.55f);
                dl->AddCircleFilled(c, Px(13), Fade(Theme::Accent().WithAlpha(1.0f), alpha), 28);
            }
            centered(std::to_string(day).c_str(), static_cast<float>(column), y, Fade(today ? IM_COL32(255, 255, 255, 255) : Theme::kInkSoft, alpha));
            if (++column == 7) { column = 0; y += Px(27); }
        }
    }

    // Заметки лежат в notes.txt рядом с программой и попадают в резервную копию настроек
    std::wstring NotesPath() { return Text::ExecutableDirectory() + L"\\notes.txt"; }
    char g_notes[4096] = "";
    bool g_notesLoaded = false;

    void LoadNotes() {
        if (g_notesLoaded) return;
        g_notesLoaded = true;
        std::ifstream in(NotesPath(), std::ios::binary);
        in.read(g_notes, sizeof(g_notes) - 1);
        g_notes[in.gcount()] = 0;
    }

    void SaveNotes() {
        std::ofstream(NotesPath(), std::ios::binary | std::ios::trunc) << g_notes;
    }

    void DrawNotes(ImDrawList* dl, ImVec2 pos, float alpha) {
        LoadNotes();
        const float inner = Px(kWidth) - Px(kPad) * 2;
        const float lift = Card(dl, pos, ImVec2(pos.x + Px(kWidth), pos.y + Px(kNotesH)), alpha, g_notesHover);
        ImVec2 p(pos.x + Px(kPad), pos.y + Px(kPad) - lift);
        Widgets::Text(dl, Theme::semibold, p, Fade(Theme::kInk, alpha), "Заметки");
        ImGui::SetCursorScreenPos(ImVec2(p.x + inner - ImGui::CalcTextSize("изменить").x - Px(12), p.y - Px(2)));
        if (ImGui::SmallButton("изменить##notes")) ImGui::OpenPopup("##notesEdit");
        ImGui::SetNextWindowSize(ImVec2(Px(380), 0));
        if (ImGui::BeginPopup("##notesEdit")) {
            ImGui::InputTextMultiline("##notesText", g_notes, sizeof(g_notes), ImVec2(-1, Px(200)));
            if (ImGui::IsItemDeactivatedAfterEdit()) SaveNotes();
            if (ImGui::Button("сохранить")) { SaveNotes(); ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
        p.y += Px(28);
        std::istringstream lines(g_notes);
        std::string line;
        int shown = 0;
        while (shown < 4 && std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (Text::Trim(line).empty()) continue;
            Widgets::Text(dl, Theme::semibold, p, Fade(Theme::kInkSoft, alpha), Fit(Theme::semibold, line, inner).c_str());
            p.y += Px(26);
            ++shown;
        }
        if (shown == 0) Widgets::Text(dl, Theme::semibold, p, Fade(Theme::kInkMuted, alpha), "пока пусто");
    }

    // Оптимизация памяти
    std::atomic<bool> g_cleaning{ false };
    std::mutex g_cleanMutex;
    std::string g_cleanText;

    float DrawOptimize(ImVec2 pos) {
        const float width = Px(kWidth), height = Px(34);
        std::string label;
        { std::lock_guard lock(g_cleanMutex); label = g_cleaning ? "оптимизирую…" : g_cleanText.empty() ? "оптимизировать память" : g_cleanText; }
        ImGui::SetCursorScreenPos(pos);
        if (ImGui::Button((label + "##optimize").c_str(), ImVec2(width, height)) && !g_cleaning.exchange(true)) {
            std::thread([] {
                const auto result = ProcessMonitor::CleanMemory(false);
                std::lock_guard lock(g_cleanMutex);
                g_cleanText = "освобождено " + std::to_string(result.freed / (1024 * 1024)) + " МБ";
                g_cleaning = false;
            }).detach();
        }
        return height;
    }

    // Настройка виджетов прямо на главном экране
    void DrawChooser(ImVec2 pos) {
        ImGui::SetCursorScreenPos(pos);
        if (ImGui::SmallButton("виджеты##chooser")) ImGui::OpenPopup("##widgetChooser");
        if (!ImGui::BeginPopup("##widgetChooser")) return;
        const auto row = [](const char* label, const char* key, const char* fallback) {
            bool value = On(key, fallback);
            if (Widgets::Check(label, &value)) Config::SetValue("widgets", key, value ? "1" : "0");
        };
        row("Показывать виджеты", "show", "1");
        ImGui::Separator();
        row("Время", "clock", "1");
        bool weather = Config::Get().weatherWidget;
        if (Widgets::Check("Погода", &weather)) Config::Update([&](Settings& s) { s.weatherWidget = weather; });
        row("Заметки", "notes", "0");
        row("Календарь", "calendar", "0");
        bool rates = Rates::Enabled();
        if (Widgets::Check("Курсы валют", &rates)) { Config::SetValue("rates", "widget", rates ? "1" : "0"); Rates::Reload(); }
        row("Мониторинг системы", "system", "1");
        row("Медиаплеер", "media", "1");
        ImGui::EndPopup();
    }
}

bool HomeWidgets::Visible() { return On("show", "1"); }

void HomeWidgets::Draw(ImDrawList* dl, float right, float top, float bottom, float dt, float appear, float minX) {
    const float ease = 1.0f - std::pow(1.0f - std::clamp(appear, 0.0f, 1.0f), 3.0f);
    const float gap = Px(14), width = Px(kWidth);
    const float x = right - width + (1.0f - ease) * Px(30); // выезжает справа
    const float chooserH = ImGui::GetFrameHeight() + Px(6);
    DrawChooser(ImVec2(x + width - ImGui::CalcTextSize("виджеты").x - Px(14), top));
    float y = top + chooserH;
    if (!Visible()) { g_boundsMin = ImVec2(x, top), g_boundsMax = ImVec2(x + width, top + chooserH); return; }

    // Каждая карточка — отдельный блок: в «Редактировании» её можно утащить куда угодно, не трогая соседей
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    ImVec2 lo(x, top), hi(x + width, top + chooserH);
    const auto card = [&](const char* id, const char* label, ImVec2 p, auto draw) {
        const ImVec2 s = HomeLayout::Shift(id, screen);
        const ImVec2 at(p.x + s.x, p.y + s.y);
        const float h = draw(at);
        if (h <= 0) return 0.0f;
        HomeLayout::Report(id, label, at, ImVec2(at.x + width, at.y + h), HomeLayout::kFixed);
        lo = ImVec2(std::min(lo.x, at.x), std::min(lo.y, at.y)), hi = ImVec2(std::max(hi.x, at.x + width), std::max(hi.y, at.y + h));
        return h;
    };
    (void)&DrawClock; // время теперь не карточкой, а текстом под верхним меню (Ui.cpp, «w_clock»)
    if (On("system", "1"))
        y += card("w_system", "система", ImVec2(x, y), [&](ImVec2 p) { const float h = DrawSystem(dl, p, dt, ease) + Px(6); return h + DrawOptimize(ImVec2(p.x, p.y + h)); }) + gap;
    if (On("media", "1") && (y + Px(126) < bottom || HomeLayout::Moved("w_media"))) {
        const float media = card("w_media", "сейчас играет", ImVec2(x, y), [&](ImVec2 p) { return DrawMedia(dl, p, dt, ease); });
        if (media > 0) y += media + gap;
    }
    const float column2 = x - width - gap;
    float y2 = top + chooserH;
    // карточка встаёт в первый столбец, если влезает, иначе во второй
    const auto place = [&](const char* id, const char* label, float height, auto draw) {
        const auto drawn = [&](ImVec2 p) { draw(p); return height; };
        if (y + height < bottom) { card(id, label, ImVec2(x, y), drawn); y += height + gap; }
        else if ((column2 >= minX && y2 + height < bottom) || HomeLayout::Moved(id)) { card(id, label, ImVec2(column2, y2), drawn); y2 += height + gap; }
    };
    if (Rates::Enabled()) place("w_rates", "курсы валют", RatesHeight(), [&](ImVec2 p) { DrawRates(dl, p, ease); });
    if (On("calendar", "0")) place("w_calendar", "календарь", Px(kCalendarH), [&](ImVec2 p) { DrawCalendar(dl, p, ease); });
    if (On("notes", "0")) place("w_notes", "заметки", Px(kNotesH), [&](ImVec2 p) { DrawNotes(dl, p, ease); });
    g_boundsMin = lo, g_boundsMax = hi;
}

bool HomeWidgets::AnyMoved() {
    for (const char* id : { "w_clock", "w_system", "w_media", "w_rates", "w_calendar", "w_notes" })
        if (HomeLayout::Moved(id)) return true;
    return false;
}
