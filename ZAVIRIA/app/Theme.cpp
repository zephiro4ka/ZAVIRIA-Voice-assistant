#include "Theme.h"

#include "Platform.h"
#include "Settings.h"
#include "TextUtils.h"

#include "../ImGui/imgui_internal.h"

#include <algorithm>
#include <cstdlib>
#include <string>

float Theme::dpi = 1.0f;
ImFont* Theme::body = nullptr;
ImFont* Theme::semibold = nullptr;
ImFont* Theme::display = nullptr;
ImFont* Theme::hero = nullptr;
ImFont* Theme::plain = nullptr;
ImFont* Theme::title = nullptr;
ImFont* Theme::poster = nullptr;
ImFont* Theme::mono = nullptr;
ImU32 Theme::kInk = IM_COL32(244, 239, 250, 255);
ImU32 Theme::kInkSoft = IM_COL32(244, 239, 250, 170);
ImU32 Theme::kInkMuted = IM_COL32(244, 239, 250, 110);

namespace {
    using Theme::Color;

    constexpr Color kPalettes[4][3] = {
        { { 167, 139, 250 }, { 244, 114, 182 }, { 99, 102, 241 } },  // отдыхаю — сирень и розовый
        { { 34, 211, 238 }, { 110, 231, 183 }, { 129, 140, 248 } },  // слушаю — бирюза и мята
        { { 251, 191, 36 }, { 192, 132, 252 }, { 244, 114, 182 } },  // думаю — золото
        { { 251, 113, 133 }, { 253, 186, 116 }, { 216, 180, 254 } }, // говорю — коралл и персик
    };
    Color g_palette[3] = { kPalettes[0][0], kPalettes[0][1], kPalettes[0][2] };
    Color g_accent = { 180, 160, 250 };
    Theme::LookColors g_look = { { 40, 9, 38 }, { 16, 3, 18 }, { 48, 15, 46 } };
    bool g_glass = true;
    bool g_plates = true;

    struct FontFace { const char* name; const char* regular; const char* bold; };
    constexpr FontFace kFaces[] = {
        { "Системный (как в Windows)", "segoeui.ttf", "seguisb.ttf" }, { "Segoe UI", "segoeui.ttf", "seguisb.ttf" }, { "Consolas (как раньше)", "consola.ttf", "consolab.ttf" },
        { "Bahnschrift", "bahnschrift.ttf", "bahnschrift.ttf" }, { "Calibri", "calibri.ttf", "calibrib.ttf" }, { "Trebuchet MS", "trebuc.ttf", "trebucbd.ttf" },
        { "Verdana", "verdana.ttf", "verdanab.ttf" }, { "Georgia", "georgia.ttf", "georgiab.ttf" }, { "Times New Roman", "times.ttf", "timesbd.ttf" },
        { "Comic Sans", "comic.ttf", "comicbd.ttf" }, { "От руки (Segoe Print)", "segoepr.ttf", "segoeprb.ttf" },
        // «@» — свой файл из папки fonts рядом с программой
        { "Comfortaa (мягкий)", "@Comfortaa.ttf", "@Comfortaa.ttf" }, { "Neucha (от руки)", "@Neucha.ttf", "@Neucha.ttf" } };
    constexpr int kFaceCount = static_cast<int>(sizeof(kFaces) / sizeof(kFaces[0]));
    constexpr int kHandwriting = 10, kSoftFace = 11, kHandFace = 12;
    int g_face = 0, g_fontScale = 100, g_cozyPattern = 0;
    float g_systemDpi = 1.0f; // масштаб Windows; Theme::dpi — он же с учётом «Размера интерфейса»
    bool g_cozyDark = false, g_paper = false, g_prebake = true;
    int g_doodleColor = -1;
    bool g_cozy = false, g_optionsRead = false, g_fontsDirty = false;
    int g_style = 0;         // 0 обычные, 1 пилюли, 2 чёткие, 3 контур (капли и стикеры убраны)
    int g_cozyAccent = 0;    // «Домашний уют»: цвет акцента — карамель, роза, шалфей, лаванда, черника
    int g_mode = 0;          // 0 тёмная, 1 светлая, 2 как в Windows
    int g_atmosphere = 0;    // палитра: 0 системная, 1 Airy, 2 домашний уют, 3 Midnight, 4 свои цвета
    Theme::Color g_shadow = { 0, 0, 0 };
    bool g_light = false;    // светлая сейчас (с учётом Windows)
    ULONGLONG g_modeChecked = 0;

    std::string FontFileOf(const std::string& registryName) { // файл шрифта по его имени в Windows
        char file[260] = {};
        DWORD size = sizeof(file);
        if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts", registryName.c_str(), RRF_RT_REG_SZ, nullptr, file, &size) != ERROR_SUCCESS) return {};
        const std::string name = file;
        return name.find('\\') == std::string::npos ? name : std::string();
    }

    // Шрифт сообщений из настроек Windows
    void SystemFont(std::string& regular, std::string& bold) {
        NONCLIENTMETRICSA metrics{ sizeof(metrics) };
        if (!SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) return;
        const std::string face = metrics.lfMessageFont.lfFaceName;
        const std::string plain = FontFileOf(face + " (TrueType)");
        if (plain.empty()) return;
        regular = plain;
        std::string strong = FontFileOf(face + " Semibold (TrueType)");
        if (strong.empty()) strong = FontFileOf(face + " Bold (TrueType)");
        bold = strong.empty() ? plain : strong;
    }

    // «Размер текста» из специальных возможностей Windows
    float SystemTextScale() {
        DWORD value = 100, size = sizeof(value);
        RegGetValueA(HKEY_CURRENT_USER, "Software\\Microsoft\\Accessibility", "TextScaleFactor", RRF_RT_REG_DWORD, nullptr, &value, &size);
        return std::clamp(value / 100.0f, 1.0f, 1.6f);
    }

