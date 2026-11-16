#include "Ui.h"

#include "Lang.h"
#include "EcoMode.h"
#include "Brain.h"
#include "LocalAi.h"
#include "Settings.h"
#include "State.h"
#include "TextUtils.h"
#include "Theme.h"
#include "HomeLayout.h"
#include "../ImGui/imgui_internal.h"
#include "CozyArt.h"
#include "Logo.h"
#include "Tour.h"
#include "SetupWizard.h"
#include "Visuals.h"
#include "Voice.h"
#include "Graphics.h"
#include "HomeWidgets.h"
#include "ProcessesPage.h"
#include "ScenariosPage.h"
#include "SettingsPage.h"
#include "Toast.h"
#include "Wallpaper.h"
#include "WeatherWidget.h"
#include "Widgets.h"
#include "MediaNow.h"
#include "Screen.h"
#include "SystemMonitor.h"
#include "Weather.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <cfloat>
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <dwmapi.h>
#include <shobjidl.h>

#pragma comment(lib, "dwmapi.lib")

using Theme::Mood;
using Theme::Palette;
using Theme::Px;
using Widgets::Icon;

namespace {
    HWND g_window = nullptr;
    float g_titleHeight = 56.0f;
    float g_titleButtonsLeft = 1e9f;
    ImVec2 g_navMin(0, 0), g_navMax(0, 0); // меню по центру шапки — за него окно не тянем
    float g_navX = -1.0f, g_navW = 0.0f;   // подсветка выбранной вкладки (едет плавно)

    bool g_historyOpen = false;
    float g_historyAnim = 0.0f;
    bool g_focusInput = false;
    char g_input[1024] = "";
    float g_time = 0.0f;
    float g_intro = 0.0f; // появление главного экрана при запуске: 0 → 1

    // Плавное «появление» элемента с задержкой (секунды от запуска)
    float Intro(float delay) {
        const float t = std::clamp((g_intro * 1.1f - delay) / 0.55f, 0.0f, 1.0f);
        return 1.0f - std::pow(1.0f - t, 3.0f);
    }

    struct Layout {
        ImVec2 screen;
        ImVec2 orbCenter;
        float orbRadius;
        float dockTop;
        float dockHeight;
        float dockCenterX = 0; // середина поля ввода (его можно сдвинуть)
        float dockScale = 1;
        float heroY = 0;       // середина крупной надписи
    };

    Mood CurrentMood() {
        if (g_app.speaking) return Mood::Speaking;
        if (g_app.thinking || g_app.transcribing) return Mood::Thinking;
        if (g_app.capturing || g_app.pttArmed) return Mood::Listening;
        return Mood::Idle;
    }

    std::string IdleHint(bool wakeWord) {
        return wakeWord ? "скажи «" + Config::Get().assistant + "» и вопрос — или нажми на сферу" : std::string("нажми на сферу или пробел — и говори");
    }

