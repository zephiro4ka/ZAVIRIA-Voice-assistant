#include "ScenariosPage.h"

#include "Input.h"
#include "Lang.h"
#include "Launcher.h"
#include "Settings.h"
#include "Scenarios.h"
#include "TextUtils.h"
#include "Theme.h"
#include "Widgets.h"
#include "Visuals.h"
#include "../ImGui/imgui_internal.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

using Theme::Px;
using Widgets::Icon;
using Scenarios::Block;
using Scenarios::Scenario;
using Scenarios::Step;

namespace {
    const char* L(const char* ru, const char* en) { return Lang::IsEnglish() ? en : ru; }

    bool g_open = false;
    float g_anim = 0.0f;
    std::vector<Scenario> g_list;
    int g_selected = 0;
    bool g_dirty = false;
    std::mutex g_addMutex;
    std::vector<Scenario> g_added; // собрала Аня — добавим в потоке интерфейса

    // отмена и повтор: снимок всех сценариев после каждой законченной правки
    std::vector<std::vector<Scenario>> g_history;
    int g_historyAt = -1;
    int g_restoreTo = -1; // откат — в конце кадра, пока на старый список никто не смотрит

    std::string g_search;

    ImU32 PageColor(float alpha) { return Theme::Background(1).Mix(Theme::Background(0), 0.5f).WithAlpha(alpha); }

    // Цвет блока по роду — чтобы схема читалась с одного взгляда
    Theme::Color StepColor(Step step) {
        if (Scenarios::IsTrigger(step)) return { 134, 215, 150 };
        switch (step) {
        case Step::Launch: case Step::Screenshot: return { 125, 175, 245 };
        case Step::OpenLink: case Step::Play: return { 110, 210, 180 };
        case Step::Place: return { 250, 165, 125 };
        case Step::Volume: case Step::Media: case Step::Mute: case Step::Notify: return { 245, 205, 95 };
        case Step::Wait: case Step::Condition: case Step::Loop: case Step::Switch: return { 250, 180, 110 };
        case Step::Command: return { 167, 139, 250 };
        case Step::SetVar: case Step::Nothing: return { 192, 196, 206 };
        default: return { 244, 114, 182 };
        }
    }

    // ================================================================ значки
    enum class G { Chat, Keyboard, Bell, Clock, Branch, Loop, Switch, Mic, Link, Apps, Window, Braces, XCircle, Gear, Speaker, Next, Mute, Minus,
                   Spark, Camera, PlayCircle, Send, Undo, Redo, Layout, Lines, List, Plus, Fit, Search, Play, Close, Pencil };

    void Glyph(ImDrawList* dl, G g, ImVec2 c, float s, ImU32 col) {
        const float w = std::max(1.1f, s * 0.1f);
        auto P = [&](float x, float y) { return ImVec2(c.x + s * x, c.y + s * y); };
        auto line = [&](float x1, float y1, float x2, float y2) { dl->AddLine(P(x1, y1), P(x2, y2), col, w); };
        auto rect = [&](float x1, float y1, float x2, float y2, float r) { dl->AddRect(P(x1, y1), P(x2, y2), col, s * r, 0, w); };
        auto circle = [&](float x, float y, float r) { dl->AddCircle(P(x, y), s * r, col, 24, w); };
        auto speaker = [&] {
            for (ImVec2 p : { P(-0.42f, -0.13f), P(-0.2f, -0.13f), P(0.04f, -0.36f), P(0.04f, 0.36f), P(-0.2f, 0.13f), P(-0.42f, 0.13f) }) dl->PathLineTo(p);
            dl->PathStroke(col, ImDrawFlags_Closed, w);
        };
        switch (g) {
        case G::Chat: Widgets::DrawIcon(dl, Icon::Chat, c, s, col); break;
        case G::Mic: Widgets::DrawIcon(dl, Icon::Mic, c, s, col); break;
        case G::Apps: case G::Layout: Widgets::DrawIcon(dl, Icon::Grid, c, s, col); break;
        case G::Window: Widgets::DrawIcon(dl, Icon::Layers, c, s, col); break;
        case G::Gear: Widgets::DrawIcon(dl, Icon::Gear, c, s, col); break;
        case G::Next: Widgets::DrawIcon(dl, Icon::Next, c, s, col); break;
        case G::Spark: Widgets::DrawIcon(dl, Icon::Spark, c, s, col); break;
        case G::Send: Widgets::DrawIcon(dl, Icon::Send, c, s, col); break;
        case G::Lines: Widgets::DrawIcon(dl, Icon::Flow, c, s, col); break;
        case G::Plus: Widgets::DrawIcon(dl, Icon::Plus, c, s, col); break;
        case G::Search: Widgets::DrawIcon(dl, Icon::Search, c, s, col); break;
        case G::Play: Widgets::DrawIcon(dl, Icon::Play, c, s, col); break;
        case G::Pencil: // карандаш наискосок
            for (ImVec2 p : { P(0.22f, -0.42f), P(0.42f, -0.22f), P(-0.18f, 0.38f), P(-0.42f, 0.42f), P(-0.38f, 0.18f) }) dl->PathLineTo(p);
            dl->PathStroke(col, ImDrawFlags_Closed, w);
            line(0.08f, -0.28f, 0.28f, -0.08f);
            break;
        case G::Close: Widgets::DrawIcon(dl, Icon::Close, c, s, col); break;
        case G::Keyboard:
            rect(-0.5f, -0.32f, 0.5f, 0.32f, 0.08f);
            for (int i = 0; i < 4; ++i) dl->AddCircleFilled(P(-0.3f + 0.2f * i, -0.08f), w * 0.8f, col);
            line(-0.22f, 0.14f, 0.22f, 0.14f);
            break;
        case G::Bell:
            dl->PathArcTo(P(0, -0.08f), s * 0.3f, IM_PI, IM_PI * 2.0f, 12);
            for (ImVec2 p : { P(0.3f, 0.18f), P(0.42f, 0.3f), P(-0.42f, 0.3f), P(-0.3f, 0.18f) }) dl->PathLineTo(p);
            dl->PathStroke(col, ImDrawFlags_Closed, w);
            line(-0.09f, 0.43f, 0.09f, 0.43f);
            break;
        case G::Clock:
            circle(0, 0, 0.42f);
            line(0, 0, 0, -0.25f);
            line(0, 0, 0.18f, 0.1f);
            break;
        case G::Branch:
            line(-0.45f, 0, -0.1f, 0);
            line(-0.1f, 0, 0.35f, -0.3f);
            line(-0.1f, 0, 0.35f, 0.3f);
            line(0.35f, -0.3f, 0.16f, -0.31f); line(0.35f, -0.3f, 0.29f, -0.12f);
            line(0.35f, 0.3f, 0.16f, 0.31f); line(0.35f, 0.3f, 0.29f, 0.12f);
            break;
        case G::Loop: {
            dl->PathArcTo(c, s * 0.36f, -IM_PI * 0.3f, IM_PI * 1.45f, 20);
            dl->PathStroke(col, 0, w);
            const ImVec2 tip(std::cos(-IM_PI * 0.3f) * 0.36f, std::sin(-IM_PI * 0.3f) * 0.36f);
            line(tip.x, tip.y, tip.x - 0.2f, tip.y - 0.04f);
            line(tip.x, tip.y, tip.x + 0.03f, tip.y - 0.2f);
            break;
        }
        case G::Switch:
            for (int i = 0; i < 3; ++i) {
                const float y = -0.28f + 0.28f * i;
                dl->AddCircleFilled(P(-0.36f, y), w * 1.1f, col);
                line(-0.18f, y, 0.44f, y);
            }
            break;
        case G::Link: rect(-0.5f, -0.04f, 0.06f, 0.3f, 0.17f); rect(-0.06f, -0.3f, 0.5f, 0.04f, 0.17f); break;
        case G::Braces: {
            const float size = s * 0.85f;
            const ImVec2 t = Theme::semibold->CalcTextSizeA(size, FLT_MAX, 0, "{x}");
            dl->AddText(Theme::semibold, size, ImVec2(c.x - t.x * 0.5f, c.y - t.y * 0.5f), col, "{x}");
            break;
        }
        case G::XCircle: circle(0, 0, 0.42f); line(-0.17f, -0.17f, 0.17f, 0.17f); line(0.17f, -0.17f, -0.17f, 0.17f); break;
        case G::Speaker:
            speaker();
            dl->PathArcTo(P(0.04f, 0), s * 0.2f, -0.9f, 0.9f, 8); dl->PathStroke(col, 0, w);
            dl->PathArcTo(P(0.04f, 0), s * 0.36f, -0.9f, 0.9f, 10); dl->PathStroke(col, 0, w);
            break;
        case G::Mute: speaker(); line(0.2f, -0.14f, 0.44f, 0.14f); line(0.44f, -0.14f, 0.2f, 0.14f); break;
        case G::Minus: line(-0.34f, 0, 0.34f, 0); break;
        case G::Camera:
            rect(-0.46f, -0.24f, 0.46f, 0.36f, 0.1f);
            circle(0, 0.06f, 0.16f);
            line(-0.16f, -0.24f, -0.1f, -0.36f); line(-0.1f, -0.36f, 0.1f, -0.36f); line(0.1f, -0.36f, 0.16f, -0.24f);
            break;
        case G::PlayCircle: circle(0, 0, 0.42f); dl->AddTriangleFilled(P(-0.1f, -0.18f), P(-0.1f, 0.18f), P(0.2f, 0), col); break;
        case G::Undo: case G::Redo: {
            const float k = g == G::Undo ? 1.0f : -1.0f;
            dl->PathLineTo(P(-0.35f * k, -0.22f));
            dl->PathLineTo(P(0.05f * k, -0.22f));
            for (int i = 1; i <= 10; ++i) {
                const float a = -IM_PI * 0.5f + IM_PI * i / 10.0f;
                dl->PathLineTo(P((0.05f + std::cos(a) * 0.3f) * k, 0.08f + std::sin(a) * 0.3f));
            }
            dl->PathLineTo(P(-0.15f * k, 0.38f));
            dl->PathStroke(col, 0, w);
            line(-0.35f * k, -0.22f, -0.2f * k, -0.36f);
            line(-0.35f * k, -0.22f, -0.2f * k, -0.08f);
            break;
        }
        case G::List: for (float y : { -0.25f, 0.0f, 0.25f }) line(-0.38f, y, 0.38f, y); break;
        case G::Fit:
            for (float x : { -1.0f, 1.0f })
                for (float y : { -1.0f, 1.0f }) { line(0.4f * x, 0.4f * y, 0.15f * x, 0.4f * y); line(0.4f * x, 0.4f * y, 0.4f * x, 0.15f * y); }
            break;
        }
    }

    G StepGlyph(Step step) {
        switch (step) {
        case Step::Phrase: return G::Chat;
        case Step::Hotkey: return G::Keyboard;
        case Step::Notify: return G::Bell;
        case Step::Wait: return G::Clock;
        case Step::Condition: return G::Branch;
        case Step::Loop: return G::Loop;
        case Step::Switch: return G::Switch;
        case Step::Say: return G::Mic;
        case Step::OpenLink: return G::Link;
        case Step::Launch: return G::Apps;
        case Step::Place: return G::Window;
        case Step::SetVar: return G::Braces;
        case Step::Volume: return G::Speaker;
        case Step::Media: return G::Next;
        case Step::Mute: return G::Mute;
        case Step::Nothing: return G::Minus;
        case Step::ChatReply: return G::Chat;
        case Step::Screenshot: return G::Camera;
        case Step::Play: return G::PlayCircle;
        case Step::Command: return G::Send;
        default: return G::Spark;
        }
    }

    // Текст, обрезанный многоточием по ширине (сначала перевод — режем уже то, что увидят)
    std::string Fit(ImFont* font, float size, const std::string& text, float maxWidth) {
        std::string s = Lang::Tr(text.c_str());
        for (char& ch : s) if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
        auto width = [&](const std::string& t) { return font->CalcTextSizeA(size, FLT_MAX, 0, t.c_str()).x; };
        if (width(s) <= maxWidth) return s;
        const float dots = width("…");
        while (!s.empty()) {
            size_t cut = s.size() - 1;
            while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut; // не разрываем букву пополам
            s.erase(cut);
            while (!s.empty() && s.back() == ' ') s.pop_back();
            if (width(s) + dots <= maxWidth) break;
        }
        return s + "…";
    }