    void ReadOptions() {
        if (g_optionsRead) return;
        g_optionsRead = true;
        g_face = std::clamp(std::atoi(Config::Value("look", "font", "0").c_str()), 0, kFaceCount - 1);
        g_fontScale = std::clamp(std::atoi(Config::Value("look", "font_scale", "100").c_str()), 80, 120);
        g_cozy = Config::Value("cozy", "enabled", "0") == "1";
        g_cozyPattern = std::atoi(Config::Value("cozy", "pattern", "0").c_str());
        g_cozyDark = Config::Value("cozy", "dark", "0") == "1";
        g_mode = std::clamp(std::atoi(Config::Value("look", "mode", "0").c_str()), 0, 2);
        g_atmosphere = std::clamp(std::atoi(Config::Value("look", "atmosphere", "0").c_str()), 0, Theme::kAtmospheres - 1);
        if (g_cozy && Config::Value("look", "atmo_moved", "0") != "1") { // один раз: старая тетрадь «Домашний уют» → новая атмосфера «Уют»
            Config::SetValue("look", "atmo_moved", "1");
            g_cozy = false;
            g_atmosphere = 2;
            Config::SetValue("cozy", "enabled", "0");
            Config::SetValue("look", "atmosphere", "2");
            if (g_cozyDark) Config::SetValue("look", "mode", "0");
        }
        g_style = std::clamp(std::atoi(Config::Value("look", "style", "0").c_str()), 0, 3);
        if (Config::Value("look", "style_v2", "0") != "1") { // старые «капли» и «стикеры» убраны — один раз на обычные
            g_style = 0;
            Config::SetValue("look", "style", "0");
            Config::SetValue("look", "style_v2", "1");
        }
        g_cozyAccent = std::clamp(std::atoi(Config::Value("look", "cozy_accent", "0").c_str()), 0, 4);
        g_fontsDirty = true;
    }

    bool WindowsLight() {
        DWORD value = 0, size = sizeof(value);
        return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                            RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS && value != 0;
    }

    Theme::Color WindowsAccent() {
        static Theme::Color s_color = { 0, 120, 212 };
        static ULONGLONG s_checked = 0;
        const ULONGLONG now = GetTickCount64();
        if (s_checked == 0 || now - s_checked > 3000) {
            s_checked = now;
            DWORD value = 0, size = sizeof(value); // ABGR
            if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS)
                s_color = { static_cast<float>(value & 0xFF), static_cast<float>((value >> 8) & 0xFF), static_cast<float>((value >> 16) & 0xFF) };
        }
        return s_color;
    }

    bool LightNow() {
        if (g_mode != 2) return g_mode == 1;
        const ULONGLONG now = GetTickCount64();
        if (now - g_modeChecked > 3000) { g_modeChecked = now; g_light = WindowsLight(); } // реестр раз в 3 с, не каждый кадр
        return g_light;
    }

    bool FileExists(const std::string& path) { return GetFileAttributesW(Text::ToWide(path).c_str()) != INVALID_FILE_ATTRIBUTES; }

    // Шрифт Windows + Segoe UI Symbol для значков (✦ ♀ …)
    ImFont* LoadFont(const char* file, float size) {
        ImGuiIO& io = ImGui::GetIO();
        const std::string path = file[0] == '@' ? Text::ToUtf8(Text::ExecutableDirectory()) + "\\fonts\\" + (file + 1)
                                                : std::string("C:\\Windows\\Fonts\\") + file;
        if (!FileExists(path)) return nullptr;
        ImFont* font = io.Fonts->AddFontFromFileTTF(path.c_str(), size);
        const char* symbols = "C:\\Windows\\Fonts\\seguisym.ttf";
        if (font && FileExists(symbols)) {
            ImFontConfig merge;
            merge.MergeMode = true;
            io.Fonts->AddFontFromFileTTF(symbols, size, &merge);
        }
        return font;
    }

    void ApplyStyle() {
        ImGui::GetStyle() = ImGuiStyle(); // масштаб не должен накапливаться при смене размера
        ImGuiStyle& s = ImGui::GetStyle();
        s.WindowRounding = 16.0f;
        s.ChildRounding = 12.0f;
        s.FrameRounding = 8.0f;
        s.PopupRounding = 12.0f;
        s.GrabRounding = 8.0f;
        s.ScrollbarRounding = 10.0f;
        s.ScrollbarSize = 6.0f;
        s.WindowPadding = ImVec2(18, 16);
        s.FramePadding = ImVec2(12, 8);   // компактные поля и кнопки
        s.ItemSpacing = ImVec2(8, 8);
        s.WindowBorderSize = 1.0f;
        s.FrameBorderSize = 1.0f;
        s.PopupBorderSize = 1.0f;
        s.GrabMinSize = 14.0f;
        s.ScaleAllSizes(Theme::dpi);

        ImVec4* c = s.Colors;
        c[ImGuiCol_Text] = ImVec4(0.96f, 0.94f, 0.98f, 1.0f);
        c[ImGuiCol_TextDisabled] = ImVec4(0.96f, 0.94f, 0.98f, 0.40f);
        c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.045f, 0.12f, 0.94f);
        c[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.055f, 0.13f, 0.98f);
        c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.05f);
        c[ImGuiCol_FrameBg] = ImVec4(0, 0, 0, 0.27f);        // поля — тёмные «таблетки»
        c[ImGuiCol_FrameBgHovered] = ImVec4(0, 0, 0, 0.36f);
        c[ImGuiCol_FrameBgActive] = ImVec4(0, 0, 0, 0.42f);
        c[ImGuiCol_Button] = ImVec4(0, 0, 0, 0.27f);
        c[ImGuiCol_ButtonHovered] = ImVec4(1, 1, 1, 0.09f);
        c[ImGuiCol_ButtonActive] = ImVec4(1, 1, 1, 0.14f);
        c[ImGuiCol_Header] = ImVec4(0.65f, 0.55f, 0.98f, 0.25f);
        c[ImGuiCol_HeaderHovered] = ImVec4(0.65f, 0.55f, 0.98f, 0.35f);
        c[ImGuiCol_HeaderActive] = ImVec4(0.65f, 0.55f, 0.98f, 0.45f);
        c[ImGuiCol_SliderGrab] = ImVec4(0.96f, 0.45f, 0.71f, 1.0f);
        c[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.6f, 0.8f, 1.0f);
        c[ImGuiCol_CheckMark] = ImVec4(0.96f, 0.45f, 0.71f, 1.0f);
        c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab] = ImVec4(1, 1, 1, 0.12f);
        c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1, 1, 1, 0.20f);
        c[ImGuiCol_ScrollbarGrabActive] = ImVec4(1, 1, 1, 0.28f);
        c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.02f, 0.01f, 0.05f, 0.60f);
        c[ImGuiCol_TextSelectedBg] = ImVec4(0.96f, 0.45f, 0.71f, 0.35f);
        c[ImGuiCol_NavCursor] = ImVec4(0, 0, 0, 0);
    }
}

ImU32 Theme::Color::WithAlpha(float a) const {
    auto channel = [](float v) { return static_cast<int>(std::clamp(v, 0.0f, 255.0f)); };
    return IM_COL32(channel(r), channel(g), channel(b), channel(a * 255.0f));
}

Theme::Color Theme::Color::Mix(const Color& to, float t) const {
    return { r + (to.r - r) * t, g + (to.g - g) * t, b + (to.b - b) * t };
}

float Theme::Px(float value) { return value * dpi; }

void Theme::Init(float dpiScale) {
    g_systemDpi = dpiScale;
    ReadOptions();
    g_fontsDirty = false;
    LoadFonts();
}