    // «Поверх всех окон» из настроек → окну (меняем, только когда переключили)
    void ApplyWindowOptions(const Settings& s) {
        static int s_onTop = -1, s_effect = -1;
        if (!g_window) return;
        const int onTop = 0; // главное окно ведёт себя как обычное; поверх других — только оверлеи
        if (onTop != s_onTop) {
            s_onTop = onTop;
            SetWindowPos(g_window, onTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        if (s.windowEffect != s_effect) { // акрил / Mica Windows 11 под прозрачными местами окна
            s_effect = s.windowEffect;
            const int kBackdrop[] = { 1 /* нет */, 2 /* Mica */, 3 /* акрил */ };
            DwmSetWindowAttribute(g_window, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */, &kBackdrop[std::clamp(s_effect, 0, 2)], sizeof(int));
        }
    }

    // ---------------------------------------------------------------- свои обои
    void SyncWallpaper(const Settings& s) {
        static std::string s_loaded;
        static bool s_first = true;
        if (s_first || s.wallpaper != s_loaded) {
            s_first = false;
            s_loaded = s.wallpaper;
            if (s.wallpaper.empty()) Wallpaper::Unload();
            else Wallpaper::Load(s.wallpaper);
        }
        std::string picked;
        if (!Wallpaper::Picked(picked)) return;
        if (CozyArt::TakePicked(picked)) return; // картинку выбирали для персонажа, а не для обоев
        if (!Wallpaper::Load(picked)) { Caption::Set("не получилось открыть эту картинку"); return; }
        s_loaded = picked;
        Config::Update([&](Settings& x) { x.wallpaper = picked; });
        if (s.autoColors) Wallpaper::ApplyAutoColors(true); // уже включено — просто подстраиваемся под новую картинку
        else Toast::Ask("Поставить автоматическое подстраивание под цвет обоев? (РАБОТАЕТ ТОЛЬКО В АНЕ!)", [] { Wallpaper::ApplyAutoColors(true); });
    }

    // ---------------------------------------------------------------- шапка
    // Логотип и настроение слева
    void DrawBrand(ImDrawList* dl, Mood mood) {
        const float pad = Px(22);
        const ImVec2 brand(pad, Px(14));
        const float logoSize = Px(32), logoGap = Px(8);
        Logo::Draw(dl, ImVec2(brand.x - Px(4), brand.y + (Theme::display->LegacySize - logoSize) * 0.5f), logoSize);
        const char* title = "ZAVIRIA";
        Widgets::Text(dl, Theme::display, ImVec2(brand.x + logoSize - Px(4) + logoGap, brand.y), Theme::kInk, title);
        const float brandWidth = logoSize - Px(4) + logoGap + Widgets::TextSize(Theme::display, title).x;

        static const char* kMoodText[] = { "отдыхаю", "слушаю", "думаю", "говорю" };
        const char* label = kMoodText[static_cast<int>(mood)];
        if (mood == Mood::Idle && !g_app.micEnabled) label = "микрофон выключен";
        if (mood == Mood::Thinking && g_app.transcribing) label = "разбираю речь";
        const float pulse = mood == Mood::Idle ? 1.0f : 1.0f + 0.5f * std::fabs(std::sin(g_time * (mood == Mood::Thinking ? 7.0f : 3.5f)));
        const ImVec2 led(brand.x + brandWidth + Px(30), brand.y + Theme::display->LegacySize * 0.55f);
        dl->AddCircleFilled(led, Px(6) * pulse, Palette(0).WithAlpha(0.18f), 16);
        dl->AddCircleFilled(led, Px(3.5f), Palette(0).WithAlpha(1.0f), 16);
        Widgets::Text(dl, Theme::semibold, ImVec2(led.x + Px(10), led.y - Theme::semibold->LegacySize * 0.55f), Theme::kInkMuted, label);
    }

    // ---------------------------------------------------------------- меню в шапке: порядок, названия, что показывать
    // [layout] nav_order=0,1,2,3 · nav_label_N · nav_hide_N · nav_style (0 — значок и текст, 1 — только значки, 2 — только текст)
    struct NavTab { Icon icon; const char* label; };
    const NavTab kNavTabs[] = { { Icon::Home, "Главная" }, { Icon::Flow, "Сценарии" }, { Icon::Grid, "Процессы" }, { Icon::Gear, "Настройки" } };
    constexpr int kNavCount = 4;
    struct NavConfig { int order[kNavCount] = { 0, 1, 2, 3 }; bool hidden[kNavCount] = {}; std::string labels[kNavCount]; int style = 0; char edit[64] = ""; bool loaded = false; };
    NavConfig g_nav;

    void LoadNav() {
        if (g_nav.loaded) return;
        g_nav.loaded = true;
        const std::string order = Config::Value("layout", "nav_order", "0,1,2,3");
        int used[kNavCount] = {}, n = 0;
        for (size_t i = 0; i < order.size() && n < kNavCount; ++i)
            if (order[i] >= '0' && order[i] < '0' + kNavCount && !used[order[i] - '0']) { used[order[i] - '0'] = 1; g_nav.order[n++] = order[i] - '0'; }
        for (int t = 0; t < kNavCount && n < kNavCount; ++t) if (!used[t]) g_nav.order[n++] = t; // забытые — в конец
        for (int t = 0; t < kNavCount; ++t) {
            g_nav.labels[t] = Config::Value("layout", ("nav_label_" + std::to_string(t)).c_str(), "");
            g_nav.hidden[t] = t != 3 && Config::Value("layout", ("nav_hide_" + std::to_string(t)).c_str(), "0") == "1"; // настройки не прячем
        }
        g_nav.style = std::clamp(std::atoi(Config::Value("layout", "nav_style", "0").c_str()), 0, 2);
    }

    void SaveNav() {
        std::string order;
        for (int i = 0; i < kNavCount; ++i) order += (i ? "," : "") + std::to_string(g_nav.order[i]);
        Config::SetValue("layout", "nav_order", order);
        for (int t = 0; t < kNavCount; ++t) {
            Config::SetValue("layout", ("nav_label_" + std::to_string(t)).c_str(), g_nav.labels[t]);
            Config::SetValue("layout", ("nav_hide_" + std::to_string(t)).c_str(), g_nav.hidden[t] ? "1" : "0");
        }
        Config::SetValue("layout", "nav_style", std::to_string(g_nav.style));
    }

    std::string NavLabel(int tab) { return g_nav.labels[tab].empty() ? kNavTabs[tab].label : g_nav.labels[tab]; }

    std::vector<int> g_navVisible; // какие вкладки нарисованы (номера вкладок по порядку) — для правки

    void MoveNavTab(int from, int to) { // from/to — места среди видимых
        if (from < 0 || to < 0 || from >= static_cast<int>(g_navVisible.size()) || to >= static_cast<int>(g_navVisible.size())) return;
        std::vector<int> order(g_nav.order, g_nav.order + kNavCount);
        const int tab = g_navVisible[from], other = g_navVisible[to];
        order.erase(std::find(order.begin(), order.end(), tab));
        auto at = std::find(order.begin(), order.end(), other);
        order.insert(to > from ? at + 1 : at, tab);
        std::copy(order.begin(), order.end(), g_nav.order);
        SaveNav();
    }

    int OpenTab() { return SettingsPage::IsOpen() ? 3 : ProcessesPage::IsOpen() ? 2 : ScenariosPage::IsOpen() ? 1 : 0; }
    void OpenNavTab(int place); // ниже: тот же переход, что и по щелчку по вкладке

    void NavTabMenu(int place) {
        if (place < 0 || place >= static_cast<int>(g_navVisible.size())) return;
        const int tab = g_navVisible[place];
        if (ImGui::IsWindowAppearing()) snprintf(g_nav.edit, sizeof(g_nav.edit), "%s", NavLabel(tab).c_str());
        ImGui::TextDisabled("название вкладки");
        ImGui::SetNextItemWidth(Px(200));
        if (ImGui::InputTextWithHint("##navLabel", kNavTabs[tab].label, g_nav.edit, sizeof(g_nav.edit))) {
            const std::string text = Text::Trim(g_nav.edit);
            g_nav.labels[tab] = text == kNavTabs[tab].label ? std::string() : text;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveNav();
        if (tab != 3) {
            bool shown = !g_nav.hidden[tab];
            if (Widgets::Check("показывать вкладку", &shown)) { g_nav.hidden[tab] = !shown; SaveNav(); ImGui::CloseCurrentPopup(); }
        } else {
            ImGui::TextDisabled("«Настройки» не прячется — иначе не выключить правку");
        }
        ImGui::Separator();
        ImGui::TextDisabled("все вкладки");
        static const char* kStyles[] = { "значок и текст", "только значки", "только текст" };
        for (int s = 0; s < 3; ++s)
            if (Widgets::Radio(kStyles[s], g_nav.style == s)) { g_nav.style = s; SaveNav(); }
        ImGui::Separator();
        if (ImGui::Button("вернуть меню как было")) {
            for (int t = 0; t < kNavCount; ++t) { g_nav.order[t] = t; g_nav.hidden[t] = false; g_nav.labels[t].clear(); }
            g_nav.style = 0;
            SaveNav();
            ImGui::CloseCurrentPopup();
        }
    }

    // Меню по центру: Главная · Сценарии · Процессы · Настройки, выбранная вкладка подсвечена и подсветка плавно переезжает.
    // В «Полной кастомизации» остров двигается и растягивается, вкладки переставляются и переименовываются.
    std::string FitText(ImFont* font, float size, const std::string& text, float width) {
        if (font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x <= width) return text;
        std::string out = text;
        while (!out.empty() && font->CalcTextSizeA(size, FLT_MAX, 0.0f, (out + "…").c_str()).x > width) {
            while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) out.pop_back(); // байты продолжения
            if (!out.empty()) out.pop_back(); // и сам символ
        }
        return out + "…";
    }

    void DrawNavigation(ImDrawList* dl, const Layout& L, float dt) {
        LoadNav();
        const int open = OpenTab();
        struct Tab { Icon icon; std::string label; int id; };
        std::vector<Tab> tabs;
        for (int i = 0; i < kNavCount; ++i) {
            const int id = g_nav.order[i];
            if (!g_nav.hidden[id] || id == open) tabs.push_back({ kNavTabs[id].icon, NavLabel(id), id });
        }
        g_navVisible.clear();
        for (const Tab& t : tabs) g_navVisible.push_back(t.id);
        const int kCount = static_cast<int>(tabs.size());
        int selected = 0;
        for (int i = 0; i < kCount; ++i) if (tabs[i].id == open) selected = i;
        const float scale = HomeLayout::Scale("nav");
        const ImVec2 shift = HomeLayout::Shift("nav", L.screen);
        const float fontSize = Theme::title->LegacySize * 0.92f * scale;
        auto textWidth = [&](const std::string& t) { return Theme::title->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, t.c_str()).x; };

        // меню — таблетка по центру шапки: значок и жирная подпись, выбранная вкладка — на светлой подложке
        Theme::railWidth = 0.0f;
        const bool showIcon = g_nav.style != 2, showText = g_nav.style != 1;
        const float iconSize = Px(14) * scale, gap = Px(7) * scale, padX = Px(13) * scale, height = Px(30) * scale; // потоньше
        auto tabWidth = [&](const Tab& t, bool compact) {
            if (compact || !showText) return height;
            return padX * 2 + (showIcon ? iconSize + gap : 0.0f) + textWidth(Lang::Tr(t.label.c_str()));
        };
        float full = 0.0f;
        for (const Tab& t : tabs) full += tabWidth(t, false);
        const bool compact = showText && showIcon && full + Px(8) > L.screen.x - Px(520) && !HomeLayout::Moved("nav");
        std::vector<float> widths(kCount);
        float total = Px(10) * scale;
        for (int i = 0; i < kCount; ++i) { widths[i] = tabWidth(tabs[i], compact); total += widths[i]; }
        const ImVec2 pos(std::round(L.screen.x * 0.5f - total * 0.5f + shift.x), std::round((g_titleHeight - height - Px(10) * scale) * 0.5f + Px(6) + shift.y));
        g_navMin = pos;
        g_navMax = ImVec2(pos.x + total, pos.y + height + Px(8) * scale);
        const float margin = Px(14);
        ImGui::SetNextWindowPos(ImVec2(g_navMin.x - margin, g_navMin.y - margin));
        ImGui::SetNextWindowSize(ImVec2(g_navMax.x - g_navMin.x + margin * 2, g_navMax.y - g_navMin.y + margin * 2));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("##navBar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::PopStyleVar();
        // меню всегда поверх страницы: иначе окно главной его накрывает и кнопки не нажимаются
        if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        dl = ImGui::GetWindowDrawList();
        Theme::Plate(dl, g_navMin, g_navMax, Theme::Surface(0.7f), (g_navMax.y - g_navMin.y) * 0.5f);
        HomeLayout::Report("nav", "меню", ImVec2(g_navMin.x - Px(6), g_navMin.y - Px(4)), ImVec2(g_navMax.x + Px(6), g_navMax.y + Px(4)), HomeLayout::kBoth);

        float x = pos.x + Px(5) * scale;
        float selectedX = x;
        for (int i = 0; i < selected; ++i) selectedX += widths[i];
        if (g_navX < 0) g_navX = selectedX, g_navW = widths[selected];
        const float k = Theme::Ease(dt, 9.0f);
        g_navX += (selectedX - g_navX) * k;
        g_navW += (widths[selected] - g_navW) * k;
        const ImVec2 hiMin(g_navX, pos.y + Px(5) * scale), hiMax(g_navX + g_navW, pos.y + Px(5) * scale + height);
        dl->AddRectFilled(hiMin, hiMax, Theme::Overlay(20), height * 0.5f);

        static float s_hover[kNavCount] = {};
        for (int i = 0; i < kCount; ++i) {
            const ImVec2 a(x, pos.y + Px(5) * scale), b(x + widths[i], pos.y + Px(5) * scale + height);
            HomeLayout::ReportChild("nav", i, a, b, { MoveNavTab, NavTabMenu, OpenNavTab });
            ImGui::SetCursorScreenPos(a);
            ImGui::PushID(i);
            const bool clicked = ImGui::InvisibleButton("##tab", ImVec2(widths[i], height));
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            s_hover[i] += ((hovered ? 1.0f : 0.0f) - s_hover[i]) * Theme::Ease(dt, 9.9f);
            if (s_hover[i] > 0.01f && i != selected) dl->AddRectFilled(a, b, Theme::Overlay(static_cast<int>(12 * s_hover[i])), height * 0.5f);
            if ((compact || !showText) && hovered) ImGui::SetTooltip("%s", Lang::Tr(tabs[i].label.c_str()));
            const bool on = i == selected;
            const ImU32 color = on ? Theme::kInk : (s_hover[i] > 0.5f ? Theme::kInkSoft : Theme::kInkMuted);
            const bool iconOnly = compact || !showText;
            if (showIcon || iconOnly) Widgets::DrawIcon(dl, tabs[i].icon, ImVec2(iconOnly ? (a.x + b.x) * 0.5f : a.x + padX + iconSize * 0.5f, (a.y + b.y) * 0.5f), iconSize, color);
            if (!iconOnly)
                dl->AddText(Theme::title, fontSize, ImVec2(a.x + padX + (showIcon ? iconSize + gap : 0.0f), (a.y + b.y) * 0.5f - fontSize * 0.55f), color, Lang::Tr(tabs[i].label.c_str()));
            if (clicked) OpenNavTab(i);
            x += widths[i];
        }
        ImGui::End();
    }

    void OpenNavTab(int place) {
        if (place < 0 || place >= static_cast<int>(g_navVisible.size())) return;
        g_historyOpen = false;
        // открыта только одна страница; повторный клик по своей вкладке — назад на главную
        const int open = OpenTab(), id = g_navVisible[place];
        const bool again = id == open;
        { // экран уезжает в сторону, где стоит новая вкладка
            int from = 0, to = place;
            for (int j = 0; j < static_cast<int>(g_navVisible.size()); ++j) {
                if (g_navVisible[j] == open) from = j;
                if (again && g_navVisible[j] == 0) to = j;
            }
            Theme::pageDirection = to >= from ? 1 : -1;
        }
        if (SettingsPage::IsOpen() && (id != 3 || again)) SettingsPage::Close();
        if (ScenariosPage::IsOpen() && (id != 1 || again)) ScenariosPage::Close();
        if (ProcessesPage::IsOpen() && (id != 2 || again)) ProcessesPage::Close();
        if (!again && id == 1) ScenariosPage::Open();
        if (!again && id == 2) ProcessesPage::Open();
        if (!again && id == 3) SettingsPage::Open();
    }

    // Кнопки окна справа
    void DrawWindowButtons(const Layout& L) {
        const float size = Px(30), gap = Px(4), y = Px(8);
        const bool plates = Theme::ButtonPlates();
        float x = L.screen.x - Px(18) - size;
        if (Widgets::IconButton("##close", ImVec2(x, y), size, Icon::Close, true, plates)) PostMessageW(g_window, WM_CLOSE, 0, 0);
        x -= size + gap;
        if (Widgets::IconButton("##maximize", ImVec2(x, y), size, Icon::Maximize, false, plates))
            ShowWindow(g_window, IsZoomed(g_window) ? SW_RESTORE : SW_MAXIMIZE);
        x -= size + gap;
        if (Widgets::IconButton("##minimize", ImVec2(x, y), size, Icon::Minimize, false, plates)) ShowWindow(g_window, SW_MINIMIZE);
        g_titleButtonsLeft = x;
    }

    void DrawHeader(ImDrawList* dl, const Layout& L, Mood mood, float dt) {
        DrawBrand(dl, mood);
        DrawNavigation(dl, L, dt);
        DrawWindowButtons(L);
    }

    // ---------------------------------------------------------------- сфера и приветствие
    void DrawOrbButton(const Layout& L) {
        ImGui::SetCursorScreenPos(ImVec2(L.orbCenter.x - L.orbRadius, L.orbCenter.y - L.orbRadius));
        if (ImGui::InvisibleButton("##orb", ImVec2(L.orbRadius * 2, L.orbRadius * 2))) Brain::ToggleListening();
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    // Главная по образцу Astra: крупная плакатная надпись «твоя Аня» с логотипом вместо звёздочки и живая подпись под ней
    void DrawHero(ImDrawList* dl, const Layout& L) {
        const std::string name = Config::Get().assistant.empty() ? std::string("Аня") : Config::Get().assistant; // просто имя помощницы
        const float size = Theme::poster->LegacySize;
        const float w2 = Theme::poster->CalcTextSizeA(size, FLT_MAX, 0.0f, name.c_str()).x;
        const float x = std::round(L.screen.x * 0.5f - w2 * 0.5f), y = std::round(L.heroY - size * 0.5f);
        dl->AddText(Theme::poster, size, ImVec2(x, y), Theme::kInk, name.c_str());

        std::string caption;
        bool live = false;
        Caption::Get(caption, live);
        if (g_app.transcribing) { caption = "секундочку…"; live = true; }
        if (!live && caption == IdleHint(Config::Get().wakeWord)) { // в покое — короткая живая фраза дня вместо инструкции
            static const char* kLines[] = { "Дела или прокрастинация?", "Чем займёмся?", "Я рядом, если что", "Спроси что угодно", "Как настроение?" };
            SYSTEMTIME now{};
            GetLocalTime(&now);
            caption = kLines[(now.wDay + now.wHour / 6) % IM_ARRAYSIZE(kLines)];
        }
        Widgets::TextWrappedCentered(dl, Theme::title, L.screen.x * 0.5f, y + size * 1.12f, std::min(L.screen.x - Px(80), Px(760)), live ? Theme::kInk : Theme::kInkSoft, Lang::Tr(caption.c_str()), 3);
    }

    // ---------------------------------------------------------------- видимый чат
    // Пока разговора нет — надпись «твоя Аня»; как только что-то спросили — по центру лента сообщений, поле ввода уезжает вниз
    float ComposerWidth(ImVec2 screen, float scale);
    float g_chatAnim = 0;
    bool g_chatFolded = false; // чат свёрнут стрелкой — видна надпись, над полем ввода стрелка «развернуть»
    int UserMessages() {
        int n = 0;
        for (const auto& m : Chat::Snapshot()) if (m.role == Role::User) ++n;
        return n;
    }
    bool HasConversation() {
        static int s_seen = 0;
        const int n = UserMessages();
        if (n > s_seen) g_chatFolded = false; // спросили что-то новое — чат раскрывается сам
        s_seen = n;
        return n > 0 && !g_chatFolded;
    }

    // Кнопка-стрелка: up — «⌃», иначе «⌄»; рядом подпись
    bool ArrowButton(const char* id, ImVec2 pos, bool up, const char* label) {
        const float h = Px(28);
        const float tw = Widgets::TextSize(Theme::semibold, label).x;
        const ImVec2 size(Px(34) + tw + Px(12), h);
        ImGui::SetCursorScreenPos(pos);
        const bool pressed = ImGui::InvisibleButton(id, size);
        const bool hover = ImGui::IsItemHovered();
        if (hover) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), Theme::Overlay(hover ? 26 : 14), h * 0.5f);
        const ImVec2 c(pos.x + Px(17), pos.y + h * 0.5f);
        const float d = Px(5), dy = up ? -Px(2.5f) : Px(2.5f);
        const ImVec2 pts[3] = { ImVec2(c.x - d, c.y - dy), ImVec2(c.x, c.y + dy), ImVec2(c.x + d, c.y - dy) };
        dl->AddPolyline(pts, 3, hover ? Theme::kInk : Theme::kInkSoft, 0, Px(1.8f));
        Widgets::Text(dl, Theme::semibold, ImVec2(pos.x + Px(30), pos.y + (h - Theme::semibold->LegacySize) * 0.5f - Px(1)), hover ? Theme::kInk : Theme::kInkSoft, label);
        return pressed;
    }

    void DrawFeed(const Layout& L, float anim) {
        const float width = std::min(ComposerWidth(L.screen, L.dockScale), Px(780));
        const float top = g_titleHeight + Px(10), bottom = L.dockTop - Px(12);
        if (bottom - top < Px(90)) return;
        const float x = L.dockCenterX - width * 0.5f;
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, anim);
        ImGui::PushFont(Theme::semibold, Theme::semibold->LegacySize);
        const char* fresh = Lang::Tr("+ новый разговор");
        ImGui::SetCursorScreenPos(ImVec2(x + width - ImGui::CalcTextSize(fresh).x - Px(18), top));
        if (ImGui::SmallButton((std::string(fresh) + "##feedNew").c_str())) { Chat::Clear(); Conversation::Clear(); }
        ImGui::PopFont();
        if (ArrowButton("##feedFold", ImVec2(x, top - Px(2)), false, Lang::Tr("свернуть"))) g_chatFolded = true;
        ImGui::SetCursorScreenPos(ImVec2(x, top + Px(34)));
        ImGui::BeginChild("##feed", ImVec2(width, bottom - top - Px(34)), 0, ImGuiWindowFlags_NoBackground);
        const auto messages = Chat::Snapshot();
        const float contentWidth = ImGui::GetContentRegionAvail().x;
        ImGui::Dummy(ImVec2(1, Px(4)));
        static std::vector<float> s_appear; // появление каждого сообщения: проявляется и чуть подъезжает снизу
        if (s_appear.size() > messages.size()) s_appear.clear();
        const size_t old = s_appear.size();
        s_appear.resize(messages.size(), old == 0 && messages.size() > 1 ? 1.0f : 0.0f); // при открытии старые — сразу на месте
        for (size_t i = 0; i < messages.size(); ++i) {
            float& t = s_appear[i];
            t = std::min(1.0f, t + ImGui::GetIO().DeltaTime * 3.2f);
            const float e = 1.0f - std::pow(1.0f - t, 3.0f);
            if (e < 1.0f) ImGui::Dummy(ImVec2(1, (1.0f - e) * Px(14)));
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * e);
            Widgets::ChatBubble(messages[i], contentWidth);
            ImGui::PopStyleVar();
        }
        if (g_app.speaking.load() || (!messages.empty() && messages.back().role == Role::User)) { // Аня думает — три точки
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float t = static_cast<float>(ImGui::GetTime());
            if (!messages.empty() && messages.back().role == Role::User)
                for (int i = 0; i < 3; ++i)
                    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + Px(16) + i * Px(12), p.y + Px(12) - std::max(0.0f, std::sin(t * 6.0f - i * 0.8f)) * Px(4)), Px(3.5f), Theme::kInkMuted, 12);
            ImGui::Dummy(ImVec2(1, Px(26)));
        }
        static size_t s_count = 0, s_length = 0; // новое сообщение или дописывается ответ — прокрутка вниз
        const size_t length = messages.empty() ? 0 : messages.back().text.size();
        if (messages.size() != s_count || length != s_length) ImGui::SetScrollHereY(1.0f);
        s_count = messages.size();
        s_length = length;
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

    // слева — маленькая колонка: новый разговор, весь разговор, правка главного экрана
    void DrawSideTools(const Layout& L) {
        const float size = Px(38), gapY = Px(10), x = Px(22);
        float y = g_titleHeight + Px(40);
        struct T { const char* id; Icon icon; const char* tip; };
        const T tools[] = { { "##newChat", Icon::Spark, "новый разговор" }, { "##history", Icon::Book, "весь разговор" }, { "##editHome", Icon::Layers, "переставить блоки на главной" } };
        for (int i = 0; i < 3; ++i) {
            if (i == 0) Theme::Plate(ImGui::GetWindowDrawList(), ImVec2(x, y), ImVec2(x + size, y + size), Theme::Surface(0.9f), Px(10), 0.4f);
            const bool pressed = Widgets::IconButton(tools[i].id, ImVec2(x, y), size, tools[i].icon, false, false);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Lang::Tr(tools[i].tip));
            if (pressed) {
                if (i == 0) { Chat::Clear(); Caption::Set(IdleHint(Config::Get().wakeWord)); }
                if (i == 1) g_chatFolded = !g_chatFolded; // разговор — только по центру, без шторки справа
                if (i == 2) HomeLayout::SetEditing(!HomeLayout::Editing());
            }
            y += size + gapY;
        }
        (void)L;
    }

    void DrawGreeting(ImDrawList* dl, const Layout& L) {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        const char* hello = now.wHour < 5 ? "не спится" : now.wHour < 12 ? "доброе утро" : now.wHour < 18 ? "привет" : "добрый вечер";
        const std::string first = std::string(Lang::Tr(hello)) + ", ";
        const std::string name = Config::Get().name;

        const float y = L.orbCenter.y + L.orbRadius * 1.75f;
        const float w1 = Widgets::TextSize(Theme::hero, first.c_str()).x;
        const float w2 = Widgets::TextSize(Theme::hero, name.c_str()).x;
        const float x = L.screen.x * 0.5f - (w1 + w2) * 0.5f;
        Widgets::Text(dl, Theme::hero, ImVec2(x, y), Theme::kInkSoft, first.c_str());
        Widgets::Text(dl, Theme::hero, ImVec2(x + w1, y), Theme::kInk, name.c_str());

        std::string caption;
        bool live = false;
        Caption::Get(caption, live);
        if (g_app.transcribing) {
            caption = "секундочку…";
            live = true;
        }
        Widgets::TextWrappedCentered(dl, Theme::body, L.screen.x * 0.5f, y + Theme::hero->LegacySize * 1.6f,
            std::min(L.screen.x - Px(80), Px(720)), live ? Theme::kInk : Theme::kInkMuted, caption, 3);
    }

    // ---------------------------------------------------------------- снимок к вопросу
    // «+» → «снимок экрана» (окно ZAVIRIA на миг прячется) или Ctrl+V с картинкой — прикрепить; вопрос уходит нейросети вместе с ним
    std::mutex g_shotMutex;
    std::vector<uint8_t> g_shot;
    std::string g_attachText, g_attachName; // не картинка: текст файла или список папки
    std::atomic<bool> g_shotBusy{ false };
    bool HasShot() { std::lock_guard lock(g_shotMutex); return !g_shot.empty() || !g_attachName.empty(); }
    void SetShot(std::vector<uint8_t> jpeg) { std::lock_guard lock(g_shotMutex); g_shot = std::move(jpeg); g_attachText.clear(); g_attachName = g_shot.empty() ? "" : "снимок"; }

    // «+» → любой файл или папка: картинка уйдёт нейросети как картинка, текстовый файл — содержимым, папка — списком
    void AttachPath(const std::wstring& path) {
        const std::wstring name = path.substr(path.find_last_of(L"\\/") + 1);
        const DWORD attr = GetFileAttributesW(path.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) return;
        std::string text;
        std::vector<uint8_t> jpeg;
        std::wstring ext = name.find(L'.') != std::wstring::npos ? Text::LowerRu(name.substr(name.find_last_of(L'.'))) : L"";
        if (attr & FILE_ATTRIBUTE_DIRECTORY) {
            text = "Папка «" + Text::ToUtf8(name) + "», внутри:\n";
            WIN32_FIND_DATAW d;
            HANDLE f = FindFirstFileW((path + L"\\*").c_str(), &d);
            int count = 0;
            if (f != INVALID_HANDLE_VALUE) {
                do {
                    if (!wcscmp(d.cFileName, L".") || !wcscmp(d.cFileName, L"..")) continue;
                    if (++count > 200) { text += "…и ещё файлы\n"; break; }
                    text += (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? "[папка] " : "") + Text::ToUtf8(d.cFileName) + "\n";
                } while (FindNextFileW(f, &d));
                FindClose(f);
            }
        } else if (ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".bmp" || ext == L".gif" || ext == L".webp" || ext == L".tif" || ext == L".tiff") {
            jpeg = Screen::FileJpeg(path);
            if (jpeg.empty()) text = "Картинка «" + Text::ToUtf8(name) + "» — не получилось открыть";
        } else {
            WIN32_FILE_ATTRIBUTE_DATA info{};
            GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info);
            const unsigned long long size = (static_cast<unsigned long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
            std::string content;
            if (size < 200 * 1024) {
                if (FILE* fp = _wfopen(path.c_str(), L"rb")) { content.resize(static_cast<size_t>(size)); content.resize(fread(content.data(), 1, content.size(), fp)); fclose(fp); }
            }
            const bool binary = content.find('\0') != std::string::npos;
            if (!content.empty() && !binary) text = "Файл «" + Text::ToUtf8(name) + "»:\n" + content;
            else text = "Файл «" + Text::ToUtf8(name) + "», " + std::to_string(size / 1024) + " КБ (содержимое не текстовое)";
        }
        std::lock_guard lock(g_shotMutex);
        g_shot = std::move(jpeg);
        g_attachText = std::move(text);
        g_attachName = Text::ToUtf8(name);
    }

    void PickAttachment(bool folder) {
        if (g_shotBusy.exchange(true)) return;
        std::thread([folder] {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            IFileOpenDialog* dialog = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
                if (folder) { DWORD o = 0; dialog->GetOptions(&o); dialog->SetOptions(o | FOS_PICKFOLDERS); }
                IShellItem* item = nullptr;
                PWSTR path = nullptr;
                if (SUCCEEDED(dialog->Show(g_window)) && SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    AttachPath(path);
                    CoTaskMemFree(path);
                }
                if (item) item->Release();
                dialog->Release();
            }
            CoUninitialize();
            g_focusInput = true;
            g_shotBusy = false;
        }).detach();
    }

    void TakeScreenShot() {
        if (g_shotBusy.exchange(true)) return;
        std::thread([] {
            const HWND window = g_window;
            ShowWindow(window, SW_MINIMIZE);
            Sleep(450); // окно успело уйти — снимаем то, что под ним
            auto jpeg = Screen::CaptureJpeg(1600, 85);
            ShowWindow(window, SW_RESTORE);
            SetForegroundWindow(window);
            if (!jpeg.empty()) SetShot(std::move(jpeg));
            g_focusInput = true;
            g_shotBusy = false;
        }).detach();
    }

    void AskAboutShot(std::string question) {
        std::vector<uint8_t> jpeg;
        std::string text, name;
        { std::lock_guard lock(g_shotMutex); jpeg.swap(g_shot); text.swap(g_attachText); name.swap(g_attachName); }
        if (jpeg.empty()) { // файл или папка — содержимым в вопросе
            if (question.empty()) question = "Что это? Расскажи коротко.";
            Chat::Add(Role::User, "📎 " + name + " · " + question);
            const size_t at = Chat::Add(Role::Anya, Lang::Tr("смотрю…"));
            std::thread([at, question, text] {
                const std::string answer = Ai::Quiet(question + "\n\n" + text, "", false);
                Chat::Update(at, answer.empty() ? Lang::Tr("Не получилось — нейросеть не ответила") : answer);
            }).detach();
            return;
        }
        if (question.empty()) question = "Что на этом снимке? Опиши коротко.";
        Chat::Add(Role::User, "🖼 " + question);
        if (!Ai::CanSee()) {
            Chat::Add(Role::Anya, Lang::Tr("Чтобы смотреть на картинки, нужна модель с «глазами» (файл mmproj рядом с моделью) или облачный ИИ."));
            return;
        }
        const size_t at = Chat::Add(Role::Anya, Lang::Tr("смотрю на снимок…"));
        std::thread([at, question, jpeg = std::move(jpeg)] {
            const std::string answer = Ai::Quiet(question, Screen::Base64(jpeg), false);
            Chat::Update(at, answer.empty() ? Lang::Tr("Не получилось — нейросеть не ответила") : answer);
        }).detach();
    }

    // ---------------------------------------------------------------- поле ввода
    void SubmitInput() {
        if (HasShot()) { AskAboutShot(Text::Trim(g_input)); g_input[0] = '\0'; g_focusInput = true; return; }
        if (Text::Trim(g_input).empty()) return;
        Brain::Interrupt();
        Brain::Submit(g_input);
        g_input[0] = '\0';
        g_focusInput = true;
    }

    // ширина поля ввода: в узком окне поле сужается, чтобы по бокам остались погода и «Система»
    float ComposerWidth(ImVec2 screen, float scale) {
        float w = std::min(screen.x - Px(48), Px(820) * scale);
        const float side = std::max(WeatherWidget::MiniWidth(), HomeWidgets::Width()) + Px(48);
        if (screen.x - side * 2 >= Px(440)) w = std::min(w, screen.x - side * 2);
        return w;
    }

    // текстовая кнопка-ссылка (значок + подпись), как в строке настроек под полем Astra
    bool LinkButton(const char* id, ImVec2 pos, Icon icon, const char* label, ImU32 ink, ImFont* font, float* width = nullptr) {
        const char* shown = Lang::Tr(label);
        const float icon_ = icon == Icon::Plus ? 0.0f : Px(14), gap = icon_ > 0 ? Px(7) : 0.0f;
        const ImVec2 t = Widgets::TextSize(font, shown);
        const ImVec2 size(icon_ + gap + t.x + Px(16), std::max(Px(30), t.y + Px(10)));
        if (width) *width = size.x;
        ImGui::SetCursorScreenPos(pos);
        const bool pressed = ImGui::InvisibleButton(id, size);
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (hovered) { dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), Theme::Overlay(14), Px(8)); ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); }
        const float cy = pos.y + size.y * 0.5f;
        if (icon_ > 0) Widgets::DrawIcon(dl, icon, ImVec2(pos.x + Px(8) + icon_ * 0.5f, cy), icon_, ink);
        Widgets::Text(dl, font, ImVec2(pos.x + Px(8) + icon_ + gap, cy - font->LegacySize * 0.55f), ink, shown);
        return pressed;
    }

    // Поле ввода по образцу Astra: большое, в две строки. Сверху — «+», текст моноширинным шрифтом, микрофон и отправка;
    // снизу — «слушать всегда», «говорить» и какая нейросеть отвечает (нажми — откроются её настройки)
    void DrawComposer(ImDrawList* dl, const Layout& L, Mood mood) {
        const float width = ComposerWidth(L.screen, L.dockScale);
        const ImVec2 p(std::round(L.dockCenterX - width * 0.5f), std::round(L.dockTop));
        const ImVec2 max(p.x + width, p.y + L.dockHeight);
        const bool typing = ImGui::GetIO().WantTextInput;
        Theme::Plate(dl, p, max, Theme::Surface(0.94f), Px(24), typing ? 0.8f : 0.0f);
        const float row1 = p.y + Px(30), row2 = max.y - Px(26);

        // «+» — быстрые фразы
        if (Widgets::IconButton("##composerPlus", ImVec2(p.x + Px(12), row1 - Px(18)), Px(36), Icon::Plus, false, false)) ImGui::OpenPopup("##quickMenu");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Lang::Tr("отправить файл (Ctrl+V — картинку)"));
        if (Widgets::BeginPopupAnimated("##quickMenu")) { // «+» — только отправить Ане файл: картинку, документ или папку
            if (Widgets::ComboItem(Lang::Tr("файл или картинка…"), false)) { ImGui::CloseCurrentPopup(); PickAttachment(false); }
            if (Widgets::ComboItem(Lang::Tr("папка…"), false)) { ImGui::CloseCurrentPopup(); PickAttachment(true); }
            Widgets::EndPopupAnimated();
        }

        // справа: микрофон (идёт разговор — «стоп») и отправка
        const float btn = Px(40);
        const bool busy = mood == Mood::Speaking || mood == Mood::Thinking;
        const ImVec2 micPos(max.x - Px(12) - btn, row1 - btn * 0.5f);
        if (mood == Mood::Listening) {
            const float wave = std::fmod(g_time * 1.2f, 1.0f);
            dl->AddCircle(ImVec2(micPos.x + btn * 0.5f, micPos.y + btn * 0.5f), btn * (0.5f + wave * 0.4f), Palette(0).WithAlpha(0.8f * (1.0f - wave)), 32, Px(2));
        }
        if (mood == Mood::Listening || busy)
            dl->AddRectFilled(micPos, ImVec2(micPos.x + btn, micPos.y + btn), Palette(0).WithAlpha(0.9f), Px(12));
        if (Widgets::IconButton("##mic", micPos, btn, busy ? Icon::Stop : Icon::Mic, false, false)) Brain::ToggleListening();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Lang::Tr(busy ? "остановить" : "говорить голосом (пробел)"));
        const bool hasText = !Text::Trim(g_input).empty() || HasShot();
        const ImVec2 sendPos(micPos.x - Px(6) - btn, micPos.y);
        bool clicked = false;
        if (hasText) {
            dl->AddRectFilled(sendPos, ImVec2(sendPos.x + btn, sendPos.y + btn), Theme::Accent().WithAlpha(1.0f), Px(12));
            clicked = Widgets::IconButton("##send", sendPos, btn, Icon::Send, false, false);
        }

        // текст — моноширинным шрифтом, без рамки
        const float left = p.x + Px(56), right = (hasText ? sendPos.x : micPos.x) - Px(10);
        ImGui::PushFont(Theme::mono, Theme::mono->LegacySize);
        ImGui::SetCursorScreenPos(ImVec2(left, row1 - ImGui::GetFrameHeight() * 0.5f));
        ImGui::PushItemWidth(right - left);
        for (ImGuiCol col : { ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive, ImGuiCol_Border })
            ImGui::PushStyleColor(col, IM_COL32(0, 0, 0, 0));
        if (g_focusInput) { ImGui::SetKeyboardFocusHere(); g_focusInput = false; }
        const bool enter = ImGui::InputTextWithHint("##input", HasShot() ? "Что спросить про файл? (Enter — «что это»)" : "Пиши~", g_input, sizeof(g_input), ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemActive() && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false) && Screen::ClipboardHasImage()) { // Ctrl+V с картинкой — прикрепить
            auto jpeg = Screen::ClipboardJpeg();
            if (!jpeg.empty()) SetShot(std::move(jpeg));
        }
        ImGui::PopStyleColor(4);
        ImGui::PopItemWidth();
        ImGui::PopFont();
        if (enter || clicked) SubmitInput();

        // нижняя строка поля
        ImFont* f = Theme::mono;
        float w = 0;
        bool wake = Config::Get().wakeWord;
        const std::string wakeLabel = std::string(Lang::Tr("слушать всегда")) + (wake ? " · " + Config::Get().assistant : std::string());
        if (LinkButton("##wakeChip", ImVec2(p.x + Px(14), row2 - Px(15)), Icon::Wave, wakeLabel.c_str(), wake ? Theme::Accent().Mix(Theme::Light() ? Theme::Color{ 40, 20, 60 } : Theme::Color{ 255, 255, 255 }, 0.25f).WithAlpha(1.0f) : Theme::kInkMuted, f, &w)) {
            wake = !wake;
            Config::Update([&](Settings& x) { x.wakeWord = wake; });
            Caption::Set(IdleHint(wake));
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Lang::Tr(wake ? "Аня ждёт своё имя — нажми, чтобы выключить" : "включить: Аня будет ждать своё имя"));
        float cx = p.x + Px(14) + w + Px(6);
        if (UserMessages() > 0) { // чат: стрелка вверх — показать, вниз — свернуть
            const bool shown = !g_chatFolded;
            float cw = 0;
            if (LinkButton("##chatChip", ImVec2(cx, row2 - Px(15)), Icon::Chat, "чат    ", shown ? Theme::kInkSoft : Theme::kInkMuted, f, &cw)) g_chatFolded = shown;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Lang::Tr(shown ? "свернуть разговор" : "показать разговор"));
            const ImVec2 r1 = ImGui::GetItemRectMax();
            const ImVec2 c(r1.x - Px(16), (ImGui::GetItemRectMin().y + r1.y) * 0.5f);
            const float d = Px(4), dy = shown ? Px(2) : -Px(2);
            const ImVec2 pts[3] = { ImVec2(c.x - d, c.y - dy), ImVec2(c.x, c.y + dy), ImVec2(c.x + d, c.y - dy) };
            dl->AddPolyline(pts, 3, Theme::kInkSoft, 0, Px(1.6f));
            cx += cw + Px(6);
        }
        if (HasShot() || g_shotBusy) { // прикреплённый снимок: нажми — убрать
            std::string chip;
            { std::lock_guard lock(g_shotMutex); chip = g_attachName; }
            if (chip.size() > 28) chip = chip.substr(0, 26) + "…";
            chip = g_shotBusy ? std::string(Lang::Tr("выбираю…")) : "📎 " + chip + " ✕";
            if (LinkButton("##shotChip", ImVec2(cx, row2 - Px(15)), Icon::Layers, chip.c_str(), Theme::Accent().WithAlpha(1.0f), f) && !g_shotBusy) { std::lock_guard lock(g_shotMutex); g_shot.clear(); g_attachText.clear(); g_attachName.clear(); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Lang::Tr("снимок уйдёт вместе с вопросом — нажми, чтобы убрать"));
        }
        { // справа: какая нейросеть отвечает
            std::string model = Ai::ActiveModel();
            if (const size_t dot = model.rfind('.'); dot != std::string::npos && dot > 0) model = model.substr(0, dot);
            if (model.empty()) model = Lang::Tr("нейросеть не выбрана");
            model = FitText(f, f->LegacySize, model, Px(220));
            const std::string label = std::string(Lang::Tr("ИИ")) + " · " + model;
            const float lw = Widgets::TextSize(f, label.c_str()).x + Px(16) + Px(21);
            if (LinkButton("##modelChip", ImVec2(max.x - Px(14) - lw, row2 - Px(15)), Icon::Spark, label.c_str(), Theme::kInkSoft, f))
                SettingsPage::Open(3);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Lang::Tr("настройки нейросети"));
        }
    }

    // под полем — быстрые действия (настоящие): весь разговор, микрофон, «что ты умеешь?»
    void DrawQuickLinks(const Layout& L) {
        const bool mic = g_app.micEnabled;
        struct Q { const char* id; Icon icon; const char* label; };
        (void)mic;
        const Q items[] = { { "##qHistory", Icon::Chevron, "весь разговор" }, { "##qSkills", Icon::Spark, "что ты умеешь?" } };
        constexpr int kItems = 2;
        float total = 0, widths[kItems];
        for (int i = 0; i < kItems; ++i) { widths[i] = Px(14) + Px(7) + Widgets::TextSize(Theme::mono, Lang::Tr(items[i].label)).x + Px(16); total += widths[i]; }
        total += Px(40) * (kItems - 1);
        float x = std::round(L.dockCenterX - total * 0.5f);
        const float y = L.dockTop + L.dockHeight + Px(14);
        for (int i = 0; i < kItems; ++i) {
            if (LinkButton(items[i].id, ImVec2(x, y), items[i].icon, items[i].label, i == 0 ? Theme::kInkMuted : Theme::kInkSoft, Theme::mono)) {
                if (i == 0) g_chatFolded = false;
                if (i == 1) { Brain::Interrupt(); Brain::Submit("что ты умеешь?"); }
            }
            x += widths[i] + Px(40);
        }
    }

    void DrawQuickPhrases(const Layout& L) {
        if (!Config::Get().quickPhrases) return;
        static const char* kPhrases[] = { "который час?", "скажи что-нибудь милое", "открой стим", "что ты умеешь?" };
        const float gap = Px(8);
        float total = -gap;
        for (const char* phrase : kPhrases) total += Widgets::TextSize(Theme::semibold, phrase).x + Px(28) + gap;
        if (total > L.screen.x - Px(32)) return; // окно слишком узкое — прячем подсказки

        float x = L.dockCenterX - total * 0.5f;
        const float y = L.dockTop + L.dockHeight + Px(14);
        for (int i = 0; i < IM_ARRAYSIZE(kPhrases); ++i) {
            ImGui::PushID(i);
            ImVec2 size;
            if (Widgets::Chip("##chip", ImVec2(x, y), kPhrases[i], &size)) {
                Brain::Interrupt();
                Brain::Submit(kPhrases[i]);
            }
            ImGui::PopID();
            x += size.x + gap;
        }
    }

    // ---------------------------------------------------------------- нижняя строка
    // Строка статуса: имя слева, «таблетки» AI · STT · TTS по центру (подсвечены, когда готовы), время ответа рядом
    void DrawFooter(ImDrawList* dl, const Layout& L) {
        const ImVec2 shift = HomeLayout::Shift("footer", L.screen); // строку можно поднять наверх — двигается целиком
        const float y = L.screen.y - Px(30) + shift.y;
        const float left = Px(22) + shift.x, right = L.screen.x + shift.x;
        (void)left; // ник слева убран по просьбе владелицы

        struct Chip { Icon icon; const char* label; int state; };
        const Chip chips[] = { { Icon::Spark, "AI", g_app.aiChecked ? (g_app.aiReady ? 1 : 2) : 0 },
                               { Icon::Mic, "STT", g_app.audioReady && g_app.micEnabled ? 1 : 0 },
                               { Icon::Wave, "TTS", g_app.ttsReady ? 1 : 0 } };
        const float chipH = Px(26), gap = Px(6);
        float total = -gap;
        for (const Chip& c : chips) total += Px(34) + Widgets::TextSize(Theme::semibold, c.label).x + gap;
        float x = L.screen.x * 0.5f - total * 0.5f + shift.x;
        for (const Chip& c : chips) {
            const float w = Px(34) + Widgets::TextSize(Theme::semibold, c.label).x;
            const ImVec2 a(x, y - Px(5)), b(x + w, y - Px(5) + chipH);
            const bool on = c.state == 1;
            dl->AddRectFilled(a, b, on ? Theme::Accent().WithAlpha(0.16f) : Theme::Overlay(8), chipH * 0.5f);
            dl->AddRect(a, b, on ? Theme::Accent().WithAlpha(0.35f) : Theme::Overlay(18), chipH * 0.5f);
            const ImU32 color = c.state == 2 ? Theme::kBad : on ? Theme::kInk : Theme::kInkMuted;
            Widgets::DrawIcon(dl, c.icon, ImVec2(a.x + Px(14), (a.y + b.y) * 0.5f), Px(12), color);
            Widgets::Text(dl, Theme::semibold, ImVec2(a.x + Px(24), (a.y + b.y) * 0.5f - Theme::semibold->LegacySize * 0.5f), color, c.label);
            x += w + gap;
        }
        if (const int latency = g_app.lastLatencyMs.load(); latency >= 0) {
            char text[48];
            sprintf_s(text, "%.1f с", latency / 1000.0f);
            Widgets::Text(dl, Theme::semibold, ImVec2(x + Px(8), y), Theme::kInkMuted, text);
        }

        // «слушать всегда» теперь в поле ввода — здесь остаётся только «экономия», у правого края
        const float toggleX = right - Px(12);
        // «экономия» — левее; если окно узкое и место занято таблетками по центру, не показываем
        const char* ecoLabel = Lang::Tr("игровой режим");
        const float ecoX = toggleX - Px(40) - Widgets::TextSize(Theme::semibold, ecoLabel).x;
        if (EcoMode::Active() && ecoX > x + Px(70)) { // игровой режим убран: переключатель виден, только если он остался включённым — чтобы выключить
            bool eco = EcoMode::Active();
            if (Widgets::Toggle("##ecomode", ImVec2(ecoX, y - Px(1)), &eco, Theme::Accent().WithAlpha(1.0f))) EcoMode::RequestToggle();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Lang::Tr(EcoMode::Busy() ? "переключаю…" : "Игровой режим: фоновые программы притихнут, все ресурсы — игре"));
            Widgets::Text(dl, Theme::semibold, ImVec2(ecoX + Px(40), y), EcoMode::Active() ? Theme::kInk : Theme::kInkMuted, ecoLabel);
        }
        HomeLayout::Report("footer", "нижняя строка", ImVec2(left - Px(10), y - Px(10)), ImVec2(right - Px(12), y + Px(26)), HomeLayout::kFixed);
    }

    void HandleHotkeys() {
        const ImGuiIO& io = ImGui::GetIO();
        if (!io.WantTextInput && !SettingsPage::IsOpen() && !ScenariosPage::IsOpen() && !ProcessesPage::IsOpen()) {
            if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) Brain::ToggleListening();
            if (ImGui::IsKeyPressed(ImGuiKey_H, false)) g_chatFolded = !g_chatFolded;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !HomeLayout::Editing()) { // в редактировании Esc только выходит из него
            if (SettingsPage::IsOpen()) SettingsPage::Close();
            else if (ScenariosPage::IsOpen()) { if (!ScenariosPage::KeepsEscape()) ScenariosPage::Close(); } // в «Рисую сама» Esc выходит из рисования
            else if (ProcessesPage::IsOpen()) ProcessesPage::Close();
            else if (g_historyOpen) g_historyOpen = false;
            else {
                Brain::Interrupt();
                g_app.pttArmed = false;
            }
        }
    }

    // ---------------------------------------------------------------- история
    void DrawHistory(const Layout& L, float dt) {
        g_historyAnim += ((g_historyOpen ? 1.0f : 0.0f) - g_historyAnim) * Theme::Ease(dt, 7.4f);
        if (g_historyAnim < 0.01f) return;

        const float width = std::min(Px(400) * HomeLayout::Scale("history"), L.screen.x - Px(24));
        const float ease = 1.0f - std::pow(1.0f - g_historyAnim, 3.0f);
        const float shiftX = std::min(0.0f, HomeLayout::Shift("history", L.screen).x); // шторку можно отодвинуть от правого края
        const float panelX = std::max(Px(12), L.screen.x - Px(12) - width * ease + (1.0f - ease) * Px(24) + shiftX);
        ImGui::SetNextWindowPos(ImVec2(panelX, g_titleHeight + Px(4)));
        HomeLayout::Report("history", "шторка разговора", ImVec2(panelX, g_titleHeight + Px(4)), ImVec2(panelX + width, L.screen.y - Px(12)), HomeLayout::kBoth);
        ImGui::SetNextWindowSize(ImVec2(width, L.screen.y - g_titleHeight - Px(16)));

        ImGui::Begin("##history", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetWindowPos();
        Widgets::Text(dl, Theme::display, ImVec2(origin.x + Px(20), origin.y + Px(18)), Theme::kInk, "разговор");

        ImGui::SetCursorPos(ImVec2(width - Px(150), Px(18)));
        ImGui::PushFont(Theme::semibold, Theme::semibold->LegacySize);
        if (ImGui::Button("очистить")) {
            Chat::Clear();
            Conversation::Clear();
        }
        ImGui::PopFont();
        if (Widgets::IconButton("##historyClose", ImVec2(origin.x + width - Px(48), origin.y + Px(14)), Px(32), Icon::Close)) g_historyOpen = false;

        ImGui::SetCursorPos(ImVec2(Px(16), Px(64)));
        ImGui::BeginChild("##messages", ImVec2(width - Px(32), 0));
        const auto messages = Chat::Snapshot();
        const float contentWidth = ImGui::GetContentRegionAvail().x;
        if (messages.empty()) {
            const char* empty = "тут будет наш разговор ✦";
            ImGui::SetCursorPos(ImVec2((contentWidth - Widgets::TextSize(Theme::body, empty).x) * 0.5f, Px(120)));
            ImGui::TextDisabled("%s", empty);
        }
        for (const auto& message : messages) Widgets::ChatBubble(message, contentWidth);

        // прокрутка вниз, когда появилось новое или дописывается ответ
        static size_t s_lastCount = 0, s_lastLength = 0;
        const size_t length = messages.empty() ? 0 : messages.back().text.size();
        if (messages.size() != s_lastCount || length != s_lastLength) ImGui::SetScrollHereY(1.0f);
        s_lastCount = messages.size();
        s_lastLength = length;

        ImGui::EndChild();
        ImGui::End();
    }

}