    // ================================================================ поля блока
    int ResizeString(ImGuiInputTextCallbackData* data) {
        if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
            auto* s = static_cast<std::string*>(data->UserData);
            s->resize(data->BufTextLen);
            data->Buf = s->data();
        }
        return 0;
    }

    bool InputString(const char* id, std::string& value, const char* hint, float width) {
        ImGui::SetNextItemWidth(width);
        const bool changed = ImGui::InputTextWithHint(id, Lang::Tr(hint), value.data(), value.capacity() + 1, ImGuiInputTextFlags_CallbackResize, ResizeString, &value);
        if (ImGui::IsItemDeactivatedAfterEdit()) g_dirty = true;
        return changed;
    }

    void InputLines(const char* id, std::string& value, float width, float height) {
        ImGui::InputTextMultiline(id, value.data(), value.capacity() + 1, ImVec2(width, height), ImGuiInputTextFlags_CallbackResize, ResizeString, &value);
        if (ImGui::IsItemDeactivatedAfterEdit()) g_dirty = true;
    }

    void Label(const char* text) {
        ImGui::Dummy(ImVec2(0, Px(2)));
        ImGui::PushFont(Theme::semibold, Theme::semibold->LegacySize);
        ImGui::TextColored(ImColor(Theme::kInkMuted), "%s", Lang::Tr(text));
        ImGui::PopFont();
    }

    void Note(const char* text) {
        ImGui::PushFont(Theme::semibold, Theme::semibold->LegacySize);
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::kInkMuted);
        ImGui::TextWrapped("%s", Lang::Tr(text));
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }

    bool ComboField(const char* id, int* value, const char* const items[], int count, float width) {
        const bool changed = Widgets::Combo(id, value, items, count, width);
        if (changed) g_dirty = true;
        return changed;
    }

    void MonitorAndPosition(Block& b, float width) {
        static const char* kPositions[] = { "развернуть", "левая половина", "правая половина", "по центру", "во весь экран (F11)" };
        const int monitors = std::max(1, Scenarios::MonitorCount());
        const float half = (width - ImGui::GetStyle().ItemSpacing.x) * 0.42f;
        char preview[48];
        snprintf(preview, sizeof(preview), "%s %d", L("монитор", "screen"), b.monitor);
        if (Widgets::BeginCombo("##monitor", preview, half)) {
            for (int m = 1; m <= std::max(monitors, b.monitor); ++m) {
                snprintf(preview, sizeof(preview), m == 1 ? L("монитор %d (основной)", "screen %d (main)") : L("монитор %d", "screen %d"), m);
                if (Widgets::ComboItem(preview, b.monitor == m)) { b.monitor = m; g_dirty = true; }
            }
            Widgets::EndCombo();
        }
        ImGui::SameLine();
        ComboField("##position", &b.position, kPositions, IM_ARRAYSIZE(kPositions), width - half - ImGui::GetStyle().ItemSpacing.x);
    }

    // Поле программы + список из «Пуска», отфильтрованный по набранному
    void AppField(std::string& app, const char* hint, float width) {
        const float pick = Px(96);
        InputString("##app", app, hint, width - pick - ImGui::GetStyle().ItemSpacing.x);
        ImGui::SameLine();
        if (Widgets::BeginCombo("##pick", L("из Пуска", "from Start"), pick, ImGuiComboFlags_HeightLarge)) {
            const std::wstring filter = Text::LowerRu(Text::ToWide(app));
            int shown = 0;
            for (const auto& name : Launcher::AppNames()) {
                if (!filter.empty() && Text::LowerRu(Text::ToWide(name)).find(filter) == std::wstring::npos) continue;
                if (Widgets::ComboItem(name.c_str(), name == app)) { app = name; g_dirty = true; }
                if (++shown >= 60) break;
            }
            if (shown == 0) ImGui::TextDisabled("%s", L("ничего не нашла", "nothing found"));
            Widgets::EndCombo();
        }
    }

    const char* const kConditionOps[] = { "равно", "не равно", "содержит", "не содержит", "больше", "меньше", "пусто", "не пусто" };
    const char* const kMedia[] = { "пауза / продолжить", "следующий трек", "предыдущий трек" };
    const char* const kMute[] = { "выключить звук", "включить звук", "переключить" };

    void ChordCheck(const std::string& text) {
        if (Text::Trim(text).empty()) return;
        std::string spaced;
        for (char ch : text) spaced += ch == ',' || ch == ';' ? ' ' : ch;
        bool ok = true;
        size_t start = 0;
        while (start < spaced.size() && ok) { // каждое сочетание по отдельности, как их нажмёт сценарий
            size_t end = spaced.find(' ', start);
            if (end == std::string::npos) end = spaced.size();
            std::string part = spaced.substr(start, end - start);
            if (!part.empty() && part != "+" && part.back() != '+' && part.front() != '+') {
                std::vector<WORD> keys;
                ok = Input::ParseChord(part, keys);
            }
            start = end + 1;
        }
        ImGui::PushFont(Theme::semibold, Theme::semibold->LegacySize);
        ImGui::TextColored(ImColor(ok ? Theme::kOk : Theme::kBad), "%s", ok ? L("✓ понятно", "✓ looks right") : L("не поняла какую-то клавишу", "some key isn't recognised"));
        ImGui::PopFont();
    }

    void BlockFields(Block& b, float width) {
        switch (b.step) {
        case Step::Phrase:
            Label("фразы после обращения или в чате, через запятую");
            InputString("##phrase", b.text, "уютный вечер, кино", width);
            break;
        case Step::Hotkey:
            Label("сочетание клавиш");
            InputString("##hotkey", b.text, "Ctrl+Alt+N", width);
            ChordCheck(b.text);
            Note("работает везде, даже когда окно ZAVIRIA свёрнуто");
            break;
        case Step::Condition:
            Label("что проверить");
            InputString("##left", b.text, "{ответ}", width);
            ComboField("##op", &b.position, kConditionOps, IM_ARRAYSIZE(kConditionOps), width);
            if (b.position < 6) InputString("##right", b.extra, "с чем сравнить", width);
            Note("выход «да» — верхний справа, «нет» — нижний");
            break;
        case Step::Loop:
            Label("сколько раз");
            Widgets::Slider("##times", &b.number, 1, 100, L("%d раз", "%d times"), width);
            if (ImGui::IsItemDeactivatedAfterEdit()) g_dirty = true;
            Note("верхний выход — что повторять, нижний — что потом; номер круга — {шаг}");
            break;
        case Step::Switch:
            Label("что сравнить");
            InputString("##value", b.text, "{ответ}", width);
            Label("варианты через |");
            InputString("##cases", b.extra, "утро | день | вечер", width);
            Note("у каждого варианта свой выход, последний — «иначе»");
            break;
        case Step::Wait:
            Label("сколько ждать");
            Widgets::Slider("##wait", &b.number, 1, 300, L("%d с", "%d s"), width);
            if (ImGui::IsItemDeactivatedAfterEdit()) g_dirty = true;
            break;
        case Step::Say: Label("что сказать"); InputString("##say", b.text, "Уютного вечера!", width); break;
        case Step::OpenLink:
        case Step::Launch: {
            Label("что открыть");
            const bool link = b.step == Step::OpenLink;
            if (Widgets::Radio(L("программу", "an app"), !link) && link) { b.step = Step::Launch; g_dirty = true; }
            ImGui::SameLine(0, Px(8));
            if (Widgets::Radio(L("ссылку", "a link"), link) && !link) { b.step = Step::OpenLink; if (b.monitor < 1) b.monitor = 1; g_dirty = true; }
            if (b.step == Step::OpenLink) {
                Label("адрес");
                InputString("##url", b.text, "например, kinopoisk.ru", width);
                Label("где открыть");
                MonitorAndPosition(b, width);
            } else {
                Label("программа");
                AppField(b.text, "например, NTE или Steam", width);
            }
            break;
        }
        case Step::Place:
            Label("программа");
            AppField(b.text, "пусто — окно из прошлого блока", width);
            Label("куда поставить");
            MonitorAndPosition(b, width);
            break;
        case Step::SetVar:
            Label("имя");
            InputString("##var", b.text, "погода", width);
            Label("значение");
            InputString("##val", b.extra, "{ответ}", width);
            break;
        case Step::Notify:
            Label("заголовок");
            InputString("##title2", b.extra, "ZAVIRIA", width);
            Label("текст");
            InputString("##notify", b.text, "Пора отдохнуть", width);
            break;
        case Step::Volume:
            Label("громкость");
            Widgets::Slider("##volume", &b.number, 0, 100, "%d%%", width);
            if (ImGui::IsItemDeactivatedAfterEdit()) g_dirty = true;
            break;
        case Step::Media: ComboField("##media", &b.number, kMedia, IM_ARRAYSIZE(kMedia), width); break;
        case Step::Mute: ComboField("##mute", &b.position, kMute, IM_ARRAYSIZE(kMute), width); break;
        case Step::Nothing: Note("просто передаёт ход дальше — удобно, чтобы собрать стрелки в одну"); break;
        case Step::ChatReply: Label("сообщение"); InputString("##chat", b.text, "{ответ}", width); break;
        case Step::Screenshot: Note("снимок ляжет в «Изображения\\ZAVIRIA», путь — в {ответ}"); break;
        case Step::Play: Label("файл или адрес"); InputString("##play", b.text, "C:\\Музыка\\трек.mp3", width); break;
        case Step::Command: Label("что сказать Ане"); InputString("##command", b.text, "какая погода", width); break;
        default: break;
        }
    }

    // Строка под заголовком карточки: что блок сделает
    std::string Describe(const Block& b) {
        const std::string text = Text::Trim(b.text), extra = Text::Trim(b.extra);
        auto item = [](const char* const items[], int count, int i) { return std::string(Lang::Tr(items[std::clamp(i, 0, count - 1)])); };
        switch (b.step) {
        case Step::Phrase: return text.empty() ? "" : "«" + Config::Get().assistant + ", " + text + "»";
        case Step::Condition:
            return text.empty() ? "" : text + " " + item(kConditionOps, IM_ARRAYSIZE(kConditionOps), b.position) + (b.position < 6 ? " " + extra : "");
        case Step::Loop: return std::to_string(std::max(1, b.number)) + L(" раз", " times");
        case Step::Switch: return text.empty() ? "" : text + (extra.empty() ? "" : " → " + extra);
        case Step::Wait: return std::to_string(b.number) + L(" с", " s");
        case Step::Volume: return std::to_string(b.number) + "%";
        case Step::Media: return item(kMedia, IM_ARRAYSIZE(kMedia), b.number);
        case Step::Mute: return item(kMute, IM_ARRAYSIZE(kMute), b.position);
        case Step::SetVar: return text.empty() ? "" : "{" + text + "} = " + extra;
        case Step::Place: return (text.empty() ? std::string(L("окно из прошлого блока", "window from the previous block")) : text) + " · " + L("монитор ", "screen ") + std::to_string(b.monitor);
        case Step::Notify: return extra.empty() || extra == "ZAVIRIA" ? text : extra + ": " + text;
        case Step::Nothing: return L("передаёт ход дальше", "passes control on");
        case Step::Screenshot: return L("в «Изображения\\ZAVIRIA»", "to Pictures\\ZAVIRIA");
        default: return text;
        }
    }

    // ================================================================ холст
    // Карточки лежат где угодно (координаты холста). Вход — кружок слева у заголовка, выходы — справа.
    float g_titleW = 0.0f; // ширина заголовка страницы — полоса инструментов стоит правее
    bool g_hotMirror = false; // под мышью — зеркальная ручка выхода (другая сторона блока)
    // Вид холста — общий для всех сценариев ([scenarios] look): блоки, порты и линии меняются вместе.
    // 0 — плавные кривые Безье (по умолчанию), 1 — прямые по диагонали, 2 — ортогональные изломы, 3 — кибер-импульсы
    enum LineStyle { kBezier, kMinimal, kOrtho, kCyber, kQuiet, kLineStyles }; // kQuiet — «Минимализм вниз» по её образцу (по умолчанию)
    int s_linkStyle = kQuiet;
    int s_flow = 0; // [scenarios] flow: 0 — лентой вниз (вход сверху, выходы снизу), 1 — рядами-змейкой слева направо
    bool Vertical() { return s_flow == 0; }
    bool s_lineLoaded = false;
    bool Routed(int style) { return style == kOrtho; } // по «коридорам» между блоками (H/V-маршрут с обходом)
    void LoadLineLook() {
        if (s_lineLoaded) return;
        s_lineLoaded = true;
        s_flow = std::clamp(std::atoi(Config::Value("scenarios", "flow", "0").c_str()), 0, 1);
        if (Config::Value("scenarios", "look_v", "") != "2") { // новый вид по умолчанию — один раз для всех
            s_linkStyle = kQuiet;
            Config::SetValue("scenarios", "look", std::to_string(kQuiet));
            Config::SetValue("scenarios", "look_v", "2");
            return;
        }
        const std::string look = Config::Value("scenarios", "look", "");
        if (!look.empty()) { s_linkStyle = std::clamp(std::atoi(look.c_str()), 0, kLineStyles - 1); return; }
        // прошлые 9 стилей ([scenarios] lines) — к самому похожему из новых
        static const int kOld[] = { kOrtho, kBezier, kBezier, kMinimal, kOrtho, kMinimal, kBezier, kCyber, kBezier };
        const int old = std::atoi(Config::Value("scenarios", "lines", "1").c_str());
        s_linkStyle = old >= 0 && old < 9 ? kOld[old] : kBezier;
    }
    constexpr float kW = 300.0f, kH = 92.0f, kPortGap = 22.0f, kGrid = 20.0f;
    constexpr float kGapX = 60.0f, kGapY = 40.0f, kRowGap = 60.0f; // плотная раскладка: блоки рядом, линиям хватает места

    constexpr float kGapV = 64.0f; // лентой вниз: между блоками — меньше высоты блока
    float NodeH(const Block& b) {
        if (Vertical()) return kH + (Scenarios::PortCount(b) > 1 ? 18.0f : 0.0f); // подписи выходов — над точками снизу
        return kH + std::max(0, Scenarios::PortCount(b) - 2) * kPortGap;
    }

    // Змейка: ряд слева направо, следующий — справа налево. Блок «смотрит» туда, откуда пришла стрелка
    // и куда она уходит: вход и выходы могут быть и справа, и слева. Тогда перенос на новый ряд —
    // короткий плавный разворот у края, а не линия через весь холст. Бит 1 — вход справа, бит 2 — выходы слева.
    std::unordered_map<int, unsigned char> g_side;
    bool InRight(const Block& b) { const auto it = g_side.find(b.id); return it != g_side.end() && (it->second & 1); }
    bool OutLeft(const Block& b) { const auto it = g_side.find(b.id); return it != g_side.end() && (it->second & 2); }
    void UpdateSides(const Scenario& s) {
        g_side.clear();
        for (const auto& b : s.blocks) if (b.sides >= 0) g_side[b.id] = static_cast<unsigned char>(b.sides);
        std::unordered_map<int, const Block*> byId, prev, next;
        for (const auto& b : s.blocks) byId[b.id] = &b;
        for (const auto& b : s.blocks)
            for (int p = 0; p < Scenarios::PortCount(b); ++p)
                for (int to : Scenarios::Port(b, p)) {
                    if (!byId.count(to)) continue;
                    if (!next.count(b.id)) next[b.id] = byId[to];
                    if (!prev.count(to)) prev[to] = &b;
                }
        for (int pass = 0; pass < 3; ++pass)
            for (const auto& b : s.blocks) {
                const float cx = b.x + kW * 0.5f, reach = kW * 0.5f;
                const Block* from = prev.count(b.id) ? prev[b.id] : nullptr;
                const Block* to = next.count(b.id) ? next[b.id] : nullptr;
                bool inRight = false, outLeft = false;
                if (from) {
                    const float fx = from->x + kW * 0.5f;
                    inRight = fx > cx + reach || (std::fabs(fx - cx) <= reach && !OutLeft(*from)); // сверху — разворот с той же стороны
                }
                if (to) {
                    const float tx = to->x + kW * 0.5f;
                    outLeft = tx < cx - reach || (std::fabs(tx - cx) <= reach && inRight);
                } else outLeft = inRight; // последний в ряду справа налево — выход смотрит дальше влево
                if (inRight && !outLeft && from && to) inRight = false; // вход и выход не на одной стороне
                if (outLeft && !inRight && from && to) outLeft = false;
                g_side[b.id] = static_cast<unsigned char>(b.sides >= 0 ? b.sides : (inRight ? 1 : 0) | (outLeft ? 2 : 0)); // сторону выбрали руками — так и оставляем
            }
    }
    ImVec2 InPortC(const Block& b) {
        if (Vertical()) return ImVec2(b.x + kW * 0.5f, b.y); // вход — точка сверху по центру
        return ImVec2(InRight(b) ? b.x + kW : b.x, b.y + 46.0f);
    }
    ImVec2 OutPortC(const Block& b, int port) {
        if (Vertical()) { const int n = Scenarios::PortCount(b); return ImVec2(b.x + kW * (port + 1.0f) / (n + 1.0f), b.y + NodeH(b)); } // выходы — снизу
        return ImVec2(OutLeft(b) ? b.x : b.x + kW, b.y + 46.0f + (Scenarios::PortCount(b) == 1 ? 0 : port) * kPortGap);
    }

    std::string PortLabel(const Block& b, int port) {
        switch (b.step) {
        case Step::Condition: return port == 0 ? L("да", "yes") : L("нет", "no");
        case Step::Loop: return port == 0 ? L("повторять", "repeat") : L("потом", "then");
        case Step::Switch: {
            const auto cases = Scenarios::Cases(b.extra);
            return port < static_cast<int>(cases.size()) ? cases[port] : L("иначе", "else");
        }
        default: return {};
        }
    }

    struct Canvas {
        ImVec2 pan{ 60, 40 };
        float zoom = 1.0f, zoomTarget = 1.0f;   // колесо меняет цель — масштаб плавно доезжает
        ImVec2 anchorScreen, anchorCanvas;      // точка, вокруг которой масштаб
        int selected = -1;
        int linkFrom = -1, linkPort = 0;         // тянем стрелку от этого выхода
        int dragNode = -1;
        ImVec2 dragOffset;
        int newStep = -1;                        // тянем новый блок из панели
        int lastScenario = -1;
        bool fitPending = true;
        std::unordered_map<int, float> appear;   // появление новых блоков
        std::string routeKey;                    // ровная разводка считается заново, только когда схема поменялась
        std::map<std::tuple<int, int, int>, std::vector<ImVec2>> routes;
    };
    Canvas g_canvas;

    float Z() { return g_canvas.zoom * Px(1.0f); }
    ImVec2 ToScreen(ImVec2 origin, ImVec2 p) { return ImVec2(origin.x + g_canvas.pan.x + p.x * Z(), origin.y + g_canvas.pan.y + p.y * Z()); }
    ImVec2 ToCanvas(ImVec2 origin, ImVec2 p) { return ImVec2((p.x - origin.x - g_canvas.pan.x) / Z(), (p.y - origin.y - g_canvas.pan.y) / Z()); }

    Block* FindBlock(Scenario& s, int id) {
        for (auto& b : s.blocks) if (b.id == id) return &b;
        return nullptr;
    }

    Theme::Color ColorOf(const Block& b) { return b.color >= 0 ? Theme::FromRgb(b.color) : StepColor(b.step); }

    int NextId(const Scenario& s) {
        int maxId = 0;
        for (const auto& b : s.blocks) maxId = std::max(maxId, b.id);
        return maxId + 1;
    }

    Block NewBlock(Step step) {
        Block b;
        b.step = step;
        switch (step) {
        case Step::Volume: b.number = 50; break;
        case Step::Wait: b.number = 3; break;
        case Step::Loop: b.number = 3; break;
        case Step::Condition: b.text = L("{ответ}", "{answer}"); b.position = 2; break;
        case Step::Switch: b.text = L("{ответ}", "{answer}"); b.extra = L("да | нет", "yes | no"); break;
        case Step::Notify: b.extra = "ZAVIRIA"; break;
        case Step::OpenLink: b.monitor = 1; break;
        default: break;
        }
        return b;
    }

    void RemoveLinksTo(Scenario& s, int id) {
        auto drop = [id](std::vector<int>& v) { v.erase(std::remove(v.begin(), v.end(), id), v.end()); };
        for (auto& b : s.blocks) {
            drop(b.next);
            for (auto& port : b.alt) drop(port);
        }
    }

    void DeleteBlock(Scenario& s, int id) {
        RemoveLinksTo(s, id);
        s.blocks.erase(std::remove_if(s.blocks.begin(), s.blocks.end(), [id](const Block& x) { return x.id == id; }), s.blocks.end());
        if (g_canvas.selected == id) g_canvas.selected = -1;
        g_dirty = true;
    }

    void Link(Scenario& s, int from, int port, int to) {
        Block* a = FindBlock(s, from);
        Block* b = FindBlock(s, to);
        if (!a || !b || from == to || Scenarios::IsTrigger(b->step)) return;
        auto& v = Scenarios::Port(*a, port);
        if (std::find(v.begin(), v.end(), to) == v.end()) v.push_back(to);
        g_dirty = true;
    }

    std::vector<ImVec2> RoundCorners(const std::vector<ImVec2>& pts, float radius);

    // Линия-«ступенька», как в схемах Windows и Figma: из порта ровно в его сторону, поворот — скруглённый угол,
    // во вход — ровно со стороны входа. outDir: 1 — выход вправо, -1 — влево; inDir: -1 — вход слева, 1 — справа.
    // Перенос на новый ряд — аккуратная скобка у края, шаг назад — понизу между рядами.
    // Мягкая кривая, как в редакторе команд Astra: из порта в его сторону, плавный изгиб без углов, во вход со стороны входа
    std::vector<ImVec2> Curve(ImVec2 a, ImVec2 b, float outDir, float inDir) {
        const float dx = b.x - a.x, dy = b.y - a.y, dist = std::hypot(dx, dy);
        const bool facing = inDir * outDir < 0.0f && outDir * dx > 0.0f;
        float k = std::clamp(dist * 0.5f, 40.0f, 180.0f);
        if (!facing) k = std::clamp(36.0f + std::fabs(dy) * 0.2f, 40.0f, 90.0f); // разворот на новый ряд — мягкая скобка, не шире блока
        const ImVec2 c1(a.x + outDir * k, a.y), c2(b.x + inDir * k, b.y);
        std::vector<ImVec2> pts;
        for (int i = 0; i <= 64; ++i) pts.push_back(ImBezierCubicCalc(a, c1, c2, b, i / 64.0f));
        return pts;
    }

    std::vector<ImVec2> Curve(ImVec2 a, ImVec2 b) {
        const float dx = b.x - a.x, dist = std::hypot(dx, b.y - a.y);
        if (dx < 0.0f) {
            // назад (перенос на новый ряд): мягкая S-кривая — выход вправо, плавный разворот вниз,
            // проход между рядами справа налево, такой же разворот и заход во вход слева
            const float dy = b.y - a.y;
            const float midY = std::fabs(dy) < 120.0f ? std::max(a.y, b.y) + 80.0f : (a.y + b.y) * 0.5f;
            auto uturn = [](ImVec2 p, ImVec2 q, float dir, std::vector<ImVec2>& out) {
                const float k = std::clamp(std::fabs(q.y - p.y) * 0.75f, 36.0f, 150.0f) * dir;
                const ImVec2 c1(p.x + k, p.y), c2(q.x + k, q.y);
                for (int i = out.empty() ? 0 : 1; i <= 24; ++i) out.push_back(ImBezierCubicCalc(p, c1, c2, q, i / 24.0f));
            };
            std::vector<ImVec2> pts;
            const ImVec2 m1(a.x, midY), m2(b.x, midY);
            uturn(a, m1, 1.0f, pts);
            for (int i = 1; i < 16; ++i) pts.push_back(ImLerp(m1, m2, i / 16.0f)); // частые точки — ровный переход цвета
            uturn(m2, b, -1.0f, pts);
            return pts;
        }
        float k = std::clamp(dist * 0.45f, 30.0f, 150.0f);
        if (dx < 40.0f) k = std::max(k, 70.0f + std::fabs(b.y - a.y) * 0.3f + std::fabs(dx) * 0.15f); // назад — мягкой петлёй
        const ImVec2 c1(a.x + k, a.y), c2(b.x - k, b.y);
        std::vector<ImVec2> pts;
        for (int i = 0; i <= 32; ++i) pts.push_back(ImBezierCubicCalc(a, c1, c2, b, i / 32.0f));
        return pts;
    }

    // Прямые углы со срезом под 45°: «Геометрический»
    std::vector<ImVec2> Chamfer(const std::vector<ImVec2>& pts, float cut) {
        if (pts.size() < 3) return pts;
        std::vector<ImVec2> out{ pts.front() };
        for (size_t i = 1; i + 1 < pts.size(); ++i) {
            const ImVec2 a = pts[i - 1], p = pts[i], b = pts[i + 1];
            const float la = std::hypot(p.x - a.x, p.y - a.y), lb = std::hypot(b.x - p.x, b.y - p.y);
            const float r = std::min({ cut, la * 0.5f, lb * 0.5f });
            if (r < 0.5f || std::fabs((p.x - a.x) * (b.y - p.y) - (p.y - a.y) * (b.x - p.x)) < 0.01f) { out.push_back(p); continue; }
            out.push_back(ImVec2(p.x + (a.x - p.x) / la * r, p.y + (a.y - p.y) / la * r));
            out.push_back(ImVec2(p.x + (b.x - p.x) / lb * r, p.y + (b.y - p.y) / lb * r));
        }
        out.push_back(pts.back());
        return out;
    }

    struct Box { float x0, y0, x1, y1; };
    struct Used { std::unordered_map<long, std::vector<std::pair<float, float>>> rows, cols; };

    // Только прямые углы: путь по «коридорам» между карточками (их края с отступом), меньше поворотов, в обход карточек
    std::vector<ImVec2> Orthogonal(ImVec2 a, ImVec2 b, const std::vector<Box>& boxes, Used& used, float outDir, float inDir) {
        constexpr float m = kGrid, bend = 80.0f;
        const ImVec2 s(a.x + outDir * m, a.y), e(b.x + inDir * m, b.y);
        const bool facing = inDir * outDir < 0.0f; // выход и вход смотрят друг на друга
        { // самый короткий путь: вправо, в промежуток между блоками, поперёк, вниз/вверх и во вход — если ни один блок не мешает
            auto blocked = [&](ImVec2 p, ImVec2 q) {
                const float lx = std::min(p.x, q.x), hx = std::max(p.x, q.x), ly = std::min(p.y, q.y), hy = std::max(p.y, q.y);
                for (const auto& r : boxes)
                    if (hx > r.x0 + 1.0f && lx < r.x1 - 1.0f && hy > r.y0 + 1.0f && ly < r.y1 - 1.0f) return true;
                return false;
            };
            auto clear = [&](const std::vector<ImVec2>& pts) {
                for (size_t k = 1; k < pts.size(); ++k) if (blocked(pts[k - 1], pts[k])) return false;
                return true;
            };
            auto remember = [&](const std::vector<ImVec2>& pts) {
                for (size_t k = 1; k < pts.size(); ++k) {
                    const ImVec2 p = pts[k - 1], q = pts[k];
                    if (std::fabs(p.y - q.y) < 0.5f) used.rows[std::lround(p.y)].push_back({ std::min(p.x, q.x), std::max(p.x, q.x) });
                    else used.cols[std::lround(p.x)].push_back({ std::min(p.y, q.y), std::max(p.y, q.y) });
                }
                return pts;
            };
            if (facing && std::fabs(a.y - b.y) < 1.0f && (b.x - a.x) * outDir > 0.0f && clear({ a, b })) return remember({ a, b });
            if (facing && (b.x - a.x) * outDir > m * 2) { // вход правее: одна ступенька посередине между блоками
                const float mid = std::round((a.x + b.x) * 0.5f);
                const std::vector<ImVec2> z{ a, ImVec2(mid, a.y), ImVec2(mid, b.y), b };
                if (clear(z)) return remember(z);
            }
            // вход левее (блок ниже или выше): через промежуток между ними
            const Box* fromBox = nullptr; const Box* toBox = nullptr;
            for (const auto& r : boxes) {
                if (a.x >= r.x0 - 1 && a.x <= r.x1 + 1 && a.y >= r.y0 && a.y <= r.y1) fromBox = &r;
                if (b.x >= r.x0 - 1 && b.x <= r.x1 + 1 && b.y >= r.y0 && b.y <= r.y1) toBox = &r;
            }
            float lo = 0, hi = 0;
            if (fromBox && toBox) {
                if (toBox->y0 >= fromBox->y1) { lo = fromBox->y1; hi = toBox->y0; }      // цель ниже
                else if (fromBox->y0 >= toBox->y1) { lo = toBox->y1; hi = fromBox->y0; } // цель выше
            }
            if (hi - lo > 8.0f) {
                const float midY = std::round((lo + hi) * 0.5f);
                const std::vector<ImVec2> z{ a, s, ImVec2(s.x, midY), ImVec2(e.x, midY), e, b };
                if (clear(z)) return remember(z);
            }
        }
        std::vector<float> xs{ s.x, e.x, std::round((s.x + e.x) * 0.5f / kGrid) * kGrid }, ys{ s.y, e.y, std::round((s.y + e.y) * 0.5f / kGrid) * kGrid };
        for (const auto& r : boxes) { xs.push_back(r.x0 - m); xs.push_back(r.x1 + m); ys.push_back(r.y0 - m); ys.push_back(r.y1 + m); }
        // поля вокруг всей схемы: чтобы к любому блоку можно было подойти слева, а длинная линия обошла всех снаружи
        float x0 = std::min(s.x, e.x), x1 = std::max(s.x, e.x), y0 = std::min(s.y, e.y), y1 = std::max(s.y, e.y);
        for (const auto& r : boxes) { x0 = std::min(x0, r.x0); x1 = std::max(x1, r.x1); y0 = std::min(y0, r.y0); y1 = std::max(y1, r.y1); }
        xs.push_back(x0 - m * 2); xs.push_back(x1 + m * 2); ys.push_back(y0 - m * 2); ys.push_back(y1 + m * 2);
        xs.push_back(e.x + inDir * m); xs.push_back(s.x + outDir * m);
        auto unique = [](std::vector<float>& v) {
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end(), [](float p, float q) { return std::fabs(p - q) < 0.5f; }), v.end());
        };
        unique(xs);
        unique(ys);
        auto inside = [&](float x, float y) {
            for (const auto& r : boxes) if (x > r.x0 - m + 0.5f && x < r.x1 + m - 0.5f && y > r.y0 - m + 0.5f && y < r.y1 + m - 0.5f) return true;
            return false;
        };
        auto index = [](const std::vector<float>& v, float x) { return static_cast<int>(std::lower_bound(v.begin(), v.end(), x - 0.5f) - v.begin()); };
        const int nx = static_cast<int>(xs.size()), ny = static_cast<int>(ys.size());
        const int si = index(xs, s.x), sj = index(ys, s.y), ei = index(xs, e.x), ej = index(ys, e.y);
        auto fallback = [&] {
            if (facing && (e.x - s.x) * outDir > 0.0f) {
                const float mid = std::round((s.x + e.x) * 0.5f / kGrid) * kGrid;
                return std::vector<ImVec2>{ a, s, ImVec2(mid, s.y), ImVec2(mid, e.y), e, b };
            }
            const float low = y1 + m * 2; // назад — понизу, под всеми блоками
            return std::vector<ImVec2>{ a, s, ImVec2(s.x, low), ImVec2(e.x, low), e, b };
        };
        if (inside(s.x, s.y) || inside(e.x, e.y)) return fallback();

        // соседи по сетке: 0 вправо, 1 влево, 2 вниз, 3 вверх
        auto penalty = [&](int i, int j, int dir, float len) {
            const bool horizontal = dir < 2;
            const float x0 = horizontal ? std::min(xs[i], xs[dir == 0 ? i + 1 : i - 1]) : ys[std::min(j, dir == 2 ? j + 1 : j - 1)];
            const float x1 = horizontal ? std::max(xs[i], xs[dir == 0 ? i + 1 : i - 1]) : ys[std::max(j, dir == 2 ? j + 1 : j - 1)];
            const float at = horizontal ? ys[j] : xs[i];
            float cost = 0.0f;
            const auto& same = horizontal ? used.rows : used.cols;
            if (const auto it = same.find(std::lround(at)); it != same.end())
                for (const auto& [p, q] : it->second) if (p < x1 - 0.5f && q > x0 + 0.5f) { cost += len * 3.0f + bend; break; } // по чужой линии — только если иначе никак
            for (const auto& [line, spans] : horizontal ? used.cols : used.rows) // поперёк чужой линии — лучше обойти
                if (line > x0 + 0.5f && line < x1 - 0.5f)
                    for (const auto& [p, q] : spans) if (at > p + 0.5f && at < q - 0.5f) cost += 150.0f;
            return cost;
        };
        const int states = nx * ny * 4;
        std::vector<float> cost(states, FLT_MAX);
        std::vector<int> from(states, -1);
        using Item = std::pair<float, int>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
        const int start = (sj * nx + si) * 4 + (outDir > 0.0f ? 0 : 1);
        cost[start] = 0;
        open.push({ 0.0f, start });
        int goal = -1;
        while (!open.empty()) {
            const auto [c, state] = open.top();
            open.pop();
            if (c > cost[state]) continue;
            const int dir = state % 4, cell = state / 4, i = cell % nx, j = cell / nx;
            if (i == ei && j == ej) {
                if (dir != (inDir < 0.0f ? 1 : 0)) { goal = state; break; } // последний шаг — прямо во вход, без разворота
                continue;
            }
            for (int d = 0; d < 4; ++d) {
                if ((d ^ 1) == dir) continue; // назад не поворачиваем
                const int ni = i + (d == 0) - (d == 1), nj = j + (d == 2) - (d == 3);
                if (ni < 0 || nj < 0 || ni >= nx || nj >= ny || inside(xs[ni], ys[nj]) || inside((xs[i] + xs[ni]) * 0.5f, (ys[j] + ys[nj]) * 0.5f)) continue;
                const float len = std::fabs(xs[ni] - xs[i]) + std::fabs(ys[nj] - ys[j]);
                // при равенстве поворачиваем раньше: линия держится у своего выхода и оставляет место соседним
                const float early = d >= 2 ? std::fabs(xs[i] - s.x) * 0.05f : 0.0f;
                const float next = c + len + (d != dir ? bend : 0.0f) + penalty(i, j, d, len) + early;
                const int ns = (nj * nx + ni) * 4 + d;
                if (next < cost[ns]) { cost[ns] = next; from[ns] = state; open.push({ next, ns }); }
            }
        }
        if (goal < 0) return fallback();
        std::vector<ImVec2> path;
        for (int st = goal; st >= 0; st = from[st]) path.push_back(ImVec2(xs[(st / 4) % nx], ys[(st / 4) / nx]));
        std::reverse(path.begin(), path.end());
        std::vector<ImVec2> pts{ a };
        for (const ImVec2& p : path) {
            if (pts.size() >= 2) { // точки на одной прямой склеиваем
                const ImVec2 q = pts[pts.size() - 2], r = pts.back();
                if ((std::fabs(q.x - r.x) < 0.5f && std::fabs(r.x - p.x) < 0.5f) || (std::fabs(q.y - r.y) < 0.5f && std::fabs(r.y - p.y) < 0.5f)) pts.pop_back();
            }
            pts.push_back(p);
        }
        pts.push_back(b);
        for (size_t k = 1; k < pts.size(); ++k) { // запомнить свои отрезки — следующие линии их обойдут
            const ImVec2 p = pts[k - 1], q = pts[k];
            if (std::fabs(p.y - q.y) < 0.5f) used.rows[std::lround(p.y)].push_back({ std::min(p.x, q.x), std::max(p.x, q.x) });
            else used.cols[std::lround(p.x)].push_back({ std::min(p.y, q.y), std::max(p.y, q.y) });
        }
        return pts;
    }

    // Точки линии на холсте (не на экране)
    float Hash01(unsigned x) { // детерминированное «случайное» 0…1 — одна и та же линия всегда одной формы
        x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
        return static_cast<float>(x & 0xffffff) / 16777215.0f;
    }

    // Сколько точек ломаной внутри блоков (у портов линия лежит на краю — это не считается)
    int Hits(const std::vector<ImVec2>& pts, const std::vector<Box>& boxes) {
        int hits = 0;
        for (size_t i = 1; i + 1 < pts.size(); ++i)
            for (const auto& r : boxes)
                if (pts[i].x > r.x0 + 3.0f && pts[i].x < r.x1 - 3.0f && pts[i].y > r.y0 + 3.0f && pts[i].y < r.y1 - 3.0f) { ++hits; break; }
        return hits;
    }

    bool SelfCross(const std::vector<ImVec2>& p) { // петля — кривая пересекает сама себя
        auto side = [](ImVec2 a, ImVec2 b, ImVec2 c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); };
        for (size_t i = 1; i < p.size(); ++i)
            for (size_t j = i + 2; j < p.size(); ++j) {
                const float d1 = side(p[i - 1], p[i], p[j - 1]), d2 = side(p[i - 1], p[i], p[j]);
                const float d3 = side(p[j - 1], p[j], p[i - 1]), d4 = side(p[j - 1], p[j], p[i]);
                if (d1 * d2 < 0.0f && d3 * d4 < 0.0f) return true;
            }
        return false;
    }

    // Одна кубическая кривая Безье: касательные — из направлений портов, длина «ручек» — от расстояния между блоками.
    // s0, s1 — доли ручек (у «Органического» разные), lean0/lean1 — сдвиг ручек поперёк.
    // Если кривая задевает блок или петляет — перебираем ручки и изгиб в постоянном порядке (без скачков): первая чистая побеждает
    std::vector<ImVec2> FitBezier(ImVec2 a, ImVec2 b, float outDir, float inDir, float s0, float s1, float lean0, float lean1, const std::vector<Box>* boxes) {
        const float dx = b.x - a.x, dy = b.y - a.y, dist = std::hypot(dx, dy);
        const bool facing = inDir * outDir < 0.0f && outDir * dx > 0.0f;
        const float base = facing ? std::clamp(dist * 0.45f, 30.0f, 220.0f) : std::clamp(40.0f + std::fabs(dy) * 0.25f + std::fabs(dx) * 0.3f, 50.0f, 200.0f);
        auto make = [&](float k, float bend) {
            const ImVec2 c1(a.x + outDir * base * s0 * k, a.y + lean0 + bend), c2(b.x + inDir * base * s1 * k, b.y + lean1 + bend);
            std::vector<ImVec2> pts;
            for (int i = 0; i <= 48; ++i) pts.push_back(ImBezierCubicCalc(a, c1, c2, b, i / 48.0f));
            return pts;
        };
        if (!boxes) return make(1.0f, 0.0f);
        static const float kScale[] = { 1.0f, 0.7f, 1.4f, 0.45f, 1.9f };
        static const float kBend[] = { 0.0f, -1.0f, 1.0f, -2.0f, 2.0f, -3.0f, 3.0f };
        std::vector<ImVec2> best;
        int bestScore = 1 << 30;
        for (float bend : kBend)
            for (float k : kScale) {
                auto pts = make(k, bend * 60.0f);
                const int score = Hits(pts, *boxes) * 10 + (SelfCross(pts) ? 5 : 0);
                if (score == 0) return pts;
                if (score < bestScore) { bestScore = score; best = std::move(pts); }
            }
        return best;
    }

    bool SegmentHitsBox(ImVec2 p, ImVec2 q, const Box& r, float pad) { // отрезок задевает прямоугольник (Лян — Барски)
        float t0 = 0.0f, t1 = 1.0f;
        const float dx = q.x - p.x, dy = q.y - p.y;
        const float pp[4] = { -dx, dx, -dy, dy }, qq[4] = { p.x - (r.x0 - pad), (r.x1 + pad) - p.x, p.y - (r.y0 - pad), (r.y1 + pad) - p.y };
        for (int i = 0; i < 4; ++i) {
            if (std::fabs(pp[i]) < 1e-6f) { if (qq[i] < 0.0f) return false; continue; }
            const float t = qq[i] / pp[i];
            if (pp[i] < 0.0f) t0 = std::max(t0, t); else t1 = std::min(t1, t);
            if (t0 > t1) return false;
        }
        return true;
    }

    // «Угловой»: H/V-маршрут натягиваем как нитку — из каждой точки сразу в самую дальнюю видимую, мимо блоков.
    // Получаются прямые отрезки и диагонали с минимумом изломов; у портов — короткий прямой выход
    std::vector<ImVec2> Pull(const std::vector<ImVec2>& pts, const std::vector<Box>& boxes) {
        const int n = static_cast<int>(pts.size());
        if (n < 3) return pts;
        constexpr float stub = 14.0f;
        const ImVec2 a = pts[0], b = pts[n - 1];
        const float outDir = pts[1].x >= a.x ? 1.0f : -1.0f, inDir = pts[n - 2].x >= b.x ? 1.0f : -1.0f;
        std::vector<ImVec2> chain{ ImVec2(a.x + outDir * stub, a.y) };
        for (int i = 1; i + 1 < n; ++i) chain.push_back(pts[i]);
        chain.push_back(ImVec2(b.x + inDir * stub, b.y));
        auto visible = [&](ImVec2 p, ImVec2 q) {
            for (const auto& r : boxes) if (SegmentHitsBox(p, q, r, 6.0f)) return false;
            return true;
        };
        std::vector<ImVec2> out{ a, chain[0] };
        const int last = static_cast<int>(chain.size()) - 1;
        for (int i = 0; i < last;) {
            int j = last;
            while (j > i + 1 && !visible(chain[i], chain[j])) --j;
            out.push_back(chain[j]);
            i = j;
        }
        out.push_back(b);
        return out;
    }

    std::vector<Box> Boxes(const Scenario& s) {
        std::vector<Box> boxes;
        for (const auto& b : s.blocks) boxes.push_back({ b.x, b.y, b.x + kW, b.y + NodeH(b) });
        return boxes;
    }

    // Форма линии для видов не по сетке. boxes == nullptr — без проверки блоков (тянем новую стрелку)
    std::vector<ImVec2> Shape(int style, ImVec2 a, ImVec2 b, float outDir, float inDir, unsigned seed, const std::vector<Box>* boxes) {
        (void)seed;
        switch (style) {
        case kMinimal: return { a, b }; // один отрезок наискосок
        case kBezier: case kQuiet: return FitBezier(a, b, outDir, inDir, 1.0f, 1.0f, 0.0f, 0.0f, boxes);
        case kCyber: return FitBezier(a, b, outDir, inDir, 0.8f, 0.8f, 0.0f, 0.0f, boxes);
        default: { // по сетке, пока тянем стрелку: простая ступенька
            const float mid = (a.x + b.x) * 0.5f;
            return { a, ImVec2(mid, a.y), ImVec2(mid, b.y), b };
        }
        }
    }

    // Углы маршрута по сетке: изломы под 90° с едва заметным скруглением
    std::vector<ImVec2> Corners(int style, const std::vector<ImVec2>& pts, float scale = 1.0f, const std::vector<Box>* boxes = nullptr) {
        (void)boxes;
        return style == kOrtho ? RoundCorners(pts, 5.0f * scale) : pts;
    }

    // Лентой вниз: из точки снизу в точку сверху. Кривые — плавная S, изломы — под 90°, назад вверх — в обход сбоку
    std::vector<ImVec2> VLink(int style, ImVec2 a, ImVec2 b) {
        if (style == kMinimal) return { a, b };
        const float dy = b.y - a.y;
        if (style == kOrtho) {
            if (dy > 24.0f) {
                const float mid = std::round((a.y + b.y) * 0.5f);
                return RoundCorners({ a, ImVec2(a.x, mid), ImVec2(b.x, mid), b }, 6.0f);
            }
            const float side = std::max(a.x, b.x) + kW * 0.5f + 30.0f;
            return RoundCorners({ a, ImVec2(a.x, a.y + 20.0f), ImVec2(side, a.y + 20.0f), ImVec2(side, b.y - 20.0f), ImVec2(b.x, b.y - 20.0f), b }, 6.0f);
        }
        ImVec2 c1, c2;
        if (dy > 20.0f) {
            const float k = std::clamp(dy * 0.55f, 24.0f, 150.0f);
            c1 = ImVec2(a.x, a.y + k), c2 = ImVec2(b.x, b.y - k);
        } else { // назад вверх: петля сбоку
            const float k = std::clamp(80.0f - dy * 0.3f, 80.0f, 220.0f), off = kW * 0.75f;
            c1 = ImVec2(a.x + off, a.y + k), c2 = ImVec2(b.x + off, b.y - k);
        }
        std::vector<ImVec2> pts;
        for (int i = 0; i <= 32; ++i) pts.push_back(ImBezierCubicCalc(a, c1, c2, b, i / 32.0f));
        return pts;
    }

    std::vector<ImVec2> LinkPoints(const Scenario& s, const Block& from, int port, const Block& to) {
        if (Vertical()) return VLink(s_linkStyle, OutPortC(from, port), InPortC(to));
        const ImVec2 a = OutPortC(from, port), b = InPortC(to);
        if (s_linkStyle == kMinimal) return { a, b };
        const std::vector<Box> boxes = Boxes(s);
        if (Routed(s_linkStyle)) {
            const auto it = g_canvas.routes.find({ from.id, port, to.id });
            return Corners(s_linkStyle, it != g_canvas.routes.end() ? it->second : std::vector<ImVec2>{ a, b }, 1.0f, &boxes);
        }
        const unsigned seed = static_cast<unsigned>(from.id) * 73856093U ^ static_cast<unsigned>(to.id) * 19349663U ^ static_cast<unsigned>(port + 1) * 83492791U;
        return Shape(s_linkStyle, a, b, OutLeft(from) ? -1.0f : 1.0f, InRight(to) ? 1.0f : -1.0f, seed, &boxes);
    }

    // Отрезки разных линий на одной прямой раздвигаем на равный шаг — чтобы не сливались в одну.
    // Крайние отрезки у портов не трогаем: линия всегда выходит из середины кружка
    void SeparateLanes(std::vector<std::vector<ImVec2>*>& lines, const std::vector<int>& source) {
        struct Seg { int line, k; float lo, hi; };
        std::map<std::pair<int, long>, std::vector<Seg>> groups; // (горизонтальный ли, координата)
        for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
            const auto& pts = *lines[i];
            for (int k = 2; k + 2 < static_cast<int>(pts.size()); ++k) {
                const ImVec2 a = pts[k], b = pts[k + 1];
                const bool horizontal = std::fabs(a.y - b.y) < 0.5f;
                groups[{ horizontal, std::lround(horizontal ? a.y : a.x) }].push_back({ i, k, horizontal ? std::min(a.x, b.x) : std::min(a.y, b.y), horizontal ? std::max(a.x, b.x) : std::max(a.y, b.y) });
            }
        }
        const float lane = 8.0f;
        for (auto& [key, segs] : groups) {
            // соседи по одной прямой, которые перекрываются и идут от разных выходов
            std::vector<Seg> busy;
            for (const auto& a : segs)
                for (const auto& b : segs)
                    if (a.line != b.line && source[a.line] != source[b.line] && a.lo < b.hi - 0.5f && b.lo < a.hi - 0.5f) { busy.push_back(a); break; }
            std::vector<int> owners;
            for (const auto& g : busy) if (std::find(owners.begin(), owners.end(), source[g.line]) == owners.end()) owners.push_back(source[g.line]);
            for (const auto& g : busy) {
                const int slot = static_cast<int>(std::find(owners.begin(), owners.end(), source[g.line]) - owners.begin());
                const float shift = (slot - (owners.size() - 1) * 0.5f) * lane;
                auto& pts = *lines[g.line];
                if (key.first) { pts[g.k].y += shift; pts[g.k + 1].y += shift; }
                else { pts[g.k].x += shift; pts[g.k + 1].x += shift; }
            }
        }
    }

    // Прямые углы со скруглением: изгиб — четверть окружности, а не острый угол
    std::vector<ImVec2> RoundCorners(const std::vector<ImVec2>& pts, float radius) {
        if (pts.size() < 3) return pts;
        std::vector<ImVec2> out{ pts.front() };
        for (size_t i = 1; i + 1 < pts.size(); ++i) {
            const ImVec2 a = pts[i - 1], p = pts[i], b = pts[i + 1];
            const float la = std::hypot(p.x - a.x, p.y - a.y), lb = std::hypot(b.x - p.x, b.y - p.y);
            const float r = std::min({ radius, la * 0.5f, lb * 0.5f });
            if (r < 0.5f || std::fabs((p.x - a.x) * (b.y - p.y) - (p.y - a.y) * (b.x - p.x)) < 0.01f) { out.push_back(p); continue; }
            const ImVec2 p1(p.x + (a.x - p.x) / la * r, p.y + (a.y - p.y) / la * r), p2(p.x + (b.x - p.x) / lb * r, p.y + (b.y - p.y) / lb * r);
            for (int t = 0; t <= 6; ++t) out.push_back(ImBezierQuadraticCalc(p1, p, p2, t / 6.0f));
        }
        out.push_back(pts.back());
        return out;
    }

    void UpdateRoutes(const Scenario& s) {
        if (!Routed(s_linkStyle) || Vertical()) return;
        std::string key;
        for (const auto& b : s.blocks) {
            key += std::to_string(b.id) + ":" + std::to_string(std::lround(b.x)) + "," + std::to_string(std::lround(b.y)) + "," + std::to_string(Scenarios::PortCount(b)) + "," + std::to_string((InRight(b) ? 1 : 0) + (OutLeft(b) ? 2 : 0)) + ">";
            for (int p = 0; p < Scenarios::PortCount(b); ++p) { for (int to : Scenarios::Port(b, p)) key += std::to_string(to) + " "; key += "|"; }
        }
        if (key == g_canvas.routeKey) return;
        g_canvas.routeKey = key;
        g_canvas.routes.clear();
        std::vector<Box> boxes;
        for (const auto& b : s.blocks) boxes.push_back({ b.x, b.y, b.x + kW, b.y + NodeH(b) });
        // длинные линии — первыми: им труднее всего найти путь, короткие потом обойдут их
        struct Job { const Block* from; int port; const Block* to; float span; };
        std::vector<Job> jobs;
        for (const auto& b : s.blocks)
            for (int p = 0; p < Scenarios::PortCount(b); ++p)
                for (int to : Scenarios::Port(b, p))
                    for (const auto& t : s.blocks)
                        if (t.id == to) {
                            const ImVec2 a = OutPortC(b, p), e = InPortC(t);
                            jobs.push_back({ &b, p, &t, std::fabs(e.y - a.y) + (e.x < a.x ? 10000.0f : 0.0f) });
                        }
        std::stable_sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) { return a.span > b.span; });
        Used used;
        for (const auto& j : jobs) g_canvas.routes[{ j.from->id, j.port, j.to->id }] = Orthogonal(OutPortC(*j.from, j.port), InPortC(*j.to), boxes, used, OutLeft(*j.from) ? -1.0f : 1.0f, InRight(*j.to) ? 1.0f : -1.0f);
        std::vector<std::vector<ImVec2>*> lines;
        std::vector<int> source;
        for (auto& [key, pts] : g_canvas.routes) { lines.push_back(&pts); source.push_back(std::get<0>(key) * 16 + std::get<1>(key)); }
        SeparateLanes(lines, source); // углы — в LinkPoints, по стилю
    }

    float DistanceToPolyline(const std::vector<ImVec2>& pts, ImVec2 p) {
        float best = 1e9f;
        for (size_t i = 1; i < pts.size(); ++i) {
            const ImVec2 q = ImLineClosestPoint(pts[i - 1], pts[i], p);
            best = std::min(best, std::hypot(q.x - p.x, q.y - p.y));
        }
        return best;
    }

    ImVec2 PointAt(const std::vector<ImVec2>& pts, float t) { // точка на ломаной по доле длины — для «бегущих» огоньков
        float total = 0;
        for (size_t i = 1; i < pts.size(); ++i) total += std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
        float want = total * t;
        for (size_t i = 1; i < pts.size(); ++i) {
            const float len = std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
            if (want <= len && len > 0) return ImLerp(pts[i - 1], pts[i], want / len);
            want -= len;
        }
        return pts.back();
    }

    // Полоса вдоль ломаной: своя толщина в начале и в конце, цвет по длине, мягкий край (feather), ровные стыки без пятен
    void Strip(ImDrawList* dl, const std::vector<ImVec2>& in, float w0, float w1, float feather, const std::function<ImU32(float)>& colorAt, float t0 = 0.0f, float t1 = 1.0f, float bulge = 0.0f) {
        std::vector<ImVec2> p;
        p.reserve(in.size());
        for (const ImVec2& q : in) if (p.empty() || std::fabs(q.x - p.back().x) + std::fabs(q.y - p.back().y) > 0.05f) p.push_back(q);
        const int n = static_cast<int>(p.size());
        if (n < 2) return;
        std::vector<float> at(n, 0.0f);
        for (int i = 1; i < n; ++i) at[i] = at[i - 1] + std::hypot(p[i].x - p[i - 1].x, p[i].y - p[i - 1].y);
        const float total = std::max(at.back(), 0.001f);
        auto normal = [&](int k) {
            const float dx = p[k + 1].x - p[k].x, dy = p[k + 1].y - p[k].y, l = std::max(std::hypot(dx, dy), 0.0001f);
            return ImVec2(-dy / l, dx / l);
        };
        const ImVec2 uv = dl->_Data->TexUvWhitePixel;
        dl->PrimReserve((n - 1) * 18, n * 4);
        const unsigned base = dl->_VtxCurrentIdx;
        for (int i = 0; i < n; ++i) {
            ImVec2 nm;
            if (i == 0) nm = normal(0);
            else if (i == n - 1) nm = normal(n - 2);
            else {
                const ImVec2 na = normal(i - 1), nb = normal(i);
                ImVec2 m(na.x + nb.x, na.y + nb.y);
                const float ml = std::hypot(m.x, m.y);
                m = ml < 0.001f ? na : ImVec2(m.x / ml, m.y / ml);
                const float scale = 1.0f / std::max(m.x * na.x + m.y * na.y, 0.5f); // на повороте линия не худеет
                nm = ImVec2(m.x * scale, m.y * scale);
            }
            const float f = at[i] / total;
            const float width = ImLerp(w0, w1, f) * (1.0f + bulge * std::sin(f * 3.1416f));
            const float core = std::max(0.0f, width * 0.5f - feather * 0.5f), outer = core + feather;
            const ImU32 col = colorAt(t0 + (t1 - t0) * f), clear = col & ~IM_COL32_A_MASK;
            dl->PrimWriteVtx(ImVec2(p[i].x - nm.x * outer, p[i].y - nm.y * outer), uv, clear);
            dl->PrimWriteVtx(ImVec2(p[i].x - nm.x * core, p[i].y - nm.y * core), uv, col);
            dl->PrimWriteVtx(ImVec2(p[i].x + nm.x * core, p[i].y + nm.y * core), uv, col);
            dl->PrimWriteVtx(ImVec2(p[i].x + nm.x * outer, p[i].y + nm.y * outer), uv, clear);
        }
        for (int i = 0; i + 1 < n; ++i)
            for (int q = 0; q < 3; ++q) {
                const ImDrawIdx a = static_cast<ImDrawIdx>(base + i * 4 + q), b = static_cast<ImDrawIdx>(a + 1), c = static_cast<ImDrawIdx>(a + 5), d = static_cast<ImDrawIdx>(a + 4);
                dl->PrimWriteIdx(a); dl->PrimWriteIdx(b); dl->PrimWriteIdx(c);
                dl->PrimWriteIdx(a); dl->PrimWriteIdx(c); dl->PrimWriteIdx(d);
            }
    }

    // Пунктир: куски ломаной «тире — пробел — тире», у каждого куска своя доля длины (для перехода цвета)
    struct Dash { std::vector<ImVec2> pts; float t0, t1; };
    std::vector<Dash> Dashes(const std::vector<ImVec2>& pts, float on, float off, float shift) {
        std::vector<Dash> out;
        float total = 0.0f;
        for (size_t i = 1; i < pts.size(); ++i) total += std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
        if (total <= 0.0f || on <= 0.0f) return out;
        const float period = on + off, phase = std::fmod(shift, period);
        float pos = 0.0f;
        Dash cur;
        bool inDash = false;
        for (size_t i = 1; i < pts.size(); ++i) {
            const ImVec2 a = pts[i - 1], b = pts[i];
            const float len = std::hypot(b.x - a.x, b.y - a.y);
            if (len <= 0.0f) continue;
            float s = 0.0f;
            while (s < len) {
                float m = std::fmod(pos + s - phase, period);
                if (m < 0.0f) m += period;
                const bool dash = m < on;
                float stop = std::min(len, s + (dash ? on - m : period - m));
                if (stop <= s) stop = std::min(len, s + 0.01f);
                if (dash) {
                    if (!inDash) { cur = Dash{ { ImLerp(a, b, s / len) }, (pos + s) / total, (pos + s) / total }; inDash = true; }
                    cur.pts.push_back(ImLerp(a, b, stop / len));
                    cur.t1 = (pos + stop) / total;
                } else if (inDash) { out.push_back(cur); inDash = false; }
                s = stop;
            }
            pos += len;
        }
        if (inDash) out.push_back(cur);
        return out;
    }

    // ---------------------------------------------------------------- цвета вида
    const Theme::Color kPortPalette[] = { { 110, 205, 130 }, { 240, 170, 80 }, { 235, 95, 105 }, { 150, 120, 240 } };
    Theme::Color Grey() { return Theme::Light() ? Theme::Color{ 125, 120, 135 } : Theme::Color{ 150, 147, 158 }; }
    // неон кибер-вида: триггер фиолетовый, управление голубое, действия бирюзовые
    Theme::Color NeonOf(const Block& b) {
        if (b.color >= 0) return Theme::FromRgb(b.color);
        if (Scenarios::IsTrigger(b.step)) return { 180, 95, 255 };
        switch (b.step) {
        case Step::Condition: case Step::Loop: case Step::Switch: case Step::Wait: case Step::SetVar: return { 70, 165, 255 };
        default: return { 50, 225, 200 };
        }
    }
    Theme::Color Dim() { return Theme::Light() ? Theme::Color{ 125, 110, 150 } : Theme::Color{ 132, 116, 168 }; } // тусклые порты и линии
    Theme::Color PortColor(const Block& b, int port) {
        switch (s_linkStyle) {
        case kQuiet: return Dim();
        case kMinimal: return Grey();
        case kOrtho: return kPortPalette[port % 4];
        case kCyber: return NeonOf(b);
        default: return ColorOf(b);
        }
    }
    Theme::Color InColor(const Block& b) {
        switch (s_linkStyle) {
        case kQuiet: return Dim();
        case kMinimal: return Grey();
        case kOrtho: return { 100, 140, 245 };
        case kCyber: return NeonOf(b);
        default: return ColorOf(b);
        }
    }
    Theme::Color BodyColor(const Block& b) { return s_linkStyle == kCyber ? NeonOf(b) : ColorOf(b); }

    // направление у конца линии: точка на несколько пикселей раньше — у кривых последний отрезок короткий
    ImVec2 EndDir(const std::vector<ImVec2>& pts, float back) {
        const ImVec2 end = pts.back();
        for (int i = static_cast<int>(pts.size()) - 2; i >= 0; --i)
            if (i == 0 || std::hypot(end.x - pts[i].x, end.y - pts[i].y) >= back) {
                const float dx = end.x - pts[i].x, dy = end.y - pts[i].y, l = std::max(std::hypot(dx, dy), 0.0001f);
                return ImVec2(dx / l, dy / l);
            }
        return ImVec2(1, 0);
    }
    void Arrow(ImDrawList* dl, ImVec2 tip, ImVec2 d, float s, ImU32 col) {
        dl->AddTriangleFilled(tip, ImVec2(tip.x - d.x * s * 1.5f - d.y * s * 0.65f, tip.y - d.y * s * 1.5f + d.x * s * 0.65f),
                              ImVec2(tip.x - d.x * s * 1.5f + d.y * s * 0.65f, tip.y - d.y * s * 1.5f - d.x * s * 0.65f), col);
    }
    void Chevron(ImDrawList* dl, ImVec2 p, ImVec2 d, float s, ImU32 col, float w) {
        const ImVec2 n(-d.y, d.x);
        dl->AddLine(ImVec2(p.x - d.x * s + n.x * s, p.y - d.y * s + n.y * s), p, col, w);
        dl->AddLine(p, ImVec2(p.x - d.x * s - n.x * s, p.y - d.y * s - n.y * s), col, w);
    }
    using Paint = std::function<ImU32(float)>;
    Paint Solid(ImU32 c) { return [c](float) { return c; }; }

    // Линия выбранного вида. from/to — цвета концов (у кривых и кибер — переход от блока к блоку). hot — под мышью (красная)
    void DrawLink(ImDrawList* dl, const std::vector<ImVec2>& pts, Theme::Color from, Theme::Color to, float alpha, float thickness, bool flow, int style, bool hot = false) {
        const int n = static_cast<int>(pts.size());
        if (n < 2) return;
        (void)hot;
        const float u = thickness / 1.6f, aa = std::max(1.0f, u * 0.8f);
        const Theme::Color c0 = from, c1 = to, white{ 255, 255, 255 };
        auto paint = [&](float a, float lift = 0.0f) { return Paint([=](float t) { return c0.Mix(c1, t).Mix(white, lift).WithAlpha(a * alpha); }); };
        std::vector<float> at(n, 0.0f);
        for (int i = 1; i < n; ++i) at[i] = at[i - 1] + std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
        const float total = at.back(), time = static_cast<float>(ImGui::GetTime());
        switch (style) {
        case kQuiet: // тонкая тусклая кривая, без стрелок и свечения
            Strip(dl, pts, 1.5f * u, 1.5f * u, aa, paint(0.6f));
            return;
        case kMinimal: // тонкая серая прямая со стрелкой
            Strip(dl, pts, 1.1f * u, 1.1f * u, aa, paint(0.8f));
            Arrow(dl, pts.back(), EndDir(pts, 6.0f * u), 4.2f * u, c1.WithAlpha(0.9f * alpha));
            return;
        case kOrtho: // ровная линия цвета порта
            Strip(dl, pts, 1.7f * u, 1.7f * u, aa, paint(0.92f));
            return;
        case kCyber: { // светящиеся точки с постоянным шагом и бегущие шевроны «>>»
            Strip(dl, pts, 9.0f * u, 9.0f * u, 4.5f * u, paint(0.06f));
            const float step = 7.5f * u;
            int seg = 1;
            for (float d = 0.0f; d <= total; d += step) {
                while (seg < n - 1 && at[seg] < d) ++seg;
                const float len = std::max(at[seg] - at[seg - 1], 0.0001f);
                const ImVec2 p = ImLerp(pts[seg - 1], pts[seg], std::clamp((d - at[seg - 1]) / len, 0.0f, 1.0f));
                const Theme::Color c = c0.Mix(c1, total > 0.0f ? d / total : 0.0f);
                dl->AddCircleFilled(p, 2.8f * u, c.WithAlpha(0.14f * alpha), 10);
                dl->AddCircleFilled(p, 1.25f * u, c.Mix(white, 0.35f).WithAlpha(0.95f * alpha), 8);
            }
            if (flow && total > 30.0f * u) {
                const int count = std::max(1, static_cast<int>(total / (110.0f * u)));
                const float dlt = 3.0f * u / total;
                for (int k = 0; k < count; ++k) {
                    const float t = std::fmod(time * (60.0f * u / total) + static_cast<float>(k) / count, 1.0f);
                    const float fade = std::clamp(std::min(t, 1.0f - t) * 8.0f, 0.0f, 1.0f);
                    const ImVec2 p = PointAt(pts, t), q = PointAt(pts, std::min(1.0f, t + dlt)), r = PointAt(pts, std::max(0.0f, t - dlt));
                    ImVec2 d(q.x - r.x, q.y - r.y);
                    const float l = std::max(std::hypot(d.x, d.y), 0.0001f);
                    d = ImVec2(d.x / l, d.y / l);
                    const ImU32 col = c0.Mix(c1, t).Mix(white, 0.3f).WithAlpha(0.95f * fade * alpha);
                    for (int j = 0; j < 2; ++j) Chevron(dl, ImVec2(p.x + d.x * j * 4.5f * u, p.y + d.y * j * 4.5f * u), d, 3.2f * u, col, std::max(1.0f, 1.4f * u));
                }
            }
            Arrow(dl, pts.back(), EndDir(pts, 6.0f * u), 4.5f * u, c1.Mix(white, 0.3f).WithAlpha(0.95f * alpha));
            return;
        }
        default: // плавные кривые: дымка, свечение, яркая середина и бегущий огонёк
            Strip(dl, pts, 10.0f * u, 10.0f * u, 5.0f * u, paint(0.07f));
            Strip(dl, pts, 4.8f * u, 4.8f * u, 2.4f * u, paint(0.16f));
            Strip(dl, pts, 2.2f * u, 2.2f * u, aa, paint(0.95f, 0.1f));
            if (flow) {
                const float k = std::fmod(time * 0.22f, 1.0f), glow = std::sin(k * 3.1416f);
                const ImVec2 p = PointAt(pts, k);
                dl->AddCircleFilled(p, 4.8f * u, c0.Mix(c1, k).WithAlpha(0.16f * glow * alpha), 14);
                dl->AddCircleFilled(p, 1.9f * u, c0.Mix(c1, k).Mix(white, 0.55f).WithAlpha(0.85f * glow * alpha), 12);
            }
        }
    }

    // Порт по виду: круг со свечением, маленький квадрат, цветной квадрат, неоновое кольцо. Под мышью растёт и показывает «+»
    void DrawPort(ImDrawList* dl, ImVec2 p, Theme::Color color, bool filled, bool hot, float zoom = -1.0f) {
        (void)filled;
        const float k = Px(1.0f) * (zoom < 0.0f ? g_canvas.zoom : zoom);
        const Theme::Color white{ 255, 255, 255 };
        switch (s_linkStyle) {
        case kQuiet: { // маленький тусклый кружок
            const float r = (hot ? 6.5f : 3.0f) * k;
            dl->AddCircleFilled(p, r, hot ? Theme::Accent().WithAlpha(1.0f) : color.WithAlpha(0.6f), 16);
            if (!hot) dl->AddCircle(p, r + 1.2f * k, Theme::Light() ? IM_COL32(255, 255, 255, 160) : IM_COL32(20, 14, 24, 200), 16, std::max(1.0f, 1.2f * k));
            break;
        }
        case kMinimal: {
            const float r = (hot ? 5.5f : 3.2f) * k;
            dl->AddRectFilled(ImVec2(p.x - r, p.y - r), ImVec2(p.x + r, p.y + r), color.WithAlpha(hot ? 1.0f : 0.85f));
            break;
        }
        case kOrtho: {
            const float r = (hot ? 6.5f : 4.6f) * k;
            dl->AddRectFilled(ImVec2(p.x - r, p.y - r), ImVec2(p.x + r, p.y + r), color.WithAlpha(1.0f), 1.6f * k);
            dl->AddRect(ImVec2(p.x - r, p.y - r), ImVec2(p.x + r, p.y + r), color.Mix({ 0, 0, 0 }, 0.35f).WithAlpha(0.8f), 1.6f * k, 0, std::max(1.0f, k));
            break;
        }
        case kCyber: {
            const float r = (hot ? 7.5f : 5.6f) * k;
            dl->AddCircleFilled(p, r * 2.0f, color.WithAlpha(0.14f), 20);
            dl->AddCircleFilled(p, r, Theme::SurfaceSolid(), 20);
            dl->AddCircle(p, r, color.Mix(white, 0.25f).WithAlpha(1.0f), 20, std::max(1.0f, 1.6f * k));
            if (!hot) dl->AddCircleFilled(p, r * 0.4f, color.Mix(white, 0.4f).WithAlpha(1.0f), 12);
            break;
        }
        default: {
            const float r = (hot ? 7.5f : 5.2f) * k;
            dl->AddCircleFilled(p, r * 2.1f, color.WithAlpha(0.16f), 20);
            dl->AddCircleFilled(p, r, color.Mix(white, 0.12f).WithAlpha(1.0f), 20);
            dl->AddCircle(p, r, Theme::SurfaceSolid(), 20, std::max(1.0f, 1.2f * k));
        }
        }
        if (hot) {
            const float s = 3.2f * k, w = std::max(1.0f, 1.4f * k);
            const ImU32 ink = s_linkStyle == kCyber ? color.Mix(white, 0.4f).WithAlpha(1.0f) : IM_COL32(255, 255, 255, 255);
            dl->AddLine(ImVec2(p.x - s, p.y), ImVec2(p.x + s, p.y), ink, w);
            dl->AddLine(ImVec2(p.x, p.y - s), ImVec2(p.x, p.y + s), ink, w);
        }
    }

    // Образец вида в меню: два порта и линия между ними
    void LineSample(ImDrawList* dl, int style, ImVec2 a, ImVec2 b) {
        const int keep = s_linkStyle;
        s_linkStyle = style;
        std::vector<ImVec2> pts;
        if (style == kOrtho) {
            const float mid = (a.x + b.x) * 0.5f;
            pts = Corners(style, { a, ImVec2(mid, a.y), ImVec2(mid, b.y), b }, 0.6f);
        } else pts = Shape(style, a, b, 1.0f, -1.0f, 0U, nullptr);
        Block one, two;
        one.step = Step::Phrase;
        two.step = Step::Launch;
        const Theme::Color end = style == kOrtho ? PortColor(one, 0) : InColor(two);
        DrawLink(dl, pts, PortColor(one, 0), end, 1.0f, Px(1.6f), true, style);
        DrawPort(dl, a, PortColor(one, 0), true, false, 0.8f);
        DrawPort(dl, b, end, true, false, 0.8f);
        s_linkStyle = keep;
    }

    // ---------------------------------------------------------------- карточка блока по виду
    void Glow(ImDrawList* dl, ImVec2 a, ImVec2 m, float r, Theme::Color c, float strength) {
        const float z = Px(1.0f) * g_canvas.zoom;
        for (int i = 1; i <= 4; ++i) {
            const float g = i * 2.0f * z;
            dl->AddRect(ImVec2(a.x - g, a.y - g), ImVec2(m.x + g, m.y + g), c.WithAlpha(std::min(1.0f, 0.085f * strength * (5 - i) / 4.0f)), r + g, 0, 2.4f * z);
        }
    }

    std::vector<ImVec2> ShapePoints(const Block& b, ImVec2 a, ImVec2 m) {
        std::vector<ImVec2> pts;
        for (size_t i = 0; i + 1 < b.shape.size(); i += 2) pts.emplace_back(ImLerp(a.x, m.x, b.shape[i]), ImLerp(a.y, m.y, b.shape[i + 1]));
        return pts;
    }
    bool Crosses(ImVec2 p1, ImVec2 p2, ImVec2 p3, ImVec2 p4) {
        auto cr = [](ImVec2 o, ImVec2 a, ImVec2 b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); };
        const float d1 = cr(p3, p4, p1), d2 = cr(p3, p4, p2), d3 = cr(p1, p2, p3), d4 = cr(p1, p2, p4);
        return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
    }
    bool SimplePolygon(const std::vector<ImVec2>& p) { // без самопересечений — можно залить как есть
        const int n = static_cast<int>(p.size());
        for (int i = 0; i < n; ++i)
            for (int j = i + 2; j < n; ++j) {
                if (i == 0 && j == n - 1) continue;
                if (Crosses(p[i], p[(i + 1) % n], p[j], p[(j + 1) % n])) return false;
            }
        return true;
    }
    std::vector<ImVec2> Hull(std::vector<ImVec2> p) { // выпуклая оболочка — заливка для каракуля с петлями
        std::sort(p.begin(), p.end(), [](ImVec2 a, ImVec2 b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
        auto cr = [](ImVec2 o, ImVec2 a, ImVec2 b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); };
        std::vector<ImVec2> h(p.size() * 2);
        size_t k = 0;
        for (size_t i = 0; i < p.size(); ++i) { while (k >= 2 && cr(h[k - 2], h[k - 1], p[i]) <= 0) --k; h[k++] = p[i]; }
        for (size_t i = p.size() - 1, t = k + 1; i > 0; --i) { while (k >= t && cr(h[k - 2], h[k - 1], p[i - 1]) <= 0) --k; h[k++] = p[i - 1]; }
        h.resize(k > 1 ? k - 1 : k);
        return h;
    }

    // нарисованный от руки контур: заливка поверхностью, свечение и «карандашная» обводка цвета блока
    void DrawnBody(ImDrawList* dl, const Block& b, ImVec2 a, ImVec2 m, bool strong, float appear) {
        const float z = Px(1.0f) * g_canvas.zoom;
        const std::vector<ImVec2> pts = ShapePoints(b, a, m);
        if (pts.size() < 3) return;
        const ImU32 fill = Theme::Surface(0.95f * appear);
        if (SimplePolygon(pts)) dl->AddConcavePolyFilled(pts.data(), static_cast<int>(pts.size()), fill);
        else { const auto hull = Hull(pts); dl->AddConvexPolyFilled(hull.data(), static_cast<int>(hull.size()), fill); }
        std::vector<ImVec2> closed = pts;
        closed.push_back(pts[0]);
        closed.push_back(pts[1]); // стык замкнут без щели
        const Theme::Color c = BodyColor(b);
        Strip(dl, closed, 9.0f * z, 9.0f * z, 4.5f * z, Solid(c.WithAlpha((strong ? 0.24f : 0.12f) * appear)));
        Strip(dl, closed, 2.3f * z, 2.3f * z, std::max(1.0f, z), Solid(c.Mix({ 255, 255, 255 }, 0.1f).WithAlpha(0.95f * appear)));
    }

    void BlockBody(ImDrawList* dl, const Block& b, ImVec2 a, ImVec2 m, bool selected, bool hot, float appear) {
        if (!b.shape.empty() && b.outline) { DrawnBody(dl, b, a, m, selected || hot, appear); return; }
        const float z = Px(1.0f) * g_canvas.zoom, h = m.y - a.y;
        const ImU32 fill = Theme::Surface(0.95f * appear);
        const Theme::Color accent = Theme::Accent(), white{ 255, 255, 255 };
        switch (s_linkStyle) {
        case kQuiet: { // тёмная полупрозрачная карточка, тонкая светлая рамка, без свечения
            const float r = 16.0f * z;
            const ImU32 card = Theme::Light() ? Theme::Surface(0.88f * appear) : IM_COL32(36, 27, 54, static_cast<int>(185 * appear));
            dl->AddRectFilled(ImVec2(a.x, a.y + 2.0f * z), ImVec2(m.x, m.y + 3.0f * z), IM_COL32(0, 0, 0, static_cast<int>(40 * appear)), r); // тень едва-едва
            dl->AddRectFilled(a, m, card, r);
            const ImU32 edge = selected ? accent.WithAlpha(0.85f * appear)
                             : Theme::Light() ? IM_COL32(40, 30, 50, static_cast<int>((hot ? 60 : 34) * appear))
                                              : IM_COL32(220, 205, 255, static_cast<int>((hot ? 52 : 26) * appear));
            dl->AddRect(a, m, edge, r, 0, (selected ? 1.5f : 1.0f) * z);
            break;
        }
        case kMinimal: { // плоский прямоугольник, тонкая серая рамка
            const float r = 4.0f * z;
            dl->AddRectFilled(a, m, fill, r);
            dl->AddRect(a, m, selected || hot ? accent.WithAlpha(0.9f * appear) : Grey().WithAlpha(0.5f * appear), r, 0, (selected ? 1.6f : 1.0f) * z);
            break;
        }
        case kOrtho: {
            const float r = 7.0f * z;
            Theme::Plate(dl, a, m, fill, r, selected || hot ? 0.6f : 0.0f);
            dl->AddRect(a, m, selected ? accent.WithAlpha(0.9f * appear) : Theme::Overlay(28), r, 0, 1.2f * z);
            break;
        }
        case kCyber: { // неоновая рамка и заливка градиентом цвета блока
            const Theme::Color c = NeonOf(b);
            const float r = 12.0f * z;
            Glow(dl, a, m, r, c, (selected || hot ? 1.7f : 1.0f) * appear);
            dl->AddRectFilled(a, m, fill, r);
            const int v0 = dl->VtxBuffer.Size;
            dl->AddRectFilled(a, m, IM_COL32(255, 255, 255, static_cast<int>((Theme::Light() ? 110 : 140) * appear)), r);
            ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, a, m,
                c.Mix(white, Theme::Light() ? 0.55f : 0.0f).WithAlpha(1.0f),
                c.Mix(Theme::Light() ? white : Theme::Color{ 8, 10, 22 }, Theme::Light() ? 0.92f : 0.85f).WithAlpha(1.0f));
            dl->AddRect(a, m, c.Mix(white, 0.2f).WithAlpha(0.95f * appear), r, 0, (selected ? 2.4f : 1.7f) * z);
            break;
        }
        default: { // пилюля со светящейся цветной обводкой
            const Theme::Color c = ColorOf(b);
            const float r = Scenarios::PortCount(b) <= 1 ? h * 0.5f : 26.0f * z;
            Glow(dl, a, m, r, c, (selected || hot ? 1.7f : 1.0f) * appear);
            Theme::Plate(dl, a, m, fill, r);
            dl->AddRect(a, m, c.Mix(white, 0.1f).WithAlpha(0.95f * appear), r, 0, (selected ? 2.4f : 1.7f) * z);
        }
        }
    }

    // Слева направо по слоям: триггеры в первом столбце, каждый следующий блок — на столбец правее самого дальнего входа
    void AutoLayout(Scenario& s) {
        std::unordered_map<int, int> layer, incoming;
        for (const auto& b : s.blocks)
            for (int p = 0; p < Scenarios::PortCount(b); ++p)
                for (int to : Scenarios::Port(b, p)) incoming[to]++;
        std::function<void(const Block&, int, std::vector<int>&)> walk = [&](const Block& b, int depth, std::vector<int>& path) {
            if (std::find(path.begin(), path.end(), b.id) != path.end()) return; // по кругу не ходим
            if (layer.count(b.id) && layer[b.id] >= depth) return;
            layer[b.id] = depth;
            path.push_back(b.id);
            for (int p = 0; p < Scenarios::PortCount(b); ++p)
                for (int to : Scenarios::Port(b, p))
                    if (const Block* t = FindBlock(s, to)) walk(*t, depth + 1, path);
            path.pop_back();
        };
        std::vector<int> path;
        for (const auto& b : s.blocks) if (Scenarios::IsTrigger(b.step) || !incoming.count(b.id)) walk(b, 0, path);
        for (const auto& b : s.blocks) if (!layer.count(b.id)) walk(b, 0, path);
        int layers = 0;
        for (const auto& [id, l] : layer) layers = std::max(layers, l + 1);
        if (Vertical()) { // лентой вниз: слой — строка, одиночные блоки чуть сдвинуты вбок по очереди, ветки — рядом
            std::unordered_map<int, float> placedX;
            float y = 0.0f;
            for (int l = 0; l < layers; ++l) {
                std::vector<Block*> row;
                for (auto& b : s.blocks) if (layer[b.id] == l) row.push_back(&b);
                if (row.empty()) continue;
                auto weight = [&](const Block* b) { // под родителем и его выходом
                    float sum = 0;
                    int n = 0;
                    for (const auto& o : s.blocks)
                        for (int p = 0; p < Scenarios::PortCount(o); ++p)
                            for (int to : Scenarios::Port(o, p))
                                if (to == b->id && placedX.count(o.id)) { sum += placedX[o.id] + (p + 1.0f) / (Scenarios::PortCount(o) + 1.0f) * kW; ++n; }
                    return n ? sum / n : b->x;
                };
                std::stable_sort(row.begin(), row.end(), [&](const Block* p, const Block* q) { return weight(p) < weight(q); });
                const float width = row.size() * kW + (row.size() - 1) * kGapX;
                float x = -width * 0.5f + (row.size() == 1 ? (l % 2 ? 40.0f : -40.0f) : 0.0f);
                float rowH = 0;
                for (Block* b : row) {
                    b->x = std::round(x / kGrid) * kGrid;
                    b->y = y;
                    b->sides = -1;
                    placedX[b->id] = b->x;
                    x += kW + kGapX;
                    rowH = std::max(rowH, NodeH(*b));
                }
                y += std::ceil((rowH + kGapV) / kGrid) * kGrid;
            }
            g_dirty = true;
            g_canvas.fitPending = true;
            g_canvas.routeKey.clear();
            return;
        }
        std::unordered_map<int, float> placedY;
        constexpr int kColumns = 4; // длинная цепочка переносится на следующую строку — иначе блоки станут мелкими
        float bandTop = 0, bandHeight = 0;
        for (int l = 0; l < layers; ++l) {
            if (l > 0 && l % kColumns == 0) { bandTop += bandHeight + kRowGap; bandHeight = 0; }
            std::vector<Block*> column;
            for (auto& b : s.blocks) if (layer[b.id] == l) column.push_back(&b);
            if (column.empty()) continue;
            // порядок в столбце — по средней высоте входов слева, чтобы линии меньше перекрещивались
            auto weight = [&](const Block* b) {
                float sum = 0;
                int n = 0;
                for (const auto& o : s.blocks)
                    for (int p = 0; p < Scenarios::PortCount(o); ++p)
                        for (int to : Scenarios::Port(o, p))
                            if (to == b->id && placedY.count(o.id)) { sum += placedY[o.id] + p * kPortGap; ++n; }
                return n ? sum / n : b->y;
            };
            std::stable_sort(column.begin(), column.end(), [&](const Block* p, const Block* q) { return weight(p) < weight(q); });
            float height = 0;
            for (const Block* b : column) height += NodeH(*b) + kGapY;
            bandHeight = std::max(bandHeight, height);
            float y = std::round((bandTop - height * 0.5f) / kGrid) * kGrid;
            for (Block* b : column) {
                const int column = (l / kColumns) % 2 ? kColumns - 1 - l % kColumns : l % kColumns; // нечётный ряд — справа налево
                b->x = column * (kW + kGapX);
                b->y = y;
                placedY[b->id] = y;
                y += std::ceil((NodeH(*b) + kGapY) / kGrid) * kGrid;
            }
        }
        g_dirty = true;
        g_canvas.fitPending = true;
    }

    // Показать все блоки: масштаб и сдвиг, чтобы схема поместилась
    void FitCanvas(const Scenario& s, ImVec2 size) {
        if (s.blocks.empty()) { g_canvas.zoom = g_canvas.zoomTarget = 1.0f; g_canvas.pan = ImVec2(size.x * 0.5f - kW * 0.5f * Px(1), Px(60)); return; }
        ImVec2 mn(FLT_MAX, FLT_MAX), mx(-FLT_MAX, -FLT_MAX);
        for (const auto& b : s.blocks) {
            mn = ImVec2(std::min(mn.x, b.x), std::min(mn.y, b.y));
            mx = ImVec2(std::max(mx.x, b.x + kW), std::max(mx.y, b.y + NodeH(b)));
        }
        mn.x -= 70.0f; mx.x += 70.0f; // место для разворотов линий у краёв
        const float w = mx.x - mn.x, h = mx.y - mn.y;
        const float fitW = (size.x - Px(160)) / std::max(1.0f, w * Px(1)), fitH = (size.y - Px(140)) / std::max(1.0f, h * Px(1));
        // лентой вниз блоки не мельчим: длинная лента начинается сверху, остальное — колесом/перетаскиванием
        g_canvas.zoom = std::clamp(Vertical() ? std::min(fitW, std::max(fitH, 0.95f)) : std::min(fitW, fitH), 0.35f, Vertical() ? 1.15f : 1.1f);
        g_canvas.zoomTarget = g_canvas.zoom;
        const bool fits = h * Z() <= size.y - Px(140);
        g_canvas.pan = ImVec2((size.x - w * Z()) * 0.5f - mn.x * Z(), fits ? (size.y - h * Z()) * 0.5f - mn.y * Z() : Px(80) - mn.y * Z());
    }

    void ZoomAround(ImVec2 origin, ImVec2 screen, float factor) {
        g_canvas.anchorScreen = screen;
        g_canvas.anchorCanvas = ToCanvas(origin, screen);
        g_canvas.zoomTarget = std::clamp(g_canvas.zoomTarget * factor, 0.3f, 1.8f);
    }

    // ---------------------------------------------------------------- кнопки
    float HoverAnim(ImGuiID id, bool on) {
        static std::unordered_map<ImGuiID, float> s_anim;
        float& t = s_anim[id];
        t += ((on ? 1.0f : 0.0f) - t) * Theme::Ease(ImGui::GetIO().DeltaTime, 12.0f);
        return t;
    }

    void FramePlate(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 fill, ImU32 edge, float rounding) {
        if (Theme::Frame(dl, a, b, fill, edge, rounding)) return; // «капли» и «стикеры»
        dl->AddRectFilled(a, b, fill, rounding);
        if (edge) dl->AddRect(a, b, edge, rounding, 0, Px(1.0f));
    }

    enum class Tone { Normal, Green, Accent };
    float ToolWidth(float h, const char* label) { return label ? Px(12) + Px(16) + Px(8) + Widgets::TextSize(Theme::semibold, Lang::Tr(label)).x + Px(14) : h; }
    // Кнопка полосы инструментов: значок и, если есть, подпись. Возвращает нажатие; ширину пишет в width
    bool Tool(const char* id, ImVec2 pos, float h, G glyph, const char* label, Tone tone = Tone::Normal, float* width = nullptr, bool enabled = true) {
        const char* shown = label ? Lang::Tr(label) : nullptr;
        const float w = ToolWidth(h, label);
        if (width) *width = w;
        ImGui::SetCursorScreenPos(pos);
        ImGui::BeginDisabled(!enabled);
        const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
        ImGui::EndDisabled();
        const float t = HoverAnim(ImGui::GetItemID(), ImGui::IsItemHovered() && enabled);
        const bool held = ImGui::IsItemActive();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 b(pos.x + w, pos.y + h);
        const Theme::Color green{ 110, 231, 183 };
        ImU32 fill = Theme::Overlay(static_cast<int>(16 * t) + (held ? 4 : 0)), edge = 0, ink = t > 0.5f ? Theme::kInk : Theme::kInkSoft; // в островке — без рамки, подсветка под мышью
        if (tone == Tone::Green) {
            fill = green.WithAlpha((Theme::Light() ? 0.30f : 0.16f) + 0.12f * t + (held ? 0.08f : 0.0f));
            edge = green.WithAlpha(0.45f + 0.3f * t);
            ink = Theme::Light() ? IM_COL32(20, 110, 70, 255) : green.Mix({ 255, 255, 255 }, 0.25f).WithAlpha(1.0f);
        } else if (tone == Tone::Accent) {
            fill = Theme::Accent().Mix({ 255, 255, 255 }, 0.1f * t).WithAlpha(held ? 0.8f : 0.95f);
            edge = Theme::Accent().Mix({ 255, 255, 255 }, 0.3f).WithAlpha(0.5f);
            ink = IM_COL32(255, 255, 255, 255);
        }
        if (!enabled) ink = Theme::kInkMuted;
        FramePlate(dl, pos, b, fill, edge, Px(8));
        const float iconX = shown ? pos.x + Px(12) + Px(8) : pos.x + w * 0.5f;
        Glyph(dl, glyph, ImVec2(iconX, pos.y + h * 0.5f), Px(15), ink);
        if (shown) Widgets::Text(dl, Theme::semibold, ImVec2(pos.x + Px(36), pos.y + (h - Theme::semibold->LegacySize) * 0.5f - Px(1)), ink, shown);
        if (ImGui::IsItemHovered() && enabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        return pressed && enabled;
    }

    // Строка меню: значок, текст, мягкое выделение (danger — красная)
    bool MenuRow(const char* id, G glyph, const char* label, bool on = false, bool danger = false) {
        const float w = ImGui::GetContentRegionAvail().x, h = Px(36);
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + h);
        const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 red = IM_COL32(251, 113, 133, 255);
        if (on) Theme::Pick(dl, a, b, Px(6), 1.0f, true);
        else if (hovered) dl->AddRectFilled(a, b, danger ? IM_COL32(251, 113, 133, 26) : Theme::Overlay(12), Px(6));
        const ImU32 ink = danger ? red : (hovered || on ? Theme::kInk : Theme::kInkSoft);
        Glyph(dl, glyph, ImVec2(a.x + Px(18), (a.y + b.y) * 0.5f), Px(14), ink);
        Widgets::Text(dl, Theme::semibold, ImVec2(a.x + Px(36), (a.y + b.y) * 0.5f - Theme::semibold->LegacySize * 0.55f), ink,
                      Fit(Theme::semibold, Theme::semibold->LegacySize, label, w - Px(44)).c_str());
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        return pressed;
    }

    void PushMenuStyle() {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(8), Px(8)));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, Px(16));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, Px(1));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Px(6), Px(2)));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, Theme::SurfaceSolid());
        ImGui::PushStyleColor(ImGuiCol_Border, Theme::Overlay(26));
    }
    void PopMenuStyle() { ImGui::PopStyleColor(2); ImGui::PopStyleVar(4); }

    // Ширину задаём, только если меню открыто: иначе она достанется следующему окну
    ImVec2 g_menuAt;
    void OpenMenu(const char* id) { // меню — прямо под кнопкой, а не там, где мышь
        g_menuAt = ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + Px(6));
        ImGui::OpenPopup(id);
    }

    bool BeginMenu(const char* id, float width) {
        if (!ImGui::IsPopupOpen(id)) return false;
        ImGui::SetNextWindowSize(ImVec2(width, 0));
        ImGui::SetNextWindowPos(ImVec2(std::min(g_menuAt.x, ImGui::GetIO().DisplaySize.x - width - Px(8)), g_menuAt.y));
        return Widgets::BeginPopupAnimated(id);
    }

    // ---------------------------------------------------------------- отмена и повтор
    void Remember() {
        if (g_historyAt + 1 < static_cast<int>(g_history.size())) g_history.resize(g_historyAt + 1);
        g_history.push_back(g_list);
        if (g_history.size() > 80) g_history.erase(g_history.begin());
        g_historyAt = static_cast<int>(g_history.size()) - 1;
    }

    void Restore(int at) { if (at >= 0 && at < static_cast<int>(g_history.size())) g_restoreTo = at; }

    void ApplyRestore() {
        const int at = g_restoreTo;
        g_restoreTo = -1;
        if (at < 0 || at >= static_cast<int>(g_history.size())) return;
        g_historyAt = at;
        g_list = g_history[at];
        g_selected = std::clamp(g_selected, 0, std::max(0, static_cast<int>(g_list.size()) - 1));
        if (g_canvas.selected >= 0 && (g_list.empty() || !FindBlock(g_list[g_selected], g_canvas.selected))) g_canvas.selected = -1;
        g_canvas.routeKey.clear();
        Scenarios::Save(g_list);
    }

    // ================================================================ «Блоки» — полоса под холстом
    struct Group { const char* name; std::vector<Step> steps; };

    const std::vector<Group>& Groups() {
        static const std::vector<Group> groups = {
            { "Триггеры", { Step::Phrase, Step::Hotkey } },
            { "Управление", { Step::Condition, Step::Loop, Step::Switch, Step::Wait, Step::SetVar } },
            { "Действия", { Step::Launch, Step::Place, Step::Say, Step::Command, Step::Volume, Step::Media, Step::Mute,
                            Step::Notify, Step::ChatReply, Step::Screenshot, Step::Play } },
        };
        return groups;
    }

    // Добавить блок кликом: правее выделенного и сразу соединить с ним, иначе — посреди видимого холста
    void AddBlock(Scenario& s, Step step, ImVec2 canvasSize, ImVec2* at = nullptr) {
        Block block = NewBlock(step);
        block.id = NextId(s);
        const Block* from = g_canvas.selected >= 0 ? FindBlock(s, g_canvas.selected) : nullptr;
        if (at) block.x = at->x, block.y = at->y;
        else if (from) block.x = from->x + kW + kGapX, block.y = from->y;
        else {
            const ImVec2 c = ToCanvas(ImVec2(0, 0), ImVec2(canvasSize.x * 0.5f, canvasSize.y * 0.45f));
            block.x = c.x - kW * 0.5f, block.y = c.y - kH * 0.5f;
        }
        block.x = std::round(block.x / kGrid) * kGrid, block.y = std::round(block.y / kGrid) * kGrid;
        if (from && !Scenarios::IsTrigger(step)) Scenarios::Port(*FindBlock(s, from->id), 0).push_back(block.id);
        s.blocks.push_back(block);
        g_canvas.selected = block.id;
        g_dirty = true;
    }

    // Блоки — колонкой слева от холста: группы сверху вниз, колесо листает список
    void DrawBlocksBar(Scenario& s, ImVec2 a, ImVec2 b, ImVec2 canvasSize) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        Theme::Plate(dl, a, b, Theme::Surface(0.92f), Px(18));
        const float pad = Px(14);
        Widgets::Text(dl, Theme::title, ImVec2(a.x + pad + Px(2), a.y + Px(10)), Theme::kInk, "Блоки");
        Widgets::Text(dl, Theme::semibold, ImVec2(a.x + pad + Px(2), a.y + Px(38)), Theme::kInkMuted, "нажми или перетащи");

        // поиск на всю ширину колонки
        const float sw = b.x - a.x - pad * 2;
        ImGui::SetCursorScreenPos(ImVec2(a.x + pad, a.y + Px(64)));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Px(30), Px(5)));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Px(10));
        ImGui::SetNextItemWidth(sw);
        ImGui::InputTextWithHint("##blockSearch", L("найти блок…", "find a block…"), g_search.data(), g_search.capacity() + 1, ImGuiInputTextFlags_CallbackResize, ResizeString, &g_search);
        ImGui::PopStyleVar(2);
        Glyph(dl, G::Search, ImVec2(a.x + pad + Px(15), (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f), Px(13), Theme::kInkMuted);

        const float top = ImGui::GetItemRectMax().y + Px(10);
        ImGui::SetCursorScreenPos(ImVec2(a.x + Px(8), top));
        ImGui::BeginChild("##blocks", ImVec2(b.x - a.x - Px(16), b.y - top - Px(8)), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        const std::wstring filter = Text::LowerRu(Text::ToWide(Text::Trim(g_search)));
        const float h = Px(36), w = ImGui::GetContentRegionAvail().x - Px(4);
        const float x = ImGui::GetCursorScreenPos().x + Px(2);
        float y = ImGui::GetCursorScreenPos().y;
        for (const auto& group : Groups()) {
            bool first = true;
            for (Step step : group.steps) {
                const char* name = Scenarios::StepName(step);
                if (!filter.empty() && Text::LowerRu(Text::ToWide(Lang::Tr(name))).find(filter) == std::wstring::npos) continue;
                if (first) { // подпись группы над её блоками
                    Widgets::Text(cdl, Theme::semibold, ImVec2(x + Px(4), y + Px(4)), Theme::kInkMuted, Lang::Tr(group.name));
                    y += Px(28);
                    first = false;
                }
                const ImVec2 p0(x, y), p1(x + w, y + h);
                ImGui::SetCursorScreenPos(p0);
                ImGui::PushID(static_cast<int>(step));
                const bool pressed = ImGui::InvisibleButton("##block", ImVec2(w, h));
                const bool hovered = ImGui::IsItemHovered();
                if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, Px(5))) g_canvas.newStep = static_cast<int>(step);
                const float t = HoverAnim(ImGui::GetItemID(), hovered);
                ImGui::PopID();
                const Theme::Color color = StepColor(step);
                FramePlate(cdl, p0, p1, Theme::Overlay(8 + static_cast<int>(12 * t)), Theme::Overlay(18 + static_cast<int>(16 * t)), Px(10));
                { // маленькая плитка цвета блока со значком
                    const ImVec2 c(p0.x + Px(19), (p0.y + p1.y) * 0.5f);
                    cdl->AddRectFilled(ImVec2(c.x - Px(11), c.y - Px(11)), ImVec2(c.x + Px(11), c.y + Px(11)), color.WithAlpha(0.85f + 0.15f * t), Px(7));
                    Glyph(cdl, StepGlyph(step), c, Px(11), IM_COL32(255, 255, 255, 245));
                }
                cdl->PushClipRect(p0, ImVec2(p1.x - Px(6), p1.y), true);
                Widgets::Text(cdl, Theme::semibold, ImVec2(p0.x + Px(38), (p0.y + p1.y) * 0.5f - Theme::semibold->LegacySize * 0.55f), Theme::kInk, name);
                cdl->PopClipRect();
                if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (pressed && g_canvas.newStep < 0) AddBlock(s, step, canvasSize);
                y += h + Px(6);
            }
            if (!first) y += Px(8);
        }
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::Dummy(ImVec2(w, Px(4)));
        ImGui::EndChild();
    }

    // ================================================================ настройки выделенного блока
    void DrawInspector(Scenario& s, ImVec2 a, ImVec2 b) {
        Block* block = FindBlock(s, g_canvas.selected);
        if (!block) return;
        Theme::Plate(ImGui::GetWindowDrawList(), a, b, Theme::Surface(0.97f), Px(18), 0.3f);
        // вся панель — своё окошко: карточки под ней не перехватывают мышь
        ImGui::SetCursorScreenPos(a);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::BeginChild("##inspectorPanel", ImVec2(b.x - a.x, b.y - a.y), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const Theme::Color color = ColorOf(*block);
        const float pad = Px(14);
        const ImVec2 head(a.x + pad, a.y + pad);
        dl->AddRectFilled(head, ImVec2(head.x + Px(34), head.y + Px(34)), color.WithAlpha(0.18f), Px(10));
        Glyph(dl, StepGlyph(block->step), ImVec2(head.x + Px(17), head.y + Px(17)), Px(17), color.WithAlpha(1.0f));
        const float textW = b.x - head.x - Px(44) - Px(40);
        Widgets::Text(dl, Theme::title, ImVec2(head.x + Px(44), head.y - Px(1)), Theme::kInk,
                      Fit(Theme::title, Theme::title->LegacySize, block->title.empty() ? Scenarios::StepName(block->step) : block->title, textW).c_str());
        Widgets::Text(dl, Theme::semibold, ImVec2(head.x + Px(44), head.y + Px(19)), Theme::kInkMuted,
                      Fit(Theme::semibold, Theme::semibold->LegacySize, !block->title.empty() ? Lang::Tr(Scenarios::StepName(block->step))
                          : Scenarios::IsTrigger(block->step) ? L("триггер — с него начинается", "trigger: the scenario starts here")
                          : block->step == Step::Condition || block->step == Step::Loop || block->step == Step::Switch || block->step == Step::Wait ? L("управление", "control")
                          : L("действие", "action"), textW).c_str());
        if (Tool("##inspectorClose", ImVec2(b.x - pad - Px(30), a.y + pad), Px(30), G::Close, nullptr)) { g_canvas.selected = -1; ImGui::EndChild(); return; }

        ImGui::SetCursorScreenPos(ImVec2(a.x + Px(4), a.y + pad + Px(46)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad - Px(4), Px(4)));
        ImGui::BeginChild("##inspector", ImVec2(b.x - a.x - Px(8), b.y - a.y - pad - Px(52)), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
        Widgets::SmoothScroll();
        const float width = ImGui::GetContentRegionAvail().x;
        ImGui::PushID(block->id);
        BlockFields(*block, width);
        ImGui::Dummy(ImVec2(0, Px(6)));
        Label("своё название");
        InputString("##title", block->title, Scenarios::StepName(block->step), width);
        Label("цвет");
        static const int kColors[] = { -1, 0x7DAFF5, 0x6ED2B4, 0xFAA57D, 0xF5CD5F, 0xF472B6, 0xA78BFA, 0x86D796 };
        const float dot = std::min(Px(26), (width - Px(6) * 7) / 8.0f);
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        for (int i = 0; i < IM_ARRAYSIZE(kColors); ++i) {
            if (i) ImGui::SameLine(0, Px(6));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##color", ImVec2(dot, dot))) { block->color = kColors[i]; g_dirty = true; }
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            const ImVec2 c(p.x + dot * 0.5f, p.y + dot * 0.5f);
            const ImU32 fill = kColors[i] < 0 ? StepColor(block->step).WithAlpha(1.0f) : Theme::FromRgb(kColors[i]).WithAlpha(1.0f);
            cdl->AddCircleFilled(c, dot * (hovered ? 0.38f : 0.32f), fill, 24);
            if (kColors[i] < 0) cdl->AddCircle(c, dot * 0.15f, Theme::Overlay(160), 16, Px(1.2f));
            if (block->color == kColors[i]) cdl->AddCircle(c, dot * 0.48f, Theme::Overlay(230), 24, Px(1.6f));
            if (hovered) ImGui::SetTooltip("%s", kColors[i] < 0 ? L("цвет по типу блока", "colour by block type") : L("свой цвет", "custom colour"));
        }
        ImGui::Dummy(ImVec2(0, Px(8)));
        if (!block->shape.empty() && MenuRow("##outline", G::Pencil, L("рисованный контур", "hand-drawn outline"), block->outline)) { block->outline = !block->outline; g_dirty = true; }
        if (MenuRow("##copy", G::Plus, "Копия блока")) {
            Block copy = *block;
            copy.id = NextId(s);
            copy.x += 40, copy.y += 40;
            copy.next.clear();
            copy.alt.clear();
            s.blocks.push_back(copy);
            g_canvas.selected = copy.id;
            g_dirty = true;
        } else if (MenuRow("##unlink", G::XCircle, "Убрать стрелки из блока")) {
            block->next.clear();
            block->alt.clear();
            g_dirty = true;
        } else if (MenuRow("##delete", G::XCircle, "Удалить блок", false, true)) {
            DeleteBlock(s, block->id);
        }
        ImGui::Dummy(ImVec2(0, Px(6)));
        Note("{ответ} — что вернул прошлый блок; ещё есть {время}, {дата}, {буфер} и свои переменные — {имя}");
        ImGui::Dummy(ImVec2(0, Px(8)));
        ImGui::PopID();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::EndChild();
    }

    void DrawNode(ImDrawList* dl, ImVec2 origin, Block& b, bool hovered, int hotPort, bool linkTarget) {
        const float z = Z();
        float& shown = g_canvas.appear.try_emplace(b.id, 0.0f).first->second;
        shown += (1.0f - shown) * Theme::Ease(ImGui::GetIO().DeltaTime, 6.2f);
        const float appear = 1.0f - std::pow(1.0f - shown, 3.0f);
        const ImVec2 a = ToScreen(origin, ImVec2(b.x, b.y));
        const float w = kW * z, h = NodeH(b) * z;
        const ImVec2 m(a.x + w, a.y + h);
        const bool selected = g_canvas.selected == b.id;
        const Theme::Color color = BodyColor(b);
        const int ports = Scenarios::PortCount(b);

        BlockBody(dl, b, a, m, selected, linkTarget || hovered, appear); // карточка — по выбранному виду холста
        dl->PushClipRect(a, m, true); // ничего не вылезает за карточку
        const bool labelsLeft = ports > 1 && OutLeft(b);
        const float labels = ports > 1 && !Vertical() ? 64 * z : 0.0f; // подписи выходов — у той стороны, где выходы (лентой вниз — над точками)
        // значок — просто линии цвета блока (как у Astra), без плитки; рядом жирное название, под ним описание моноширинным
        const float drawnInset = !b.shape.empty() && b.outline ? 30 * z : 0.0f; // рисованный контур скруглён — текст подальше от края
        const float left = a.x + (labelsLeft ? labels : 0.0f) + 22 * z + drawnInset;
        ImFont* const titleFont = s_linkStyle == kQuiet ? Theme::mono : Theme::title; // «Минимализм вниз» — моноширинный заголовок
        const float titleSize = (s_linkStyle == kQuiet ? Theme::mono->LegacySize * 1.25f : Theme::title->LegacySize * 1.05f) * g_canvas.zoom, descSize = Theme::mono->LegacySize * 0.95f * g_canvas.zoom;
        const float titleY = a.y + 20 * z;
        if (s_linkStyle == kQuiet) { // значок — на тихой плитке своего цвета: глаз сразу находит тип блока
            const ImVec2 c(left + 11 * z, titleY + titleSize * 0.5f);
            dl->AddRectFilled(ImVec2(c.x - 15 * z, c.y - 15 * z), ImVec2(c.x + 15 * z, c.y + 15 * z), color.WithAlpha(0.16f * appear), 9 * z);
        }
        Glyph(dl, StepGlyph(b.step), ImVec2(left + 11 * z, titleY + titleSize * 0.5f), (s_linkStyle == kQuiet ? 16 : 18) * z, color.Mix(Theme::Light() ? Theme::Color{ 40, 30, 50 } : Theme::Color{ 255, 255, 255 }, 0.15f).WithAlpha(appear));
        const float textX = left + (s_linkStyle == kQuiet ? 36 : 32) * z, textW = m.x - textX - 18 * z - drawnInset - (labelsLeft ? 0.0f : labels);
        const std::string shownTitle = Fit(titleFont, titleSize, Lang::Tr(b.title.empty() ? Scenarios::StepName(b.step) : b.title.c_str()), textW);
        dl->AddText(titleFont, titleSize, ImVec2(textX, titleY), Theme::kInk, shownTitle.c_str());
        if (s_linkStyle == kQuiet) dl->AddText(titleFont, titleSize, ImVec2(textX + std::max(0.6f, 0.7f * z), titleY), Theme::kInk, shownTitle.c_str()); // моноширинный «жирный»
        const std::string described = Describe(b);
        dl->AddText(Theme::mono, descSize, ImVec2(left, titleY + titleSize + 12 * z), described.empty() ? Theme::Overlay(90) : Theme::kInkMuted,
                    Fit(Theme::mono, descSize, described.empty() ? L("не настроено", "not set up") : described, m.x - left - 18 * z - drawnInset - (labelsLeft ? 0.0f : labels)).c_str());
        if (ports > 1)
            for (int p = 0; p < ports; ++p) {
                const float size = Theme::semibold->LegacySize * 0.88f * g_canvas.zoom;
                const std::string label = Fit(Theme::semibold, size, PortLabel(b, p), Vertical() ? kW * z / (ports + 1) - 6 * z : labels - 8 * z);
                const ImVec2 t = Theme::semibold->CalcTextSizeA(size, FLT_MAX, 0, label.c_str());
                const ImVec2 port = ToScreen(origin, OutPortC(b, p));
                const ImVec2 at = Vertical() ? ImVec2(port.x - t.x * 0.5f, port.y - t.y - 7 * z)
                                             : ImVec2(labelsLeft ? a.x + 14 * z : m.x - 14 * z - t.x, port.y - t.y * 0.5f);
                dl->AddText(Theme::semibold, size, at, Theme::kInkMuted, label.c_str());
            }
        dl->PopClipRect();

        if (!Scenarios::IsTrigger(b.step)) { // вход виден всегда; тянут стрелку сюда — кольцо цвета темы
            const ImVec2 in = ToScreen(origin, InPortC(b));
            DrawPort(dl, in, linkTarget ? Theme::Accent() : InColor(b), true, false);
            if (linkTarget) dl->AddCircle(in, Px(11) * g_canvas.zoom, Theme::Accent().WithAlpha(0.85f), 24, Px(2));
        }
        for (int p = 0; p < ports; ++p) {
            DrawPort(dl, ToScreen(origin, OutPortC(b, p)), PortColor(b, p), true, hotPort == p && !g_hotMirror);
            if (!Vertical() && (hovered || selected || hotPort == p)) { // вторая ручка — с другой стороны: тяни оттуда, и выход переедет
                ImVec2 c = ToScreen(origin, OutPortC(b, p));
                c.x = OutLeft(b) ? m.x : a.x;
                if (hotPort == p && g_hotMirror) DrawPort(dl, c, PortColor(b, p), false, true);
                else dl->AddCircle(c, Px(5) * g_canvas.zoom, PortColor(b, p).WithAlpha(0.6f), 16, Px(1.2f));
            }
        }
    }

    // ================================================================ «Рисую сама»
    // Обвела фигуру мышью — она становится блоком (сразу спрашиваем, какое это действие); провела линию
    // от блока к блоку — связь в текущем виде. Неудачный штрих (не замкнут, слишком мал) тает
    struct Stroke { std::vector<ImVec2> pts; float life = 1.0f; };
    struct DrawMode {
        bool on = false;
        bool drawing = false;
        std::vector<ImVec2> stroke;   // точки на холсте
        std::vector<Stroke> fading;   // тают
        std::vector<ImVec2> pending;  // замкнутая фигура ждёт выбора действия
        int kind = 0;                 // 0 каракуль, 1 круг/овал, 2 прямоугольник
        bool openPick = false;
    };
    DrawMode g_draw;
    int g_keepEscFrame = -10; // в этом кадре Esc нужен рисованию или меню — страницу не закрывать

    void Fade(std::vector<ImVec2>& pts, float life = 1.0f) {
        if (pts.size() > 1) g_draw.fading.push_back({ pts, life });
        pts.clear();
    }
    void SetDraw(bool on) {
        g_draw.on = on;
        g_draw.drawing = false;
        Fade(g_draw.stroke);
        Fade(g_draw.pending);
        g_canvas.linkFrom = g_canvas.dragNode = -1;
    }
    void UndoStroke() { // ждёт выбора — убрать фигуру; иначе отменить последнее изменение (блок или связь из штриха)
        if (!g_draw.pending.empty()) Fade(g_draw.pending);
        else Restore(g_historyAt - 1);
    }

    float Length(const std::vector<ImVec2>& p) {
        float total = 0.0f;
        for (size_t i = 1; i < p.size(); ++i) total += std::hypot(p[i].x - p[i - 1].x, p[i].y - p[i - 1].y);
        return total;
    }
    void Bounds(const std::vector<ImVec2>& p, ImVec2& lo, ImVec2& hi) {
        lo = ImVec2(1e9f, 1e9f), hi = ImVec2(-1e9f, -1e9f);
        for (const ImVec2 q : p) lo = ImMin(lo, q), hi = ImMax(hi, q);
    }
    // круг, прямоугольник или каракуль: насколько фигура заполняет свою рамку и насколько она «круглая»
    int Classify(const std::vector<ImVec2>& p, ImVec2 lo, ImVec2 hi) {
        float area = 0.0f;
        for (size_t i = 0; i < p.size(); ++i) {
            const ImVec2 a = p[i], b = p[(i + 1) % p.size()];
            area += a.x * b.y - b.x * a.y;
        }
        area = std::fabs(area) * 0.5f;
        const float per = Length(p) + std::hypot(p.back().x - p.front().x, p.back().y - p.front().y);
        const float filled = area / std::max(1.0f, (hi.x - lo.x) * (hi.y - lo.y)); // прямоугольник ≈ 1, овал ≈ 0,79
        const float round = 4.0f * 3.1416f * area / std::max(1.0f, per * per);    // круг ≈ 1, квадрат ≈ 0,79
        if (filled > 0.86f) return 2;
        if (filled > 0.64f && round > 0.5f) return 1;
        return 0;
    }
    // контур в доли рамки: ровный шаг по длине, лёгкое сглаживание дрожи руки
    std::vector<float> NormalizeShape(const std::vector<ImVec2>& in, ImVec2 lo, ImVec2 hi) {
        std::vector<ImVec2> ring = in;
        ring.push_back(in.front());
        constexpr int kCount = 64;
        std::vector<ImVec2> p(kCount);
        for (int i = 0; i < kCount; ++i) p[i] = PointAt(ring, static_cast<float>(i) / kCount);
        std::vector<ImVec2> smooth(kCount);
        for (int i = 0; i < kCount; ++i) {
            const ImVec2 a = p[(i + kCount - 1) % kCount], b = p[i], c = p[(i + 1) % kCount];
            smooth[i] = ImVec2((a.x + b.x * 2 + c.x) * 0.25f, (a.y + b.y * 2 + c.y) * 0.25f);
        }
        ImVec2 l, h;
        Bounds(smooth, l, h);
        (void)lo, (void)hi;
        std::vector<float> out;
        for (const ImVec2 q : smooth) {
            out.push_back(std::clamp((q.x - l.x) / std::max(1.0f, h.x - l.x), 0.0f, 1.0f));
            out.push_back(std::clamp((q.y - l.y) / std::max(1.0f, h.y - l.y), 0.0f, 1.0f));
        }
        return out;
    }

    Block* BlockAt(Scenario& s, ImVec2 p, float pad) {
        for (auto it = s.blocks.rbegin(); it != s.blocks.rend(); ++it)
            if (p.x >= it->x - pad && p.x <= it->x + kW + pad && p.y >= it->y - pad && p.y <= it->y + NodeH(*it) + pad) return &*it;
        return nullptr;
    }

    void FinishStroke(Scenario& s) {
        std::vector<ImVec2> pts = std::move(g_draw.stroke);
        g_draw.stroke.clear();
        if (pts.size() < 4 || Length(pts) < 24.0f) { Fade(pts); return; }
        Block* from = BlockAt(s, pts.front(), 16.0f);
        Block* to = BlockAt(s, pts.back(), 16.0f);
        if (from && to && from != to) { // линия от блока к блоку — связь
            if (Scenarios::IsTrigger(to->step)) { Fade(pts); return; }
            const int ports = Scenarios::PortCount(*from);
            int port = 0;
            for (int p = 1; p < ports; ++p) {
                const ImVec2 c = OutPortC(*from, p), best = OutPortC(*from, port);
                if (std::hypot(c.x - pts.front().x, c.y - pts.front().y) < std::hypot(best.x - pts.front().x, best.y - pts.front().y)) port = p;
            }
            // начали у левого края — выходы слева; закончили у правого края — вход справа
            const int fcur = from->sides >= 0 ? from->sides : (InRight(*from) ? 1 : 0) | (OutLeft(*from) ? 2 : 0);
            const int tcur = to->sides >= 0 ? to->sides : (InRight(*to) ? 1 : 0) | (OutLeft(*to) ? 2 : 0);
            if (!Vertical()) {
                if (pts.front().x < from->x + kW * 0.25f) from->sides = fcur | 2;
                else if (pts.front().x > from->x + kW * 0.75f) from->sides = fcur & ~2;
                if (pts.back().x > to->x + kW * 0.75f) to->sides = tcur | 1;
                else if (pts.back().x < to->x + kW * 0.25f) to->sides = tcur & ~1;
            }
            Link(s, from->id, port, to->id);
            g_canvas.routeKey.clear();
            Fade(pts, 0.55f); // штрих растворяется — на его месте линия
            return;
        }
        ImVec2 lo, hi;
        Bounds(pts, lo, hi);
        const float w = hi.x - lo.x, h = hi.y - lo.y;
        const float gap = std::hypot(pts.back().x - pts.front().x, pts.back().y - pts.front().y);
        const bool closed = gap <= std::max(30.0f, 0.3f * std::min(w, h)) && Length(pts) >= 1.3f * (w + h);
        if (closed && w >= 50.0f && h >= 30.0f) { // полноценная фигура — спросим, какой это блок
            g_draw.pending = pts;
            g_draw.kind = Classify(pts, lo, hi);
            g_draw.openPick = true;
            return;
        }
        Fade(pts);
    }

    void DrawInput(Scenario& s, ImVec2 origin, bool start) {
        const ImGuiIO& io = ImGui::GetIO();
        const ImVec2 m = ToCanvas(origin, io.MousePos);
        if (start && g_draw.pending.empty()) { g_draw.drawing = true; g_draw.stroke = { m }; }
        if (!g_draw.drawing) return;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const ImVec2 last = g_draw.stroke.back();
            if (std::hypot(m.x - last.x, m.y - last.y) > 2.5f / std::max(0.2f, g_canvas.zoom)) g_draw.stroke.push_back(m);
        } else {
            g_draw.drawing = false;
            FinishStroke(s);
        }
    }

    void CreateDrawn(Scenario& s, Step step, ImVec2 canvasSize) {
        ImVec2 lo, hi;
        Bounds(g_draw.pending, lo, hi);
        const Block probe = NewBlock(step);
        ImVec2 at((lo.x + hi.x) * 0.5f - kW * 0.5f, (lo.y + hi.y) * 0.5f - NodeH(probe) * 0.5f);
        g_canvas.selected = -1; // не цеплять к выделенному — связи рисуют сами
        AddBlock(s, step, canvasSize, &at);
        Block& b = s.blocks.back();
        b.shape = NormalizeShape(g_draw.pending, lo, hi);
        b.outline = g_draw.kind != 2; // прямоугольник становится обычной карточкой вида, круг и каракуль — своей формой
        g_canvas.appear[b.id] = 0.35f;
        g_draw.pending.clear();
    }

    // карандашный штрих: широкое свечение цвета темы и яркая середина
    void Pencil(ImDrawList* dl, ImVec2 origin, const std::vector<ImVec2>& cpts, float alpha, bool closed) {
        if (cpts.size() < 2 || alpha <= 0.0f) return;
        std::vector<ImVec2> pts;
        for (const ImVec2 q : cpts) pts.push_back(ToScreen(origin, q));
        if (closed) { pts.push_back(pts[0]); pts.push_back(pts[1]); }
        const float z = Px(1.0f) * std::max(0.7f, g_canvas.zoom);
        const Theme::Color c = Theme::Accent();
        Strip(dl, pts, 11.0f * z, 11.0f * z, 5.5f * z, Solid(c.WithAlpha(0.14f * alpha)));
        Strip(dl, pts, 2.6f * z, 2.6f * z, std::max(1.0f, z), Solid(c.Mix({ 255, 255, 255 }, 0.25f).WithAlpha(0.95f * alpha)));
    }

    void DrawStrokes(ImDrawList* dl, ImVec2 origin) {
        const float dt = ImGui::GetIO().DeltaTime;
        for (auto& f : g_draw.fading) { Pencil(dl, origin, f.pts, f.life, false); f.life -= dt * 2.0f; }
        g_draw.fading.erase(std::remove_if(g_draw.fading.begin(), g_draw.fading.end(), [](const Stroke& f) { return f.life <= 0.0f; }), g_draw.fading.end());
        if (!g_draw.pending.empty()) Pencil(dl, origin, g_draw.pending, 0.75f + 0.25f * std::sin(static_cast<float>(ImGui::GetTime()) * 4.0f), true);
        if (g_draw.stroke.size() > 1) {
            Pencil(dl, origin, g_draw.stroke, 1.0f, false);
            if (g_draw.stroke.size() > 10) { // кольцо у начала: доведи сюда — фигура замкнётся
                const ImVec2 a = ToScreen(origin, g_draw.stroke.front()), m = ImGui::GetIO().MousePos;
                const bool closeBy = std::hypot(m.x - a.x, m.y - a.y) < 30.0f * Z();
                dl->AddCircle(a, Px(closeBy ? 11.0f : 8.0f) * std::max(0.7f, g_canvas.zoom), Theme::Accent().WithAlpha(closeBy ? 0.95f : 0.45f), 24, Px(closeBy ? 2.2f : 1.4f));
            }
        }
    }

    // плашка над холстом в режиме рисования: что делать и видимые кнопки «отменить штрих» и «готово»
    void DrawHintBar(ImVec2 cmin, ImVec2 cmax) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const char* text = L("обведи фигуру — будет блок · линия от блока к блоку — связь", "draw a shape to add a block · a line between blocks links them");
        const char* undo = L("отменить штрих", "undo stroke");
        const char* done = L("готово", "done");
        const float h = Px(34), pad = Px(6);
        const float tw = Widgets::TextSize(Theme::semibold, text).x;
        const float w = Px(16) + Px(20) + tw + Px(14) + ToolWidth(h, undo) + Px(6) + ToolWidth(h, done) + pad;
        const float x0 = std::max(cmin.x + Px(12), (cmin.x + cmax.x - w) * 0.5f), y0 = cmin.y + Px(12);
        Theme::Plate(dl, ImVec2(x0, y0), ImVec2(x0 + w, y0 + h + pad * 2), Theme::Surface(0.95f), Px(14), 0.6f);
        Glyph(dl, G::Pencil, ImVec2(x0 + Px(24), y0 + pad + h * 0.5f), Px(15), Theme::Accent().WithAlpha(1.0f));
        Widgets::Text(dl, Theme::semibold, ImVec2(x0 + Px(40), y0 + pad + (h - Theme::semibold->LegacySize) * 0.5f - Px(1)), Theme::kInk, text);
        float x = x0 + Px(40) + tw + Px(14), bw = 0;
        if (Tool("##undoStroke", ImVec2(x, y0 + pad), h, G::Undo, undo, Tone::Normal, &bw)) UndoStroke();
        x += bw + Px(6);
        if (Tool("##drawDone", ImVec2(x, y0 + pad), h, G::Close, done, Tone::Accent)) SetDraw(false);
    }

    // выбор действия для нарисованной фигуры — те же блоки, что в панели «Блоки»
    void DrawPickMenu(Scenario& s, ImVec2 canvasSize) {
        if (g_draw.pending.empty()) return;
        const ImGuiIO& io = ImGui::GetIO();
        if (g_draw.openPick) {
            g_draw.openPick = false;
            const float estimate = Px(12 * 37 + 60); // две колонки: управление слева, действия справа
            g_menuAt = ImVec2(io.MousePos.x + Px(14), std::clamp(io.MousePos.y - Px(40), Px(20), std::max(Px(20), io.DisplaySize.y - estimate - Px(10))));
            ImGui::OpenPopup("##drawPick");
        }
        PushMenuStyle();
        bool shown = false;
        if (BeginMenu("##drawPick", Px(540))) {
            shown = true;
            const char* kinds[] = { L("Каракуль", "A doodle"), L("Круг", "A circle"), L("Прямоугольник", "A rectangle") };
            const std::string head = std::string(kinds[std::clamp(g_draw.kind, 0, 2)]) + L(" — какой это блок?", ": which block is it?");
            ImGui::PushFont(Theme::title, Theme::title->LegacySize);
            ImGui::TextUnformatted(head.c_str());
            ImGui::PopFont();
            if (ImGui::BeginTable("##drawPickCols", 2, ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupColumn("##c0", ImGuiTableColumnFlags_WidthFixed, Px(255));
                ImGui::TableSetupColumn("##c1", ImGuiTableColumnFlags_WidthFixed, Px(255));
                ImGui::TableNextColumn();
                const auto& groups = Groups();
                for (size_t gi = 0; gi < groups.size(); ++gi) {
                    if (gi + 1 == groups.size()) ImGui::TableNextColumn(); // действия — отдельной колонкой
                    Note(groups[gi].name);
                    for (const Step st : groups[gi].steps) {
                        ImGui::PushID(static_cast<int>(st));
                        if (MenuRow("##drawStep", StepGlyph(st), Scenarios::StepName(st))) { CreateDrawn(s, st, canvasSize); ImGui::CloseCurrentPopup(); }
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }
            Widgets::EndPopupAnimated();
        }
        PopMenuStyle();
        if (!shown && !ImGui::IsPopupOpen("##drawPick")) Fade(g_draw.pending); // закрыли, не выбрав — фигура тает
    }

    // ================================================================ полоса инструментов
    // Строка над холстом: какой сценарий, включён ли, проверить, вид линий и правка схемы
    void DrawToolbar(Scenario& s, ImVec2 a0, ImVec2 b0, ImVec2 canvasSize) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        Theme::Plate(dl, a0, b0, Theme::Surface(0.92f), Px(14)); // вся полоса — один плавающий островок
        const float inset = Px(4);
        const ImVec2 a(a0.x + inset, a0.y + inset), b(b0.x - inset, b0.y - inset);
        const float h = b.y - a.y, y = a.y;
        float w = 0;

        // справа налево: настройки сценария, повтор, отмена, раскладка, линии
        float right = b.x;
        right -= h;
        if (Tool("##gear", ImVec2(right, y), h, G::Gear, nullptr)) OpenMenu("##scenarioGear");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("ещё", "more"));
        right -= Px(6) + h;
        if (Tool("##redo", ImVec2(right, y), h, G::Redo, nullptr, Tone::Normal, &w, g_historyAt + 1 < static_cast<int>(g_history.size()))) Restore(g_historyAt + 1);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("повторить (Ctrl+Y)", "redo (Ctrl+Y)"));
        right -= Px(6) + h;
        if (Tool("##undo", ImVec2(right, y), h, G::Undo, nullptr, Tone::Normal, &w, g_historyAt > 0)) Restore(g_historyAt - 1);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("отменить (Ctrl+Z)", "undo (Ctrl+Z)"));
        right -= Px(6) + h;
        if (Tool("##layout", ImVec2(right, y), h, G::Layout, nullptr)) AutoLayout(s);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("разложить блоки ровно", "tidy up the blocks"));
        LoadLineLook();
        const char* kLook[kLineStyles] = { L("вид: плавные кривые", "look: smooth curves"), L("вид: минимализм", "look: minimal"),
                                           L("вид: изломы 90°", "look: right angles"), L("вид: кибер-импульсы", "look: cyber pulses"), L("вид: минимализм вниз", "look: quiet flow") };
        const char* lines = kLook[s_linkStyle];
        right -= Px(10) + ToolWidth(h, lines);
        dl->AddLine(ImVec2(right + ToolWidth(h, lines) + Px(5), y + Px(8)), ImVec2(right + ToolWidth(h, lines) + Px(5), b.y - Px(8)), Theme::Overlay(20), Px(1)); // разделитель групп
        if (Tool("##lines", ImVec2(right, y), h, G::Lines, lines)) OpenMenu("##lineStyle");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("вид блоков и линий", "how blocks and lines look"));
        const char* drawLabel = L("рисую сама", "draw by hand");
        right -= Px(8) + ToolWidth(h, drawLabel);
        if (Tool("##draw", ImVec2(right, y), h, G::Pencil, drawLabel, g_draw.on ? Tone::Accent : Tone::Normal)) SetDraw(!g_draw.on);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", L("обведи фигуру мышью — получится блок; проведи линию от блока к блоку — связь",
                                      "draw a closed shape to make a block; draw a line from block to block to link them"));
        right -= Px(8) + ToolWidth(h, L("проверить", "test"));
        if (Tool("##run", ImVec2(right, y), h, G::Play, L("проверить", "test"), Tone::Accent))
            Scenarios::Run(s, g_canvas.selected >= 0 && FindBlock(s, g_canvas.selected) && Scenarios::IsTrigger(FindBlock(s, g_canvas.selected)->step) ? g_canvas.selected : -1);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("выполнить сценарий сейчас", "run the scenario now"));
        right -= Px(14) + Px(32);
        if (Widgets::Toggle("##enabled", ImVec2(right, y + (h - Px(18)) * 0.5f), &s.enabled, Theme::Accent().WithAlpha(1.0f))) g_dirty = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s.enabled ? L("сценарий включён", "scenario on") : L("сценарий выключен", "scenario off"));
        right -= Px(14);

        // слева: список сценариев и название во всю оставшуюся ширину
        float x = a.x;
        if (Tool("##scenarioList", ImVec2(x, y), h, G::List, nullptr)) OpenMenu("##scenarios");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("все сценарии", "all scenarios"));
        x += h + Px(8);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Px(10), (h - ImGui::GetFontSize()) * 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Px(8));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0)); // название — прямо на островке, поле видно под мышью
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Theme::Overlay(10));
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
        ImGui::PushFont(Theme::title, Theme::title->LegacySize);
        InputString("##name", s.name, "название сценария", std::max(Px(80), right - x));
        ImGui::PopFont();
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(2);
        (void)dl;

        PushMenuStyle();
        if (BeginMenu("##scenarios", Px(300))) {
            for (int i = 0; i < static_cast<int>(g_list.size()); ++i) {
                ImGui::PushID(i);
                if (MenuRow("##pick", G::Lines, g_list[i].name.c_str(), i == g_selected)) { g_selected = i; ImGui::CloseCurrentPopup(); }
                ImGui::PopID();
            }
            ImGui::Dummy(ImVec2(0, Px(4)));
            if (MenuRow("##new", G::Plus, "новый сценарий")) {
                Scenario fresh;
                fresh.hasLayout = true;
                Block trigger = NewBlock(Step::Phrase);
                trigger.id = 1;
                fresh.blocks.push_back(trigger); // каждому сценарию нужен триггер — сразу кладём фразу
                g_list.push_back(fresh);
                g_selected = static_cast<int>(g_list.size()) - 1;
                g_canvas.selected = 1;
                g_dirty = true;
                ImGui::CloseCurrentPopup();
            }
            Widgets::EndPopupAnimated();
        }
        if (BeginMenu("##lineStyle", Px(400))) {
            const char* kStyles[kLineStyles] = { L("Плавные кривые Безье", "Smooth Bezier curves"), L("Прямые по диагонали", "Straight diagonals"),
                                                 L("Ортогональные изломы", "Right-angle steps"), L("Кибер-импульсы", "Cyber pulses"), L("Минимализм вниз", "Quiet flow") };
            for (const int i : { kQuiet, kBezier, kMinimal, kOrtho, kCyber }) {
                ImGui::PushID(i);
                if (MenuRow("##style", G::Lines, kStyles[i], s_linkStyle == i)) { ScenariosPage::SetLook(i); ImGui::CloseCurrentPopup(); }
                const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
                LineSample(ImGui::GetWindowDrawList(), i, ImVec2(r1.x - Px(112), r1.y - Px(11)), ImVec2(r1.x - Px(18), r0.y + Px(11)));
                ImGui::PopID();
            }
            Note(L("блоки, порты и линии меняются вместе", "blocks, ports and lines change together"));
            ImGui::Dummy(ImVec2(0, Px(4)));
            Note(L("раскладка", "layout"));
            const char* kFlows[] = { L("лентой вниз", "top to bottom"), L("рядами змейкой", "rows, snake-like") };
            for (int i = 0; i < 2; ++i) {
                ImGui::PushID(200 + i);
                if (MenuRow("##flow", i == 0 ? G::List : G::Layout, kFlows[i], s_flow == i) && s_flow != i) {
                    s_flow = i;
                    Config::SetValue("scenarios", "flow", std::to_string(i));
                    AutoLayout(s);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopID();
            }
            Widgets::EndPopupAnimated();
        }
        if (BeginMenu("##scenarioGear", Px(280))) {
            if (MenuRow("##fit", G::Fit, "показать всё")) { FitCanvas(s, canvasSize); ImGui::CloseCurrentPopup(); }
            ImGui::Dummy(ImVec2(0, Px(4)));
            if (MenuRow("##remove", G::XCircle, "удалить сценарий", false, true)) {
                g_list.erase(g_list.begin() + g_selected);
                g_selected = std::max(0, g_selected - 1);
                g_canvas.selected = -1;
                g_canvas.lastScenario = -1;
                g_dirty = true;
                ImGui::CloseCurrentPopup();
            }
            Note("удалённое можно вернуть кнопкой «отменить»");
            Widgets::EndPopupAnimated();
        }
        PopMenuStyle();
    }

    // ================================================================ холст целиком
    void DrawCanvas(Scenario& s, ImVec2 cmin, ImVec2 cmax) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiIO& io = ImGui::GetIO();
        const ImVec2 size(cmax.x - cmin.x, cmax.y - cmin.y);
        const ImVec2 origin = cmin;
        if (g_canvas.lastScenario != g_selected) { // другой сценарий — показать его целиком
            g_canvas.lastScenario = g_selected;
            g_canvas.selected = g_canvas.linkFrom = g_canvas.dragNode = -1;
            g_canvas.appear.clear();
            g_canvas.routeKey.clear();
            g_canvas.fitPending = true;
        }
        if (g_canvas.fitPending) { FitCanvas(s, size); g_canvas.fitPending = false; }
        LoadLineLook();
        UpdateSides(s);
        UpdateRoutes(s);
        g_hotMirror = false;

        const bool inspector = g_canvas.selected >= 0 && FindBlock(s, g_canvas.selected);
        const float side = inspector ? std::min(Px(330), size.x * 0.42f) : 0.0f;
        const float zb = Px(32);
        const ImVec2 zoomMax(cmax.x - Px(12), cmax.y - Px(12)), zoomMin(zoomMax.x - zb * 3 - Px(12), zoomMax.y - zb - Px(8));

        // фон и точечная сетка
        dl->AddRectFilled(cmin, cmax, Theme::Light() ? IM_COL32(255, 255, 255, 60) : s_linkStyle == kQuiet ? IM_COL32(24, 14, 42, 205) : IM_COL32(0, 0, 0, 34), Px(16));
        Theme::Island(dl, cmin, cmax, Px(16));
        ImGui::PushClipRect(ImVec2(cmin.x + Px(1), cmin.y + Px(1)), ImVec2(cmax.x - Px(1), cmax.y - Px(1)), true);
        const float step = kGrid * Z() * (g_canvas.zoom < 0.6f ? 2.0f : 1.0f);
        for (float gx = std::fmod(g_canvas.pan.x, step) + (g_canvas.pan.x < 0 ? step : 0); gx < size.x; gx += step)
            for (float gy = std::fmod(g_canvas.pan.y, step) + (g_canvas.pan.y < 0 ? step : 0); gy < size.y; gy += step)
                dl->AddRectFilled(ImVec2(cmin.x + gx - Px(0.7f), cmin.y + gy - Px(0.7f)), ImVec2(cmin.x + gx + Px(0.7f), cmin.y + gy + Px(0.7f)), Theme::Overlay(20));

        // мышь: сначала то, что лежит поверх (масштаб), потом карточки, последним — пустой холст
        bool zoomIn = false, zoomOut = false, zoomFit = false;
        const G zoomGlyphs[] = { G::Minus, G::Plus, G::Fit };
        bool zoomHover[3] = {};
        for (int i = 0; i < 3; ++i) {
            ImGui::SetCursorScreenPos(ImVec2(zoomMin.x + Px(6) + i * zb, zoomMin.y + Px(4)));
            ImGui::PushID(i);
            const bool pressed = ImGui::InvisibleButton("##zoom", ImVec2(zb, zb));
            zoomHover[i] = ImGui::IsItemHovered();
            ImGui::PopID();
            if (pressed) (i == 0 ? zoomOut : i == 1 ? zoomIn : zoomFit) = true;
        }
        const char* zoomTips[] = { "отдалить", "приблизить", "вписать всё" };
        for (int i = 0; i < 3; ++i) if (zoomHover[i]) ImGui::SetTooltip("%s", Lang::Tr(zoomTips[i]));

        int hoveredNode = -1, hotNode = -1, hotPort = -1;
        std::vector<Block*> order;
        for (auto& b : s.blocks) if (b.id != g_canvas.selected) order.push_back(&b);
        if (Block* sel = FindBlock(s, g_canvas.selected)) order.push_back(sel); // выделенная — сверху
        ImDrawListSplitter layers; // плашка «Рисую сама» — поверх схемы, хотя её кнопки ловят мышь первыми
        layers.Split(dl, 2);
        layers.SetCurrentChannel(dl, 1);
        if (g_draw.on) DrawHintBar(cmin, cmax);
        layers.SetCurrentChannel(dl, 0);
        for (auto it = order.rbegin(); it != order.rend() && !g_draw.on; ++it) { // верхние карточки ловят мышь первыми; в рисовании весь холст — для штрихов
            Block& b = **it;
            ImGui::PushID(b.id);
            const float pr = Px(9) * g_canvas.zoom;
            for (int p = 0; p < Scenarios::PortCount(b); ++p)
                for (int side = 0; side < (Vertical() ? 1 : 2); ++side) { // выход можно тянуть и справа, и слева (лентой вниз — одна точка снизу)
                    ImVec2 c = ToScreen(origin, OutPortC(b, p));
                    if (side == 1) c.x = ToScreen(origin, ImVec2(OutLeft(b) ? b.x + kW : b.x, 0)).x; // зеркальная ручка
                    ImGui::SetCursorScreenPos(ImVec2(c.x - pr, c.y - pr));
                    ImGui::PushID(p * 2 + side);
                    ImGui::InvisibleButton("##out", ImVec2(pr * 2, pr * 2));
                    if (ImGui::IsItemHovered()) { hotNode = b.id; hotPort = p; g_hotMirror = side == 1; ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); }
                    if (ImGui::IsItemActivated()) {
                        g_canvas.linkFrom = b.id; g_canvas.linkPort = p;
                        if (side == 1) { // потянули с другой стороны — выходы блока переезжают туда
                            const int cur = b.sides >= 0 ? b.sides : (InRight(b) ? 1 : 0) | (OutLeft(b) ? 2 : 0);
                            b.sides = cur ^ 2;
                            g_side[b.id] = static_cast<unsigned char>(b.sides);
                            g_canvas.routeKey.clear();
                            g_dirty = true;
                        }
                    }
                    ImGui::PopID();
                }
            const ImVec2 a = ToScreen(origin, ImVec2(b.x, b.y));
            ImGui::SetCursorScreenPos(a);
            ImGui::InvisibleButton("##node", ImVec2(kW * Z(), NodeH(b) * Z()), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
            if (ImGui::IsItemHovered()) hoveredNode = b.id;
            if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                g_canvas.dragNode = b.id;
                g_canvas.selected = b.id;
                const ImVec2 m = ToCanvas(origin, io.MousePos);
                g_canvas.dragOffset = ImVec2(m.x - b.x, m.y - b.y);
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) g_canvas.selected = b.id;
            if (ImGui::IsItemHovered() && g_canvas.dragNode < 0 && g_canvas.linkFrom < 0) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            ImGui::PopID();
        }
        ImGui::SetCursorScreenPos(cmin);
        ImGui::InvisibleButton("##canvasBg", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
        const bool bgActive = ImGui::IsItemActive();
        const bool bgStart = ImGui::IsItemActivated();
        if (bgStart) g_canvas.selected = -1;
        if (g_draw.on || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) g_keepEscFrame = ImGui::GetFrameCount();
        if (g_draw.on) DrawInput(s, origin, bgStart && ImGui::IsMouseClicked(ImGuiMouseButton_Left));
        if (bgActive && !g_draw.drawing) { g_canvas.pan.x += io.MouseDelta.x; g_canvas.pan.y += io.MouseDelta.y; ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll); }
        const bool canvasHovered = ImGui::IsMouseHoveringRect(cmin, cmax) && ImGui::IsWindowHovered();

        // линии
        const float thick = Px(1.6f) * std::max(0.75f, g_canvas.zoom);
        int removeFrom = -1, removePort = 0, removeTo = -1;
        for (const auto& from : s.blocks)
            for (int p = 0; p < Scenarios::PortCount(from); ++p)
                for (int to : Scenarios::Port(from, p)) {
                    const Block* target = nullptr;
                    for (const auto& t : s.blocks) if (t.id == to) target = &t;
                    if (!target) continue;
                    std::vector<ImVec2> pts = LinkPoints(s, from, p, *target);
                    for (auto& q : pts) q = ToScreen(origin, q);
                    const bool hovered = !g_draw.on && canvasHovered && hoveredNode < 0 && hotNode < 0 && g_canvas.linkFrom < 0 && g_canvas.dragNode < 0 && DistanceToPolyline(pts, io.MousePos) < Px(7);
                    const Theme::Color red{ 255, 120, 120 };
                    DrawLink(dl, pts, hovered ? red : PortColor(from, p), hovered ? red : (s_linkStyle == kOrtho ? PortColor(from, p) : InColor(*target)), 1.0f, hovered ? thick * 1.35f : thick, !hovered, s_linkStyle, hovered);
                    if (hovered) {
                        ImGui::SetTooltip("%s", L("правый клик — убрать стрелку", "right-click to remove the arrow"));
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) removeFrom = from.id, removePort = p, removeTo = to;
                    }
                }
        if (removeFrom >= 0) {
            auto& v = Scenarios::Port(*FindBlock(s, removeFrom), removePort);
            v.erase(std::remove(v.begin(), v.end(), removeTo), v.end());
            g_dirty = true;
        }

        // карточки
        int linkTarget = -1;
        if (g_canvas.linkFrom >= 0)
            for (const Block* b : order) {
                const ImVec2 a = ToScreen(origin, ImVec2(b->x, b->y));
                if (b->id != g_canvas.linkFrom && !Scenarios::IsTrigger(b->step) &&
                    ImRect(ImVec2(a.x - Px(10), a.y - Px(14)), ImVec2(a.x + kW * Z() + Px(10), a.y + NodeH(*b) * Z() + Px(10))).Contains(io.MousePos))
                    linkTarget = b->id;
            }
        for (Block* b : order) DrawNode(dl, origin, *b, hoveredNode == b->id, hotNode == b->id ? hotPort : -1, linkTarget == b->id);
        DrawStrokes(dl, origin);
        if (s.blocks.empty()) {
            const char* hint = L("Перетащи триггер из панели «Узлы» — с него начнётся сценарий", "Drag a trigger from the Nodes panel: the scenario starts with it");
            const ImVec2 t = Widgets::TextSize(Theme::semibold, hint);
            Widgets::Text(dl, Theme::semibold, ImVec2(cmin.x + (size.x - t.x) * 0.5f, cmin.y + size.y * 0.42f), Theme::kInkMuted, hint);
        }

        // тянем новую стрелку
        if (g_canvas.linkFrom >= 0) {
            if (const Block* from = FindBlock(s, g_canvas.linkFrom)) {
                const ImVec2 a = OutPortC(*from, g_canvas.linkPort), m = ToCanvas(origin, io.MousePos);
                const float outDir = OutLeft(*from) ? -1.0f : 1.0f;
                std::vector<ImVec2> pts = Vertical() ? VLink(s_linkStyle, a, m) : Routed(s_linkStyle)
                    ? Corners(s_linkStyle, { a, ImVec2((a.x + m.x) * 0.5f, a.y), ImVec2((a.x + m.x) * 0.5f, m.y), m })
                    : Shape(s_linkStyle, a, m, outDir, (m.x - a.x) * outDir > 0 ? -outDir : outDir, 12345U, nullptr);
                for (auto& q : pts) q = ToScreen(origin, q);
                DrawLink(dl, pts, PortColor(*from, g_canvas.linkPort), PortColor(*from, g_canvas.linkPort), 0.9f, thick, false, s_linkStyle);
            }
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (linkTarget >= 0) {
                    if (Block* t = FindBlock(s, linkTarget)) { // отпустили на правой половине — вход справа
                        const float mid = ToScreen(origin, ImVec2(t->x + kW * 0.5f, 0)).x;
                        const int cur = t->sides >= 0 ? t->sides : (InRight(*t) ? 1 : 0) | (OutLeft(*t) ? 2 : 0);
                        t->sides = (cur & 2) | (io.MousePos.x > mid ? 1 : 0);
                        g_side[t->id] = static_cast<unsigned char>(t->sides);
                    }
                    Link(s, g_canvas.linkFrom, g_canvas.linkPort, linkTarget);
                }
                g_canvas.linkFrom = -1;
            }
        }

        // масштаб и мини-карта — поверх схемы
        Theme::Plate(dl, zoomMin, zoomMax, Theme::Surface(0.92f), Px(12));
        for (int i = 0; i < 3; ++i) {
            const ImVec2 p(zoomMin.x + Px(6) + i * zb, zoomMin.y + Px(4));
            if (zoomHover[i]) dl->AddRectFilled(p, ImVec2(p.x + zb, p.y + zb), Theme::Overlay(16), Px(9));
            Glyph(dl, zoomGlyphs[i], ImVec2(p.x + zb * 0.5f, p.y + zb * 0.5f), Px(13), zoomHover[i] ? Theme::kInk : Theme::kInkSoft);
        }
        Widgets::Text(dl, Theme::semibold, ImVec2(cmin.x + Px(14), cmax.y - Px(26)), Theme::kInkMuted,
                      g_draw.on ? L("рисуй левой кнопкой · средняя — двигать холст · колесо — масштаб · Ctrl+Z — отменить штрих · Esc — выйти",
                                    "draw with the left button · middle button moves the canvas · wheel zooms · Ctrl+Z undoes a stroke · Esc exits")
                                : Vertical() ? L("колесо — масштаб · тяни пустое место — двигать · от точки снизу блока — линия · правый клик по блоку — настройки",
                                               "wheel zooms · drag empty space to move · drag from the dot under a block to link · right-click a block for settings")
                                : "колесо — масштаб · тяни пустое место — двигать · от точки слева или справа — линия · правый клик по блоку — настройки");
        layers.Merge(dl);
        ImGui::PopClipRect();
        DrawPickMenu(s, size);

        if (zoomIn) ZoomAround(origin, ImVec2(cmin.x + size.x * 0.5f, cmin.y + size.y * 0.5f), 1.2f);
        if (zoomOut) ZoomAround(origin, ImVec2(cmin.x + size.x * 0.5f, cmin.y + size.y * 0.5f), 1.0f / 1.2f);
        if (zoomFit) FitCanvas(s, size);
        if (canvasHovered && io.MouseWheel != 0.0f && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
            ZoomAround(origin, io.MousePos, io.MouseWheel > 0 ? 1.12f : 1.0f / 1.12f);
        if (std::fabs(g_canvas.zoomTarget - g_canvas.zoom) > 0.0005f) { // плавно доезжаем до нового масштаба
            g_canvas.zoom += (g_canvas.zoomTarget - g_canvas.zoom) * Theme::Ease(io.DeltaTime, 7.4f);
            g_canvas.pan = ImVec2(g_canvas.anchorScreen.x - origin.x - g_canvas.anchorCanvas.x * Z(), g_canvas.anchorScreen.y - origin.y - g_canvas.anchorCanvas.y * Z());
        }

        // двигаем карточку
        if (g_canvas.dragNode >= 0) {
            Block* b = FindBlock(s, g_canvas.dragNode);
            if (b && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                const ImVec2 m = ToCanvas(origin, io.MousePos);
                ImVec2 p(m.x - g_canvas.dragOffset.x, m.y - g_canvas.dragOffset.y);
                if (!io.KeyAlt) p = ImVec2(std::round(p.x / kGrid) * kGrid, std::round(p.y / kGrid) * kGrid); // по сетке; Alt — свободно
                if (p.x != b->x || p.y != b->y) { b->x = p.x, b->y = p.y; g_dirty = true; }
            } else g_canvas.dragNode = -1;
        }

        // клавиши
        if (!io.WantTextInput && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
            if (g_canvas.selected >= 0 && ImGui::IsKeyPressed(ImGuiKey_Delete)) DeleteBlock(s, g_canvas.selected);
            if (g_draw.on && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                g_keepEscFrame = ImGui::GetFrameCount(); if (g_draw.pending.empty()) SetDraw(false); else Fade(g_draw.pending); }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) { if (!io.KeyShift) UndoStroke(); else Restore(g_historyAt + 1); }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) Restore(g_historyAt + 1);
        }

        // настройки выделенного блока — справа, над мини-картой
        if (inspector && FindBlock(s, g_canvas.selected))
            DrawInspector(s, ImVec2(cmax.x - Px(12) - side, cmin.y + Px(12)), ImVec2(cmax.x - Px(12), zoomMin.y - Px(10)));

        // новый блок из панели: тень под мышью, отпустили над холстом — кладём
        if (g_canvas.newStep >= 0) {
            const Step stepType = static_cast<Step>(g_canvas.newStep);
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            const ImVec2 m = io.MousePos, a(m.x - Px(24), m.y - Px(18)), b(a.x + kW * Z(), a.y + kH * Z());
            Theme::Plate(fg, a, b, Theme::Surface(0.9f), Px(18) * g_canvas.zoom, 1.0f);
            Glyph(fg, StepGlyph(stepType), ImVec2(a.x + 40 * Z(), a.y + 42 * Z()), 22 * Z(), StepColor(stepType).WithAlpha(1.0f));
            fg->AddText(Theme::title, Theme::title->LegacySize * 1.12f * g_canvas.zoom, ImVec2(a.x + 76 * Z(), a.y + 30 * Z()), Theme::kInk, Lang::Tr(Scenarios::StepName(stepType)));
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (ImRect(cmin, cmax).Contains(m)) {
                    ImVec2 at = ToCanvas(origin, a);
                    AddBlock(s, stepType, size, &at);
                }
                g_canvas.newStep = -1;
            }
        }
    }

    void DrawEditor(ImVec2 areaMin, ImVec2 areaMax) {
        if (g_list.empty()) { // ни одного сценария — сразу заводим пустой с фразой
            Scenario fresh;
            fresh.hasLayout = true;
            Block trigger = NewBlock(Step::Phrase);
            trigger.id = 1;
            fresh.blocks.push_back(trigger);
            g_list.push_back(fresh);
            g_selected = 0;
        }
        g_selected = std::clamp(g_selected, 0, static_cast<int>(g_list.size()) - 1);
        auto& s = g_list[g_selected];
        Scenarios::Normalize(s);
        if (s.relayout) { s.relayout = false; AutoLayout(s); }

        const float barH = Px(48), gap = Px(12), sideW = std::clamp((areaMax.x - areaMin.x) * 0.16f, Px(200), Px(250)); // блоки — колонкой слева
        const ImVec2 canvasMin(areaMin.x + sideW + gap, areaMin.y + barH + gap), canvasMax(areaMax.x, areaMax.y);
        const ImVec2 canvasSize(canvasMax.x - canvasMin.x, canvasMax.y - canvasMin.y);
        { // один раз переложить старые схемы змейкой: прежний перенос рисовал линию через весь холст
            static bool s_checked = false;
            if (!s_checked) {
                s_checked = true;
                if (Config::Value("scenarios", "snake", "0") != "1") {
                    for (auto& old : g_list) if (old.blocks.size() > 4) AutoLayout(old);
                    Config::SetValue("scenarios", "snake", "1");
                }
                LoadLineLook();
                if (Vertical() && Config::Value("scenarios", "flow_done", "0") != "1") { // новая раскладка по умолчанию — лентой вниз
                    for (auto& old : g_list) AutoLayout(old);
                    Config::SetValue("scenarios", "flow_done", "1");
                }
            }
        }

        DrawToolbar(s, ImVec2(areaMin.x + g_titleW, areaMin.y), ImVec2(areaMax.x - Px(48), areaMin.y + barH), canvasSize);
        if (g_list.empty()) return; // удалили последний — в следующем кадре заведём новый
        auto& current = g_list[std::clamp(g_selected, 0, static_cast<int>(g_list.size()) - 1)];
        DrawCanvas(current, canvasMin, canvasMax);
        DrawBlocksBar(current, ImVec2(areaMin.x, areaMin.y + barH + gap), ImVec2(areaMin.x + sideW, areaMax.y), canvasSize);
    }
}