void Theme::LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    dpi = g_systemDpi * SystemTextScale() * g_fontScale / 100.0f; // весь интерфейс растёт вместе с текстом: блоки, отступы, значки
    const FontFace& chosen = kFaces[g_face == 0 && g_cozy ? kHandwriting : g_face];
    std::string regular = chosen.regular, bold = chosen.bold;
    if (g_face == 0 && !g_cozy) SystemFont(regular, bold);
    struct { const char* regular; const char* bold; } face{ regular.c_str(), bold.c_str() };
    const float k = 1.55f; // текст крупнее, а блоки того же размера
    body = LoadFont(face.regular, Px(15.5f * k));
    semibold = LoadFont(face.bold, Px(13.5f * k));
    title = LoadFont(face.bold, Px(16.0f * k));
    display = LoadFont(face.bold, Px(22.0f * k));
    const bool systemFace = g_face == 0 && !g_cozy;
    hero = LoadFont(systemFace ? "seguibl.ttf" : face.bold, Px(32.0f * k)); // заголовки страниц — очень жирные, как у Astra
    plain = ImGui::GetIO().Fonts->AddFontFromFileTTF(FileExists("C:\\Windows\\Fonts\\consola.ttf") ? "C:\\Windows\\Fonts\\consola.ttf" : "C:\\Windows\\Fonts\\arial.ttf", Px(36));
    poster = LoadFont(systemFace ? "seguibl.ttf" : face.bold, Px(46.0f * k)); // Segoe UI Black — плакатная надпись
    if (!poster) poster = LoadFont("segoeuib.ttf", Px(46.0f * k));
    mono = LoadFont("CascadiaMono.ttf", Px(11.0f * k));
    if (!mono) mono = LoadFont("consola.ttf", Px(11.0f * k));
    if (!body) body = LoadFont("segoeui.ttf", Px(15.5f * k)); // выбранного шрифта нет — Segoe UI
    if (!semibold) semibold = LoadFont("seguisb.ttf", Px(13.5f * k));
    if (!title) title = semibold;
    if (!display) display = LoadFont("seguisb.ttf", Px(22.0f * k));
    if (!hero) hero = LoadFont("seguisb.ttf", Px(32.0f * k));
    if (!body) body = io.Fonts->AddFontDefault();
    if (!semibold) semibold = body;
    if (!title) title = semibold;
    if (!display) display = semibold;
    if (!hero) hero = body;
    if (!poster) poster = hero;
    if (!mono) mono = semibold;
    io.FontDefault = body;
    ApplyStyle();
}

void Theme::UpdatePalette(Mood mood, float dt) {
    static constexpr Color kCozy[4][3] = {
        { { 212, 168, 126 }, { 198, 128, 104 }, { 236, 214, 184 } },  // отдыхаю — карамель и терракота
        { { 168, 186, 152 }, { 214, 184, 140 }, { 232, 212, 180 } },  // слушаю — шалфей и песок
        { { 224, 186, 120 }, { 200, 140, 108 }, { 238, 216, 176 } },  // думаю — мёд
        { { 206, 132, 110 }, { 222, 172, 130 }, { 240, 220, 192 } },  // говорю — терракота и сливки
    };
    const bool cozy = !g_cozy && g_atmosphere == 2;
    const auto& pal = cozy ? kCozy[static_cast<int>(mood)] : kPalettes[static_cast<int>(mood)];
    Color target[3] = { pal[0], pal[1], pal[2] };
    if (mood == Mood::Idle && !cozy) { // в покое сфера — любимого цвета
        target[0] = g_accent;
        target[1] = g_accent.Mix({ 244, 114, 182 }, 0.55f);
        target[2] = g_accent.Mix({ 99, 102, 241 }, 0.5f);
    }
    const float t = std::min(1.0f, dt * 2.5f);
    for (int i = 0; i < 3; ++i) g_palette[i] = g_palette[i].Mix(target[i], t);
}

const Theme::Color& Theme::Palette(int index) { return g_palette[index % 3]; }

// ---------------------------------------------------------------------- внешний вид
Theme::Color Theme::FromRgb(int rgb) {
    return { static_cast<float>((rgb >> 16) & 0xFF), static_cast<float>((rgb >> 8) & 0xFF), static_cast<float>(rgb & 0xFF) };
}

int Theme::ToRgb(const Color& c) {
    auto channel = [](float v) { return std::clamp(static_cast<int>(v + 0.5f), 0, 255); };
    return (channel(c.r) << 16) | (channel(c.g) << 8) | channel(c.b);
}

Theme::LookColors Theme::LookPreset(int look, int customRgb) {
    switch (look) {
    case 1: return { { 26, 26, 31 }, { 12, 12, 15 }, { 36, 36, 43 } };    // графит
    case 2: return { { 14, 14, 14 }, { 4, 4, 4 }, { 26, 26, 26 } };       // уголь
    case 3: return { { 10, 16, 38 }, { 3, 6, 16 }, { 18, 26, 54 } };      // полночь
    case 4: {                                                             // своя: из одного цвета
        const Color base = FromRgb(customRgb);
        return { base, base.Mix({ 0, 0, 0 }, 0.6f), base.Mix({ 255, 255, 255 }, 0.08f) };
    }
    default: return { { 40, 9, 38 }, { 16, 3, 18 }, { 48, 15, 46 } };     // ночь — сливовая
    }
}

Theme::Atmo Theme::AtmospherePalette(int atmosphere, bool light) {
    switch (atmosphere) {
    case 0: { // как в Windows 11: нейтральные серые и системный акцент
        const Color win = WindowsAccent();
        if (light) return { { 243, 243, 243 }, { 235, 235, 235 }, { 251, 251, 251 }, win.Mix({ 0, 0, 0 }, 0.12f), { 26, 26, 26 }, { 0, 0, 0 } };
        return { { 32, 32, 32 }, { 26, 26, 26 }, { 44, 44, 44 }, win.Mix({ 255, 255, 255 }, 0.35f), { 255, 255, 255 }, { 0, 0, 0 } };
    }
    case 1: // Airy: лаванда и молоко, много воздуха
        if (light) return { { 248, 246, 253 }, { 238, 234, 249 }, { 255, 255, 255 }, { 139, 108, 239 }, { 36, 30, 56 }, { 70, 50, 140 } };
        return { { 38, 34, 58 }, { 26, 23, 40 }, { 52, 47, 72 }, { 183, 160, 255 }, { 240, 236, 250 }, { 0, 0, 0 } };
    case 2: // домашний уют: тёплое молоко, крем, беж и приглушённый коричневый
        {
            static const Color kLight[5] = { { 158, 110, 80 }, { 176, 96, 112 }, { 104, 132, 96 }, { 128, 106, 170 }, { 84, 108, 152 } };
            static const Color kDark[5] = { { 212, 168, 126 }, { 226, 150, 162 }, { 160, 190, 146 }, { 186, 166, 226 }, { 150, 174, 222 } };
            const int k = std::clamp(g_cozyAccent, 0, 4);
            if (light) return { { 250, 246, 240 }, { 239, 230, 217 }, { 255, 251, 245 }, kLight[k], { 58, 44, 36 }, { 120, 84, 52 } };
            return { { 46, 38, 33 }, { 27, 23, 21 }, { 60, 51, 45 }, kDark[k], { 242, 234, 222 }, { 8, 4, 2 } };
        }
    case 3: // Midnight: слива и приглушённый фиолетовый
        if (light) return { { 240, 233, 243 }, { 230, 220, 236 }, { 252, 248, 253 }, { 120, 70, 150 }, { 40, 22, 48 }, { 60, 20, 70 } };
        return { { 34, 20, 40 }, { 16, 9, 22 }, { 48, 30, 54 }, { 190, 140, 240 }, { 244, 238, 250 }, { 0, 0, 0 } };
    default:
        return { { 40, 9, 38 }, { 16, 3, 18 }, { 48, 15, 46 }, { 180, 160, 250 }, { 244, 239, 250 }, { 0, 0, 0 } };
    }
}