// ---------------------------------------------------------------------
void Ui::Init(HWND window) {
    g_window = window;
    g_titleHeight = Px(64); // меню «парит» чуть ниже края окна
    Visuals::Init();
}

void Ui::Frame(float dt, bool active) {
    g_time += dt;
    g_intro = std::min(1.0f, g_intro + dt * 1.1f);
    static bool s_setupChecked = false;
    if (!s_setupChecked) { s_setupChecked = true; if (SetupWizard::Needed()) SetupWizard::Open(); } // первый запуск — знакомство
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const Mood mood = CurrentMood();
    const Settings settings = Config::Get();
    SyncWallpaper(settings);
    Theme::Apply(settings);
    Theme::UpdatePalette(mood, dt);

    Layout L;
    L.screen = screen;
    // логотип (или персонаж из «Кто в центре») — маленький знак над надписью; нажми — и говори
    L.orbRadius = Px(20) * (0.7f + 0.3f * Intro(0.0f));
    L.orbCenter = ImVec2(screen.x * 0.5f, std::round(g_titleHeight + (screen.y - g_titleHeight) * 0.34f) - Px(86));
    // по образцу Astra: крупная надпись чуть выше середины, под ней — большое поле ввода и быстрые действия
    L.heroY = std::round(g_titleHeight + (screen.y - g_titleHeight) * 0.34f);
    L.dockHeight = Px(112);
    L.dockTop = L.heroY + Px(118) + (1.0f - Intro(0.25f)) * Px(30); // поле ввода выезжает снизу
    g_chatAnim += ((HasConversation() ? 1.0f : 0.0f) - g_chatAnim) * Theme::Ease(dt, 6.0f);
    { // идёт разговор — поле ввода плавно уезжает вниз, освобождая место под ленту
        const float e = g_chatAnim * g_chatAnim * (3 - 2 * g_chatAnim);
        L.dockTop += (screen.y - L.dockHeight - Px(104) - L.dockTop) * e; // ниже — быстрые действия и нижняя строка
    }
    L.dockCenterX = screen.x * 0.5f;
    { // «Полная кастомизация»: свои места и размеры блоков
        const ImVec2 orb = HomeLayout::Shift("orb", screen), dock = HomeLayout::Shift("dock", screen);
        L.orbCenter = ImVec2(L.orbCenter.x + orb.x, L.orbCenter.y + orb.y);
        L.orbRadius *= HomeLayout::Scale("orb");
        L.dockTop += dock.y;
        L.dockCenterX += dock.x;
        L.dockScale = HomeLayout::Scale("dock");
    }

    ImDrawList* background = ImGui::GetBackgroundDrawList();
    Visuals::BackdropOptions backdrop;
    backdrop.style = static_cast<int>(settings.backdrop);
    backdrop.speed = settings.backdropSpeed / 100.0f;
    backdrop.animate = (active || settings.animateInactive) && !EcoMode::Active(); // экономия: фон замирает
    // акрил Windows виден только на главном экране; под страницами фон Ани плотный — сквозь них видно её, а не рабочий стол
    const float page = std::max({ SettingsPage::Visibility(), ScenariosPage::Visibility(), ProcessesPage::Visibility() });
    const float effectOpacity = Graphics::Transparent() && settings.windowEffect > 0 ? settings.windowOpacity / 100.0f : 1.0f;
    backdrop.opacity = effectOpacity + (1.0f - effectOpacity) * page;
    backdrop.wallpaperDim = settings.wallpaperDim / 100.0f;
    backdrop.customColor = settings.animationColor >= 0;
    if (backdrop.customColor) backdrop.color = Theme::FromRgb(settings.animationColor);
    Visuals::DrawBackground(background, screen, dt, backdrop);
    if (page < 0.97f && g_chatAnim < 0.5f) Visuals::DrawOrb(background, L.orbCenter, L.orbRadius, mood, g_time, dt); // под страницами сфера не просвечивает; в чате её место занимает лента
    if (!IsZoomed(g_window)) { // в окне — мягкий край: фон и рисунки плавно растворяются к рамке, по контуру тонкий свет
        // два слоя: широкий и слабый + узкий у самой рамки — переход без заметной границы
        const ImU32 clear = Theme::Background(1).WithAlpha(0.0f);
        static const float kBands[3] = { 260.0f, 130.0f, 46.0f }, kAlpha[3] = { 0.24f, 0.26f, 0.3f }; // три слоя — переход совсем незаметный
        for (int layer = 0; layer < 3; ++layer) {
            const float band = Px(kBands[layer]);
            const ImU32 edge = Theme::Background(1).WithAlpha(kAlpha[layer]);
            background->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(band, screen.y), edge, clear, clear, edge);
            background->AddRectFilledMultiColor(ImVec2(screen.x - band, 0), screen, clear, edge, edge, clear);
            background->AddRectFilledMultiColor(ImVec2(0, screen.y - band), screen, clear, clear, edge, edge);
            background->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(screen.x, band * 0.6f), edge, edge, clear, clear);
        }
    }

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(screen);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f); // без рамки ImGui по краю всего окна — это была светлая линия у края
    ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    DrawHeader(dl, L, mood, dt);
    const bool covered = SettingsPage::Covers() || ScenariosPage::Covers() || ProcessesPage::Covers(); // под страницами главный экран не рисуем — не просвечивает и быстрее
    if (!covered) {
        if (g_chatAnim < 0.5f) { DrawOrbButton(L); DrawHero(dl, L); }
        if (g_chatAnim > 0.02f) DrawFeed(L, std::clamp((g_chatAnim - 0.3f) / 0.7f, 0.0f, 1.0f));
        DrawComposer(dl, L, mood);
        DrawQuickLinks(L);
        // значки слева убраны по просьбе владелицы
        DrawFooter(dl, L);
    }
    // погода слева — если между краем окна и кольцами сферы хватает места (рисуем последней: раскрытая — поверх всего)
    const float weatherLeft = Px(20) - (1.0f - Intro(0.35f)) * Px(40); // выезжает слева
    const bool pageOpen = SettingsPage::IsOpen() || ScenariosPage::IsOpen() || ProcessesPage::IsOpen();
    const bool editing = HomeLayout::Editing();
    if (!covered) {
        const float dockW = std::min(screen.x - Px(32), Px(720) * L.dockScale);
        HomeLayout::Report("orb", "сфера", ImVec2(L.orbCenter.x - L.orbRadius * 1.15f, L.orbCenter.y - L.orbRadius * 1.15f),
                           ImVec2(L.orbCenter.x + L.orbRadius * 1.15f, L.orbCenter.y + L.orbRadius * 1.15f), HomeLayout::kBoth);
        const float realW = ComposerWidth(screen, L.dockScale); (void)dockW; // рамка ровно по полю ввода (обучение подсвечивало шире)
        HomeLayout::Report("dock", "поле ввода", ImVec2(std::round(L.dockCenterX - realW * 0.5f), std::round(L.dockTop)), ImVec2(std::round(L.dockCenterX - realW * 0.5f) + realW, std::round(L.dockTop) + L.dockHeight), HomeLayout::kBoth);
    }
    const ImVec2 weatherShift = HomeLayout::Shift("weather", screen), widgetsShift = HomeLayout::Shift("widgets", screen);
    const bool custom = editing || HomeLayout::Moved("orb"); // свои места — показываем всё, тесноту решает сам пользователь
    const bool weatherMoved = custom || HomeLayout::Moved("weather"), widgetsMoved = custom || HomeLayout::Moved("widgets") || HomeWidgets::AnyMoved();
    const float sideRoom = (screen.x - ComposerWidth(screen, L.dockScale)) * 0.5f; // место по бокам от поля
    if (!covered && settings.weatherWidget && HomeWidgets::Visible() && (weatherMoved || sideRoom > WeatherWidget::MiniWidth() + Px(40))) {
        const float centerY = g_titleHeight + (screen.y - g_titleHeight) * 0.36f + weatherShift.y; // место сферы по умолчанию + свой сдвиг
        WeatherWidget::Draw(dl, weatherLeft + weatherShift.x, centerY, g_titleHeight + Px(8), weatherMoved ? screen.y - Px(12) : L.dockTop - Px(12), !pageOpen && !editing, dt);
        ImVec2 a, b;
        WeatherWidget::Bounds(a, b);
        HomeLayout::Report("weather", "погода", a, b, HomeLayout::kFixed);
    }
    // справа — «Система» и «Сейчас играет», если между сферой и краем хватает места
    if (!covered && (widgetsMoved || sideRoom > HomeWidgets::Width() + Px(40))) {
        HomeWidgets::Draw(dl, screen.x - Px(20) + widgetsShift.x, g_titleHeight + Px(12) + widgetsShift.y, widgetsMoved ? screen.y - Px(12) : L.dockTop - Px(12), dt, Intro(0.45f), widgetsMoved ? 0.0f : L.dockCenterX + ComposerWidth(screen, L.dockScale) * 0.5f + Px(16)); // не заходит на поле и надпись
        // карточки сообщают о себе сами (w_clock, w_system…) — каждую можно двигать отдельно
    }
    if (!pageOpen && Config::Value("widgets", "clock", "1") == "1") { // время — просто текстом под меню, без плашки (выключается в «Виджетах»)
        SYSTEMTIME now{};
        GetLocalTime(&now);
        static const char* kDays[] = { "воскресенье", "понедельник", "вторник", "среда", "четверг", "пятница", "суббота" };
        static const char* kMonths[] = { "января", "февраля", "марта", "апреля", "мая", "июня", "июля", "августа", "сентября", "октября", "ноября", "декабря" };
        char time[8];
        snprintf(time, sizeof(time), "%02d:%02d", now.wHour, now.wMinute);
        const std::string date = Lang::IsEnglish() ? std::string(Lang::Tr(kMonths[now.wMonth - 1])) + " " + std::to_string(now.wDay) + ", " + Lang::Tr(kDays[now.wDayOfWeek])
                                                   : std::to_string(now.wDay) + " " + kMonths[now.wMonth - 1] + ", " + kDays[now.wDayOfWeek];
        const ImVec2 shift = HomeLayout::Shift("w_clock", screen);
        const ImVec2 ts = Widgets::TextSize(Theme::display, time), ds = Widgets::TextSize(Theme::semibold, date.c_str());
        const float top = g_navMax.y + Px(14) + shift.y, cx = screen.x * 0.5f + shift.x;
        const float alpha = Intro(0.3f);
        const ImU32 ink = Theme::kInk, muted = Theme::kInkMuted;
        auto fade = [&](ImU32 c) { return (c & 0x00FFFFFF) | (static_cast<ImU32>(((c >> 24) & 0xFF) * alpha) << 24); };
        Widgets::Text(dl, Theme::display, ImVec2(std::round(cx - ts.x * 0.5f), top), fade(ink), time);
        Widgets::Text(dl, Theme::semibold, ImVec2(std::round(cx - ds.x * 0.5f), top + ts.y + Px(2)), fade(muted), date.c_str());
        const float w = std::max(ts.x, ds.x);
        HomeLayout::Report("w_clock", "время", ImVec2(cx - w * 0.5f - Px(8), top - Px(4)), ImVec2(cx + w * 0.5f + Px(8), top + ts.y + ds.y + Px(8)), HomeLayout::kFixed);
    }
    HandleHotkeys();
    ImGui::End();

    (void)&DrawHistory; // шторка «разговор» справа больше не показывается: чат только по центру
    if (SettingsPage::PollReveal()) { ScenariosPage::Close(); ProcessesPage::Close(); } // «где настройка …?»
    if (ScenariosPage::PollAdded()) { SettingsPage::Close(); ProcessesPage::Close(); }   // Аня собрала сценарий
    if (SettingsPage::IsOpen() && ScenariosPage::IsOpen()) ScenariosPage::Close(); // открыта может быть только одна страница
    if (SettingsPage::IsOpen() && ProcessesPage::IsOpen()) ProcessesPage::Close();
    if (ScenariosPage::IsOpen() && ProcessesPage::IsOpen()) ProcessesPage::Close(); // две страницы сразу — «Процессы» просвечивали под «Сценариями»
    ScenariosPage::Draw(screen, g_titleHeight, dt);
    ProcessesPage::Draw(screen, g_titleHeight, dt);
    SettingsPage::Draw(screen, g_titleHeight, dt);
    { // «Редактирование»: на «Сценариях» правый клик занят блоками схемы
        const char* page = ScenariosPage::IsOpen() ? nullptr : SettingsPage::IsOpen() ? "set_" : ProcessesPage::IsOpen() ? "proc_" : "";
        HomeLayout::ContextMenu(page && !SetupWizard::IsOpen() && !Tour::Active());
        HomeLayout::DrawEditor(screen, g_titleHeight, page);
    }
    SetupWizard::Draw(screen, g_titleHeight, dt);
    Tour::Draw(screen, dt, SetupWizard::IsOpen());
    g_app.setupOpen = SetupWizard::IsOpen() || Tour::Active();
    Toast::Draw(screen, dt);
    EcoMode::Draw(screen.x, screen.y, dt); // окно «что будет ограничено» и итог выключения
    ApplyWindowOptions(settings);
}

void Ui::SetHistory(bool open) { g_chatFolded = !open; }

float Ui::TitleHeight() { return g_titleHeight; } // настройки начинаются под шапкой — её можно тянуть всегда

bool Ui::IsCaption(float x, float y) {
    if (y >= g_titleHeight || x >= g_titleButtonsLeft - Px(8) || HomeLayout::HitsBlock(x, y)) return false;
    return !(x >= g_navMin.x && x <= g_navMax.x && y >= g_navMin.y && y <= g_navMax.y);
}