void ScenariosPage::Open() {
    if (g_open) return;
    g_list = Scenarios::Load();
    g_selected = std::clamp(g_selected, 0, std::max(0, static_cast<int>(g_list.size()) - 1));
    g_history.clear();
    g_historyAt = -1;
    Remember();
    g_canvas.lastScenario = -1;
    g_open = true;
}

void ScenariosPage::Close() {
    if (g_open && g_dirty) Scenarios::Save(g_list);
    g_dirty = false;
    g_open = false;
}

void ScenariosPage::Add(const Scenarios::Scenario& scenario) {
    std::lock_guard lock(g_addMutex);
    g_added.push_back(scenario);
}

bool ScenariosPage::PollAdded() {
    std::vector<Scenarios::Scenario> added;
    {
        std::lock_guard lock(g_addMutex);
        added.swap(g_added);
    }
    if (added.empty()) return false;
    if (!g_open) g_list = Scenarios::Load(); // открытая вкладка — дописываем к тому, что в ней (с несохранёнными правками)
    for (auto& s : added) {
        Scenarios::Normalize(s);
        AutoLayout(s); // собранное голосом — сразу аккуратно сверху вниз
        g_list.push_back(std::move(s));
    }
    Scenarios::Save(g_list);
    g_dirty = false;
    if (!g_open) Open();
    else Remember();
    g_selected = static_cast<int>(g_list.size()) - 1;
    g_canvas.lastScenario = -1; // показать новый сценарий целиком
    return true;
}