const char* Theme::AtmosphereName(int atmosphere) {
    static const char* kNames[kAtmospheres] = { "Системная", "Airy", "Домашний уют", "Midnight", "Свои цвета" };
    return kNames[std::clamp(atmosphere, 0, kAtmospheres - 1)];
}
int Theme::Atmosphere() { ReadOptions(); return g_atmosphere; }
int Theme::CozyAccent() { ReadOptions(); return g_cozyAccent; }
void Theme::SetCozyAccent(int index) { ReadOptions(); g_cozyAccent = std::clamp(index, 0, 4); Config::SetValue("look", "cozy_accent", std::to_string(g_cozyAccent)); }
bool Theme::CozyAtmosphere() { ReadOptions(); return g_atmosphere == 2 && !g_cozy; }
void Theme::SetAtmosphere(int atmosphere) {
    ReadOptions();
    g_atmosphere = std::clamp(atmosphere, 0, kAtmospheres - 1);
    Config::SetValue("look", "atmosphere", std::to_string(g_atmosphere));
    if (g_cozy) SetCozy(false, g_cozyPattern); // тетрадь перекрывает атмосферы — выбрали атмосферу, значит тетрадь выключаем
}

Theme::Color Theme::AccentPreset(int index) {
    static const Color kAccents[kAccentPresets] = {
        { 180, 160, 250 }, { 125, 175, 245 }, { 110, 210, 180 }, { 250, 165, 125 }, { 245, 205, 95 }, { 192, 196, 206 } };
    return kAccents[std::clamp(index, 0, kAccentPresets - 1)];
}

void Theme::Apply(const Settings& s) {
    ReadOptions();
    if (g_fontsDirty) { g_fontsDirty = false; LoadFonts(); g_prebake = true; }
    if (g_prebake) { // все буквы готовим заранее: иначе первое появление новых букв при переходе даёт рывок
        g_prebake = false;
        for (ImFont* font : { body, semibold, title, display, hero, poster, mono }) {
            if (!font) continue;
            ImFontBaked* baked = font->GetFontBaked(font->LegacySize);
            for (ImWchar c = 0x20; c < 0x7F; ++c) baked->FindGlyph(c);
            for (ImWchar c = 0x410; c <= 0x451; ++c) baked->FindGlyph(c);
            for (ImWchar c : { 0x401, 0x2014, 0xAB, 0xBB, 0x2026, 0x2192, 0xB0 }) baked->FindGlyph(static_cast<ImWchar>(c));
        }
    }
    g_accent = s.accent >= kAccentPresets ? FromRgb(s.accentColor) : AccentPreset(s.accent);
    g_look = LookPreset(static_cast<int>(s.look), s.lookColor);
    if (s.backgroundColor >= 0) { // свой цвет фона поверх темы
        g_look.top = FromRgb(s.backgroundColor);
        g_look.bottom = g_look.top.Mix({ 0, 0, 0 }, 0.6f);
    }
    g_glass = s.glass;
    g_plates = s.buttonPlates;
    g_cozyDark = false; // тетрадь — всегда белый лист в клетку с рисунками карандашом (тёмный вариант убран)
    g_paper = g_cozy && !g_cozyDark;
    g_doodleColor = s.animationColor;
    if (g_cozy) { // тетрадь слушается настроек цвета; без них — свой тёплый лист
        if (g_paper) { // всегда белый тёплый лист: свой цвет фона на тетрадь не действует (иначе был «просто тёмный фон»)
            g_look = { { 251, 245, 230 }, { 243, 232, 209 }, { 255, 251, 241 } };
        } else if (static_cast<int>(s.look) == 0) {
            g_look = { { 42, 37, 46 }, { 28, 25, 33 }, { 54, 48, 58 } };
        }
        if (g_paper) g_accent = g_accent.Mix({ 90, 55, 40 }, 0.2f); // бледный цвет на бумаге не видно
        g_glass = false;
    }
    g_light = !g_cozy && LightNow();
    if (g_light) { // светлая: та же тема, только высветленная; чернила — тёмные в тон
        // светлая «слюда» Windows 11: чистый светлый фон с лёгким сиреневым оттенком, островки — белые.
        // Не высветленная тёмная тема (от неё всё было серым, «как бумагу наложили»), а свои цвета
        g_look = { { 247, 245, 252 }, { 236, 231, 247 }, { 255, 255, 255 } };
        g_accent = g_accent.Mix({ 30, 10, 60 }, 0.3f);
        g_paper = true;
    }
    Color inkColor = g_paper ? (g_light ? Color{ 28, 20, 44 } : Color{ 58, 46, 38 }) : Color{ 244, 239, 250 };
    g_shadow = g_light ? Color{ 30, 15, 60 } : Color{ 0, 0, 0 };
    if (!g_cozy && g_atmosphere != kAtmosphereCustom) { // атмосфера задаёт всё сама
        const Atmo p = AtmospherePalette(g_atmosphere, g_light);
        g_look = { p.top, p.bottom, p.surface };
        g_accent = p.accent;
        inkColor = p.ink;
        g_shadow = p.shadow;
    }
    { // плавный переход: все цвета перетекают к новым за ~0,4 с — без вспышек и скачков яркости
        static bool s_first = true;
        static Theme::LookColors s_look;
        static Color s_accent, s_ink, s_shadow;
        if (s_first) { s_first = false; s_look = g_look; s_accent = g_accent; s_ink = inkColor; s_shadow = g_shadow; }
        const float k = Ease(ImGui::GetIO().DeltaTime, 9.0f);
        s_look = { s_look.top.Mix(g_look.top, k), s_look.bottom.Mix(g_look.bottom, k), s_look.surface.Mix(g_look.surface, k) };
        s_accent = s_accent.Mix(g_accent, k);
        s_ink = s_ink.Mix(inkColor, k);
        s_shadow = s_shadow.Mix(g_shadow, k);
        g_look = s_look; g_accent = s_accent; inkColor = s_ink; g_shadow = s_shadow;
    }
    GImTextHalo = 0;
    kInk = inkColor.WithAlpha(1.0f);
    kInkSoft = inkColor.WithAlpha(g_paper ? 225.0f / 255.0f : 220.0f / 255.0f);
    kInkMuted = inkColor.WithAlpha(g_paper ? 190.0f / 255.0f : 175.0f / 255.0f);

    ImVec4* c = ImGui::GetStyle().Colors;
    auto accent = [&](float a) { return ImGui::ColorConvertU32ToFloat4(g_accent.WithAlpha(a)); };
    c[ImGuiCol_WindowBg] = ImGui::ColorConvertU32ToFloat4(g_look.surface.WithAlpha(g_glass ? 0.94f : 1.0f)); // панель разговора
    // меню и подсказки — «акрил»: плотнее панелей, но фон чуть угадывается
    c[ImGuiCol_PopupBg] = ImGui::ColorConvertU32ToFloat4(g_look.surface.Mix({ 0, 0, 0 }, g_light ? 0.0f : 0.2f).Mix(g_accent, 0.03f).WithAlpha(g_glass ? 0.96f : 0.99f));
    const ImVec4 ink = ImGui::ColorConvertU32ToFloat4(kInk);
    c[ImGuiCol_Text] = ink;
    c[ImGuiCol_TextDisabled] = ImVec4(ink.x, ink.y, ink.z, g_paper ? 0.58f : 0.40f);
    c[ImGuiCol_Border] = g_paper ? ImVec4(0.3f, 0.2f, 0.12f, 0.16f) : ImVec4(1, 1, 1, 0.08f);
    // как в Windows 11: поля чуть темнее фона, кнопки чуть светлее, под мышью — ещё светлее
    const ImVec4 ink3 = g_light ? ImVec4(0.2f, 0.1f, 0.35f, 1) : ImVec4(0.25f, 0.15f, 0.05f, 1);
    auto tint = [&](float a) { return g_paper ? ImVec4(ink3.x, ink3.y, ink3.z, a) : ImVec4(1, 1, 1, a); };
    c[ImGuiCol_FrameBg] = g_light ? ImVec4(1, 1, 1, 0.72f) : g_paper ? tint(0.07f) : ImVec4(0, 0, 0, 0.22f);
    c[ImGuiCol_FrameBgHovered] = g_light ? ImVec4(1, 1, 1, 0.9f) : g_paper ? tint(0.11f) : ImVec4(0, 0, 0, 0.30f);
    c[ImGuiCol_FrameBgActive] = g_light ? ImVec4(1, 1, 1, 1.0f) : g_paper ? ImVec4(1, 1, 1, 0.55f) : ImVec4(0, 0, 0, 0.38f);
    c[ImGuiCol_Button] = g_light ? ImVec4(1, 1, 1, 0.8f) : g_paper ? tint(0.08f) : ImVec4(1, 1, 1, 0.06f);
    c[ImGuiCol_ButtonHovered] = g_light ? ImVec4(1, 1, 1, 1.0f) : g_paper ? tint(0.13f) : ImVec4(1, 1, 1, 0.10f);
    c[ImGuiCol_ButtonActive] = g_light ? ImVec4(0.96f, 0.95f, 0.98f, 0.9f) : g_paper ? tint(0.18f) : ImVec4(1, 1, 1, 0.045f);
    c[ImGuiCol_ScrollbarGrab] = g_paper ? ImVec4(0.3f, 0.2f, 0.12f, 0.28f) : ImVec4(1, 1, 1, 0.12f);
    c[ImGuiCol_ScrollbarGrabHovered] = g_paper ? ImVec4(0.3f, 0.2f, 0.12f, 0.4f) : ImVec4(1, 1, 1, 0.20f);
    c[ImGuiCol_ScrollbarGrabActive] = g_paper ? ImVec4(0.3f, 0.2f, 0.12f, 0.5f) : ImVec4(1, 1, 1, 0.28f);
    c[ImGuiCol_Header] = accent(0.22f);
    c[ImGuiCol_HeaderHovered] = g_paper ? tint(0.09f) : ImVec4(1, 1, 1, 0.07f); // наведение — нейтральное, цвет — только у выбранного
    c[ImGuiCol_HeaderActive] = accent(0.32f);
    c[ImGuiCol_SliderGrab] = accent(1.0f);
    c[ImGuiCol_SliderGrabActive] = ImGui::ColorConvertU32ToFloat4(g_accent.Mix({ 255, 255, 255 }, 0.3f).WithAlpha(1.0f));
    c[ImGuiCol_CheckMark] = accent(1.0f);
    c[ImGuiCol_TextSelectedBg] = accent(0.35f);
}

Theme::Color Theme::Accent() { return g_accent; }
Theme::Color Theme::Background(int index) { return index == 0 ? g_look.top : g_look.bottom; }
ImU32 Theme::SurfaceSolid() { return g_look.surface.WithAlpha(1.0f); }
ImU32 Theme::Surface(float alpha) { return g_look.surface.WithAlpha(g_cozy ? 1.0f : alpha * (g_glass ? 0.82f : 1.0f)); }
ImU32 Theme::Control(int level) {
    if (g_light) return IM_COL32(255, 255, 255, std::clamp(165 + level * 4, 0, 255));
    return Overlay(level);
}
ImU32 Theme::Overlay(int alpha) {
    if (!g_paper || alpha >= 90) return IM_COL32(255, 255, 255, std::clamp(alpha, 0, 255)); // яркое — ручки, текст на кнопках — остаётся белым
    return g_light ? IM_COL32(50, 30, 90, std::clamp(alpha * 2, 0, 255)) : IM_COL32(70, 50, 30, std::clamp(alpha * 2, 0, 255));
}

int Theme::Mode() { ReadOptions(); return g_mode; }
int Theme::Style() { ReadOptions(); return g_style; }
void Theme::ReloadOptions() { g_optionsRead = false; ReadOptions(); }

void Theme::SetStyle(int style) {
    ReadOptions();
    g_style = std::clamp(style, 0, 3);
    Config::SetValue("look", "style", std::to_string(g_style));
    if (g_face == kSoftFace || g_face == kHandFace) SetFont(0, g_fontScale); // шрифты старых «капель» и «стикеров» больше не нужны
}