void ScenariosPage::SetLook(int look) {
    s_lineLoaded = true;
    s_linkStyle = std::clamp(look, 0, static_cast<int>(kLineStyles) - 1);
    Config::SetValue("scenarios", "look", std::to_string(s_linkStyle));
    g_canvas.routeKey.clear();
}
void ScenariosPage::SetDrawMode(bool on) { SetDraw(on); }
bool ScenariosPage::KeepsEscape() { // Ui и страница могут увидеть Esc в разном порядке — помним прошлый кадр
    return g_draw.on || g_keepEscFrame >= ImGui::GetFrameCount() - 1 || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
}

bool ScenariosPage::IsOpen() { return g_open; }
bool ScenariosPage::Covers() { return g_open && g_anim > 0.97f; }
float ScenariosPage::Visibility() { return g_anim; }

void ScenariosPage::Draw(ImVec2 screen, float top, float dt) {
    g_anim += ((g_open ? 1.0f : 0.0f) - g_anim) * Theme::Ease(dt, g_open ? 13.0f : 24.0f); // старая страница исчезает сразу, не наезжая на новую
    if (g_anim < 0.04f) return;
    const float ease = 1.0f - std::pow(1.0f - g_anim, 3.0f);

    ImGui::SetNextWindowPos(ImVec2(Theme::railWidth + Theme::PageShift(g_open, g_anim, screen.x), top)); // правее панели вкладок
    ImGui::SetNextWindowSize(ImVec2(screen.x - Theme::railWidth, screen.y - top));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ease);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    { // над страницей — мягкий переход от меню: без резкой границы между фоном Ани и страницей
        const float fadeH = Px(56), x0 = Theme::railWidth, x1 = screen.x;
        const ImU32 solid = PageColor(0.86f * ease), clear = PageColor(0.0f);
        ImGui::GetBackgroundDrawList()->AddRectFilledMultiColor(ImVec2(x0, top - fadeH), ImVec2(x1, top), clear, clear, solid, solid);
    }
    ImGui::PushStyleColor(ImGuiCol_WindowBg, PageColor(0.86f)); // сквозь страницу — живой фон Ани
    ImGui::Begin("##scenariosPage", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const ImVec2 origin(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y + (1.0f - ease) * Px(14));
    const ImVec2 size = ImGui::GetWindowSize();
    const float margin = Px(16);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // шапка в одну строку: заголовок, островок инструментов, закрыть — холсту достаётся вся высота
    Widgets::Text(dl, Theme::hero, ImVec2(origin.x + margin + Px(4), origin.y + margin + Px(24) - Theme::hero->LegacySize * 0.55f), Theme::kInk, "Сценарии");
    g_titleW = Widgets::TextSize(Theme::hero, Lang::Tr("Сценарии")).x + Px(28);
    if (ImGui::IsMouseHoveringRect(ImVec2(origin.x + margin, origin.y + margin), ImVec2(origin.x + margin + g_titleW, origin.y + margin + Px(48))))
        ImGui::SetTooltip("%s", Lang::Tr("скажи фразу — и Аня выполнит блоки по стрелкам"));
    if (Widgets::IconButton("##scenariosClose", ImVec2(origin.x + size.x - margin - Px(40), origin.y + margin + Px(4)), Px(40), Icon::Close)) Close();
    DrawEditor(ImVec2(origin.x + margin, origin.y + margin), ImVec2(origin.x + size.x - margin, origin.y + size.y - margin));

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(4);
    ApplyRestore();

    // изменения сохраняем, когда закончили редактировать поле или поменяли схему
    if (g_dirty && !ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        Scenarios::Save(g_list);
        Remember();
        g_dirty = false;
    }
}