namespace {
    using Theme::Px;
    // форма зависит только от размера: при прокрутке плашка не «дышит»
    uint32_t ShapeSeed(ImVec2 a, ImVec2 b) {
        const uint32_t w = static_cast<uint32_t>(b.x - a.x), h = static_cast<uint32_t>(b.y - a.y);
        uint32_t x = w * 2654435761u ^ (h + 0x9E37u) * 40503u;
        x ^= x >> 15; x *= 2246822519u; x ^= x >> 13;
        return x;
    }
    float Unit(uint32_t seed, int k) { return ((seed >> (k * 8)) & 255) / 255.0f; }

    // скруглённый прямоугольник с разными углами, край идёт мягкой волной (wave — высота волны)
    void BlobPath(ImDrawList* dl, ImVec2 a, ImVec2 b, float radius, uint32_t seed, float spread, float wave = 0.0f) {
        const float limit = std::min(b.x - a.x, b.y - a.y) * 0.5f;
        float r[4];
        for (int i = 0; i < 4; ++i) r[i] = std::min(limit, radius * (1.0f - spread + 2.0f * spread * Unit(seed, i)));
        constexpr float pi = 3.14159265f;
        const ImVec2 centers[4] = { ImVec2(a.x + r[0], a.y + r[0]), ImVec2(b.x - r[1], a.y + r[1]), ImVec2(b.x - r[2], b.y - r[2]), ImVec2(a.x + r[3], b.y - r[3]) };
        const float starts[4] = { pi, pi * 1.5f, 0.0f, pi * 0.5f };
        if (wave <= 0.0f) {
            for (int i = 0; i < 4; ++i) dl->PathArcTo(centers[i], r[i], starts[i], starts[i] + pi * 0.5f, 8);
            return;
        }
        // точки по контуру через равный шаг, каждую сдвигаем по нормали на синус
        const float phase = Unit(seed, 3) * 6.28f, step = Px(7);
        const ImVec2 cmid((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        float travelled = 0.0f;
        auto put = [&](ImVec2 p) {
            const float dx = p.x - cmid.x, dy = p.y - cmid.y, len = std::sqrt(dx * dx + dy * dy) + 0.001f;
            const float k = wave * std::sin(travelled / Px(46) + phase) * 0.7f + wave * 0.3f * std::sin(travelled / Px(17) + phase * 2.0f);
            dl->PathLineTo(ImVec2(p.x + dx / len * k, p.y + dy / len * k));
        };
        for (int i = 0; i < 4; ++i) {
            const int arcSteps = std::max(3, static_cast<int>(r[i] * pi * 0.5f / step));
            for (int j = 0; j <= arcSteps; ++j) {
                const float t = starts[i] + pi * 0.5f * j / arcSteps;
                put(ImVec2(centers[i].x + std::cos(t) * r[i], centers[i].y + std::sin(t) * r[i]));
                travelled += r[i] * pi * 0.5f / arcSteps;
            }
            const ImVec2 from = ImVec2(centers[i].x + std::cos(starts[i] + pi * 0.5f) * r[i], centers[i].y + std::sin(starts[i] + pi * 0.5f) * r[i]);
            const int n = (i + 1) % 4;
            const ImVec2 to = ImVec2(centers[n].x + std::cos(starts[n]) * r[n], centers[n].y + std::sin(starts[n]) * r[n]);
            const float dx = to.x - from.x, dy = to.y - from.y, len = std::sqrt(dx * dx + dy * dy);
            const int lineSteps = static_cast<int>(len / step);
            for (int j = 1; j < lineSteps; ++j) {
                travelled += len / lineSteps;
                put(ImVec2(from.x + dx * j / lineSteps, from.y + dy * j / lineSteps));
            }
        }
    }

    void Tilted(ImVec2 a, ImVec2 b, float angle, ImVec2 out[4]) {
        const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        const float cs = std::cos(angle), sn = std::sin(angle);
        const ImVec2 p[4] = { a, ImVec2(b.x, a.y), b, ImVec2(a.x, b.y) };
        for (int i = 0; i < 4; ++i) {
            const float x = p[i].x - c.x, y = p[i].y - c.y;
            out[i] = ImVec2(c.x + x * cs - y * sn, c.y + x * sn + y * cs);
        }
    }

    void Sticker(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 fill, ImU32 border, bool panel) {
        const uint32_t seed = ShapeSeed(a, b);
        const float w = b.x - a.x;
        // широкое почти не наклоняем, иначе края уезжают на десятки пикселей
        const float maxTilt = panel ? 0.009f * std::min(1.0f, Px(420) / std::max(w, 1.0f)) : 0.045f * std::min(1.0f, Px(260) / std::max(w, 1.0f));
        const float angle = (Unit(seed, 0) - 0.5f) * 2.0f * maxTilt;
        ImVec2 q[4], s[4];
        Tilted(a, b, angle, q);
        const ImVec2 off(Px(panel ? 5.0f : 3.0f), Px(panel ? 6.0f : 3.5f));
        for (int i = 0; i < 4; ++i) s[i] = ImVec2(q[i].x + off.x, q[i].y + off.y);
        // на тёмном чёрная тень не видна — тень цветом темы, как у наклейки на картоне
        dl->AddQuadFilled(s[0], s[1], s[2], s[3], g_light ? IM_COL32(38, 26, 58, 150) : g_accent.Mix({ 0, 0, 0 }, 0.45f).WithAlpha(0.85f));
        const ImU32 alpha = (fill >> IM_COL32_A_SHIFT) & 0xFF;
        const ImU32 paper = (g_light ? g_look.surface.Mix({ 255, 255, 255 }, 0.7f) : g_look.surface.Mix({ 255, 255, 255 }, 0.1f)).WithAlpha(1.0f);
        dl->AddQuadFilled(q[0], q[1], q[2], q[3], alpha < 160 ? paper : fill); // полупрозрачная кнопка — на плотном листке
        if (alpha < 160 && alpha > 0) dl->AddQuadFilled(q[0], q[1], q[2], q[3], fill);
        if (border) dl->AddQuad(q[0], q[1], q[2], q[3], border, Px(1.1f));
        if (panel && b.y - a.y > Px(56)) { // скотч сверху
            const float tw = std::min(w * 0.2f, Px(78)), th = Px(17), cx = a.x + w * (0.3f + 0.4f * Unit(seed, 1));
            ImVec2 t[4];
            Tilted(ImVec2(cx - tw * 0.5f, a.y - th * 0.55f), ImVec2(cx + tw * 0.5f, a.y + th * 0.45f), (Unit(seed, 2) - 0.5f) * 0.25f, t);
            dl->AddQuadFilled(t[0], t[1], t[2], t[3], g_accent.Mix({ 255, 255, 255 }, 0.25f).WithAlpha(0.55f));
        }
    }
}

bool Theme::Panel(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 fill, ImU32 border, float rounding) {
    if (g_style >= 0) return false; // «капли» и «стикеры» убраны: новые формы рисует обычный путь (Plate / FrameHook)
    if (g_style == 2) { Sticker(dl, a, b, fill, border, true); return true; }
    const uint32_t seed = ShapeSeed(a, b);
    const float radius = std::max(rounding, Px(34)), wave = Px(3.2f);
    const ImVec2 shift(Px(-7), Px(8));
    BlobPath(dl, ImVec2(a.x + shift.x, a.y + shift.y), ImVec2(b.x + shift.x, b.y + shift.y), radius, seed ^ 0x5bd1e995u, 0.8f, wave * 1.4f);
    dl->PathFillConcave(g_accent.WithAlpha(g_light ? 0.22f : 0.2f));
    BlobPath(dl, a, b, radius, seed, 0.8f, wave);
    dl->PathFillConcave(fill);
    if (border) { BlobPath(dl, a, b, radius, seed, 0.8f, wave); dl->PathStroke(border, ImDrawFlags_Closed, Px(1.2f)); }
    return true;
}

bool Theme::Frame(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 fill, ImU32 border, float rounding) {
    if (g_style >= 0) return false; // «капли» и «стикеры» убраны: новые формы рисует обычный путь (Plate / FrameHook)
    if (g_style == 2) { Sticker(dl, a, b, fill, border, false); return true; }
    const uint32_t seed = ShapeSeed(a, b);
    const float radius = std::max(rounding, (b.y - a.y) * 0.5f);
    BlobPath(dl, a, b, radius, seed, 0.65f, b.x - a.x > Px(60) ? Px(1.6f) : 0.0f);
    dl->PathFillConcave(fill);
    if (border) { BlobPath(dl, a, b, radius, seed, 0.65f, b.x - a.x > Px(60) ? Px(1.6f) : 0.0f); dl->PathStroke(border, ImDrawFlags_Closed, Px(1.1f)); }
    return true;
}

namespace {
    void MulAlpha(ImDrawVert& v, float k) {
        const ImU32 a = static_cast<ImU32>(((v.col >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(k, 0.0f, 1.0f) + 0.5f);
        v.col = (v.col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
    }
}

// Форма плашек: «пилюли» — сильно скруглённые, «чёткие» — почти прямые углы, «контур» — рамка вместо заливки
float StyleRounding(float rounding, float height, bool control) {
    switch (g_style) {
    case 1: return control ? height * 0.5f : std::max(rounding, Px(26));
    case 2: return std::min(rounding, control ? Px(4) : Px(7));
    default: return rounding;
    }
}

// Стиль Windows 11 + островки: полупрозрачная «слюда», мягкая короткая тень, тонкий край — светлее сверху.
// Никаких бликов и цветных пятен: порядок и воздух, любимый цвет — только там, где выбор или наведение.
void Theme::Plate(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 fill, float rounding, float glow) {
    const float fillAlpha = ((fill >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
    rounding = StyleRounding(rounding, b.y - a.y, false);
    if (g_style == 3) { // «контур»: почти прозрачная плашка и тонкая рамка любимого цвета
        dl->AddRectFilled(a, b, (fill & ~IM_COL32_A_MASK) | (static_cast<ImU32>(fillAlpha * 70) << IM_COL32_A_SHIFT), rounding);
        dl->AddRect(a, b, g_accent.WithAlpha((0.45f + 0.35f * glow) * fillAlpha), rounding, 0, Px(1.4f));
        return;
    }
    const float h = b.y - a.y;
    // тень: три мягких слоя чуть вниз — островок парит над фоном
    for (int i = 3; i >= 1; --i) {
        const float g = Px(2.5f * i);
        dl->AddRectFilled(ImVec2(a.x - g * 0.6f, a.y - g * 0.15f + Px(2)), ImVec2(b.x + g * 0.6f, b.y + g + Px(2)),
                          g_shadow.WithAlpha((g_light ? 10.0f : 18.0f) / 255.0f * fillAlpha), rounding + g);
    }
    // мягкое свечение любимым цветом — только у выбранного или под мышью
    if (glow > 0.01f)
        for (int i = 2; i >= 1; --i) {
            const float g = Px(3.0f * i);
            dl->AddRectFilled(ImVec2(a.x - g, a.y - g), ImVec2(b.x + g, b.y + g), g_accent.WithAlpha((g_light ? 0.05f : 0.06f) * glow * fillAlpha), rounding + g);
        }
    // заливка «слюда»: почти ровная, сверху на волос светлее
    const Color base = { static_cast<float>(fill & 0xFF), static_cast<float>((fill >> 8) & 0xFF), static_cast<float>((fill >> 16) & 0xFF) };
    const Color top = base.Mix(g_accent, 0.035f + 0.05f * glow).Mix({ 255, 255, 255 }, g_light ? 0.30f : 0.05f + 0.02f * glow);
    const Color bottom = base.Mix({ 0, 0, 0 }, g_light ? 0.0f : 0.06f);
    int start = dl->VtxBuffer.Size;
    dl->AddRectFilled(a, b, fill, rounding);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, start, dl->VtxBuffer.Size, a, ImVec2(a.x, b.y), top.WithAlpha(1.0f), bottom.WithAlpha(1.0f));
    // тонкий край как у карточек Windows 11: сверху заметнее, книзу тает
    start = dl->VtxBuffer.Size;
    const ImU32 edge = g_light ? Color{ 40, 25, 70 }.Mix(g_accent, 0.6f * glow).WithAlpha(0.16f + 0.3f * glow) : Color{ 255, 255, 255 }.Mix(g_accent, 0.25f + 0.5f * glow).WithAlpha(0.13f + 0.22f * glow);
    dl->AddRect(a, b, edge, rounding, 0, Px(1.0f));
    for (int i = start; i < dl->VtxBuffer.Size; ++i) {
        ImDrawVert& v = dl->VtxBuffer[i];
        MulAlpha(v, (1.0f - 0.6f * std::clamp((v.pos.y - a.y) / std::max(h, 1.0f), 0.0f, 1.0f)) * fillAlpha);
    }
}

void Theme::Island(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding) {
    rounding = StyleRounding(rounding, b.y - a.y, false);
    if (g_style == 3) { dl->AddRect(a, b, g_accent.WithAlpha(0.25f), rounding, 0, Px(1.0f)); return; }
    dl->AddRectFilled(a, b, g_light ? IM_COL32(255, 255, 255, 150) : Overlay(g_paper ? 9 : 8), rounding);
    const int start = dl->VtxBuffer.Size;
    dl->AddRect(a, b, g_light ? IM_COL32(40, 25, 70, 22) : Overlay(g_paper ? 20 : 18), rounding, 0, Px(1.0f));
    const float h = std::max(b.y - a.y, 1.0f);
    for (int i = start; i < dl->VtxBuffer.Size; ++i) {
        ImDrawVert& v = dl->VtxBuffer[i];
        MulAlpha(v, 1.0f - 0.45f * std::clamp((v.pos.y - a.y) / h, 0.0f, 1.0f));
    }
}

void Theme::Pick(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, float amount, bool horizontal) {
    if (amount <= 0.01f) return;
    // выбранный пункт: ровная полупрозрачная подложка любимого цвета и тонкий край, без пятен
    const int start = dl->VtxBuffer.Size;
    dl->AddRectFilled(a, b, g_accent.WithAlpha(1.0f), rounding);
    for (int i = start; i < dl->VtxBuffer.Size; ++i) {
        ImDrawVert& v = dl->VtxBuffer[i];
        const float t = horizontal ? (v.pos.x - a.x) / std::max(b.x - a.x, 1.0f) : (v.pos.y - a.y) / std::max(b.y - a.y, 1.0f);
        v.col = g_accent.Mix({ 255, 255, 255 }, g_light ? 0.0f : 0.06f).WithAlpha(((g_light ? 0.20f : 0.24f) - 0.06f * std::clamp(t, 0.0f, 1.0f)) * amount);
    }
    dl->AddRect(a, b, g_accent.Mix({ 255, 255, 255 }, g_light ? 0.0f : 0.3f).WithAlpha(0.28f * amount), rounding, 0, Px(1.0f));
}

void Theme::FadeEdges(ImDrawList* dl, int vtxStart, float top, float bottom, float band) {
    if (band <= 0.0f) return;
    for (int i = vtxStart; i < dl->VtxBuffer.Size; ++i) {
        ImDrawVert& v = dl->VtxBuffer[i];
        const float t = std::min((v.pos.y - top) / band, (bottom - v.pos.y) / band);
        if (t < 1.0f) { const float s = std::clamp(t, 0.0f, 1.0f); MulAlpha(v, s * s * (3.0f - 2.0f * s)); }
    }
}

namespace {
    // Кнопки и поля ImGui в обычной форме — как в Windows 11: ровная заливка, край светлее сверху,
    // у поля, в котором печатают, — полоска любимого цвета снизу.
    bool FrameHook(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 fill, ImU32 border, float rounding) {
        rounding = StyleRounding(rounding, b.y - a.y, true);
        if (g_style == 3) { // «контур»: кнопки — только рамкой
            dl->AddRectFilled(a, b, (fill & ~IM_COL32_A_MASK) | ((((fill >> IM_COL32_A_SHIFT) & 0xFF) / 3) << IM_COL32_A_SHIFT), rounding);
            dl->AddRect(a, b, g_accent.WithAlpha(0.55f), rounding, 0, Px(1.2f));
            return true;
        }
        dl->AddRectFilled(a, b, fill, rounding);
        if (!border) return true;
        const float h = std::max(b.y - a.y, 1.0f);
        const int start = dl->VtxBuffer.Size;
        dl->AddRect(a, b, g_light ? IM_COL32(40, 25, 70, 34) : Theme::Overlay(g_paper ? 22 : 30), rounding, 0, Theme::Px(1.0f));
        for (int i = start; i < dl->VtxBuffer.Size; ++i) {
            ImDrawVert& v = dl->VtxBuffer[i];
            MulAlpha(v, 1.0f - 0.55f * std::clamp((v.pos.y - a.y) / h, 0.0f, 1.0f));
        }
        if ((fill & IM_COL32_A_MASK) && fill == ImGui::GetColorU32(ImGuiCol_FrameBgActive) && b.x - a.x > Theme::Px(60)) {
            const float inset = std::max(rounding, Theme::Px(4));
            dl->AddRectFilled(ImVec2(a.x + inset, b.y - Theme::Px(2)), ImVec2(b.x - inset, b.y), g_accent.WithAlpha(1.0f), Theme::Px(1));
        }
        return true;
    }
    const bool g_hooked = (GImFrameHook = &FrameHook, true);
}
bool Theme::Light() { return g_light; }
void Theme::SetMode(int mode) {
    ReadOptions();
    g_mode = std::clamp(mode, 0, 2);
    g_modeChecked = 0;
    Config::SetValue("look", "mode", std::to_string(g_mode));
}

int Theme::FontCount() { return kFaceCount; }
const char* Theme::FontName(int index) { return kFaces[std::clamp(index, 0, kFaceCount - 1)].name; }
ImFont* Theme::FontPreview(int index) {
    index = std::clamp(index, 0, kFaceCount - 1);
    static ImFont* cache[kFaceCount] = {};
    static bool tried[kFaceCount] = {};
    if (!tried[index]) {
        tried[index] = true;
        std::string regular = kFaces[index].regular, bold = kFaces[index].bold;
        if (index == 0) SystemFont(regular, bold);
        cache[index] = LoadFont(regular.c_str(), Px(21.0f));
    }
    return cache[index] ? cache[index] : body;
}

int Theme::FontIndex() { ReadOptions(); return g_face; }
int Theme::FontScale() { ReadOptions(); return g_fontScale; }

void Theme::SetFont(int index, int scale) {
    ReadOptions();
    g_face = std::clamp(index, 0, kFaceCount - 1);
    g_fontScale = std::clamp(scale, 80, 120);
    Config::SetValue("look", "font", std::to_string(g_face));
    Config::SetValue("look", "font_scale", std::to_string(g_fontScale));
    g_fontsDirty = true;
}

int Theme::DoodleColor() { return g_doodleColor; }
bool Theme::Cozy() { ReadOptions(); return g_cozy; }
bool Theme::CozyDark() { ReadOptions(); return g_cozyDark; }
bool Theme::Paper() { ReadOptions(); return g_paper; } // светлая поверхность: тетрадь, светлая тема или светлая атмосфера
void Theme::SetCozyDark(bool on) {
    ReadOptions();
    g_cozyDark = on;
    Config::SetValue("cozy", "dark", on ? "1" : "0");
}
int Theme::CozyPattern() { ReadOptions(); return g_cozyPattern; }

void Theme::SetCozy(bool on, int pattern) {
    ReadOptions();
    if (on != g_cozy && g_face == 0) g_fontsDirty = true; // «автоматически» на бумаге — почерк
    g_cozy = on;
    g_cozyPattern = pattern;
    Config::SetValue("cozy", "enabled", on ? "1" : "0");
    Config::SetValue("cozy", "pattern", std::to_string(pattern));
}

bool Theme::ButtonPlates() { return g_plates; }
