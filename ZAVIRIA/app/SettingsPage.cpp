#include "SettingsPage.h"

#include <map>
#include "Lang.h"
#include "PieMenu.h"
#include "ScreenTranslate.h"
#include "Graphics.h"
#include "Launcher.h"
#include "Knowledge.h"
#include "Security.h"
#include "Updater.h"
#include "RemoteDesktop.h"
#include "Autostart.h"
#include "EcoMode.h"
#include "HomeLayout.h"
#include "Uninstall.h"
#include "QrCode.h"
#include "Brain.h"
#include "DiscordRpc.h"
#include "Telegram.h"
#include "TelegramLogin.h"
#include "Wallpaper.h"
#include "LookFile.h"
#include "LocalAi.h"
#include "CozyArt.h"
#include "Http.h"
#include "Json.h"
#include "Platform.h"
#include <shellapi.h>
#include <shobjidl.h>
#include <filesystem>
#include "Ears.h"
#include "ModelStore.h"
#include "NeuralVoice.h"
#include "Overlay.h"
#include "Rates.h"
#include "Tour.h"
#include "Visuals.h"
#include "Notch.h"
#include "IslandSettings.h"
#include "Settings.h"
#include "TextUtils.h"
#include "Theme.h"
#include "Voice.h"
#include "Weather.h"
#include "Widgets.h"
#include "../ImGui/imgui_internal.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

using Theme::Px;
using Widgets::Icon;

namespace {
    // ---------------------------------------------------------------- разделы
    enum Section { General, PhoneSync, SecurityLink, AiVoice, OverlayPage, WidgetsSection, Appearance, About, kSections,
                   VoiceSection = 200, Hearing, AiSection, SearchSection, Overlay, Privacy, SystemSection, Updates }; // имена после kSections — только для инструкций

    struct SectionInfo { Icon icon; const char* name; const char* about; };
    const SectionInfo kInfo[kSections] = {
        { Icon::User, "Общее", "Обучение, имена, обновления и запуск" },
        { Icon::Phone, "Синхронизация", "Связь с телефоном — скоро" },
        { Icon::Lock, "Безопасность и связь", "Телефон, Telegram и Discord" },
        { Icon::Spark, "ИИ и голос", "Нейросеть, её характер, распознавание и озвучка" },
        { Icon::Layers, "Оверлей", "Музыка и нагрузка поверх всех окон" },
        { Icon::Grid, "Виджеты", "Что показывать на главном экране" },
        { Icon::Sun, "Внешний вид", "Фон, тема и эффекты окна" },
        { Icon::Book, "О приложении", "Версия ZAVIRIA, сброс и удаление" },
    };

    // плашка «В разработке»: раздел виден, но пока ничего не делает
    float SoonBadge(ImDrawList* dl, ImVec2 pos, ImFont* font, bool draw = true) {
        const char* text = Lang::Tr("В разработке");
        const float size = font->LegacySize * 0.82f;
        const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
        const ImVec2 pad(Px(8), Px(3));
        const ImVec2 b(pos.x + ts.x + pad.x * 2, pos.y + ts.y + pad.y * 2);
        if (draw) {
            const Theme::Color accent = Theme::Accent();
            dl->AddRectFilled(pos, b, accent.WithAlpha(0.22f), (b.y - pos.y) * 0.5f);
            dl->AddRect(pos, b, accent.WithAlpha(0.55f), (b.y - pos.y) * 0.5f, 0, Px(1));
            dl->AddText(font, size, ImVec2(pos.x + pad.x, pos.y + pad.y), Theme::kInk, text);
        }
        return b.x - pos.x;
    }

    bool g_open = false;
    bool g_justOpened = false;
    float g_anim = 0.0f;
    int g_section = General;
    int g_lastShown = -1;
    float g_sectionTop[16] = {};  // где на общей странице начинается каждый раздел
    int g_scrollToSection = -1;
    double g_holdSectionUntil = 0; // пока едем к разделу, подсветка меню не прыгает
    char g_query[128] = "";
    int g_sectionDir = 1;         // разделы листаются вверх или вниз
    float g_contentAnim = 1.0f;   // появление карточек при смене раздела или поиска
    bool g_colorDirty = false;    // цвет меняли в палитре — сохраним в файл, когда палитра закроется

    char g_name[64] = "", g_spoken[64] = "", g_city[96] = "", g_toCollege[16] = "", g_backHome[16] = "";
    char g_prompt[1024] = "";
    char g_token[128] = "", g_owner[256] = "", g_discordApp[32] = "", g_loginSecret[128] = "";

    std::mutex g_cityMutex;
    std::string g_cityStatus; // «ищу…», «нашла: …», «не нашла»
    bool g_citySearching = false;

    // копия строки в буфер ImGui (вместо strncpy_s — работает и на Linux)
    void Copy(char* buffer, size_t size, const std::string& text) { snprintf(buffer, size, "%s", text.c_str()); }
    template <size_t N> void Copy(char (&buffer)[N], const std::string& text) { Copy(buffer, N, text); }

    float ControlWidth(float width) { return std::min(width, Px(380)); }
    ImU32 Accent() { return Theme::Accent().WithAlpha(1.0f); }
    ImU32 PageColor(float alpha) { return Theme::Background(1).Mix(Theme::Background(0), 0.5f).WithAlpha(alpha); }

    // ---------------------------------------------------------------- маленькие помощники
    void ToggleSetting(const char* id, bool Settings::*field) {
        bool value = Config::Get().*field;
        if (Widgets::Toggle(id, ImGui::GetCursorScreenPos(), &value, Accent()))
            Config::Update([&](Settings& s) { s.*field = value; });
    }

    // Слайдер пишет в настройки на лету, а в файл — когда отпустили
    void SliderSetting(const char* id, int Settings::*field, int min, int max, float width, const char* format = "%d") {
        int value = Config::Get().*field;
        if (Widgets::Slider(id, &value, min, max, format, ControlWidth(width))) Config::Update([&](Settings& s) { s.*field = value; }, false);
        if (ImGui::IsItemDeactivatedAfterEdit()) Config::Save();
    }

    void TextSetting(const char* id, char* buffer, size_t size, float width, std::string Settings::*field, const char* fallback) {
        ImGui::SetNextItemWidth(ControlWidth(width));
        if (ImGui::InputText(id, buffer, size)) {
            const std::string value = Text::Trim(buffer);
            Config::Update([&](Settings& s) { s.*field = value.empty() ? fallback : value; });
        }
    }

    // «8:00», «08:00» → «08:00»; ерунда — пусто
    std::string NormalizeTime(const char* text) {
        const char* colon = std::strchr(text, ':');
        if (!colon || !std::isdigit(static_cast<unsigned char>(text[0])) || !std::isdigit(static_cast<unsigned char>(colon[1]))) return {};
        const int h = std::atoi(text), m = std::atoi(colon + 1);
        if (h < 0 || h > 23 || m < 0 || m > 59) return {};
        char out[8];
        snprintf(out, sizeof(out), "%02d:%02d", h, m);
        return out;
    }

    void TimeSetting(const char* id, char* buffer, size_t size, const char* key, const char* fallback) {
        ImGui::SetNextItemWidth(Px(90));
        ImGui::InputTextWithHint(id, fallback, buffer, size);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            const std::string time = NormalizeTime(buffer);
            if (time.empty()) Copy(buffer, size, Config::Value("weather", key, fallback)); // не время — вернуть как было
            else { Config::SetValue("weather", key, time); Copy(buffer, size, time); }
        }
    }

    // ---------------------------------------------------------------- элементы настроек
    char g_assistant[64] = "";
    void DrawAssistant(float w) {
        TextSetting("##assistant", g_assistant, sizeof(g_assistant), w, &Settings::assistant, "Аня");
    }
    void DrawName(float w) { TextSetting("##name", g_name, sizeof(g_name), w, &Settings::name, "друг"); }

    void DrawSpokenName(float w) { TextSetting("##spoken", g_spoken, sizeof(g_spoken), w, &Settings::spokenName, ""); }

    void DrawWakeWord(float) { ToggleSetting("##wakeWord", &Settings::wakeWord); }

    void DrawVoice(float w) {
        const auto voices = Voice::List();
        const std::wstring current = Config::Get().voice;
        const Voice::Info* chosen = Voice::Choose(voices, current);
        const std::string preview = current.empty()
            ? "авто — " + (chosen ? Text::ToUtf8(chosen->name) : std::string("нет голосов"))
            : Text::ToUtf8(current);
        const float listenWidth = ImGui::CalcTextSize("послушать").x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x; // список и кнопка вместе не шире блока
        if (Widgets::BeginCombo("##voice", preview.c_str(), std::min(ControlWidth(w), w - listenWidth))) {
            auto select = [](const std::wstring& name) {
                Config::Update([&](Settings& s) { s.voice = name; });
                Voice::ReloadVoice();
            };
            if (Widgets::ComboItem("авто (Silero — Ксения)", current.empty())) select(L"");
            for (const auto& v : voices) {
                const std::string label = Text::ToUtf8(v.name) + (v.female ? "  ♀" : "");
                if (Widgets::ComboItem(label.c_str(), v.name == current)) select(v.name);
            }
            Widgets::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("послушать")) {
            Brain::Interrupt();
            Voice::Speak("Привет, " + Config::Get().name + "! Вот так я звучу. Нравится?");
        }
    }

    void DrawNeuralPitch(float w) { SliderSetting("##neuralPitch", &Settings::neuralPitch, -6, 6, w); }
    void DrawPitch(float w) { SliderSetting("##pitch", &Settings::pitch, -10, 10, w); }
    void DrawRate(float w) { SliderSetting("##rate", &Settings::rate, -5, 6, w); }
    void DrawSilence(float w) { SliderSetting("##silence", &Settings::silenceMs, 300, 2000, w, "%d мс"); }
    void DrawBodySounds(float) { ToggleSetting("##bodySounds", &Settings::bodySounds); }

    void DrawPersonality(float w) {
        static const char* kNames[] = { "Строгая и холодная", "По умолчанию (50 на 50)", "Милая и верная", "Свой характер (промт)" };
        int current = static_cast<int>(Config::Get().personality);
                if (Widgets::Combo("##personality", &current, kNames, IM_ARRAYSIZE(kNames), ControlWidth(w)))
            Config::Update([&](Settings& s) { s.personality = static_cast<Personality>(current); });
        if (current != static_cast<int>(Personality::Custom)) return;

        if (ImGui::InputTextMultiline("##customPrompt", g_prompt, sizeof(g_prompt), ImVec2(w, Px(96)), ImGuiInputTextFlags_WordWrap))
            Config::Update([&](Settings& s) { s.customPrompt = g_prompt; }, false);
        if (ImGui::IsItemDeactivatedAfterEdit()) Config::Save();
        if (!g_prompt[0]) ImGui::TextDisabled("например: «отвечай как пират» или «будь дерзкой и шути»");
    }

    void StatusLine(const std::string& status, bool good);
    void IniText(const char* id, char* buffer, size_t size, const char* section, const char* key, const char* hint, float w, void (*reload)(),
                 ImGuiInputTextFlags flags);

    // Итог проверки: фраза переводится целиком, а подробности после «: » (ответ сервиса) — как есть
    void TrStatus(const std::string& status, bool good) {
        const size_t colon = status.find(": ");
        StatusLine(colon == std::string::npos ? std::string(Lang::Tr(status.c_str()))
                                              : std::string(Lang::Tr(status.substr(0, colon).c_str())) + status.substr(colon), good);
    }

    void DrawModel(float w) {
        const auto models = Ai::Models();
        const std::string current = Config::Get().model;
        auto fileName = [](const std::string& path) { return path.substr(path.find_last_of("\\/") + 1); };
        const std::string preview = models.empty() ? std::string("модели .gguf не найдены")
                                  : current.empty() ? "авто — " + Ai::ActiveModel() : fileName(current);
        if (Widgets::BeginCombo("##model", preview.c_str(), ControlWidth(w))) {
            auto select = [](const std::string& model) {
                Config::Update([&](Settings& s) { s.model = model; });
                Ai::Restart(); // перезапуск нейросети с новой моделью
            };
            if (Widgets::ComboItem("авто", current.empty())) select("");
            for (const auto& m : models)
                if (Widgets::ComboItem(fileName(m).c_str(), m == current)) select(m);
            Widgets::EndCombo();
        }
        if (Ai::CloudOn()) ImGui::TextDisabled("сейчас отвечает облачный ИИ — эта модель подождёт");

        // своя модель: копируем .gguf (и «глаза» mmproj рядом) в папку models
        float progress = 0.0f;
        if (Ai::Importing(progress)) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float barW = ControlWidth(w);
            const ImVec2 cursor = ImGui::GetCursorScreenPos();
            const ImVec2 a(cursor.x, cursor.y + Px(8));
            dl->AddRectFilled(a, ImVec2(a.x + barW, a.y + Px(6)), Theme::Overlay(24), Px(3));
            dl->AddRectFilled(a, ImVec2(a.x + barW * std::clamp(progress, 0.0f, 1.0f), a.y + Px(6)), Theme::Accent().WithAlpha(1.0f), Px(3));
            ImGui::Dummy(ImVec2(barW, Px(18)));
            ImGui::TextDisabled("%s %d%%", Lang::Tr("копирую модель…"), static_cast<int>(progress * 100.0f + 0.5f));
            return;
        }
        if (ImGui::Button("Добавить свою модель…")) Ai::ImportModel();
        bool good = false;
        if (const std::string status = Ai::ImportStatus(good); !status.empty()) TrStatus(status, good);
    }

    // ---------------------------------------------------------------- облачный ИИ
    struct CloudPreset { const char* name; const char* url; const char* model; };
    constexpr CloudPreset kCloudPresets[] = {
        { "OpenAI", "https://api.openai.com/v1", "gpt-4o-mini" },
        { "OpenRouter", "https://openrouter.ai/api/v1", "openai/gpt-4o-mini" },
        { "DeepSeek", "https://api.deepseek.com/v1", "deepseek-chat" },
    };
    constexpr int kCloudCustom = IM_ARRAYSIZE(kCloudPresets); // «свой адрес»
    char g_cloudUrl[256] = "", g_cloudKey[256] = "", g_cloudModel[128] = "";
    bool g_cloudCustom = false;

    bool CloudToggle() { return Config::Value("ai", "cloud", "0") == "1"; }
    void CloudReload() { if (CloudToggle()) Ai::CloudChanged(); } // адрес или ключ поменяли при включённом облаке

    void DrawCloudOn(float) {
        bool on = CloudToggle();
        if (Widgets::Toggle("##cloudOn", ImGui::GetCursorScreenPos(), &on, Accent())) {
            Config::SetValue("ai", "cloud", on ? "1" : "0");
            Ai::CloudChanged();
        }
    }

    int CloudPresetIndex() {
        const std::string url = Text::Trim(g_cloudUrl);
        for (int i = 0; i < kCloudCustom; ++i)
            if (url == kCloudPresets[i].url) return i;
        return kCloudCustom;
    }

    void DrawCloudUrl(float w) {
        static const char* kNames[] = { kCloudPresets[0].name, kCloudPresets[1].name, kCloudPresets[2].name, "Свой адрес" };
        int current = g_cloudCustom ? kCloudCustom : CloudPresetIndex();
        if (!g_cloudCustom && Text::Trim(g_cloudUrl).empty()) current = -1; // ещё ничего не выбрали
        const char* preview = current < 0 ? "выбери сервис" : kNames[current];
        if (Widgets::BeginCombo("##cloudUrl", preview, ControlWidth(w))) {
            for (int i = 0; i <= kCloudCustom; ++i) {
                if (!Widgets::ComboItem(kNames[i], i == current)) continue;
                if (i == kCloudCustom) { g_cloudCustom = true; break; }
                g_cloudCustom = false;
                const std::string model = Text::Trim(g_cloudModel);
                bool presetModel = model.empty();
                for (const CloudPreset& p : kCloudPresets) presetModel |= model == p.model;
                Copy(g_cloudUrl, kCloudPresets[i].url);
                Config::SetValue("ai", "cloud_url", kCloudPresets[i].url);
                if (presetModel) { // модель от другого сервиса здесь не подойдёт — подставляем обычную для этого
                    Copy(g_cloudModel, kCloudPresets[i].model);
                    Config::SetValue("ai", "cloud_model", kCloudPresets[i].model);
                }
                CloudReload();
            }
            Widgets::EndCombo();
        }
        if (current == kCloudCustom)
            IniText("##cloudUrlText", g_cloudUrl, sizeof(g_cloudUrl), "ai", "cloud_url", "https://…/v1", w, CloudReload, 0);
    }

    void DrawCloudKey(float w) {
        IniText("##cloudKey", g_cloudKey, sizeof(g_cloudKey), "ai", "cloud_key", "sk-…", w, CloudReload, ImGuiInputTextFlags_Password);
    }

    void DrawCloudModel(float w) {
        const int preset = CloudPresetIndex();
        IniText("##cloudModel", g_cloudModel, sizeof(g_cloudModel), "ai", "cloud_model", preset < kCloudCustom ? kCloudPresets[preset].model : "название модели", w,
                [] {}, 0);
    }

    void DrawCloudCheck(float) {
        if (ImGui::Button("Проверить")) Ai::TestCloud();
        bool good = false;
        const std::string status = Ai::CloudStatus(good);
        if (!status.empty()) TrStatus(status, good);
        else if (CloudToggle() && !Ai::CloudOn()) { TrStatus("пока отвечает своя модель — нужны адрес и ключ", false); }
    }

    void DrawAiIdle(float w) {
        static const char* kNames[] = { "никогда", "через 5 минут", "через 10 минут", "через 30 минут" };
        static const int kMinutes[] = { 0, 5, 10, 30 };
        const int now = std::atoi(Config::Value("ai", "idle_minutes", "10").c_str());
        int current = 2;
        for (int i = 0; i < 4; ++i) if (kMinutes[i] == now) current = i;
                if (Widgets::Combo("##aiIdle", &current, kNames, 4, ControlWidth(w))) Config::SetValue("ai", "idle_minutes", std::to_string(kMinutes[current]));
    }

    void DrawSearchEngine(float w) {
        static const char* kNames[] = { "Google", "Яндекс", "DuckDuckGo", "Bing" };
        int current = static_cast<int>(Config::Get().search);
                if (Widgets::Combo("##search", &current, kNames, IM_ARRAYSIZE(kNames), ControlWidth(w)))
            Config::Update([&](Settings& s) { s.search = static_cast<SearchEngine>(current); });
    }

    void DrawQuickPhrases(float) { ToggleSetting("##quickPhrases", &Settings::quickPhrases); }
    void DrawWeatherWidget(float) { ToggleSetting("##weatherWidget", &Settings::weatherWidget); }

    void FindCityAsync(const std::string& name) {
        {
            std::lock_guard lock(g_cityMutex);
            if (g_citySearching) return;
            g_citySearching = true;
            g_cityStatus = "ищу…";
        }
        std::thread([name] {
            const auto city = Weather::FindCity(name);
            if (city) Weather::SetCity(*city);
            std::lock_guard lock(g_cityMutex);
            g_citySearching = false;
            g_cityStatus = city ? "нашла: " + city->name + (city->country.empty() ? "" : ", " + city->country) : "не нашла такой город";
        }).detach();
    }

    void DrawCity(float w) {
        ImGui::SetNextItemWidth(ControlWidth(w) - Px(90));
        const bool enter = ImGui::InputTextWithHint("##city", "например, Ташкент", g_city, sizeof(g_city), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((ImGui::Button("найти") || enter) && !Text::Trim(g_city).empty()) FindCityAsync(g_city);
        std::lock_guard lock(g_cityMutex);
        if (!g_cityStatus.empty()) ImGui::TextDisabled("%s", g_cityStatus.c_str());
    }

    // ---------------------------------------------------------------- внешний вид
    ImVec4 ToVec(int rgb) {
        const Theme::Color c = Theme::FromRgb(rgb);
        return ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f);
    }
    int FromVec(const ImVec4& v) { return Theme::ToRgb({ v.x * 255.0f, v.y * 255.0f, v.z * 255.0f }); }

    // Палитра во всплывающем окне; true — цвет поменяли
    bool ColorPopup(const char* id, int& rgb) {
        bool changed = false;
        if (ImGui::BeginPopup(id)) {
            // Держим точное значение между кадрами: из округлённого цвета колесо на тёмных оттенках теряет положение
            static std::unordered_map<std::string, ImVec4> s_exact;
            ImVec4& v = s_exact[id];
            if (s_exact.size() > 0 && (v.w != 1.0f || FromVec(v) != rgb)) v = ToVec(rgb);
            ImGui::SetNextItemWidth(Px(220));
            if (ImGui::ColorPicker3("##picker", &v.x, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueWheel)) {
                rgb = FromVec(v);
                changed = true;
            }
            ImGui::EndPopup();
        }
        return changed;
    }

    void DrawLookPresets(float w) {
        const float right = ImGui::GetCursorScreenPos().x + w; // переносим по ширине колонки, а не окна — иначе вылезает за островок
        for (int i = 0; i < LookFile::PresetCount(); ++i) {
            if (i) {
                ImGui::SameLine(0, Px(8));
                const float need = ImGui::CalcTextSize(Lang::Tr(LookFile::PresetName(i))).x + Px(34);
                if (ImGui::GetCursorScreenPos().x + need > right) ImGui::NewLine();
            }
            if (Widgets::Radio(LookFile::PresetName(i), false)) LookFile::ApplyPreset(i);
        }
    }
    void DrawLookFile(float w) {
        if (Widgets::Action("сохранить в файл", Widgets::Icon::Download, Widgets::ActionStyle::Primary)) LookFile::SaveWithDialog();
        if (Widgets::Action("открыть файл", Widgets::Icon::Book, Widgets::ActionStyle::Normal, true)) LookFile::OpenWithDialog();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w); // перенос по ширине колонки — не вылезает за островок
        ImGui::TextDisabled("%s", "или просто перетащи файл .zavirialook на окно Ани");
        if (const std::string status = LookFile::Status(); !status.empty()) ImGui::TextDisabled("%s", status.c_str());
        ImGui::PopTextWrapPos();
    }
    void DrawStyle(float) {
        static const char* kNames[] = { "обычные", "пилюли", "чёткие", "контур" };
        const int style = Theme::Style();
        for (int i = 0; i < 4; ++i) {
            if (i) ImGui::SameLine(0, Px(8));
            if (Widgets::Radio(kNames[i], style == i)) Theme::SetStyle(i);
        }
    }
    void DrawMode(float) {
        static const char* kNames[] = { "тёмная", "светлая", "как в Windows" };
        const int mode = Theme::Mode();
        for (int i = 0; i < 3; ++i) {
            if (i) ImGui::SameLine(0, Px(8));
            if (Widgets::Radio(kNames[i], mode == i)) Theme::SetMode(i);
        }
    }
    // Атмосфера: плитки с настоящей палитрой (в текущем светлом или тёмном варианте); «Свои цвета» — прежние темы
    void DrawLookTiles(float w);
    void DrawLook(float w) {
        const int current = Theme::Atmosphere();
        const bool light = Theme::Light();
        const ImVec2 tile(Px(104), Px(64));
        const float gap = Px(14), rowHeight = tile.y + Theme::semibold->LegacySize + Px(20);
        const int perRow = std::max(1, static_cast<int>((w + gap) / (tile.x + gap)));
        const ImVec2 start = ImGui::GetCursorScreenPos();
        for (int i = 0; i < Theme::kAtmospheres; ++i) {
            const Theme::Atmo p = Theme::AtmospherePalette(i, light);
            const ImVec2 pos(start.x + (i % perRow) * (tile.x + gap), start.y + Px(4) + (i / perRow) * rowHeight);
            ImGui::PushID(i);
            const ImU32 panel = i == Theme::kAtmosphereCustom ? Theme::SurfaceSolid() : p.surface.WithAlpha(1.0f);
            const ImU32 top = i == Theme::kAtmosphereCustom ? Theme::Background(0).WithAlpha(1.0f) : p.top.WithAlpha(1.0f);
            const ImU32 bottom = i == Theme::kAtmosphereCustom ? Theme::Background(1).WithAlpha(1.0f) : p.accent.Mix(p.bottom, 0.55f).WithAlpha(1.0f);
            if (Widgets::ThemeTile("##atmo", pos, tile, top, bottom, panel, Theme::AtmosphereName(i), current == i)) Theme::SetAtmosphere(i);
            ImGui::PopID();
        }
        const int rows = (Theme::kAtmospheres + perRow - 1) / perRow;
        ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + rows * rowHeight));
        ImGui::Dummy(ImVec2(w, 0));
        if (current == Theme::kAtmosphereCustom) DrawLookTiles(w);
        if (current == 2) { // «Домашний уют»: не только карамель
            static const char* kCozy[] = { "карамель", "роза", "шалфей", "лаванда", "черника" };
            ImGui::TextDisabled("%s", Lang::Tr("цвет уюта"));
            const int on = Theme::CozyAccent();
            for (int i = 0; i < 5; ++i) {
                if (i) ImGui::SameLine(0, Px(8));
                if (Widgets::Radio(kCozy[i], on == i)) Theme::SetCozyAccent(i);
            }
        }
    }
    void DrawLookTiles(float w) {
        static const char* kNames[] = { "Ночь", "Графит", "Уголь", "Полночь", "Своя" };
        const Settings s = Config::Get();
        const ImVec2 tile(Px(92), Px(58));
        const float gap = Px(14), rowHeight = tile.y + Theme::semibold->LegacySize + Px(20);
        const int perRow = std::max(1, static_cast<int>((w + gap) / (tile.x + gap)));
        const ImVec2 start = ImGui::GetCursorScreenPos();
        for (int i = 0; i < IM_ARRAYSIZE(kNames); ++i) {
            const auto look = Theme::LookPreset(i, s.lookColor);
            const ImVec2 pos(start.x + (i % perRow) * (tile.x + gap), start.y + Px(4) + (i / perRow) * rowHeight);
            ImGui::PushID(i);
            if (Widgets::ThemeTile("##look", pos, tile, look.top.WithAlpha(1.0f), look.bottom.WithAlpha(1.0f), look.surface.WithAlpha(1.0f),
                    kNames[i], static_cast<int>(s.look) == i)) {
                Config::Update([&](Settings& x) { x.look = static_cast<Look>(i); });
                if (i == 4) ImGui::OpenPopup("##lookColor");
            }
            int rgb = s.lookColor;
            if (i == 4 && ColorPopup("##lookColor", rgb)) {
                Config::Update([&](Settings& x) { x.lookColor = rgb; }, false);
                g_colorDirty = true;
            }
            ImGui::PopID();
        }
        const int rows = (IM_ARRAYSIZE(kNames) + perRow - 1) / perRow;
        ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + rows * rowHeight));
        ImGui::Dummy(ImVec2(w, 0));
    }

    void DrawAccent(float w) {
        const Settings s = Config::Get();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float r = Px(12), step = Px(38);
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float cy = start.y + r + Px(6);
        for (int i = 0; i < Theme::kAccentPresets; ++i) {
            ImGui::PushID(i);
            if (Widgets::Swatch("##accent", ImVec2(start.x + r + Px(4) + i * step, cy), r, Theme::AccentPreset(i).WithAlpha(1.0f), s.accent == i))
                Config::Update([&](Settings& x) { x.accent = i; });
            ImGui::PopID();
        }
        // свой цвет: кружок с радужной каймой и палитрой
        const ImVec2 own(start.x + r + Px(4) + Theme::kAccentPresets * step + Px(12), cy);
        for (int k = 0; k < 24; ++k) {
            float red, green, blue;
            ImGui::ColorConvertHSVtoRGB(k / 24.0f, 0.55f, 1.0f, red, green, blue);
            const float a0 = k * 6.2831853f / 24.0f, a1 = (k + 1) * 6.2831853f / 24.0f;
            dl->PathArcTo(own, r + Px(1.5f), a0, a1 + 0.02f, 3);
            dl->PathStroke(ImGui::ColorConvertFloat4ToU32(ImVec4(red, green, blue, 1.0f)), 0, Px(3));
        }
        if (Widgets::Swatch("##accentOwn", own, r - Px(3), Theme::FromRgb(s.accentColor).WithAlpha(1.0f), s.accent == Theme::kAccentPresets)) {
            Config::Update([&](Settings& x) { x.accent = Theme::kAccentPresets; });
            ImGui::OpenPopup("##accentColor");
        }
        Widgets::Text(dl, Theme::semibold, ImVec2(own.x + r + Px(10), cy - Theme::semibold->LegacySize * 0.55f), Theme::kInkSoft, "свой цвет");
        int rgb = s.accentColor;
        if (ColorPopup("##accentColor", rgb)) {
            Config::Update([&](Settings& x) { x.accentColor = rgb; x.accent = Theme::kAccentPresets; }, false);
            g_colorDirty = true;
        }
        ImGui::SetCursorScreenPos(ImVec2(start.x, cy + r + Px(10)));
        ImGui::Dummy(ImVec2(w, 0));
    }

    void DrawWallpaper(float w) {
        const std::string path = Config::Get().wallpaper;
        if (ImGui::Button(path.empty() ? "выбрать картинку…" : "другая картинка…")) Wallpaper::PickAsync();
        if (!path.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("убрать")) {
                Config::Update([](Settings& x) { x.wallpaper.clear(); });
                if (Config::Get().autoColors) Wallpaper::ApplyAutoColors(false);
            }
            const std::string name = path.substr(path.find_last_of("\\/") + 1);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
            ImGui::TextDisabled("%s", name.c_str());
            ImGui::PopTextWrapPos();
        }
    }
    void DrawWallpaperDim(float w) { SliderSetting("##wallpaperDim", &Settings::wallpaperDim, 0, 100, w, "%d%%"); }
    void DrawAutoColors(float) {
        bool on = Config::Get().autoColors;
        if (Widgets::Toggle("##autoColors", ImGui::GetCursorScreenPos(), &on, Accent())) Wallpaper::ApplyAutoColors(on && Wallpaper::Loaded());
    }

    void DrawBackdrop(float w) {
        static const char* kNames[] = { "Сияние", "Нити", "Волны", "Звёздный полёт", "Спокойный (без движения)" };
        int current = static_cast<int>(Config::Get().backdrop);
                if (Widgets::Combo("##backdrop", &current, kNames, IM_ARRAYSIZE(kNames), ControlWidth(w)))
            Config::Update([&](Settings& x) { x.backdrop = static_cast<Backdrop>(current); });
    }

    void DrawBackdropSpeed(float w) { // середина ползунка — обычная скорость
        const int speed = Config::Get().backdropSpeed;
        int position = speed <= 100 ? (speed - 20) * 50 / 80 : 50 + (speed - 100) * 50 / 200;
        if (Widgets::Slider("##backdropSpeed", &position, 0, 100, "%d%%", ControlWidth(w))) {
            const int value = position <= 50 ? 20 + position * 80 / 50 : 100 + (position - 50) * 200 / 50;
            Config::Update([&](Settings& s) { s.backdropSpeed = value; }, false);
        }
    }
    void DrawAnimateInactive(float) { ToggleSetting("##animateInactive", &Settings::animateInactive); }

    // Свой цвет поверх автоматического: образец цвета (нажми — палитра) и «сбросить»
    void ColorOverride(const char* id, int Settings::*field, const Theme::Color& fallback, const char* autoLabel) {
        const int value = Config::Get().*field;
        const ImVec2 pos = ImGui::GetCursorScreenPos(), size(Px(48), Px(28));
        ImGui::PushID(id);
        if (ImGui::InvisibleButton("##swatch", size)) ImGui::OpenPopup("##color");
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), (value >= 0 ? Theme::FromRgb(value) : fallback).WithAlpha(1.0f), Px(8));
        dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), Theme::Overlay(hovered ? 90 : 40), Px(8));
        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (size.y - ImGui::GetFrameHeight()) * 0.5f);
        if (value >= 0) {
            if (ImGui::Button("сбросить")) Config::Update([&](Settings& x) { x.*field = -1; });
        } else {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ImGui::GetFrameHeight() - ImGui::GetTextLineHeight()) * 0.5f);
            ImGui::TextDisabled("%s", autoLabel);
        }
        int rgb = value >= 0 ? value : Theme::ToRgb(fallback);
        if (ColorPopup("##color", rgb)) {
            Config::Update([&](Settings& x) { x.*field = rgb; }, false);
            g_colorDirty = true;
        }
        ImGui::PopID();
    }

    void DrawBackgroundColor(float) {
        const Settings s = Config::Get();
        ColorOverride("bg", &Settings::backgroundColor, Theme::LookPreset(static_cast<int>(s.look), s.lookColor).top, "по теме");
    }
    void DrawAnimationColor(float) { ColorOverride("anim", &Settings::animationColor, Theme::Palette(0), "по моему настроению"); }
    void DrawGlass(float) { ToggleSetting("##glass", &Settings::glass); }
    void DrawPlates(float) { ToggleSetting("##plates", &Settings::buttonPlates); }
    void DrawTray(float) { ToggleSetting("##tray", &Settings::tray); }
    void DrawStartHidden(float);

    // ---------------------------------------------------------------- автозапуск, эффект окна
    void DrawAutostart(float) {
        bool on = Autostart::Enabled();
        if (Widgets::Toggle("##autostart", ImGui::GetCursorScreenPos(), &on, Accent())) Autostart::Set(on);
    }

    // ---------------------------------------------------------------- режим экономии
    void DrawEco(float) {
        bool on = EcoMode::Active();
        if (Widgets::Toggle("##ecoMode", ImGui::GetCursorScreenPos(), &on, Accent())) EcoMode::RequestToggle();
        if (EcoMode::Busy()) { ImGui::SameLine(); ImGui::TextDisabled("%s", Lang::Tr("переключаю…")); }
    }
    void DrawEcoPreview(float) { if (ImGui::Button("что будет ограничено")) EcoMode::OpenPreview(); }
    void DrawEcoBrightness(float w) {
        static int s_level = -1;
        if (s_level < 0) s_level = std::clamp(std::atoi(Config::Value("eco", "brightness", "40").c_str()), 5, 100);
        Widgets::Slider("##ecoBrightness", &s_level, 5, 100, "%d%%", ControlWidth(w));
        if (ImGui::IsItemDeactivatedAfterEdit()) Config::SetValue("eco", "brightness", std::to_string(s_level));
    }

    // Список имён exe: «таблетки» (нажми — убрать) и поле, чтобы добавить
    void EcoList(const char* key, char* buffer, size_t size, float w) {
        auto items = EcoMode::List(key);
        const ImVec2 start = ImGui::GetCursorScreenPos();
        float x = start.x, y = start.y, chipH = 0;
        int remove = -1;
        for (size_t i = 0; i < items.size(); ++i) {
            const std::string label = items[i] + "  ×";
            const float chipW = Widgets::TextSize(Theme::semibold, label.c_str()).x + Px(28);
            if (x > start.x && x + chipW > start.x + w) { x = start.x; y += chipH + Px(6); }
            ImVec2 chip;
            if (Widgets::Chip((std::string("##") + key + std::to_string(i)).c_str(), ImVec2(x, y), label.c_str(), &chip)) remove = static_cast<int>(i);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Lang::Tr("убрать из списка"));
            chipH = chip.y;
            x += chip.x + Px(6);
        }
        if (remove >= 0) { items.erase(items.begin() + remove); EcoMode::SetList(key, items); }
        if (!items.empty()) ImGui::SetCursorScreenPos(ImVec2(start.x, y + chipH + Px(8)));
        else { ImGui::TextDisabled("%s", Lang::Tr("список пуст")); }
        ImGui::SetNextItemWidth(std::min(ControlWidth(w) - Px(110), Px(260)));
        const bool enter = ImGui::InputTextWithHint((std::string("##add") + key).c_str(), "program.exe", buffer, size, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((ImGui::Button((std::string(Lang::Tr("добавить")) + "##" + key).c_str()) || enter) && !Text::Trim(buffer).empty()) {
            items.push_back(buffer);
            EcoMode::SetList(key, items);
            buffer[0] = 0;
        }
    }
    char g_ecoWhite[96] = "", g_ecoSuspend[96] = "", g_ecoClose[96] = "";
    void DrawEcoWhitelist(float w) { EcoList("whitelist", g_ecoWhite, sizeof(g_ecoWhite), w); }
    void DrawEcoSuspend(float w) { EcoList("suspend", g_ecoSuspend, sizeof(g_ecoSuspend), w); }
    void DrawEcoClose(float w) { EcoList("close", g_ecoClose, sizeof(g_ecoClose), w); }

    void DrawWindowEffect(float w) {
        static const char* kNames[] = { "Нет — плотный фон", "Mica — мягкий оттенок обоев", "Акрил — размытие того, что за окном" };
        int current = Config::Get().windowEffect;
                if (Widgets::Combo("##windowEffect", &current, kNames, IM_ARRAYSIZE(kNames), ControlWidth(w))) Config::Update([&](Settings& x) { x.windowEffect = current; });
    }
    void DrawWindowOpacity(float w) { SliderSetting("##windowOpacity", &Settings::windowOpacity, 0, 100, w, "%d%%"); }

    // ---------------------------------------------------------------- телеграм и Discord (раздел anya.ini, не Settings)
    void IniToggle(const char* id, const char* section, const char* key, const char* fallback, void (*reload)()) {
        bool on = Config::Value(section, key, fallback) == "1";
        if (Widgets::Toggle(id, ImGui::GetCursorScreenPos(), &on, Accent())) {
            Config::SetValue(section, key, on ? "1" : "0");
            reload();
        }
    }

    void IniText(const char* id, char* buffer, size_t size, const char* section, const char* key, const char* hint, float w, void (*reload)(),
                 ImGuiInputTextFlags flags = 0) {
        ImGui::SetNextItemWidth(ControlWidth(w));
        ImGui::InputTextWithHint(id, hint, buffer, size, flags);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            Config::SetValue(section, key, Text::Trim(buffer));
            reload();
        }
    }

    void StatusLine(const std::string& status, bool good) {
        ImGui::PushStyleColor(ImGuiCol_Text, good ? Theme::kOk : Theme::kInkMuted);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(("● " + status).c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }

    void DrawAutoLang(float) { IniToggle("##autoLang", "ears", "autolang", "1", [] {}); } // Ears читает при каждой фразе
    void DrawTelegram(float) { IniToggle("##telegram", "telegram", "enabled", "1", Telegram::Reload); }
    void DrawTelegramToken(float w) {
        IniText("##token", g_token, sizeof(g_token), "telegram", "token", "123456:ABC… от @BotFather", w, Telegram::Reload, ImGuiInputTextFlags_Password);
        StatusLine(Telegram::Status(), Telegram::Ready());
    }
    // QR-код: камера телефона → файл с GitHub (открывается всегда и сразу). Пока QR на экране, ПК рассылает по Wi-Fi
    // код привязки — свежее приложение при первом запуске ловит его и подключается само
    // Без браузера: по Bluetooth (окно Windows «Отправка файлов») или файлом по кабелю — Проводник сразу показывает ZAVIRIA.apk
    void DrawPhoneFileWays(float width) {
        const std::wstring apk = Text::ExecutableDirectory() + L"\\phone\\zaviria.apk";
        const bool have = GetFileAttributesW(apk.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (!have) { ImGui::TextDisabled("%s", Lang::Tr("приложения для телефона рядом с программой нет")); return; }
        { // Quick Share от Google (если стоит): одна кнопка — на телефоне всплывёт «Принять?»
            static const std::wstring kQuickShare = L"C:\\Program Files\\Google\\NearbyShare\\nearby_share.exe";
            if (GetFileAttributesW(kQuickShare.c_str()) != INVALID_FILE_ATTRIBUTES &&
                Widgets::Action("отправить через Quick Share", Icon::Phone, Widgets::ActionStyle::Primary)) {
                const std::wstring arg = L"--files-from=context-menu \"" + apk + L"\""; // так открывает его пункт «Отправить» в меню Проводника — файл уже выбран
                ShellExecuteW(nullptr, L"open", kQuickShare.c_str(), arg.c_str(), nullptr, SW_SHOWNORMAL);
            }
        }
        if (Widgets::Action("отправить по Bluetooth", Icon::Send)) {
            ShellExecuteW(nullptr, L"open", L"fsquirt.exe", L"-send", nullptr, SW_SHOWNORMAL); // мастер Windows: выбрать телефон, потом файл
            const std::wstring select = L"/select,\"" + apk + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", select.c_str(), nullptr, SW_SHOWNORMAL);
        }
        if (Widgets::Action("показать файл", Icon::Download, Widgets::ActionStyle::Normal, true)) { // рядом с Bluetooth — обе короткие
            const std::wstring select = L"/select,\"" + apk + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", select.c_str(), nullptr, SW_SHOWNORMAL);
        }
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(Px(220), width));
        ImGui::TextDisabled("%s", Lang::Tr("Bluetooth: в окне Windows выбери телефон, потом файл ZAVIRIA.apk из открывшейся папки — на телефоне прими файл и нажми на него. Кабель: подключи телефон («Передача файлов»), перетащи ZAVIRIA.apk в «Download» и открой его на телефоне через «Файлы»."));
        ImGui::PopTextWrapPos();
    }

    void DrawPhoneInstall(float w) {
        static bool s_show = false;
        if (!s_show) {
            if (Widgets::Action("показать QR-код", Icon::Phone)) {
                s_show = true;
                if (Security::PhoneCode().empty()) Security::OpenPhoneCode();
                RemoteDesktop::ShareCode(true);
                RemoteDesktop::StartPhoneBeacon();
            }
            DrawPhoneFileWays(w);
            return;
        }
        if (Security::PhoneCode().empty()) { Security::OpenPhoneCode(); RemoteDesktop::StartPhoneBeacon(); } // 5 минут прошли — новый код
        static const std::string kLink = "https://github.com/zephiro4ka/ZAVIRIA-Voice-assistant/raw/main/phone/ZAVIRIA.apk";
        static const std::vector<bool> s_modules = QrCode::Encode(kLink);
        if (s_modules.empty()) { ImGui::TextUnformatted(kLink.c_str()); return; }
        const float cell = Px(3.6f), quiet = Px(10), side = QrCode::kSize * cell + quiet * 2; // компактно: код слева, текст и кнопки справа
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + side, p.y + side), IM_COL32(255, 255, 255, 255), Px(14)); // белая подложка — камера читает только тёмное на светлом
        for (int y = 0; y < QrCode::kSize; ++y)
            for (int x = 0; x < QrCode::kSize; ++x)
                if (s_modules[y * QrCode::kSize + x])
                    dl->AddRectFilled(ImVec2(p.x + quiet + x * cell, p.y + quiet + y * cell), ImVec2(p.x + quiet + (x + 1) * cell, p.y + quiet + (y + 1) * cell), IM_COL32(20, 20, 28, 255));
        const float startX = ImGui::GetCursorPosX(), colX = startX + side + Px(14);
        const float colW = std::max(Px(160), w - side - Px(14));
        ImGui::SetCursorScreenPos(ImVec2(p.x + side + Px(14), p.y));
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(colX + colW);
        ImGui::TextColored(ImColor(Theme::kInkMuted), "%s", Lang::Tr("Наведи камеру — скачается ZAVIRIA. Открой её: телефон в том же Wi-Fi подключится сам."));
        const std::string status = RemoteDesktop::BeaconStatus();
        if (!status.empty()) ImGui::TextColored(ImColor(Theme::kInkMuted), "%s", Lang::Tr(status.c_str()));
        ImGui::PopTextWrapPos();
        if (Widgets::Action("сбросить код", Icon::Spark)) { Security::ClosePhoneCode(); Security::OpenPhoneCode(); }
        if (Widgets::Action("скрыть", Icon::Close, Widgets::ActionStyle::Normal, true)) { s_show = false; RemoteDesktop::ShareCode(false); }
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos(ImVec2(p.x, std::max(p.y + side, ImGui::GetItemRectMax().y) + Px(6)));
        ImGui::Dummy(ImVec2(1, 1));
    }

    void DrawPhoneLink(float width) {
        static std::string s_error;
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(Px(220), width)); // текст по ширине колонки, не за край
        struct PopWrap { ~PopWrap() { ImGui::PopTextWrapPos(); } } popWrap;
        const std::string code = Security::PhoneCode(), name = Config::Value("phone", "name", "");
        const bool paired = !Config::Value("phone", "key", "").empty();
        if (!code.empty()) {
            // без QR: телефон в том же Wi-Fi сам находит компьютер по маяку, нужен только код
            const std::string spaced = code.substr(0, 4) + " " + code.substr(4);
            ImGui::TextDisabled("Введи этот код в приложении ZAVIRIA на телефоне (телефон в том же Wi-Fi):");
            ImGui::PushFont(Theme::plain, Theme::plain->LegacySize);
            ImGui::TextUnformatted(spaced.c_str());
            ImGui::PopFont();
            ImGui::TextDisabled("Действует 5 минут");
            const std::string beacon = RemoteDesktop::BeaconStatus();
            if (!beacon.empty()) ImGui::TextDisabled("%s", Lang::Tr(beacon.c_str()));
            if (Widgets::Action("сбросить код", Icon::Spark)) { Security::ClosePhoneCode(); Security::OpenPhoneCode(); } // старый код больше не подойдёт
            return;
        }
        if (paired) {
            ImGui::Text("Подключён: %s", name.c_str());
            ImGui::TextDisabled("Связь защищена: SPAKE2 и сквозное шифрование");
            const auto unlink = RemoteDesktop::PhoneUnlinkState(); // отключение — только с согласия обеих сторон
            if (unlink.done) ImGui::TextDisabled("Отключение подтверждено, телефон отвязывается…");
            else if (unlink.phone && !unlink.pc) {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "Телефон просит отключиться от компьютера");
                if (Widgets::Action("подтвердить отключение", Widgets::Icon::Close, Widgets::ActionStyle::Danger)) RemoteDesktop::ApprovePhoneUnlink();
                if (Widgets::Action("отклонить", Widgets::Icon::Previous, Widgets::ActionStyle::Normal, true)) RemoteDesktop::CancelPhoneUnlink();
            } else if (unlink.pc && !unlink.phone) {
                ImGui::TextDisabled("Подтверди отключение на телефоне (откроется в приложении ZAVIRIA)");
                if (Widgets::Action("отмена", Widgets::Icon::Previous)) RemoteDesktop::CancelPhoneUnlink();
            } else if (Widgets::Action("отключить телефон", Widgets::Icon::Close, Widgets::ActionStyle::Danger)) RemoteDesktop::ApprovePhoneUnlink();
            return;
        }
        if (Widgets::Action("подключить телефон", Widgets::Icon::Phone, Widgets::ActionStyle::Primary)) {
            s_error.clear();
            Security::OpenPhoneCode();
            RemoteDesktop::StartPhoneBeacon();
        }
        if (!s_error.empty()) ImGui::TextDisabled("%s", s_error.c_str());
    }

    void DrawTwoFactor(float w) {
        static std::string s_secret, s_error;
        static char s_code[16] = "";
        if (Security::TwoFactorEnabled()) {
            ImGui::TextUnformatted("Включено.");
            if (ImGui::Button("выключить")) { Security::DisableTwoFactor(); s_secret.clear(); s_error.clear(); }
            return;
        }
        if (s_secret.empty()) {
            if (ImGui::Button("настроить код")) s_secret = Security::NewTwoFactorSecret();
            return;
        }
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
        ImGui::TextUnformatted("Добавь этот ключ в Google Authenticator: «ввести ключ вручную», тип «по времени». Никому его не показывай.");
        ImGui::PopTextWrapPos();
        ImGui::TextUnformatted(s_secret.c_str());
        if (ImGui::Button("скопировать ключ")) ImGui::SetClipboardText(s_secret.c_str());
        ImGui::SetNextItemWidth(std::min(w, Px(220)));
        ImGui::InputTextWithHint("##totp", "код из приложения", s_code, sizeof(s_code), ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine();
        if (ImGui::Button("подтвердить")) {
            if (Security::EnableTwoFactor(s_secret, s_code)) { s_secret.clear(); s_error.clear(); s_code[0] = 0; ImGui::SetClipboardText(""); }
            else s_error = "Код не подошёл. Проверь время на телефоне.";
        }
        if (!s_error.empty()) ImGui::TextDisabled("%s", s_error.c_str());
    }

    void DrawTelegramOwner(float w) {
        IniText("##owner", g_owner, sizeof(g_owner), "telegram", "owner", "например: 1234567890, 0987654321", w, Telegram::Reload);
    }
    void DrawSyncSoon(float) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = SoonBadge(nullptr, p, Theme::semibold, false);
        ImGui::Dummy(ImVec2(w, Theme::semibold->LegacySize * 0.82f + Px(6)));
        SoonBadge(ImGui::GetWindowDrawList(), p, Theme::semibold);
    }
    void DrawGreet(float) { IniToggle("##greet", "telegram", "greet", "1", [] {}); }
    void DrawRemoteDesktop(float) { IniToggle("##remoteDesktop", "telegram", "remote_desktop", "1", Telegram::Reload); }
    void DrawLockOnStandby(float) { IniToggle("##lockOnStandby", "telegram", "lock_on_standby", "0", Telegram::Reload); }
    void DrawDiscord(float) { IniToggle("##discord", "discord", "enabled", "1", DiscordRpc::Reload); }
    void DrawDiscordApp(float w) {
        IniText("##discordApp", g_discordApp, sizeof(g_discordApp), "discord", "app_id", Lang::Tr("встроенное ZAVIRIA"), w, DiscordRpc::Reload, ImGuiInputTextFlags_CharsDecimal);
        StatusLine(DiscordRpc::Status(), DiscordRpc::Status().rfind("показываю", 0) == 0);
    }
    void DrawDiscordMusic(float) { IniToggle("##discordMusic", "discord", "show_music", "1", DiscordRpc::Reload); }
    void DrawDiscordVideo(float) { IniToggle("##discordVideo", "discord", "show_video", "1", DiscordRpc::Reload); }

    // ---------------------------------------------------------------- полная кастомизация
    void DrawEditBlocks(float) {
        bool on = HomeLayout::Editing();
        if (Widgets::Check("После нажатия на данную галочку вы можете редактировать позицию ЛЮБОГО БЛОКА", &on)) {
            HomeLayout::SetEditing(on);
            if (on) SettingsPage::Close(); // сразу на главный экран — там блоки уже можно тянуть
        }
    }

    // Окно подтверждения: danger — красная кнопка; needCheck — сначала поставить «я понимаю»
    bool Confirm(const char* id, const char* text, const char* action, bool danger, bool needCheck) {
        bool confirmed = false;
        ImGui::SetNextWindowSize(ImVec2(Px(460), 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(22), Px(20)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Px(18));
        if (ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
            static bool s_understood = false;
            if (ImGui::IsWindowAppearing()) s_understood = false;
            ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
            ImGui::TextUnformatted(text);
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, Px(8)));
            if (needCheck) Widgets::Check("Да, я понимаю — это нельзя отменить", &s_understood);
            ImGui::Dummy(ImVec2(0, Px(8)));
            const bool enabled = !needCheck || s_understood;
            ImGui::BeginDisabled(!enabled);
            if (danger) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(200, 55, 60, 255)), ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(225, 70, 75, 255));
            if (ImGui::Button(action)) confirmed = true;
            if (danger) ImGui::PopStyleColor(2);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("отмена")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(2);
        return confirmed;
    }

    void DrawResetAll(float) {
        if (ImGui::Button("вернуть всё по умолчанию")) ImGui::OpenPopup("##confirmReset");
        if (Confirm("##confirmReset", "Все настройки вернутся как были при установке: имя, голос, внешний вид, блоки и показатели. "
                                       "Телеграм-бот, Discord и контакты останутся. Программа перезапустится.", "сбросить и перезапустить", false, false))
            Uninstall::ResetSettings();
    }

    void DrawRemoveAll(float) {
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(170, 45, 52, 230));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(205, 60, 66, 255));
        if (ImGui::Button("удалить программу полностью")) ImGui::OpenPopup("##confirmRemove");
        ImGui::PopStyleColor(2);
        static std::string s_text;
        if (ImGui::IsPopupOpen("##confirmRemove") && s_text.empty()) {
            const std::string folder = Text::ToUtf8(Uninstall::Folder());
            s_text = Uninstall::SafeToDeleteFolder()
                ? "Будет удалена папка программы со ВСЕМИ данными — настройки, токен бота, сценарии, журналы, голоса и нейросеть, "
                  "если они лежат в ней:\n\n" + folder + "\n\nАвтозапуск тоже отключится. Программа закроется."
                : "Программа лежит в общей папке, поэтому удалю только её файлы (ZAVIRIA.exe, настройки, сценарии, журналы, модели):\n\n" + folder +
                  "\n\nАвтозапуск тоже отключится. Программа закроется.";
        }
        if (!ImGui::IsPopupOpen("##confirmRemove")) s_text.clear();
        if (Confirm("##confirmRemove", s_text.c_str(), "удалить навсегда", true, true)) Uninstall::RemoveEverything();
    }

    void DrawToCollege(float) { TimeSetting("##toCollege", g_toCollege, sizeof(g_toCollege), "to_college", "08:00"); }
    void DrawBackHome(float) { TimeSetting("##backHome", g_backHome, sizeof(g_backHome), "back_home", "15:10"); }

    // inlineControl — переключатель справа от названия, а не под ним
    // ---- тонкая настройка нейросети
    std::unordered_map<std::string, int> g_aiValues; // не читать anya.ini каждый кадр
    int g_aiSeen = -1;
    char g_aiExtra[480] = "", g_aiArgs[480] = "", g_aiAbout[480] = "";

    int& AiCached(const char* key, const char* fallback) {
        if (g_aiSeen != Ai::TuningVersion()) { // профиль сменили (в том числе с телефона)
            g_aiSeen = Ai::TuningVersion();
            g_aiValues.clear();
            Copy(g_aiExtra, Config::Value("ai", "extra_prompt", ""));
            Copy(g_aiArgs, Config::Value("ai", "engine_args", ""));
            Copy(g_aiAbout, Config::Value("ai", "about_me", ""));
        }
        const auto it = g_aiValues.find(key);
        if (it != g_aiValues.end()) return it->second;
        return g_aiValues[key] = std::atoi(Config::Value("ai", key, fallback).c_str());
    }

    void AiSlider(const char* key, const char* fallback, int min, int max, float w, const char* format, bool sampling) {
        int& value = AiCached(key, fallback);
        const std::string id = std::string("##ai_") + key;
        if (Widgets::Slider(id.c_str(), &value, min, max, format, ControlWidth(w))) {
            Config::SetValue("ai", key, std::to_string(value));
            if (sampling && Ai::Profile() != Ai::kProfiles - 1) { // тронули ползунок — значит, это уже свой профиль
                Config::SetValue("ai", "profile", std::to_string(Ai::kProfiles - 1));
                g_aiValues.erase("profile");
            }
        }
    }

    void DrawAiProfile(float w) {
        const char* names[Ai::kProfiles];
        for (int i = 0; i < Ai::kProfiles; ++i) names[i] = Ai::ProfileName(i);
        int current = Ai::Profile();
        AiCached("profile", "0");
        if (Widgets::Combo("##aiProfile", &current, names, Ai::kProfiles, ControlWidth(w))) Ai::SetProfile(current);
    }
    void DrawChatTemp(float w) { AiSlider("chat_temp", "70", 0, 200, w, "%d%%", true); }
    void DrawChatTopP(float w) { AiSlider("chat_top_p", "90", 0, 100, w, "%d%%", true); }
    void DrawChatTopK(float w) { AiSlider("chat_top_k", "40", 0, 200, w, "%d", true); }
    void DrawChatMinP(float w) { AiSlider("chat_min_p", "5", 0, 100, w, "%d%%", true); }
    void DrawChatRepeat(float w) { AiSlider("chat_repeat", "100", 50, 150, w, "%d%%", true); }
    void DrawChatTokens(float w) { AiSlider("chat_tokens", "600", 50, 4000, w, "%d", true); }
    void DrawTaskTemp(float w) { AiSlider("task_temp", "60", 0, 200, w, "%d%%", true); }
    void DrawTaskTopP(float w) { AiSlider("task_top_p", "95", 0, 100, w, "%d%%", true); }
    void DrawTaskTopK(float w) { AiSlider("task_top_k", "20", 0, 200, w, "%d", true); }
    void DrawTaskMinP(float w) { AiSlider("task_min_p", "5", 0, 100, w, "%d%%", true); }
    void DrawTaskRepeat(float w) { AiSlider("task_repeat", "100", 50, 150, w, "%d%%", true); }
    void DrawTaskTokens(float w) { AiSlider("task_tokens", "5000", 200, 16000, w, "%d", true); }
    void DrawAiSeed(float w) { AiSlider("seed", "-1", -1, 9999, w, "%d", true); }
    void DrawThinkMode(float w) {
        static const char* kNames[] = { "только когда попросишь", "ещё и для задач", "никогда" };
        int current = std::clamp(AiCached("think_mode", "0"), 0, 2);
        if (Widgets::Combo("##thinkMode", &current, kNames, 3, ControlWidth(w))) {
            Config::SetValue("ai", "think_mode", std::to_string(current));
            AiCached("think_mode", "0") = current;
        }
    }
    void DrawThinkSeconds(float w) { AiSlider("think_seconds", "10", 3, 300, w, "%d с", false); }
    void AiTextField(const char* id, char* buffer, size_t size, const char* key, const char* hint, float w) {
        AiCached("profile", "0"); // подхватить значение из файла
        ImGui::SetNextItemWidth(ControlWidth(w));
        ImGui::InputTextWithHint(id, hint, buffer, size);
        if (ImGui::IsItemDeactivatedAfterEdit()) Config::SetValue("ai", key, Text::Trim(buffer));
    }
    void DrawExtraPrompt(float w) { AiTextField("##aiExtra", g_aiExtra, sizeof(g_aiExtra), "extra_prompt", "например: отвечай с лёгким сарказмом", w); }
    void DrawGpuLayers(float w) { AiSlider("gpu_layers", "99", 0, 99, w, "%d", false); }
    void DrawKnowledge(float) {
        const auto stats = Knowledge::GetStats();
        ImGui::TextDisabled("%s %zu, %zu %s", Lang::Tr("записей:"), stats.entries, stats.bytes / 1024, Lang::Tr("КБ из 4096"));
        ImGui::SameLine();
        if (ImGui::Button("забыть выученное")) Knowledge::Clear();
    }
    void DrawPrecise(float) { IniToggle("##aiPrecise", "ai", "precise", "1", [] {}); }
    void DrawPreciseTemp(float w) { AiSlider("precise_temp", "30", 0, 100, w, "%d%%", false); }
    void DrawAiThreads(float w) { AiSlider("threads", "0", 0, 64, w, "%d", false); }
    void DrawAiContext(float w) {
        static const int kSizes[] = { 2048, 4096, 8192, 16384, 32768, 65536, 131072 };
        static const char* kNames[] = { "2 048", "4 096", "8 192", "16 384", "32 768", "65 536", "131 072" };
        int& value = AiCached("context", "16384");
        int current = 3;
        for (int i = 0; i < 7; ++i) if (kSizes[i] == value) current = i;
        if (Widgets::Combo("##aiContext", &current, kNames, 7, ControlWidth(w))) {
            value = kSizes[current];
            Config::SetValue("ai", "context", std::to_string(value));
        }
    }
    void DrawAiParallel(float w) { AiSlider("parallel", "2", 1, 4, w, "%d", false); }
    void DrawAiVision(float) { IniToggle("##aiVision", "ai", "vision", "1", [] {}); }
    void DrawEngineArgs(float w) { AiTextField("##aiArgs", g_aiArgs, sizeof(g_aiArgs), "engine_args", "например: --mlock", w); }
    void DrawPrivacyScreen(float) { IniToggle("##privacyScreen", "privacy", "screen", "1", [] {}); }
    void DrawClearLog(float) {
        static float s_done = 0;
        if (ImGui::Button("очистить журнал")) {
            std::ofstream(std::filesystem::path(Text::ExecutableDirectory()) / "anya.log", std::ios::trunc);
            s_done = static_cast<float>(ImGui::GetTime());
        }
        if (s_done > 0 && ImGui::GetTime() - s_done < 3) { ImGui::SameLine(); ImGui::TextDisabled("очищено"); }
    }
    void DrawAboutMe(float w) { AiTextField("##aboutMe", g_aiAbout, sizeof(g_aiAbout), "about_me", "например: учусь в колледже, люблю короткие ответы", w); }
    void DrawFileSearch(float w) {
        static const char* kNames[] = { "ZAVIRIA (по умолчанию)", "Поиск Windows", "Everything" };
        static int s_current = -1;
        if (s_current < 0) s_current = std::clamp(std::atoi(Config::Value("search", "files", "0").c_str()), 0, 2);
        if (Widgets::Combo("##fileSearch", &s_current, kNames, 3, ControlWidth(w))) Config::SetValue("search", "files", std::to_string(s_current));
    }
    constexpr const char* kVersion = Updater::kVersion;
    std::mutex g_updateMutex;
    std::string g_updateStatus;
    std::atomic<bool> g_updateChecking{ false };
    void DrawUpdates(float) {
        ImGui::TextUnformatted((std::string("ZAVIRIA ") + kVersion).c_str());
        if (ImGui::Button(g_updateChecking.load() ? "проверяю…" : "проверить обновления") && !g_updateChecking.exchange(true))
            std::thread([] {
                const auto tag = Json::FindStrings(Http::HttpsGet(L"api.github.com", L"/repos/zephiro4ka/ZAVIRIA-Voice-assistant/releases/latest", 12000), "tag_name", 1);
                std::lock_guard lock(g_updateMutex);
                g_updateStatus = tag.empty() ? "не удалось проверить — нет связи с GitHub" : tag[0] == std::string("v") + kVersion ? "у тебя последняя версия" : "вышла версия " + tag[0];
                g_updateChecking = false;
            }).detach();
        ImGui::SameLine();
        if (ImGui::Button("страница релизов")) ShellExecuteW(nullptr, L"open", L"https://github.com/zephiro4ka/ZAVIRIA-Voice-assistant/releases/latest", nullptr, nullptr, SW_SHOWNORMAL);
        std::lock_guard lock(g_updateMutex);
        if (!g_updateStatus.empty()) ImGui::TextDisabled("%s", g_updateStatus.c_str());
    }
    void DrawLanguage(float w) {
        static const char* kNames[] = { "Как в системе", "Русский", "English" };
        static const char* kCodes[] = { "system", "ru", "en" };
        const std::string now = Config::Value("anya", "lang", "system");
        int current = now == "ru" ? 1 : now == "en" ? 2 : 0;
        if (Widgets::Combo("##language", &current, kNames, 3, ControlWidth(w))) {
            Config::SetValue("anya", "lang", kCodes[current]);
            Lang::Reload();
            Ears::ReloadModel(); // распознавание сразу на новом языке
        }
    }
    void DrawAutoUpdate(float) {
        IniToggle("##autoUpdate", "update", "auto", "0", [] {});
        const std::string status = Updater::Status();
        if (!status.empty()) ImGui::TextDisabled("%s", status.c_str());
        if (Updater::HasPending() && ImGui::Button("установить сейчас")) Updater::InstallNow();
    }
    void DrawStartHidden(float) { IniToggle("##startHidden", "anya", "start_hidden", "0", [] {}); }
    void DrawIslandOn(float) { IniToggle("##islandOn", "overlay", "island", "1", Overlay::Reload); }
    void DrawNotchOn(float) { IniToggle("##notchOn", "overlay", "notch", "1", Overlay::Reload); } // старый островок сверху уступает место чёлке
    // Мини-экран (как выбор места уведомлений в Telegram), но без заготовок: островок тянут мышкой куда угодно —
    // он прилипает к ближайшему краю, место вдоль края любое. [overlay] notch_edge 0 верх/1 низ/2 лево/3 право, notch_along 0…1000
    void DrawNotchPlace(float w) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        MONITORINFO info{ sizeof(info) };
        GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &info);
        const float aspect = static_cast<float>(info.rcMonitor.right - info.rcMonitor.left) / std::max(1.0f, static_cast<float>(info.rcMonitor.bottom - info.rcMonitor.top));
        const float sw = std::min(w, Px(380)), sh = sw / aspect;
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const ImVec2 a(start.x + (w - sw) * 0.5f, start.y + Px(6)), b(a.x + sw, a.y + sh);
        // монитор: рамка, экран в цвет темы, ножка
        dl->AddRectFilled(ImVec2(a.x - Px(7), a.y - Px(7)), ImVec2(b.x + Px(7), b.y + Px(7)), Theme::Overlay(Theme::Paper() ? 30 : 26), Px(12));
        dl->AddRectFilledMultiColor(a, b, Theme::Background(0).WithAlpha(1.0f), Theme::Background(0).Mix(Theme::Accent(), 0.12f).WithAlpha(1.0f),
            Theme::Background(1).WithAlpha(1.0f), Theme::Background(1).WithAlpha(1.0f));
        dl->AddRectFilled(ImVec2((a.x + b.x) * 0.5f - Px(26), b.y + Px(7)), ImVec2((a.x + b.x) * 0.5f + Px(26), b.y + Px(26)), Theme::Overlay(20));
        dl->AddRectFilled(ImVec2((a.x + b.x) * 0.5f - Px(60), b.y + Px(26)), ImVec2((a.x + b.x) * 0.5f + Px(60), b.y + Px(32)), Theme::Overlay(26), Px(3));

        static int s_edge = -1;
        static float s_along = 0.5f;
        static bool s_drag = false;
        if (!s_drag) {
            s_edge = std::clamp(std::atoi(Config::Value("overlay", "notch_edge", "0").c_str()), 0, 3);
            s_along = std::clamp(std::atoi(Config::Value("overlay", "notch_along", "500").c_str()) / 1000.0f, 0.0f, 1.0f);
        }
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("##notchPlace", ImVec2(sw, sh));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemActive()) {
            s_drag = true;
            const ImVec2 m = ImGui::GetIO().MousePos;
            const float fx = std::clamp((m.x - a.x) / sw, 0.0f, 1.0f), fy = std::clamp((m.y - a.y) / sh, 0.0f, 1.0f);
            const float d[4] = { fy, 1.0f - fy, fx * sw / sh, (1.0f - fx) * sw / sh }; // расстояние до краёв в одних единицах
            s_edge = static_cast<int>(std::min_element(d, d + 4) - d);
            s_along = s_edge < 2 ? fx : fy;
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            // островок на экране едет вместе с мышкой: сохраняем на ходу (не чаще ~25 раз в секунду)
            static int s_savedEdge = -1, s_savedAlong = -1;
            static double s_savedAt = 0;
            const int along = static_cast<int>(std::lround(s_along * 1000));
            if ((s_edge != s_savedEdge || std::abs(along - s_savedAlong) >= 3) && ImGui::GetTime() - s_savedAt > 0.04) {
                Config::SetValue("overlay", "notch_edge", std::to_string(s_edge));
                Config::SetValue("overlay", "notch_along", std::to_string(along));
                s_savedEdge = s_edge; s_savedAlong = along; s_savedAt = ImGui::GetTime();
                Notch::Reload();
            }
        } else if (s_drag) { // отпустили — точное место
            s_drag = false;
            Config::SetValue("overlay", "notch_edge", std::to_string(s_edge));
            Config::SetValue("overlay", "notch_along", std::to_string(static_cast<int>(std::lround(s_along * 1000))));
            Notch::Reload();
        }
        if (hovered && !s_drag) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        // островок на мини-экране: прилипшая «таблетка» у края
        const float len = sw * 0.26f, thick = Px(11);
        ImVec2 p0, p1;
        if (s_edge < 2) {
            const float c = std::clamp(a.x + s_along * sw, a.x + len * 0.5f, b.x - len * 0.5f);
            p0 = ImVec2(c - len * 0.5f, s_edge == 0 ? a.y : b.y - thick);
            p1 = ImVec2(c + len * 0.5f, s_edge == 0 ? a.y + thick : b.y);
        } else {
            const float l = sh * 0.3f, c = std::clamp(a.y + s_along * sh, a.y + l * 0.5f, b.y - l * 0.5f);
            p0 = ImVec2(s_edge == 2 ? a.x : b.x - thick, c - l * 0.5f);
            p1 = ImVec2(s_edge == 2 ? a.x + thick : b.x, c + l * 0.5f);
        }
        const ImDrawFlags corners = s_edge == 0 ? ImDrawFlags_RoundCornersBottom : s_edge == 1 ? ImDrawFlags_RoundCornersTop : s_edge == 2 ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersLeft;
        if (s_drag || hovered) dl->AddRectFilled(ImVec2(p0.x - Px(4), p0.y - Px(4)), ImVec2(p1.x + Px(4), p1.y + Px(4)), Theme::Accent().WithAlpha(0.25f), Px(9), corners);
        dl->AddRectFilled(p0, p1, Theme::Accent().Mix({ 20, 14, 30 }, 0.55f).WithAlpha(1.0f), Px(6), corners);
        dl->AddRectFilled(p0, p1, Theme::Accent().WithAlpha(0.35f), Px(6), corners);
        ImGui::SetCursorScreenPos(ImVec2(start.x, b.y + Px(40)));
        ImGui::TextDisabled("%s", Lang::Tr("перетащи островок к любому краю — он прилипнет там, где отпустишь"));
    }

    void DrawNotchEyes(float) { IniToggle("##notchEyes", "overlay", "notch_eyes", "1", [] {}); }
    // ---- круговое меню (зажать среднюю кнопку мыши): свой список программ и их порядок по кругу
    // ---- перевод области экрана
    void DrawTranslateOn(float) { IniToggle("##trOn", "translate", "enabled", "1", ScreenTranslate::Reload); }
    void DrawTranslateKey(float) {
        std::string got;
        if (PieMenu::TakeCaptured(got, 1) && !got.empty()) {
            if (got.find("|k|") == std::string::npos) got = "4|k|84"; // переводу нужна клавиша, кнопка мыши не подходит
            Config::SetValue("translate", "hotkey", got);
            ScreenTranslate::Reload();
        }
        const std::string now = Config::Value("translate", "hotkey", "4|k|84");
        ImGui::AlignTextToFramePadding();
        if (PieMenu::Capturing(1)) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Accent()), "%s", Lang::Tr("нажми сочетание клавиш… (Esc — отмена)"));
        else ImGui::TextUnformatted(PieMenu::TriggerName(now).c_str());
        if (!PieMenu::Capturing(1)) {
            if (Widgets::Action("изменить", Icon::Gear, Widgets::ActionStyle::Normal, false)) PieMenu::StartCapture(1);
            if (Widgets::Action("попробовать", Icon::Search, Widgets::ActionStyle::Primary, true)) ScreenTranslate::Run();
        }
    }
    void DrawTranslateLang(float w) {
        int count = 0;
        const auto* langs = ScreenTranslate::Languages(count);
        const std::string code = Config::Value("translate", "lang", Lang::IsEnglish() ? "en" : "ru");
        int current = 0;
        for (int i = 0; i < count; ++i) if (code == langs[i].code) current = i;
        if (Widgets::BeginCombo("##trLang", langs[current].name, std::min(ControlWidth(w), Px(240)), ImGuiComboFlags_HeightLarge)) {
            for (int i = 0; i < count; ++i)
                if (Widgets::ComboItem(langs[i].name, i == current)) Config::SetValue("translate", "lang", langs[i].code);
            Widgets::EndCombo();
        }
    }

    void DrawPieOn(float) { IniToggle("##pieOn", "pie", "enabled", "1", [] {}); }
    void DrawPieTap(float) { IniToggle("##pieTap", "pie", "tap_open", "0", PieMenu::ReloadTrigger); }
    // Чем открывать круговое меню: своё сочетание клавиш или кнопок мыши (в том числе боковых)
    void DrawPieTrigger(float) {
        // список сочетаний через «;» — по умолчанию одно (колёсико), можно добавить ещё
        std::vector<std::string> list;
        {
            const std::string all = Config::Value("pie", "trigger", "0|m|3");
            size_t from = 0;
            while (from <= all.size()) {
                const size_t semi = all.find(';', from);
                const std::string t = all.substr(from, semi == std::string::npos ? std::string::npos : semi - from);
                if (!t.empty()) list.push_back(t);
                if (semi == std::string::npos) break;
                from = semi + 1;
            }
            if (list.empty()) list.push_back("0|m|3");
        }
        auto save = [&] {
            std::string all;
            for (const auto& t : list) all += (all.empty() ? "" : ";") + t;
            Config::SetValue("pie", "trigger", all);
            PieMenu::ReloadTrigger();
        };
        constexpr int kAdd = 99; // запись нового; 10+i — замена i-го
        std::string got;
        for (int i = 0; i < static_cast<int>(list.size()); ++i)
            if (PieMenu::TakeCaptured(got, 10 + i) && !got.empty() && std::find(list.begin(), list.end(), got) == list.end()) { list[i] = got; save(); }
        if (PieMenu::TakeCaptured(got, kAdd) && !got.empty() && std::find(list.begin(), list.end(), got) == list.end() && list.size() < 8) { list.push_back(got); save(); }
        int remove = -1;
        for (int i = 0; i < static_cast<int>(list.size()); ++i) {
            ImGui::PushID(i);
            ImGui::AlignTextToFramePadding();
            if (PieMenu::Capturing(10 + i)) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Accent()), "%s", Lang::Tr("нажми сочетание или кнопку мыши… (Esc — отмена)"));
            else {
                ImGui::TextUnformatted(PieMenu::TriggerName(list[i]).c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton(Lang::Tr("изменить"))) PieMenu::StartCapture(10 + i);
                if (list.size() > 1) { ImGui::SameLine(); if (ImGui::SmallButton(Lang::Tr("убрать"))) remove = i; }
            }
            ImGui::PopID();
        }
        if (remove >= 0) { list.erase(list.begin() + remove); save(); }
        if (PieMenu::Capturing(kAdd)) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Accent()), "%s", Lang::Tr("нажми сочетание или кнопку мыши… (Esc — отмена)"));
        else if (list.size() < 8 && Widgets::Action("+ ещё сочетание", Icon::Plus, Widgets::ActionStyle::Normal, false)) PieMenu::StartCapture(kAdd);
    }
    // Редактор кругового меню: сверху — картинка, как круг выглядит; ниже — путь по папкам и пункты этой папки.
    // Папки вкладываются сколько угодно: в папке — свой круг со своими пунктами.
    PieMenu::Node* PieLevel(PieMenu::Node& root, const std::vector<int>& path) {
        PieMenu::Node* n = &root;
        for (int i : path) {
            if (i < 0 || i >= static_cast<int>(n->items.size()) || n->items[i].kind != "folder") return nullptr;
            n = &n->items[i];
        }
        return n;
    }
    std::mutex g_piePickMutex;
    std::string g_piePicked; // файл из окна выбора (оно в своём потоке)

    void DrawPieApps(float w) {
        static PieMenu::Node s_root;
        static bool s_loaded = false, s_dirty = true;
        static std::vector<int> s_path;
        static void* s_tex = nullptr;
        static int s_texSize = 0;
        static char s_siteName[96] = "", s_siteUrl[256] = "";
        static bool s_addSite = false;
        if (!s_loaded) { s_root = PieMenu::Root(); s_loaded = true; }
        PieMenu::Node* level = PieLevel(s_root, s_path);
        if (!level) { s_path.clear(); level = &s_root; }
        bool changed = false;
        {
            std::lock_guard lock(g_piePickMutex);
            if (!g_piePicked.empty()) {
                const std::string path = g_piePicked;
                std::string name = path.substr(path.find_last_of("\\/") + 1);
                if (const size_t dot = name.rfind('.'); dot != std::string::npos && dot > 0) name = name.substr(0, dot);
                level->items.push_back({ "path", name.empty() ? path : name, path });
                g_piePicked.clear();
                changed = true;
            }
        }

        const float x0 = ImGui::GetCursorScreenPos().x; // всё в строке — от левого края редактора
        // 1. как выглядит круг (эта папка)
        const int size = static_cast<int>(std::min(Px(280), w));
        if (s_dirty || !s_tex || s_texSize != size) {
            if (s_tex) static_cast<IUnknown*>(s_tex)->Release();
            const auto px = PieMenu::RenderPicture(s_path, size);
            s_tex = Graphics::CreateTexture(px.data(), size, size);
            s_texSize = size;
            s_dirty = false;
        }
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(at.x + (w - size) * 0.5f, at.y));
        if (s_tex) ImGui::Image(reinterpret_cast<ImTextureID>(s_tex), ImVec2(static_cast<float>(size), static_cast<float>(size)));
        else ImGui::Dummy(ImVec2(static_cast<float>(size), static_cast<float>(size)));

        // 2. где мы: Главное › Игры › …
        ImGui::PushFont(Theme::semibold, Theme::semibold->LegacySize);
        if (ImGui::SmallButton(Lang::Tr("Главное"))) { s_path.clear(); s_dirty = true; }
        {
            PieMenu::Node* n = &s_root;
            for (size_t d = 0; d < s_path.size() && n && s_path[d] < static_cast<int>(n->items.size()); ++d) {
                n = &n->items[s_path[d]];
                ImGui::SameLine();
                ImGui::TextDisabled("\xE2\x80\xBA");
                ImGui::SameLine();
                ImGui::PushID(static_cast<int>(d));
                if (ImGui::SmallButton(n->name.empty() ? "…" : n->name.c_str())) { s_path.resize(d + 1); s_dirty = true; }
                ImGui::PopID();
            }
        }
        ImGui::PopFont();
        level = PieLevel(s_root, s_path);
        if (!level) { s_path.clear(); level = &s_root; }

        // 3. пункты этой папки: первый — сверху, дальше по часовой
        const float row = ImGui::GetFrameHeight();
        int moveFrom = -1, moveTo = -1, remove = -1, enter = -1;
        auto kindName = [](const std::string& k) { return k == "folder" ? "папка" : k == "site" ? "сайт" : k == "path" ? "файл" : "программа"; };
        for (int i = 0; i < static_cast<int>(level->items.size()); ++i) {
            auto& item = level->items[i];
            ImGui::PushID(i);
            const float y = ImGui::GetCursorScreenPos().y;
            const float buttons = row * (item.kind == "folder" ? 4 : 3) + ImGui::GetStyle().ItemSpacing.x * 3;
            ImGui::SetCursorScreenPos(ImVec2(x0, y));
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%d", i + 1);
            ImGui::SameLine();
            ImGui::SetCursorScreenPos(ImVec2(x0 + Px(26), y));
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", Lang::Tr(kindName(item.kind)));
            ImGui::SameLine();
            ImGui::SetCursorScreenPos(ImVec2(x0 + Px(112), y));
            if (item.kind == "app") { ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(item.name.c_str()); }
            else { // подпись папки, сайта, файла можно поменять прямо тут
                char buf[128];
                snprintf(buf, sizeof(buf), "%s", item.name.c_str());
                ImGui::SetNextItemWidth(std::max(Px(80), w - Px(112) - buttons - Px(12)));
                if (ImGui::InputText("##name", buf, sizeof(buf))) item.name = buf;
                if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
            }
            ImGui::SameLine();
            ImGui::SetCursorScreenPos(ImVec2(x0 + w - buttons, y));
            if (item.kind == "folder") {
                if (Widgets::IconButton("##open", ImGui::GetCursorScreenPos(), row, Icon::Chevron)) enter = i;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Lang::Tr("открыть папку"));
                ImGui::SameLine();
            }
            ImGui::BeginDisabled(i == 0);
            if (ImGui::Button("\xE2\x86\x91", ImVec2(row, row))) { moveFrom = i; moveTo = i - 1; }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(i + 1 == static_cast<int>(level->items.size()));
            if (ImGui::Button("\xE2\x86\x93", ImVec2(row, row))) { moveFrom = i; moveTo = i + 1; }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (Widgets::IconButton("##del", ImGui::GetCursorScreenPos(), row, Icon::Trash, true)) remove = i;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Lang::Tr(item.kind == "folder" ? "удалить папку со всем, что в ней" : "убрать из меню"));
            ImGui::PopID();
        }
        if (level->items.empty()) ImGui::TextDisabled("%s", Lang::Tr("Пока пусто — добавь пункты ниже."));
        if (moveFrom >= 0) { std::swap(level->items[moveFrom], level->items[moveTo]); changed = true; }
        if (remove >= 0) { level->items.erase(level->items.begin() + remove); changed = true; }
        if (enter >= 0) { s_path.push_back(enter); s_dirty = true; level = PieLevel(s_root, s_path); }
        if (!level) { s_path.clear(); level = &s_root; }

        // 4. добавить: программа из «Пуска», папка (ещё один круг), сайт, файл с диска
        static char s_find[96] = "";
        if (Widgets::BeginCombo("##pieAdd", Lang::Tr("+ программа"), std::min(ControlWidth(w), Px(220)), ImGuiComboFlags_HeightLarge)) {
            if (ImGui::IsWindowAppearing()) { s_find[0] = '\0'; ImGui::SetKeyboardFocusHere(); }
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##pieFind", Lang::Tr("поиск…"), s_find, sizeof(s_find));
            const std::wstring filter = Text::LowerRu(Text::ToWide(s_find));
            int shown = 0;
            for (const auto& name : Launcher::AppNames()) {
                if (!filter.empty() && Text::LowerRu(Text::ToWide(name)).find(filter) == std::wstring::npos) continue;
                if (Widgets::ComboItem(name.c_str(), false)) { level->items.push_back({ "app", name }); changed = true; }
                if (++shown >= 80) break;
            }
            if (shown == 0) ImGui::TextDisabled("%s", Lang::Tr("ничего не нашла"));
            Widgets::EndCombo();
        }
        if (Widgets::Action("+ папка", Icon::Layers, Widgets::ActionStyle::Normal, true)) {
            level->items.push_back({ "folder", Lang::Tr("Новая папка") });
            changed = true;
        }
        if (Widgets::Action("+ сайт", Icon::Search, Widgets::ActionStyle::Normal, true)) s_addSite = !s_addSite;
        if (Widgets::Action("+ файл", Icon::Book, Widgets::ActionStyle::Normal, true)) {
            std::thread([] {
                CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
                IFileOpenDialog* dialog = nullptr;
                if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
                    IShellItem* item = nullptr;
                    PWSTR path = nullptr;
                    if (SUCCEEDED(dialog->Show(nullptr)) && SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                        std::lock_guard lock(g_piePickMutex);
                        g_piePicked = Text::ToUtf8(path);
                        CoTaskMemFree(path);
                    }
                    if (item) item->Release();
                    dialog->Release();
                }
                CoUninitialize();
            }).detach();
        }
        if (s_addSite) {
            ImGui::SetNextItemWidth(Px(150));
            ImGui::InputTextWithHint("##siteName", Lang::Tr("название"), s_siteName, sizeof(s_siteName));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(std::max(Px(120), w - Px(150) - Px(130)));
            ImGui::InputTextWithHint("##siteUrl", "youtube.com", s_siteUrl, sizeof(s_siteUrl));
            ImGui::SameLine();
            ImGui::BeginDisabled(!s_siteUrl[0]);
            if (ImGui::Button(Lang::Tr("добавить"))) {
                level->items.push_back({ "site", s_siteName[0] ? s_siteName : s_siteUrl, s_siteUrl });
                s_siteName[0] = s_siteUrl[0] = '\0';
                s_addSite = false;
                changed = true;
            }
            ImGui::EndDisabled();
        }
        if (changed) { PieMenu::SetRoot(s_root); s_dirty = true; }
        if (Widgets::Action("показать меню", Icon::Spark, Widgets::ActionStyle::Primary)) PieMenu::Preview();
    }

    void DrawIslandWindow(float) { if (Widgets::Action("открыть настройки островка", Widgets::Icon::Gear, Widgets::ActionStyle::Primary)) IslandSettings::Open(); }
    void DrawNotchSize(float w) {
        static const char* kNames[] = { "обычный", "крупнее" };
        int current = std::clamp(std::atoi(Config::Value("overlay", "notch_size", "0").c_str()), 0, 1);
        if (Widgets::Combo("##notchSize", &current, kNames, 2, ControlWidth(w))) Config::SetValue("overlay", "notch_size", std::to_string(current));
    }
    void DrawNotchMusic(float) { IniToggle("##notchMusic", "overlay", "notch_music", "1", [] {}); }
    void DrawNotchShelf(float) { IniToggle("##notchShelf", "overlay", "notch_shelf", "1", [] {}); }
    void DrawNotchWeather(float) { IniToggle("##notchWeather", "overlay", "notch_weather", "1", [] {}); }
    void DrawNotchTimers(float) { IniToggle("##notchTimers", "overlay", "notch_timers", "1", [] {}); }
    void DrawNotchStopwatch(float) { IniToggle("##notchStopwatch", "overlay", "notch_stopwatch", "1", [] {}); }
    void DrawNotchPomodoro(float) { IniToggle("##DrawNotchPomodoro", "overlay", "notch_pomodoro", "1", [] {}); }
    void DrawNotchWater(float) { IniToggle("##DrawNotchWater", "overlay", "notch_water", "1", [] {}); }
    void DrawNotchLyrics(float) { IniToggle("##DrawNotchLyrics", "overlay", "notch_lyrics", "0", [] {}); }
    void DrawNotchLayout(float) { IniToggle("##DrawNotchLayout", "overlay", "notch_layout", "1", [] {}); }
    void DrawNotchPower(float) { IniToggle("##DrawNotchPower", "overlay", "notch_power", "1", [] {}); }
    void DrawNotchWave(float) { IniToggle("##DrawNotchWave", "overlay", "notch_wave", "1", [] {}); }
    void DrawNotchIdle(float) { IniToggle("##notchIdle", "overlay", "notch_idle", "1", [] {}); }
    void DrawPageMusic(float) { IniToggle("##pageMusic", "overlay", "page_music", "1", Overlay::Reload); }
    void DrawPageTime(float) { IniToggle("##pageTime", "overlay", "page_time", "1", Overlay::Reload); }
    void DrawPageNext(float) { IniToggle("##pageNext", "overlay", "page_next", "1", Overlay::Reload); }
    void DrawResOn(float) { IniToggle("##resOn", "overlay", "res", "0", Overlay::Reload); }
    void DrawResPin(float) { IniToggle("##resPin", "overlay", "pinned", "0", Overlay::Reload); }
    void DrawIslandPlace(float w) {
        static const char* kNames[] = { "сверху по центру", "сверху слева", "сверху справа", "снизу по центру" };
        int current = std::clamp(std::atoi(Config::Value("overlay", "place", "0").c_str()), 0, 3);
        if (Widgets::Combo("##islandPlace", &current, kNames, 4, ControlWidth(w))) { Config::SetValue("overlay", "place", std::to_string(current)); Overlay::Reload(); }
    }
    void DrawResOpacity(float w) {
        static int s_value = -1;
        if (s_value < 0) s_value = std::clamp(std::atoi(Config::Value("overlay", "opacity", "85").c_str()), 20, 100);
        if (Widgets::Slider("##resOpacity", &s_value, 0, 100, "%d%%", ControlWidth(w))) {
            s_value = std::max(s_value, 20);
            Config::SetValue("overlay", "opacity", std::to_string(s_value));
            Overlay::Reload();
        }
    }
    void DrawCozy(float) {
        bool on = Theme::Cozy();
        if (Widgets::Toggle("##cozy", ImGui::GetCursorScreenPos(), &on, Accent())) Theme::SetCozy(on, Theme::CozyPattern());
    }
    void DrawCozyDark(float) {
        bool on = Theme::CozyDark();
        if (Widgets::Toggle("##cozyDark", ImGui::GetCursorScreenPos(), &on, Accent())) Theme::SetCozyDark(on);
    }
    void DrawMascot(float w) {
        const char* names[CozyArt::kMascots];
        for (int i = 0; i < CozyArt::kMascots; ++i) names[i] = CozyArt::MascotName(i);
        int current = CozyArt::Mascot();
        if (Widgets::Combo("##mascot", &current, names, CozyArt::kMascots, ControlWidth(w))) {
            if (current == CozyArt::kMascotCustom) CozyArt::PickCustom();
            else CozyArt::SetMascot(current);
        }
        if (CozyArt::Mascot() == CozyArt::kMascotCustom && ImGui::Button("другая картинка…")) CozyArt::PickCustom();
    }
    void DrawCozyPattern(float w) {
        const char* names[CozyArt::kPatterns];
        for (int i = 0; i < CozyArt::kPatterns; ++i) names[i] = CozyArt::PatternName(i);
        int current = std::clamp(Theme::CozyPattern(), 0, CozyArt::kPatterns - 1);
        if (Widgets::Combo("##cozyPattern", &current, names, CozyArt::kPatterns, ControlWidth(w))) Theme::SetCozy(Theme::Cozy(), current);
    }
    void DrawFontFace(float w) {
        std::vector<const char*> names;
        for (int i = 0; i < Theme::FontCount(); ++i) names.push_back(Theme::FontName(i));
        const int current = Theme::FontIndex();
        if (Widgets::BeginCombo("##fontFace", names[current], ControlWidth(w))) {
            for (int i = 0; i < Theme::FontCount(); ++i) // каждый шрифт написан сам собой
                if (Widgets::ComboItem(names[i], i == current, Theme::FontPreview(i))) Theme::SetFont(i, Theme::FontScale());
            Widgets::EndCombo();
        }
    }
    void DrawFontSize(float w) {
        static int s_scale = -1;
        if (s_scale < 0) s_scale = Theme::FontScale();
        Widgets::Slider("##fontSize", &s_scale, 80, 120, "%d%%", ControlWidth(w));
        if (ImGui::IsItemDeactivatedAfterEdit()) Theme::SetFont(Theme::FontIndex(), s_scale); // шрифты грузим, когда ползунок отпустили
    }
    void DrawAiPreload(float) { IniToggle("##aiPreload", "ai", "preload", "0", [] {}); }
    void DrawGameMode(float w) {
        static const char* kNames[] = { "выгружать", "отвечать с процессора", "ничего не менять" };
        const char* fallback = Config::Value("ai", "game_unload", "1") == "1" ? "1" : "2";
        int current = std::clamp(AiCached("game_mode", fallback), 0, 2);
        if (Widgets::Combo("##gameMode", &current, kNames, 3, ControlWidth(w))) {
            Config::SetValue("ai", "game_mode", std::to_string(current));
            AiCached("game_mode", fallback) = current;
        }
    }
    void DrawWidgetsOn(float) { IniToggle("##widgetsOn", "widgets", "show", "1", [] {}); }
    void DrawClockWidget(float) { IniToggle("##clockW", "widgets", "clock", "1", [] {}); }
    void DrawNotesWidget(float) { IniToggle("##notesW", "widgets", "notes", "0", [] {}); }
    void DrawCalendarWidget(float) { IniToggle("##calendarW", "widgets", "calendar", "0", [] {}); }
    void DrawSystemWidget(float) { IniToggle("##systemW", "widgets", "system", "1", [] {}); }
    void DrawRatesWidget(float) { IniToggle("##ratesWidget", "rates", "widget", "1", Rates::Reload); }
    void DrawRatesCurrency(float w) {
        static const char* kCodes[] = { "auto", "UZS", "RUB", "KZT", "BYN", "KGS", "TJS", "UAH", "AZN", "AMD", "GEL", "TRY", "NOK", "EUR", "USD" };
        static const char* kNames[] = { "по городу из настроек", "UZS — сум", "RUB — рубль", "KZT — тенге", "BYN — белорусский рубль", "KGS — сом", "TJS — сомони", "UAH — гривна",
                                        "AZN — манат", "AMD — драм", "GEL — лари", "TRY — лира", "NOK — норвежская крона", "EUR — евро", "USD — доллар" };
        static int s_current = -1;
        if (s_current < 0) {
            s_current = 0;
            const std::string now = Config::Value("rates", "currency", "auto");
            for (int i = 0; i < IM_ARRAYSIZE(kCodes); ++i) if (now == kCodes[i]) s_current = i;
        }
        if (Widgets::Combo("##ratesCurrency", &s_current, kNames, IM_ARRAYSIZE(kNames), ControlWidth(w))) {
            Config::SetValue("rates", "currency", kCodes[s_current]);
            Rates::Reload();
        }
    }
    void DrawAiApply(float) {
        if (ImGui::Button("перезапустить нейросеть")) Ai::Restart();
        ImGui::SameLine();
        if (ImGui::Button("всё по умолчанию")) ImGui::OpenPopup("##confirmAiReset");
        if (Confirm("##confirmAiReset", "Все настройки нейросети вернутся как при установке, и она перезапустится.", "вернуть", false, false)) {
            Ai::ResetTuning();
            Ai::Restart();
        }
    }

    bool g_openTelegram = false, g_openDiscord = false, g_openPhone = false, g_guideTelegram = false, g_guideDiscord = false;

    // Стрелочка раскрытия меню пункта
    void Chevron(const char* id, bool* open) {
        const float size = ImGui::GetFrameHeight();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::PushID(id);
        if (ImGui::InvisibleButton("##arrow", ImVec2(size, size))) *open = !*open;
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c(at.x + size * 0.5f, at.y + size * 0.5f);
        dl->AddCircleFilled(c, size * 0.5f, Theme::Overlay(hovered ? 26 : 12), 28);
        const float r = Px(5), lift = *open ? Px(2) : -Px(2);
        const ImVec2 points[3] = { ImVec2(c.x - r, c.y + lift), ImVec2(c.x, c.y - lift), ImVec2(c.x + r, c.y + lift) };
        dl->AddPolyline(points, 3, Theme::kInk, 0, Px(1.8f));
    }

    void GuideButton(const char* id, bool* shown) {
        ImGui::PushID(id);
        if (ImGui::Button(*shown ? "скрыть инструкцию" : "показать как это сделать")) *shown = !*shown;
        ImGui::PopID();
        ImGui::SameLine();
    }

    void DrawTelegramExpand(float w) {
        DrawTelegram(w);
        ImGui::SameLine();
        if (g_openTelegram) GuideButton("tgGuide", &g_guideTelegram);
        Chevron("tg", &g_openTelegram);
    }
    void DrawDiscordExpand(float w) {
        DrawDiscord(w);
        ImGui::SameLine();
        Chevron("ds", &g_openDiscord);
    }
    void DrawPhoneControl(float) {
        const bool online = RemoteDesktop::PhoneOnline();
        ImGui::TextDisabled("%s", online ? "телефон на связи" : "телефон не отвечает: включи на нём защиту");
        const auto task = [](const char* label, Widgets::Icon icon, Widgets::ActionStyle style, bool sameLine, const char* name) {
            if (Widgets::Action(label, icon, style, sameLine)) RemoteDesktop::PhoneTask(name);
        };
        ImGui::Dummy(ImVec2(0, Px(2)));
        task("заблокировать", Widgets::Icon::Lock, Widgets::ActionStyle::Normal, false, "lock");
        task("где телефон", Widgets::Icon::Search, Widgets::ActionStyle::Normal, true, "locate");
        task("сирена", Widgets::Icon::Wave, Widgets::ActionStyle::Danger, true, "siren");
        task("выключить сирену", Widgets::Icon::Stop, Widgets::ActionStyle::Normal, true, "siren_off");
        const std::string reports = RemoteDesktop::PhoneReports(), place = RemoteDesktop::PhoneLastPlace();
        if (!reports.empty()) ImGui::TextDisabled("%s", reports.c_str());
        if (!place.empty() && Widgets::Action("показать на карте", Widgets::Icon::Home)) ShellExecuteW(nullptr, L"open", Text::ToWide("https://www.google.com/maps?q=" + place).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    // Вход как «Войти с Google»: окно Telegram в браузере, на компьютере ничего подтверждать не нужно
    void DrawTelegramAccount(float) {
        static std::string s_error, s_seen = "?";
        const std::string account = TelegramLogin::Account();
        if (account != s_seen) { s_seen = account; Copy(g_owner, Config::Value("telegram", "owner", "")); } // «Кому можно» поменялся
        const TelegramLogin::State state = TelegramLogin::Current();
        if (state.phase == TelegramLogin::Phase::Waiting || state.phase == TelegramLogin::Phase::Checking) {
            ImGui::TextDisabled("%s", Lang::Tr(state.phase == TelegramLogin::Phase::Waiting ? "Подтверди вход в открывшемся окне Telegram…" : "Проверяю вход…"));
            if (state.phase == TelegramLogin::Phase::Waiting && Widgets::Action("отменить вход", Icon::Close)) TelegramLogin::Cancel();
            return;
        }
        if (!account.empty()) {
            StatusLine(std::string(Lang::Tr("Вошла как")) + " " + account, true);
            if (Widgets::Action("выйти", Icon::Close)) { TelegramLogin::SignOut(); s_error.clear(); }
            return;
        }
        if (Widgets::Action("Войти через Telegram", Icon::Send, Widgets::ActionStyle::Primary)) {
            std::string error;
            s_error = TelegramLogin::Begin(error) ? std::string() : error;
        }
        const std::string& shown = state.phase == TelegramLogin::Phase::Failed && s_error.empty() ? state.message : s_error;
        if (!shown.empty()) ImGui::TextDisabled("%s", Lang::Tr(shown.c_str()));
    }

    void DrawTelegramSecret(float w) {
        IniText("##loginSecret", g_loginSecret, sizeof(g_loginSecret), "telegram", "login_secret", "Client Secret из @BotFather → Login Widget", w, [] {}, ImGuiInputTextFlags_Password);
        if (Widgets::Action("скопировать адрес входа", Icon::Layers)) ImGui::SetClipboardText(TelegramLogin::kRedirect);
    }

    void DrawPhoneExpand(float w) {
        Chevron("phone", &g_openPhone);
        if (g_openPhone) DrawPhoneLink(w);
    }
    void DrawRecommended(float width);
    void DrawAbout(float width);
    void DrawLearnGuide(float width);
    void DrawCustomVoice(float width);
    template <int Guide> void DrawGuide(float width);
    void DrawTourAgain(float);
    void DrawModelStore(float width);
    void DrawSpeechStore(float width);
    enum { GuideTelegram = 100, GuideDiscord = 101 };

    struct Item { Section section; const char* title; const char* hint; void (*draw)(float width); bool inlineControl = false; };
    const Item kItems[] = {
        { General, "Обучение", "Ещё раз покажу по шагам, что и где находится.", DrawTourAgain },
        { General, "Язык", "Интерфейс, распознавание и озвучка.", DrawLanguage },
        { General, "Имя помощницы", "Так она отзывается на голос и подписывается в окне, Discord и телеграме. Склоняю сама: «Лиза» → «с Лизой».", DrawAssistant },
        { General, "Как тебя зовут", "Так Аня пишет твоё имя на экране и в разговоре.", DrawName },
        { General, "Как произносить имя вслух", "Ник не всегда читается красиво: например, «Sasha_07» можно произносить как «Саша».", DrawSpokenName },
        { General, "Запускать вместе с Windows", "Аня тихо стартует в трее, когда включаешь компьютер.", DrawAutostart, true },
        { General, "Фоновый режим", "Крестик не закрывает программу, а прячет её к часам. Она продолжает слушать и почти не нагружает компьютер. Открыть — нажать на значок, выйти — правой кнопкой по нему.", DrawTray, true },
        { General, "Запускать сразу в фоне", "Программа стартует без окна и ждёт, когда её позовут. Нужен включённый фоновый режим.", DrawStartHidden, true },
        { General, "Поисковик", "Куда Аня отправляет «найди …», «загугли …» и «найди в интернете …».", DrawSearchEngine },
        { General, "Поиск файлов", "Как выполнять «найди файл …»: своим поиском ZAVIRIA, через Windows или через Everything.", DrawFileSearch },
        { General, "Анимировать в неактивном окне", "Выключено — пока ты в другом окне, фон замирает и видеокарта отдыхает. "
                                                     "Когда я слушаю или говорю, всё оживает само.", DrawAnimateInactive, true },
        { General, "Как это работает", "Нажми на ссылку — перейду к настройке и подсвечу её.", DrawLearnGuide },
        { General, "Общие простыми словами", "Как устроен этот раздел.", DrawGuide<General> },
        { General, "Поиск простыми словами", "Как просить её что-то найти.", DrawGuide<SearchSection> },
        { General, "Система простыми словами", "Если что-то пошло не так.", DrawGuide<SystemSection> },
        { General, "Обновления простыми словами", "Как обновиться.", DrawGuide<Updates> },
        // синхронизация с телефоном пока в разработке: вместо установки и привязки — одна строка-заглушка
        { PhoneSync, "Связь с телефоном", "Приложение ZAVIRIA для телефона и синхронизация скоро появятся. Пока подключить телефон нельзя.", DrawSyncSoon },
        { SecurityLink, "Телеграм-бот", "Управляй компьютером с телефона: снимок экрана, «реши с экрана», громкость, блокировка, "
                                       "«выключить, но остаться на связи».", DrawTelegramExpand, true },
        { SecurityLink, "Токен бота", "Выдаёт @BotFather. Никому не показывай — с ним можно управлять ботом.", DrawTelegramToken },
        { SecurityLink, "Кому можно", "Один ID или несколько через запятую. У каждого будет полное управление компьютером, всем остальным бот откажет.", DrawTelegramOwner },
        { SecurityLink, "Вход через Telegram", "Как «Войти с Google»: подтверди вход в Telegram, и аккаунт сам появится в «Кому можно». ID искать не нужно.", DrawTelegramAccount },
        { SecurityLink, "Ключ входа", "Client Secret из @BotFather → твой бот → Login Widget. Там же в Allowed URLs добавь адрес входа — кнопка ниже его скопирует.", DrawTelegramSecret },
        { SecurityLink, "Код для опасных команд", "Выключение, рабочий стол, снимок экрана и свободные команды боту будут требовать код из Google Authenticator. Код действует 15 минут.", DrawTwoFactor },
        { SecurityLink, "Уведомления в телеграме", "Сообщения, которые бот шлёт сам: запуск, свежая кнопка пульта, блокировка компьютера. Выключено — бот пишет только в ответ.", DrawGreet, true },
        { SecurityLink, "Пульт в телеграме", "Приложение в меню бота: быстрые команды, запуск программ и удалённый рабочий стол. Вход только для владельца. "
                                            "Пока включено, работает защищённый туннель cloudflared.", DrawRemoteDesktop, true },
        { SecurityLink, "Снимки экрана", "Разрешить боту и нейросети снимать экран по запросу с телефона.", DrawPrivacyScreen, true },
        { SecurityLink, "Блокировать при «Выключить»", "Надёжнее, но тогда «☀ Включить» покажет экран блокировки — пароль придётся вводить "
                                                     "за компьютером: снять блокировку с телефона Windows не даёт.", DrawLockOnStandby, true },
        { SecurityLink, "Показывать активность в Discord", "В профиле Discord: «Играет в ZAVIRIA», а когда играет музыка или видео — «Слушает» или «Смотрит». Ничего вводить не нужно.", DrawDiscordExpand, true },
        { SecurityLink, "Своё приложение Discord", "Не обязательно: ZAVIRIA уже встроена и показывается сразу. Нужно, только если хочешь своё название и картинку — "
                                                  "вставь Application ID своего приложения с discord.com/developers. Пусто — встроенное.", DrawDiscordApp },
        { SecurityLink, "Показывать музыку", "«Слушает: трек — исполнитель» из Яндекс Музыки, Spotify и других плееров.", DrawDiscordMusic, true },
        { SecurityLink, "Показывать видео", "«Смотрит: название видео» из браузера (YouTube).", DrawDiscordVideo, true },
        { SecurityLink, "Телеграм: по шагам", "Сделаешь один раз за пару минут. Иди по порядку.", DrawGuide<GuideTelegram> },
        { SecurityLink, "Приватность простыми словами", "Что и куда уходит.", DrawGuide<Privacy> },
        { AiVoice, "Характер", "Как Аня разговаривает. Можно написать свой характер.", DrawPersonality },
        { AiVoice, "Свои указания", "Добавляются к системному промпту: как отвечать, о чём помнить.", DrawExtraPrompt },
        { AiVoice, "Что помнить обо мне", "Интересы, привычки, как к тебе обращаться. Нейросеть учитывает это в каждом ответе.", DrawAboutMe },
        { AiVoice, "Скачать нейросеть", "Выбери подходящую и нажми «скачать». Всё остальное программа сделает сама.", DrawModelStore },
        { AiVoice, "Нейросеть", "Модель .gguf для домашки и сложных вопросов. Работает прямо на компьютере.", DrawModel },
        { AiVoice, "Профиль ответов", "Стандарт, Быстро, Умно, Креативно или Свой. Меняется сразу и с пульта на телефоне.", DrawAiProfile },
        { AiVoice, "Отвечать через облако", "Нейросеть в интернете по ключу API", DrawCloudOn, true },
        { AiVoice, "Адрес API", "Сервис, совместимый с OpenAI", DrawCloudUrl },
        { AiVoice, "Ключ API", "Хранится зашифрованным, как токен бота", DrawCloudKey },
        { AiVoice, "Модель в облаке", "Например, gpt-4o-mini или deepseek-chat", DrawCloudModel },
        { AiVoice, "Проверить облако", "Подошли ли адрес, ключ и модель", DrawCloudCheck },
        { AiVoice, "Модель распознавания", "Быстрая подходит почти всем. Остальные можно скачать здесь же.", DrawSpeechStore },
        { AiVoice, "Сама определять язык", "Говори по-русски или по-английски — Аня поймёт язык каждой фразы и ответит на нём. Выключи — будет слушать только на языке программы.", DrawAutoLang, true },
        { AiVoice, "Голос", "Silero звучит живее всех, Supertonic и голоса Windows — запасные.", DrawVoice },
        { AiVoice, "Свой голос", "Можно поставить голос, который скачаешь сам. Три шага.", DrawCustomVoice },
        { AiVoice, "Скорость речи", "Насколько быстро она говорит.", DrawRate },
        { AiVoice, "Пауза — конец фразы", "Меньше — отвечает быстрее, но может обрезать фразу на паузе.", DrawSilence },
        { AiVoice, "Реакция на кашель и чих", "«Будь здоров!» на чих и «Всё в порядке? Не заболел?» на кашель.", DrawBodySounds, true },
        { AiVoice, "Высота нейроголоса", "0 — естественно, без обработки.", DrawNeuralPitch },
        { AiVoice, "Высота голосов Windows", "Только для голосов Windows.", DrawPitch },
        { AiVoice, "Освобождать видеокарту", "Если долго не спрашиваешь — нейросеть выгружается, и видеокарта свободна для игр. "
                                               "Первый ответ после этого — на 5–15 секунд дольше.", DrawAiIdle },
        { AiVoice, "Голос простыми словами", "Как поменять то, как она говорит.", DrawGuide<VoiceSection> },
        { AiVoice, "Распознавание простыми словами", "Если она тебя плохо слышит.", DrawGuide<Hearing> },
        { AiVoice, "Интеллект простыми словами", "Что менять, а что лучше не трогать.", DrawGuide<AiSection> },
        { OverlayPage, "Островок Ани у края экрана", "Островок, прилипший к краю экрана. Подведи к нему мышку — раскроется: музыка, полка для файлов и другое. Сам выдвигается, когда Аня слушает, думает и отвечает. Фокус не забирает.", DrawNotchOn, true },
        { OverlayPage, "Настроить островок", "Место у края, глазки, страницы и всплывашки — в отдельном окне. Его же открывает шестерёнка в самом островке.", DrawIslandWindow },
        { OverlayPage, "Перевод области экрана", "Нажми сочетание, выдели мышкой текст на экране — рядом появится перевод. Щёлкни по переводу, чтобы скопировать.", DrawTranslateOn, true },
        { OverlayPage, "Сочетание для перевода", "Клавиши, которые начинают выделение.", DrawTranslateKey },
        { OverlayPage, "Переводить на", "Язык, на который переводить текст с экрана.", DrawTranslateLang },
        { OverlayPage, "Круговое меню", "Зажми среднюю кнопку мыши (колёсико) в любом месте — вокруг курсора раскроется круг с программами. Отпусти на нужной или щёлкни по ней. Короткое нажатие колёсика работает как обычно.", DrawPieOn, true },
        { OverlayPage, "Открывать коротким нажатием", "Меню открывается и от быстрого нажатия (касание тачпада тремя пальцами) и ждёт выбора. Обычный щелчок этой кнопкой тогда не работает.", DrawPieTap, true },
        { OverlayPage, "Чем открывать круговое меню", "Одно или несколько сочетаний — любое открывает меню: клавиши (с Ctrl, Alt, Shift, Win) или кнопка мыши — колёсико, правая, боковые, тоже с клавишами.", DrawPieTrigger },
        { OverlayPage, "Что в круговом меню", "Картинка — как выглядит круг. Первый пункт — сверху, дальше по часовой. Папка открывает следующий круг со своими пунктами — вкладывай сколько угодно.", DrawPieApps },
        { OverlayPage, "Окно нагрузки", "Полупрозрачное окно поверх всего: процессор, память, диск, сеть и видеокарта.", DrawResOn, true },
        { OverlayPage, "Закрепить окно нагрузки", "Закреплённое окно не мешает: клики проходят сквозь него, а при наведении оно почти исчезает. Пока не закреплено, его можно таскать мышкой.", DrawResPin, true },
        { OverlayPage, "Прозрачность окна нагрузки", "Чем меньше, тем прозрачнее.", DrawResOpacity },
        { OverlayPage, "Оверлей простыми словами", "Зачем это нужно.", DrawGuide<Overlay> },
        { WidgetsSection, "Показывать виджеты", "Главный выключатель всех карточек.", DrawWidgetsOn, true },
        { WidgetsSection, "Время", "Часы и дата.", DrawClockWidget, true },
        { WidgetsSection, "Погода", "Слева от сферы: 4 часа, а при наведении — весь день.", DrawWeatherWidget, true },
        { WidgetsSection, "Город", "Для погоды, прогноза и виджета.", DrawCity },
        { WidgetsSection, "Заметки", "Твои напоминания на главном экране.", DrawNotesWidget, true },
        { WidgetsSection, "Календарь", "Месяц с отмеченным сегодняшним днём.", DrawCalendarWidget, true },
        { WidgetsSection, "Курсы валют", "Карточка на главном экране, под блоками системы и музыки.", DrawRatesWidget, true },
        { WidgetsSection, "Валюта курсов", "В какой валюте показывать. По умолчанию — валюта страны твоего города.", DrawRatesCurrency },
        { WidgetsSection, "Мониторинг системы", "Нагрузка и кнопка оптимизации памяти.", DrawSystemWidget, true },
        { WidgetsSection, "Быстрые фразы", "Подсказки-кнопки под полем ввода.", DrawQuickPhrases, true },
        { Appearance, "Фон", "Анимация на главном экране.", DrawBackdrop },
        { Appearance, "Свои обои", "Любая картинка на фоне Ани. Анимация фона идёт поверх неё.", DrawWallpaper },
        { Appearance, "Цвета по обоям", "Тема, любимый цвет и цвет анимации подбираются под картинку. Только в Ане — Windows не трогаю. "
                                       "Выключишь — вернутся прежние цвета.", DrawAutoColors, true },
        { Appearance, "Затемнение обоев", "Чтобы текст и сфера хорошо читались на яркой картинке.", DrawWallpaperDim },
        { Appearance, "Скорость анимации", "Насколько быстро движется фон.", DrawBackdropSpeed },
        { Appearance, "Тетрадь с рисунками", "Бумага в клетку, рисунки от руки и нарисованная Аня вместо обоев и анимации.", DrawCozy, true },
        { Appearance, "Кто в центре", "Персонаж вместо шара. Можно поставить свою картинку.", DrawMascot },
        { Appearance, "Рисунки на листе", "Какие каракули нарисованы на фоне тетради и «Домашнего уюта».", DrawCozyPattern },
        { Appearance, "Готовые варианты", "Нажми — и всё оформление сразу станет таким. «Как после установки» возвращает вид по умолчанию.", DrawLookPresets },
        { Appearance, "Внешний вид файлом", "Сохрани своё оформление в файл и отправь кому хочешь: у другого человека Аня станет такой же. Обои в файл не попадают.", DrawLookFile },
        { Appearance, "Форма плашек и кнопок", "«Пилюли» — сильно скруглённые плашки и кнопки, «чёткие» — почти прямые углы, «контур» — тонкая рамка любимого цвета вместо заливки.", DrawStyle },
        { Appearance, "Светлая или тёмная", "«Как в Windows» — переключается вместе с системой.", DrawMode },
        { Appearance, "Тема", "Атмосфера оформления: своя палитра у каждой, светлая и тёмная. «Свои цвета» — прежние темы и любой цвет.", DrawLook },
        { Appearance, "Любимый цвет", "Этим цветом я крашу кнопки, переключатели и всё, что ты выбираешь, — и сферу, когда отдыхаю.", DrawAccent },
        { Appearance, "Цвет фона", "Свой цвет фона поверх темы.", DrawBackgroundColor },
        { Appearance, "Цвет анимации", "Без него фон переливается цветами моего настроения.", DrawAnimationColor },
        { Appearance, "Эффект окна", "Как в Windows 11: сквозь Аню видно размытый фон. Акрил — заметнее, Mica — спокойнее.", DrawWindowEffect },
        { Appearance, "Плотность фона", "Насколько фон Ани закрывает эффект окна. Меньше — прозрачнее.", DrawWindowOpacity },
        { Appearance, "Стекло", "Полупрозрачные панели вместо плотных.", DrawGlass, true },
        { Appearance, "Подложка под кнопками окна", "Подложка за кнопками «свернуть», «развернуть» и «закрыть». Без неё — только значки.", DrawPlates, true },
        { Appearance, "Шрифт", "Как выглядит весь текст программы.", DrawFontFace },
        { Appearance, "Размер интерфейса", "Текст и блоки вместе: от 80% до 120%, посередине — обычный размер.", DrawFontSize },
        { Appearance, "Внешний вид простыми словами", "С чего начать, если хочется сделать красиво.", DrawGuide<Appearance> },
        { About, "О программе", "Голосовая помощница, которая работает прямо на твоём компьютере.", DrawAbout },
        { About, "Версия", "Проверить, вышла ли новая версия на GitHub.", DrawUpdates },
        { About, "Автообновления", "Сама скачает новую версию и поставит при следующем запуске.", DrawAutoUpdate, true },
        { About, "Настройки по умолчанию", "Если что-то сломалось или запуталась — вернуть всё как было при установке.", DrawResetAll },
        { About, "Журнал", "Файл с действиями программы лежит только на этом компьютере.", DrawClearLog },
        { About, "Удалить программу", "Полностью: все данные, настройки и автозапуск. Перед удалением спрошу ещё раз.", DrawRemoveAll },
        { AiVoice, "Точный режим для учёбы и кода", "Вопросы по предметам, фактам и программированию Аня узнаёт сама и отвечает строже: "
                                                     "меньше выдумок, ответ подлиннее, код показывает на экране, а вслух не читает.", DrawPrecise, true },
        { AiVoice, "База знаний", "Переводы и проверенные ответы из интернета, которые Аня запомнила. Знакомое отвечает сразу, без сети и нейросети.", DrawKnowledge },
        { AiVoice, "Точный режим: температура", "Ниже — точнее и строже. 30% подходит для кода и задач.", DrawPreciseTemp },
        { AiVoice, "Размышления", "Когда модель думает «про себя» перед ответом. Долго, зато точнее.", DrawThinkMode },
        { AiVoice, "Время на размышления", "Сколько модель может думать при голосовом ответе. Потом ответит сразу.", DrawThinkSeconds },
        { AiVoice, "Разговор: температура", "Выше — живее и неожиданнее, ниже — точнее и однообразнее.", DrawChatTemp },
        { AiVoice, "Разговор: top-p", "Какую долю вероятных слов рассматривать.", DrawChatTopP },
        { AiVoice, "Разговор: top-k", "Из скольких лучших слов выбирать. 0 — без ограничения.", DrawChatTopK },
        { AiVoice, "Разговор: min-p", "Отсекает слишком маловероятные слова.", DrawChatMinP },
        { AiVoice, "Разговор: штраф за повторы", "100% — без штрафа, выше — меньше повторов.", DrawChatRepeat },
        { AiVoice, "Разговор: длина ответа", "Предел в токенах (примерно слоги).", DrawChatTokens },
        { AiVoice, "Задачи: температура", "Для размышлений и задач из телеграма.", DrawTaskTemp },
        { AiVoice, "Задачи: top-p", "Какую долю вероятных слов рассматривать.", DrawTaskTopP },
        { AiVoice, "Задачи: top-k", "Из скольких лучших слов выбирать. 0 — без ограничения.", DrawTaskTopK },
        { AiVoice, "Задачи: min-p", "Отсекает слишком маловероятные слова.", DrawTaskMinP },
        { AiVoice, "Задачи: штраф за повторы", "100% — без штрафа, выше — меньше повторов.", DrawTaskRepeat },
        { AiVoice, "Задачи: длина ответа", "Предел в токенах вместе с размышлениями.", DrawTaskTokens },
        { AiVoice, "Seed", "Одинаковое число — повторяемые ответы, −1 — каждый раз случайно.", DrawAiSeed },
        { AiVoice, "Загружать при запуске", "Выключено — видеокарта свободна, пока не спросишь; первый ответ на 5–15 секунд дольше.", DrawAiPreload, true },
        { AiVoice, "Во время игры", "Когда на весь экран открыта игра. «С процессора» — видеопамять вся у игры, Аня отвечает той же моделью 4B, "
                                     "чуть медленнее (около 8 слов в секунду). «Выгружать» — нейросеть уходит через минуту тишины и на вопрос загружается на видеокарту.", DrawGameMode },
        { AiVoice, "Глаза модели", "Понимать картинки. Загружаются только когда приходит снимок экрана (+0,9 ГБ видеопамяти до выгрузки).", DrawAiVision, true },
        { AiVoice, "Слои на видеокарте", "99 — сколько поместится: остальное возьмёт процессор. 0 — только процессор. Нужен перезапуск.", DrawGpuLayers },
        { AiVoice, "Потоки процессора", "0 — движок выберет сам. Нужен перезапуск.", DrawAiThreads },
        { AiVoice, "Размер контекста", "Сколько нейросеть помнит за раз, делится между запросами. Больше — больше памяти. Нужен перезапуск.", DrawAiContext },
        { AiVoice, "Запросов одновременно", "2 — можно говорить с Аней, пока она решает задачу для телеграма. Нужен перезапуск.", DrawAiParallel },
        { AiVoice, "Свои параметры движка", "Для знающих: дополнительные аргументы llama-server. Ошибка в них не даст нейросети запуститься.", DrawEngineArgs },
        { AiVoice, "Применить", "Настройки ответов работают сразу, настройки движка — после перезапуска.", DrawAiApply },
    };

    // ---------------------------------------------------------------- «где эта настройка?» — подсветка
    std::atomic<int> g_revealRequest{ -1 }; // номер пункта, который попросили показать (из любого потока)
    int g_reveal = -1;                      // какой пункт светится сейчас
    float g_revealTime = 0.0f;              // сколько он уже светится
    bool g_revealScroll = false;            // надо доехать до него прокруткой
    float g_scrollTo = -1.0f;
    void GlassPanel(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, bool scrolled = false);
    constexpr float kRevealSeconds = 6.0f;
    ImU32 Orange(float alpha) { return IM_COL32(255, 152, 48, static_cast<int>(255 * std::clamp(alpha, 0.0f, 1.0f))); }

    // ---------------------------------------------------------------- поиск
    std::wstring Lower(const char* text) { return Text::LowerRu(Text::ToWide(text)); }

    bool Matches(const Item& item, const std::wstring& query) {
        if (query.empty()) return true; // все разделы на одной странице
        const std::wstring hay = Lower(item.title) + L" " + Lower(item.hint) + L" " + Lower(kInfo[item.section].name);
        return hay.find(query) != std::wstring::npos;
    }

    // ---------------------------------------------------------------- части страницы
    void DrawSidebar(ImDrawList* dl, ImVec2 pos, float width) {
        const float itemHeight = Px(37), pad = Px(7);
        const bool compact = width < Px(120);
        const ImVec2 max(pos.x + width, pos.y + pad * 2 + itemHeight * static_cast<float>(kSections));
        GlassPanel(dl, pos, max, Px(20));

        { // плашка выбора перетекает между пунктами и слегка вытягивается на ходу
            static float s_y = -1.0f;
            const float target = pos.y + pad + g_section * itemHeight, dt = ImGui::GetIO().DeltaTime;
            if (s_y < 0.0f) s_y = target - pos.y;
            s_y += (target - pos.y - s_y) * Theme::Ease(dt, 13.0f);
            const float distance = std::fabs(target - pos.y - s_y), y = pos.y + s_y, stretch = std::min(distance * 0.35f, itemHeight * 0.45f);
            const ImVec2 a(pos.x + pad, y - stretch * 0.5f), b(pos.x + width - pad, y + itemHeight - Px(4) + stretch * 0.5f);
            if (!g_query[0]) {
                Theme::Pick(dl, a, b, Px(10), 1.0f, true);
                dl->AddRectFilled(ImVec2(a.x + Px(1), a.y + Px(10)), ImVec2(a.x + Px(4), b.y - Px(10)), Theme::Accent().WithAlpha(1.0f), Px(2));
            }
        }

        for (int i = 0; i < kSections; ++i) {
            const ImVec2 a(pos.x + pad, pos.y + pad + i * itemHeight), b(pos.x + width - pad, a.y + itemHeight - Px(4));
            ImGui::SetCursorScreenPos(a);
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##section", ImVec2(b.x - a.x, b.y - a.y)) && true) {
                g_section = i;
                g_query[0] = '\0';
                g_scrollToSection = i;
                g_holdSectionUntil = ImGui::GetTime() + 0.8;
            }
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            const bool selected = i == g_section && !g_query[0];
            static float s_hover[kSections] = {}, s_pick[kSections] = {};
            const float k = Theme::Ease(ImGui::GetIO().DeltaTime, 8.7f);
            s_hover[i] += ((hovered ? 1.0f : 0.0f) - s_hover[i]) * k;
            s_pick[i] += ((selected ? 1.0f : 0.0f) - s_pick[i]) * k;
            const float fill = std::max(s_pick[i] * 22.0f, s_hover[i] * 12.0f);
            if (fill > 0.5f) dl->AddRectFilled(a, b, Theme::Overlay(static_cast<int>(s_hover[i] * 12.0f)), Px(10));
            const ImU32 color = selected ? Theme::kInk : Theme::kInkMuted;
            const float cy = (a.y + b.y) * 0.5f;
            Widgets::DrawIcon(dl, kInfo[i].icon, ImVec2(compact ? (a.x + b.x) * 0.5f : a.x + Px(20), cy), Px(15), color);
            if (compact) {
                if (hovered) ImGui::SetTooltip("%s", kInfo[i].name);
            } else {
                ImFont* font = Theme::semibold; // одинаковый размер: длинные названия не вылезают за плашку
                Widgets::Text(dl, font, ImVec2(a.x + Px(38), cy - font->LegacySize * 0.55f), color, kInfo[i].name);
                if (i == PhoneSync) { // точка-метка «в разработке» у правого края пункта меню
                    const ImVec2 c(b.x - Px(14), cy);
                    dl->AddCircleFilled(c, Px(3.5f), Theme::Accent().WithAlpha(0.9f));
                    if (hovered) ImGui::SetTooltip("%s", Lang::Tr("В разработке"));
                }
            }
        }
    }

    bool DrawSearchField(ImDrawList* dl, ImVec2 pos, float width) {
        const float height = Px(38);
        static bool s_active = false; // поле в фокусе (с прошлого кадра) — подсветить рамку
        const bool active = s_active;
        const ImVec2 max(pos.x + width, pos.y + height);
        dl->AddRectFilled(pos, max, Theme::Control(active ? 16 : 10), Px(10));
        dl->AddRect(pos, max, Theme::Overlay(active ? 30 : 22), Px(10), 0, Px(1.0f));
        if (active) dl->AddRectFilled(ImVec2(pos.x + Px(10), max.y - Px(2)), ImVec2(max.x - Px(10), max.y), Theme::Accent().WithAlpha(1.0f), Px(1));
        Widgets::DrawIcon(dl, Icon::Search, ImVec2(pos.x + Px(20), pos.y + height * 0.5f), Px(15), Theme::kInkMuted);

        ImGui::SetCursorScreenPos(ImVec2(pos.x + Px(36), pos.y + (height - ImGui::GetFrameHeight()) * 0.5f));
        ImGui::SetNextItemWidth(width - Px(48));
        for (ImGuiCol col : { ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive, ImGuiCol_Border })
            ImGui::PushStyleColor(col, IM_COL32(0, 0, 0, 0));
        if (g_justOpened) ImGui::SetKeyboardFocusHere();
        const bool changed = ImGui::InputTextWithHint("##settingsSearch", "поиск настроек…", g_query, sizeof(g_query));
        s_active = ImGui::IsItemActive();
        ImGui::PopStyleColor(4);
        return changed;
    }

    // Текст по центру с переносом по словам; возвращает высоту
    float CenteredText(ImDrawList* dl, ImFont* font, float centerX, float top, float maxWidth, ImU32 color, const std::string& text) {
        std::vector<std::string> lines(1);
        size_t at = 0;
        while (at < text.size()) {
            size_t end = text.find(' ', at);
            if (end == std::string::npos) end = text.size();
            const std::string word = text.substr(at, end - at);
            const std::string candidate = lines.back().empty() ? word : lines.back() + " " + word;
            if (!lines.back().empty() && Widgets::TextSize(font, candidate.c_str()).x > maxWidth) lines.push_back(word);
            else lines.back() = candidate;
            at = end + 1;
        }
        const float height = font->LegacySize * 1.3f;
        for (size_t i = 0; i < lines.size(); ++i)
            Widgets::Text(dl, font, ImVec2(centerX - Widgets::TextSize(font, lines[i].c_str()).x * 0.5f, top + height * i), color, lines[i].c_str());
        return height * lines.size();
    }

    void DrawModelRows(int first, int count, float width);
    void DrawModelStore(float width) { DrawModelRows(0, ModelStore::Count(), width); }
    void DrawSpeechStore(float width) { DrawModelRows(ModelStore::SpeechFirst(), ModelStore::SpeechCount(), width); }
    void DrawModelRows(int first, int count, float width) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float rowWidth = std::min(width, Px(720));
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const auto progress = ModelStore::Now();
        const std::string active = Ai::ActiveModel();
        float y = start.y;
        for (int i = first; i < first + count; ++i) {
            const auto& model = ModelStore::Get(i);
            const float buttonW = Px(150);
            const float aboutW = rowWidth - Px(32) - buttonW;
            const float aboutH = Theme::mono->CalcTextSizeA(Theme::mono->LegacySize, FLT_MAX, aboutW, model.about).y;
            const float rowH = std::max(Px(88), Px(10) + Theme::title->LegacySize * 1.35f + aboutH + Px(22));
            const ImVec2 a(start.x, y), b(start.x + rowWidth, y + rowH);
            const bool speech = std::string(model.id) == "speech";
            const bool used = speech ? Config::Value("ears", "model", "ggml-small.bin") == model.files[0].name : active == model.files[0].name;
            dl->AddRectFilled(a, b, used ? Theme::Accent().WithAlpha(0.22f) : Theme::Overlay(10), Px(14));
            if (used) dl->AddRect(a, b, Theme::Accent().WithAlpha(0.8f), Px(14), 0, Px(1.3f));
            char size[32];
            snprintf(size, sizeof(size), "%.1f ГБ", ModelStore::SizeOf(i) / 1073741824.0);
            Widgets::Text(dl, Theme::title, ImVec2(a.x + Px(16), a.y + Px(10)), Theme::kInk, (std::string(Lang::Tr(model.name)) + " · " + size).c_str());
            dl->AddText(Theme::mono, Theme::mono->LegacySize, ImVec2(a.x + Px(16), a.y + Px(10) + Theme::title->LegacySize * 1.35f), Theme::kInkMuted, Lang::Tr(model.about), nullptr, aboutW);
            ImGui::PushID(i);
            const float buttonX = b.x - buttonW + Px(10);
            ImGui::SetCursorScreenPos(ImVec2(buttonX, a.y + Px(14)));
            if (progress.running && progress.index == i) {
                const float k = progress.total ? static_cast<float>(progress.done) / static_cast<float>(progress.total) : 0.0f;
                char percent[16];
                snprintf(percent, sizeof(percent), "%d%%", static_cast<int>(k * 100.0f));
                const float barW = rowWidth - Px(32), barY = b.y - Px(14);
                dl->AddRectFilled(ImVec2(a.x + Px(16), barY), ImVec2(a.x + Px(16) + barW, barY + Px(6)), Theme::Overlay(24), Px(3));
                dl->AddRectFilled(ImVec2(a.x + Px(16), barY), ImVec2(a.x + Px(16) + barW * k, barY + Px(6)), Theme::Accent().WithAlpha(1.0f), Px(3));
                ImGui::TextUnformatted(percent);
                ImGui::SameLine();
                if (ImGui::SmallButton("отмена")) ModelStore::Cancel();
            } else if (used) {
                ImGui::TextUnformatted("работает");
            } else if (ModelStore::Installed(i)) {
                if (ImGui::Button("включить")) {
                    if (speech) { Config::SetValue("ears", "model", model.files[0].name); Ears::ReloadModel(); }
                    else { Config::Update([&](Settings& s) { s.model = ModelStore::Path(i); }); Ai::Restart(); }
                }
            } else {
                ImGui::BeginDisabled(progress.running);
                if (ImGui::Button("скачать")) ModelStore::Download(i);
                ImGui::EndDisabled();
            }
            ImGui::PopID();
            y += rowH + Px(8);
        }
        if (!progress.status.empty() && progress.index >= first && progress.index < first + count) {
            ImGui::SetCursorScreenPos(ImVec2(start.x, y));
            ImGui::TextDisabled("%s", progress.status.c_str());
            y += ImGui::GetTextLineHeightWithSpacing();
        }
        ImGui::SetCursorScreenPos(start);
        ImGui::Dummy(ImVec2(rowWidth, y - start.y));
    }

    void DrawTourAgain(float) { if (ImGui::Button("показать обучение")) Tour::Start(); }

    // Подчёркнутая ссылка по центру
    bool GuideLink(ImDrawList* dl, float centerX, float y, const std::string& label, ImVec2& size) {
        size = Widgets::TextSize(Theme::semibold, label.c_str());
        const ImVec2 at(centerX - size.x * 0.5f, y);
        ImGui::SetCursorScreenPos(at);
        const bool clicked = ImGui::InvisibleButton(label.c_str(), size);
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const ImU32 color = Theme::Accent().Mix({ 255, 255, 255 }, hovered ? 0.8f : 0.55f).WithAlpha(1.0f);
        Widgets::Text(dl, Theme::semibold, at, color, label.c_str());
        dl->AddLine(ImVec2(at.x, at.y + size.y + Px(1)), ImVec2(at.x + size.x, at.y + size.y + Px(1)), color, Px(1));
        return clicked;
    }

    // Объяснения простыми словами: строка, ссылка на настройку и/или на сайт
    struct GuideRow { const char* text; const char* setting; const char* url; const char* urlLabel; };
    void DrawGuideRows(const GuideRow* rows, int count, float width) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float rowWidth = std::min(width, Px(640));
        const ImVec2 start = ImGui::GetCursorScreenPos();
        float y = start.y;
        // ссылка стоит слева, ровно под текстом шага
        const auto linkCenter = [&](const std::string& label) { return start.x + Widgets::TextSize(Theme::semibold, label.c_str()).x * 0.5f; };
        const float fontSize = Theme::semibold->LegacySize;
        for (int r = 0; r < count; ++r) {
            const GuideRow& row = rows[r];
            const char* text = Lang::Tr(row.text); // перевод до переноса строк
            dl->AddText(Theme::semibold, fontSize, ImVec2(start.x, y), Theme::kInkSoft, text, nullptr, rowWidth);
            y += Theme::semibold->CalcTextSizeA(fontSize, FLT_MAX, rowWidth, text).y + Px(3);
            ImGui::PushID(r);
            ImVec2 size(0, 0);
            const std::string openLabel = row.url ? std::string(Lang::IsEnglish() ? "open: " : "открыть: ") + Lang::Tr(row.urlLabel) : std::string();
            if (row.url && GuideLink(dl, linkCenter(openLabel), y, openLabel, size)) {
                ShellExecuteW(nullptr, L"open", Text::ToWide(row.url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            if (row.url) y += size.y + Px(4);
            if (row.setting) {
                int index = -1;
                for (int i = 0; i < static_cast<int>(std::size(kItems)); ++i) if (std::strcmp(kItems[i].title, row.setting) == 0) index = i;
                if (index >= 0) {
                    const std::string goLabel = std::string(Lang::IsEnglish() ? "go to: " : "перейти: ") + Lang::Tr(row.setting);
                    if (GuideLink(dl, linkCenter(goLabel), y, goLabel, size)) g_revealRequest = index;
                    y += size.y + Px(4);
                }
            }
            ImGui::PopID();
            y += Px(6);
        }
        ImGui::SetCursorScreenPos(start);
        ImGui::Dummy(ImVec2(rowWidth, y - start.y));
    }

    template <int Guide> void DrawGuide(float width) {
        static const GuideRow kGeneral[] = {
            { "Здесь её имя и то, как она обращается к тебе.", "Имя помощницы", nullptr, nullptr },
            { "Хочешь, чтобы она реагировала только на кнопку, а не на имя? Выключи это.", "Слушать всегда", nullptr, nullptr } };
        static const GuideRow kAppearance[] = {
            { "Самое простое — выбрать готовый вид: тетрадка с рисунками.", "Тетрадь с рисунками", nullptr, nullptr },
            { "Или поставить свою картинку на фон.", "Свои обои", nullptr, nullptr },
            { "Мелкий текст? Сделай всё крупнее.", "Размер интерфейса", nullptr, nullptr } };
        static const GuideRow kVoice[] = {
            { "Выбери голос и нажми «послушать».", "Голос", nullptr, nullptr },
            { "Говорит слишком быстро или медленно — поправь здесь.", "Скорость речи", nullptr, nullptr } };
        static const GuideRow kHearing[] = {
            { "Она обрывает тебя на полуслове? Сдвинь ползунок вправо.", "Пауза — конец фразы", nullptr, nullptr } };
        static const GuideRow kAi[] = {
            { "Обычно тут ничего менять не надо. Хочешь другой стиль ответов — начни с этого.", "Профиль ответов", nullptr, nullptr },
            { "Характер: строгая, милая или какая угодно.", "Характер", nullptr, nullptr },
            { "Расскажи о себе, и она будет это учитывать.", "Что помнить обо мне", nullptr, nullptr },
            { "Всё, что ниже профиля, — для тех, кто разбирается. Сломалось — внизу есть кнопка «всё по умолчанию».", "Применить", nullptr, nullptr } };
        static const GuideRow kSearch[] = {
            { "Скажи: «найди котиков» — откроется браузер. Здесь выбирается поисковик.", "Поисковик", nullptr, nullptr },
            { "Скажи: «найди файл диплом» — она поищет на компьютере.", "Поиск файлов", nullptr, nullptr } };
        static const GuideRow kOverlay[] = {
            { "Подведи мышку к верхнему краю экрана — появится островок: музыка, время с погодой и таймеры.", "Островок", nullptr, nullptr },
            { "Хочешь видеть нагрузку на компьютер поверх игры — включи это.", "Окно нагрузки", nullptr, nullptr } };
        static const GuideRow kTelegram[] = {
            { "1. Открой Telegram и нажми «Запустить» у создателя ботов.", nullptr, "https://t.me/BotFather", "BotFather" },
            { "2. Отправь ему сообщение: /newbot", nullptr, nullptr, nullptr },
            { "3. Он спросит имя. Напиши любое, например ZAVIRIA.", nullptr, nullptr, nullptr },
            { "4. Потом спросит ник. В конце обязательно bot, например sasha_zaviria_bot. Занят — придумай другой.", nullptr, nullptr, nullptr },
            { "5. Придёт сообщение с длинным кодом вида 123456:ABC… Скопируй код и вставь сюда:", "Токен бота", nullptr, nullptr },
            { "6. В @BotFather открой своего бота → Login Widget. В Allowed URLs добавь адрес входа (кнопка «скопировать адрес входа»).", "Ключ входа", nullptr, nullptr },
            { "7. Там же скопируй Client Secret и вставь сюда:", "Ключ входа", nullptr, nullptr },
            { "8. Нажми «Войти через Telegram» и подтверди вход. Всё! Слева от поля ввода появится кнопка «Пульт».", "Вход через Telegram", nullptr, nullptr },
            { "Код бота и Client Secret — как пароли. Никому их не показывай.", nullptr, nullptr, nullptr } };
        static const GuideRow kDiscord[] = {
            { "1. Открой страницу Discord и войди в свой аккаунт.", nullptr, "https://discord.com/developers/applications", "страницу Discord" },
            { "2. Справа вверху нажми синюю кнопку New Application.", nullptr, nullptr, nullptr },
            { "3. Напиши ZAVIRIA, поставь галочку и нажми Create.", nullptr, nullptr, nullptr },
            { "4. Найди строку Application ID и нажми Copy рядом с ней.", nullptr, nullptr, nullptr },
            { "5. Вставь скопированное сюда:", "ID приложения Discord", nullptr, nullptr },
            { "6. Включи статус. Discord должен быть открыт на компьютере.", "Показывать активность в Discord", nullptr, nullptr } };
        static const GuideRow kPrivacy[] = {
            { "Голос и нейросеть работают прямо на компьютере. Ничего не уходит в облако.", nullptr, nullptr, nullptr },
            { "Управлять с телефона может только один человек — ты.", "Кому можно", nullptr, nullptr },
            { "Не хочешь, чтобы бот снимал экран? Выключи.", "Снимки экрана", nullptr, nullptr } };
        static const GuideRow kSystem[] = {
            { "Чтобы программа запускалась сама вместе с компьютером.", "Запускать вместе с Windows", nullptr, nullptr },
            { "Чтобы при закрытии окна она оставалась работать возле часов.", "Фоновый режим", nullptr, nullptr },
            { "Что-то сломалось? Верни всё как было.", "Настройки по умолчанию", nullptr, nullptr } };
        static const GuideRow kUpdates[] = {
            { "Нажми кнопку — скажу, вышла ли новая версия.", "Версия", nullptr, nullptr } };
#define GUIDE(id, rows) if (Guide == id) { DrawGuideRows(rows, static_cast<int>(std::size(rows)), width); return; }
        GUIDE(General, kGeneral) GUIDE(Appearance, kAppearance) GUIDE(VoiceSection, kVoice) GUIDE(Hearing, kHearing) GUIDE(AiSection, kAi)
        GUIDE(SearchSection, kSearch) GUIDE(Overlay, kOverlay) GUIDE(GuideTelegram, kTelegram) GUIDE(GuideDiscord, kDiscord)
        GUIDE(Privacy, kPrivacy) GUIDE(SystemSection, kSystem) GUIDE(Updates, kUpdates)
#undef GUIDE
    }

    void DrawCustomVoice(float width) {
        static const GuideRow kRows[] = {
            { "1. Скачай файл голоса с расширением .json. Открой страницу и нажми стрелку скачивания рядом с файлом.", nullptr, "https://huggingface.co/Supertone/supertonic-3/tree/main/voice_styles", "страницу голосов" },
            { "2. Нажми кнопку ниже: откроется папка. Перетащи скачанный файл туда.", nullptr, nullptr, nullptr } };
        static const GuideRow kAfter[] = {
            { "3. Закрой и снова открой ZAVIRIA. Голос появится в списке выше как «Свой голос».", nullptr, nullptr, nullptr },
            { "Подходят только файлы голосов для Supertonic. Голоса Windows ставятся отдельно, в параметрах системы.", nullptr, "ms-settings:speech", "голоса Windows" } };
        { // инструкция спрятана под стрелкой — не занимает место, пока не нужна
            static bool s_open = false;
            static float s_anim = 0;
            s_anim += ((s_open ? 1.0f : 0.0f) - s_anim) * std::min(1.0f, ImGui::GetIO().DeltaTime * 14.0f);
            const char* label = Lang::Tr("как поставить свой голос");
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const ImVec2 size(Px(26) + Widgets::TextSize(Theme::semibold, label).x + Px(8), Px(28));
            if (ImGui::InvisibleButton("##voiceHow", size)) s_open = !s_open;
            const bool hover = ImGui::IsItemHovered();
            if (hover) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 ink = hover ? Theme::kInk : Theme::kInkSoft;
            const ImVec2 c(at.x + Px(9), at.y + size.y * 0.5f);
            const float r = Px(4.5f), ang = s_anim * 1.5708f; // ▸ поворачивается в ▾
            auto rot = [&](float x, float y) { return ImVec2(c.x + x * std::cos(ang) - y * std::sin(ang), c.y + x * std::sin(ang) + y * std::cos(ang)); };
            const ImVec2 tri[3] = { rot(-r * 0.6f, -r), rot(r * 0.9f, 0), rot(-r * 0.6f, r) };
            dl->AddTriangleFilled(tri[0], tri[1], tri[2], ink);
            Widgets::Text(dl, Theme::semibold, ImVec2(at.x + Px(22), c.y - Theme::semibold->LegacySize * 0.55f), ink, label);
            if (!s_open) return;
        }
        DrawGuideRows(kRows, static_cast<int>(std::size(kRows)), width);
        if (ImGui::Button("открыть папку с голосами")) {
            const std::wstring folder = NeuralVoice::VoicesFolder();
            if (!folder.empty()) ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::Dummy(ImVec2(0, Px(8)));
        ImGui::PushID("after");
        DrawGuideRows(kAfter, static_cast<int>(std::size(kAfter)), width);
        ImGui::PopID();
    }

    // «Обучение»: что на что влияет — со ссылками на сами настройки
    void DrawLearnGuide(float width) {
        struct Link { const char* what; const char* title; };
        static const Link kLinks[] = {
            { "Манера речи: строгая, дружелюбная, милая или твоя собственная.", "Характер" },
            { "Правила на каждый ответ: как отвечать и чего избегать.", "Свои указания" },
            { "Факты о тебе. Нейросеть учитывает их в каждом разговоре, сама ничего не запоминает.", "Что помнить обо мне" },
            { "Как к тебе обращаться и как произносить ник вслух.", "Как произносить имя вслух" },
            { "Выше — живее и неожиданнее, ниже — точнее.", "Разговор: температура" },
            { "Когда модель сначала думает, а потом отвечает.", "Размышления" },
            { "Какая модель отвечает на вопросы.", "Нейросеть" },
        };
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float rowWidth = std::min(width, Px(620));
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float centerX = start.x + rowWidth * 0.5f;
        float y = start.y;
        for (const Link& link : kLinks) {
            int index = -1;
            for (int i = 0; i < static_cast<int>(std::size(kItems)); ++i) if (std::strcmp(kItems[i].title, link.title) == 0) index = i;
            if (index < 0) continue;
            y += CenteredText(dl, Theme::semibold, centerX, y, rowWidth, Theme::kInkSoft, link.what) + Px(1);
            const std::string label = std::string(kInfo[kItems[index].section].name) + " → " + link.title;
            const ImVec2 size = Widgets::TextSize(Theme::semibold, label.c_str());
            const ImVec2 at(centerX - size.x * 0.5f, y);
            ImGui::SetCursorScreenPos(at);
            ImGui::PushID(link.title);
            if (ImGui::InvisibleButton("##link", size)) g_revealRequest = index;
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            const ImU32 color = Theme::Accent().Mix({ 255, 255, 255 }, hovered ? 0.8f : 0.55f).WithAlpha(1.0f);
            Widgets::Text(dl, Theme::semibold, at, color, label.c_str());
            dl->AddLine(ImVec2(at.x, at.y + size.y + Px(1)), ImVec2(at.x + size.x, at.y + size.y + Px(1)), color, Px(1));
            y += size.y + Px(9);
        }
        ImGui::SetCursorScreenPos(start);
        ImGui::Dummy(ImVec2(rowWidth, y - start.y));
    }

    // Сложные настройки спрятаны, чтобы не пугать новичков
    bool g_more[kSections] = {};
    bool Collapsed(const char* title) { // эти пункты живут внутри меню своего раздела и открываются стрелочкой
        const std::string name = title;
        if (name == "Токен бота" || name == "Ключ входа" || name == "Кому можно" || name == "Уведомления в телеграме" || name == "Пульт в телеграме" ||
            name == "Снимки экрана" || name == "Блокировать при «Выключить»") return !g_openTelegram;
        if (name == "ID приложения Discord" || name == "Показывать музыку" || name == "Показывать видео") return !g_openDiscord;
        if (name == "Адрес API" || name == "Ключ API" || name == "Модель в облаке" || name == "Проверить облако") return !CloudToggle();
        if (name == "Телеграм: по шагам") return !g_openTelegram || !g_guideTelegram;
        if (name == "Discord: по шагам") return !g_openDiscord || !g_guideDiscord;
        return false;
    }

    // Что пользователь трогает чаще — то и показываем первым
    std::map<std::string, int> g_usage;
    bool g_usageLoaded = false;
    void LoadUsage() {
        if (g_usageLoaded) return;
        g_usageLoaded = true;
        std::string text = Config::Value("settings", "usage", "");
        for (size_t at = 0; at < text.size();) {
            size_t end = text.find('|', at);
            if (end == std::string::npos) end = text.size();
            const std::string pair = text.substr(at, end - at);
            if (const size_t colon = pair.rfind('='); colon != std::string::npos) g_usage[pair.substr(0, colon)] = std::atoi(pair.c_str() + colon + 1);
            at = end + 1;
        }
    }
    void NoteUse(const char* title) {
        LoadUsage();
        ++g_usage[title];
        std::vector<std::pair<int, std::string>> top;
        for (const auto& [name, count] : g_usage) top.push_back({ count, name });
        std::sort(top.rbegin(), top.rend());
        std::string text;
        for (size_t i = 0; i < std::min<size_t>(top.size(), 12); ++i) text += (i ? "|" : "") + top[i].second + "=" + std::to_string(top[i].first);
        Config::SetValue("settings", "usage", text);
    }

    void DrawRecommended(float width) {
        LoadUsage();
        std::vector<std::pair<int, std::string>> top;
        for (const auto& [name, count] : g_usage) top.push_back({ count, name });
        std::sort(top.rbegin(), top.rend());
        std::vector<std::string> titles;
        for (const auto& entry : top) if (titles.size() < 4) titles.push_back(entry.second);
        for (const char* fallback : { "Имя помощницы", "Голос", "Тема", "Телеграм-бот" }) if (titles.size() < 4 && std::find(titles.begin(), titles.end(), fallback) == titles.end()) titles.push_back(fallback);
        for (const std::string& title : titles) {
            int index = -1;
            for (int i = 0; i < static_cast<int>(std::size(kItems)); ++i) if (title == kItems[i].title) index = i;
            if (index < 0) continue;
            ImGui::PushID(index);
            if (ImGui::Button(title.c_str())) g_revealRequest = index;
            ImGui::PopID();
            ImGui::SameLine();
        }
        ImGui::NewLine();
        (void)width;
    }

    void DrawAbout(float) {
        ImGui::Text("ZAVIRIA, версия %s", kVersion);
        ImGui::TextDisabled("Создатель: Зефир");
        if (ImGui::Button("страница на GitHub")) ShellExecuteW(nullptr, L"open", L"https://github.com/zephiro4ka/ZAVIRIA-Voice-assistant", nullptr, nullptr, SW_SHOWNORMAL);
    }

    bool IsAdvanced(const char* title) {
        static const char* kTitles[] = { "Время на размышления", "Размышления", "Seed", "Глаза модели", "Слои на видеокарте", "Потоки процессора", "Размер контекста",
            "Запросов одновременно", "Свои параметры движка", "Применить", "Загружать при запуске", "Освобождать в играх", "Эффект окна", "Плотность фона", "Затемнение обоев",
            "Цвета по обоям", "Скорость анимации", "Анимировать в неактивном окне", "Цвет анимации", "Цвет фона", "Стекло", "Подложка под кнопками окна", "Высота нейроголоса",
            "Высота голосов Windows", "Как произносить имя вслух", "Своё приложение Discord" };
        const std::string name = title;
        if (name.find("простыми словами") != std::string::npos || name == "Как это работает") return true;
        if (name.rfind("Разговор:", 0) == 0 || name.rfind("Задачи:", 0) == 0) return true;
        for (const char* t : kTitles) if (name == t) return true;
        return false;
    }

    // видимая полоса списка карточек: края в ней растворяются (Theme::FadeEdges)
    float g_viewTop = 0.0f, g_viewBottom = 0.0f;

    // за плашкой видно тетрадь с каракулями, поэтому не совсем плотная
    // плашка в прокручиваемом списке: true — нарисована полосками (вышла за край), false — целиком помещается
    bool Strips(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 fill, ImU32 edge, ImU32 topEdge, float rounding) {
        const float top = g_viewTop, bottom = g_viewBottom;
        if (top >= bottom || Theme::Style() != 0 || (a.y >= top - Px(40) && b.y <= bottom + Px(40))) return false;
        // карточка длиннее окна: растворение идёт по вершинам, а у большого прямоугольника они далеко за краем —
        // и вся плашка пропадала. Рисуем видимую часть полосками, тогда края тают плавно.
        const float y0 = std::max(a.y, top - Px(2)), y1 = std::min(b.y, bottom + Px(2));
        if (y1 <= y0) return true;
        const float step = Px(14);
        for (float y = y0; y < y1;) {
            const bool first = y == a.y, last = y1 == b.y && y1 - y <= std::max(step, rounding) * 2.0f;
            const float next = last ? y1 : std::min(y1, y + (first ? std::max(step, rounding) : step));
            const ImDrawFlags flags = (first ? ImDrawFlags_RoundCornersTop : 0) | (next == b.y ? ImDrawFlags_RoundCornersBottom : 0);
            dl->AddRectFilled(ImVec2(a.x, y), ImVec2(b.x, next), fill, flags ? rounding : 0.0f, flags ? flags : ImDrawFlags_RoundCornersNone);
            const float s0 = first ? y + rounding : y, s1 = next == b.y ? next - rounding : next;
            if (s1 > s0) { // боковые края
                dl->AddLine(ImVec2(a.x + Px(0.5f), s0), ImVec2(a.x + Px(0.5f), s1), edge, Px(1));
                dl->AddLine(ImVec2(b.x - Px(0.5f), s0), ImVec2(b.x - Px(0.5f), s1), edge, Px(1));
            }
            y = next;
        }
        if (a.y >= y0) { // верхний край с углами виден
            dl->PathArcTo(ImVec2(a.x + rounding, a.y + rounding), rounding - Px(0.5f), IM_PI, IM_PI * 1.5f);
            dl->PathArcTo(ImVec2(b.x - rounding, a.y + rounding), rounding - Px(0.5f), IM_PI * 1.5f, IM_PI * 2.0f);
            dl->PathStroke(topEdge, 0, Px(1));
        }
        if (b.y <= y1) {
            dl->PathArcTo(ImVec2(b.x - rounding, b.y - rounding), rounding - Px(0.5f), 0.0f, IM_PI * 0.5f);
            dl->PathArcTo(ImVec2(a.x + rounding, b.y - rounding), rounding - Px(0.5f), IM_PI * 0.5f, IM_PI);
            dl->PathStroke(edge, 0, Px(1));
        }
        return true;
    }

    void GlassPanel(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, bool scrolled) {
        const ImU32 base = Theme::Surface(1.05f), fill = (base & ~IM_COL32_A_MASK) | (205u << IM_COL32_A_SHIFT);
        if (scrolled && Strips(dl, a, b, fill, Theme::Overlay(Theme::Paper() ? 16 : 18), Theme::Overlay(Theme::Paper() ? 26 : 34), rounding)) return;
        Theme::Plate(dl, a, b, fill, rounding);
    }

    // Разделитель, будто проведённый карандашом
    void PencilLine(ImDrawList* dl, float x0, float x1, float y, float seed) {
        ImVec2 points[64];
        const int count = 64;
        for (int i = 0; i < count; ++i) {
            const float k = static_cast<float>(i) / (count - 1), x = x0 + (x1 - x0) * k;
            points[i] = ImVec2(x, y + std::sin(k * 19.0f + seed * 1.7f) * Px(0.9f) + std::sin(k * 47.0f + seed) * Px(0.45f));
        }
        dl->AddPolyline(points, count, Theme::Overlay(24), 0, Px(1.1f));
    }

    std::string FirstSentence(const std::string& text) {
        const size_t dot = text.find_first_of(".!?");
        return dot == std::string::npos ? text : text.substr(0, dot);
    }

    std::string FitLine(ImFont* font, const std::string& text, float width) {
        if (Widgets::TextSize(font, text.c_str()).x <= width) return text;
        std::wstring w = Text::ToWide(text);
        while (!w.empty() && Widgets::TextSize(font, (Text::ToUtf8(w) + "…").c_str()).x > width) w.pop_back();
        return Text::ToUtf8(w) + "…";
    }

    const char* GroupTitle(const char* title) {
        struct Pair { const char* item; const char* group; };
        static const Pair kGroups[] = {
            { "Имя помощницы", "Помощница" }, { "Версия", "Обновления" }, { "Запускать вместе с Windows", "Запуск" }, { "Поисковик", "Поиск" },
            { "Телеграм-бот", "Telegram" }, { "Показывать активность в Discord", "Discord" },
            { "Характер", "Поведение" }, { "Скачать нейросеть", "Нейросеть" }, { "Отвечать через облако", "Облачный ИИ" }, { "Модель распознавания", "Распознавание" }, { "Голос", "Озвучка" },
            { "Островок «Сейчас играет»", "Музыка" }, { "Окно нагрузки", "Нагрузка" },
            { "Показывать виджеты", "На главном экране" },
            { "Фон", "Фон" }, { "Тема", "Цвета" }, { "Эффект окна", "Окно" }, { "Шрифт", "Текст" },
        };
        for (const Pair& pair : kGroups) if (std::strcmp(pair.item, title) == 0) return pair.group;
        return nullptr;
    }

    // Карточка раздела: заголовок, пояснение и его настройки (только подходящие под поиск)
    void DrawCard(Section section, const std::wstring& query, float width) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        // без большой карточки вокруг раздела: заголовок прямо на странице, под ним — островки групп (как в Telegram и Windows 11)
        // как у Astra: раздел — одна большая карточка, внутри строки через тонкие линии
        const float pad = Px(28), inner = width - pad * 2;
        const ImVec2 start = ImGui::GetCursorScreenPos();

        dl->ChannelsSplit(2);
        dl->ChannelsSetCurrent(1);
        ImGui::SetCursorScreenPos(ImVec2(start.x + pad, start.y + pad));
        ImGui::BeginGroup();
        const ImVec2 head = ImGui::GetCursorScreenPos();
        const float tile = Px(26);
        // шапка карточки: тонкий значок, жирный заголовок, под ним пояснение моноширинным
        Widgets::DrawIcon(dl, kInfo[section].icon, ImVec2(head.x + tile * 0.5f, head.y + Theme::title->LegacySize * 0.5f), Px(17), Theme::kInkSoft);
        Widgets::Text(dl, Theme::title, ImVec2(head.x + tile + Px(6), head.y - Px(2)), Theme::kInk, kInfo[section].name);
        if (section == PhoneSync) {
            const float nameW = Theme::title->CalcTextSizeA(Theme::title->LegacySize, FLT_MAX, 0.0f, Lang::Tr(kInfo[section].name)).x;
            SoonBadge(dl, ImVec2(head.x + tile + Px(16) + nameW, head.y + Px(2)), Theme::semibold);
        }
        Widgets::Text(dl, Theme::mono, ImVec2(head.x, head.y + Theme::title->LegacySize + Px(8)), Theme::kInkMuted, kInfo[section].about);
        // редкие настройки прячутся за стрелочкой в шапке раздела
        if (false) { // «показать редкие настройки» убрано по её просьбе: редкие настройки больше не показываются
            ImGui::SetCursorScreenPos(ImVec2(head.x + inner - ImGui::GetFrameHeight(), head.y));
            Chevron(kInfo[section].name, &g_more[section]);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(g_more[section] ? "скрыть редкие настройки" : "показать редкие настройки");
        }
        ImGui::SetCursorScreenPos(head);
        ImGui::Dummy(ImVec2(inner, Theme::title->LegacySize + Theme::mono->LegacySize + Px(22)));
        const float gridLeft = head.x;
        float gridTop = ImGui::GetCursorScreenPos().y, gridRow = 0;
        int blockIndex = 0;
        bool hasMore = false;

        ImVec2 glowMin(0, 0), glowMax(0, 0);
        // строки собираются в островки по группам (как в Telegram); рисуются под строками, когда известна высота
        struct Span { float top, bottom; };
        std::vector<Span> islands;
        float islandTop = -1.0f;
        auto closeIsland = [&](float bottomY) { if (islandTop >= 0.0f && bottomY > islandTop + 1.0f) islands.push_back({ islandTop, bottomY }); islandTop = -1.0f; };
        const bool cozyLines = Theme::Cozy();
        for (const Item& item : kItems) {
            if (item.section != section || !Matches(item, query)) continue;
            if (Collapsed(item.title) && query.empty() && &item - kItems != g_reveal) continue;
            if (IsAdvanced(item.title)) continue; // редкие — убраны из интерфейса
            const bool glowing = &item - kItems == g_reveal;
            const std::string name = item.title;
            const bool wide = name.find("простыми словами") != std::string::npos || name.find("по шагам") != std::string::npos || name == "Как это работает" || name == "Скачать нейросеть" ||
                              name == "Модель распознавания" || name == "Свой голос" || name == "Полная кастомизация" || name == "Тема" || name == "Любимый цвет" || name == "Фон" || name == "Шрифт" ||
                              name == "Кто в центре" || name == "Где островок" || name == "Рисунки на листе" || name == "Нейросеть" || name == "Голос" || name == "Характер" || name == "Свои указания" || name == "Что помнить обо мне" || name == "Часто настраиваешь" || name == "Управление телефоном" || name == "Что в круговом меню";
            if (query.empty())
                if (const char* group = GroupTitle(item.title)) {
                    closeIsland(gridTop);
                    gridTop += blockIndex ? Px(18) : Px(4);
                    // подпись группы — моноширинная, тихим цветом
                    const ImVec2 chip = Widgets::TextSize(Theme::mono, group);
                    Widgets::Text(dl, Theme::mono, ImVec2(gridLeft + Px(8), gridTop), Theme::Accent().Mix(Theme::Paper() ? Theme::Color{ 30, 18, 24 } : Theme::Color{ 255, 255, 255 }, Theme::Paper() ? 0.35f : 0.45f).WithAlpha(1.0f), group);
                    gridTop += chip.y + Px(8);
                }
            if (islandTop < 0.0f) islandTop = gridTop;
            static std::unordered_map<std::string, float> s_controlWidth, s_rowTall;
            { // строки далеко за экраном не рисуем: страница длинная, а кадр должен быть лёгким
                const float known = s_rowTall[name], top = ImGui::GetWindowPos().y, bottomEdge = top + ImGui::GetWindowHeight();
                if (known > 0 && !glowing && (gridTop + known < top - Px(200) || gridTop > bottomEdge + Px(200))) {
                    gridTop += known;
                    ++blockIndex;
                    continue;
                }
            }
            const float rowPad = Px(9), side = Px(8);
            const ImVec2 a(gridLeft, gridTop), b(gridLeft + inner, gridTop + s_rowTall[name]);
            const bool over = ImGui::IsMouseHoveringRect(a, b) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
            if (over) dl->AddRectFilled(a, b, Theme::Overlay(8), Px(12));
            ImGui::PushID(item.title);
            const float textWidth = wide ? inner - side * 2 : (item.inlineControl ? inner - std::min(inner * 0.44f, Px(344)) - side * 3 : inner * 0.5f - side * 2);
            const std::string title = Text::Brand(item.title, Config::Get().assistant), hint = Lang::IsEnglish() ? std::string(Lang::Tr(item.hint)) : Text::Brand(item.hint, Config::Get().assistant);
            float y = a.y + rowPad;
            Widgets::Text(dl, Theme::title, ImVec2(a.x + side, y), Theme::kInk, FitLine(Theme::title, title, textWidth).c_str());
            y += Theme::title->LegacySize + Px(2);
            // у совсем понятных строк подпись не нужна — без неё чище
            static const std::unordered_set<std::string> kNoHint = { "Язык", "Как тебя зовут", "Версия", "Автообновления", "Запускать вместе с Windows",
                "Показывать виджеты", "Время", "Город", "Заметки", "Календарь", "Быстрые фразы", "Скорость речи", "Скорость анимации", "Шрифт", "Фон",
                "Стекло", "Цвет фона", "Светлая или тёмная", "Показывать музыку", "Показывать видео", "Прозрачность окна нагрузки", "Журнал",
                "Токен бота", "Ключ API", "Адрес API", "Модель в облаке", "Проверить облако", "Отвечать через облако", "Настроить островок", "Тема", "Любимый цвет" };
            const std::string shortHint = kNoHint.count(item.title) ? std::string() : FitLine(Theme::mono, FirstSentence(hint), textWidth);
            if (!shortHint.empty()) {
                Widgets::Text(dl, Theme::mono, ImVec2(a.x + side, y + Px(2)), Theme::kInkMuted, shortHint.c_str());
                if (ImGui::IsMouseHoveringRect(ImVec2(a.x, y), ImVec2(a.x + textWidth, y + Theme::semibold->LegacySize)) && shortHint.find("…") != std::string::npos) ImGui::SetTooltip("%s", hint.c_str());
                y += Theme::mono->LegacySize + Px(4);
            }
            float bottom = y;
            if (wide) {
                ImGui::SetCursorScreenPos(ImVec2(a.x + side, y + Px(8)));
                ImGui::BeginGroup();
                item.draw(inner - side * 2);
                ImGui::EndGroup();
            } else if (item.inlineControl) {
                float& control = s_controlWidth[name];
                ImGui::SetCursorScreenPos(ImVec2(a.x + inner - side - control, a.y + rowPad + Px(2)));
                ImGui::BeginGroup();
                item.draw(inner * 0.45f);
                ImGui::EndGroup();
                control = ImGui::GetItemRectSize().x;
            } else {
                const float controlWidth = inner * 0.5f - side; // кнопки и поля — правая половина, перенос по её ширине
                ImGui::SetCursorScreenPos(ImVec2(a.x + inner - side - controlWidth, a.y + rowPad));
                ImGui::BeginGroup();
                item.draw(controlWidth);
                ImGui::EndGroup();
            }
            bottom = std::max(bottom, ImGui::GetItemRectMax().y); 
            ImGui::PopID();
            const float tall = bottom + rowPad - a.y;
            s_rowTall[name] = tall;
            if (a.y > islandTop + 1.0f) { // разделитель между строками островка: тонкая линия с отступом, в «уюте» — карандаш
                if (cozyLines) PencilLine(dl, a.x + side, a.x + inner - side, a.y, static_cast<float>(blockIndex));
                else dl->AddLine(ImVec2(a.x + side + Px(4), a.y), ImVec2(a.x + inner - side - Px(4), a.y), Theme::Overlay(Theme::Paper() ? 14 : 16), Px(1));
            }
            if (glowing) {
                glowMin = a; glowMax = ImVec2(a.x + inner, a.y + tall);
                if (g_revealScroll) { // доехать, чтобы блок был в верхней трети
                    g_scrollTo = ImGui::GetScrollY() + (a.y - ImGui::GetWindowPos().y) - ImGui::GetWindowHeight() * 0.3f;
                    g_revealScroll = false;
                }
            }
            gridTop += tall;
            ++blockIndex;
        }
        closeIsland(gridTop);
        {
            ImGui::SetCursorScreenPos(ImVec2(gridLeft, gridTop + gridRow + Px(10)));
            ImGui::Dummy(ImVec2(inner, 0));
        }
        ImGui::EndGroup();

        const ImVec2 cardMax(start.x + width, ImGui::GetItemRectMax().y + pad - Px(14));
        dl->ChannelsSetCurrent(0);
        GlassPanel(dl, start, cardMax, Px(28), true); // одна большая карточка на раздел
        (void)islands;
        if (glowMax.x > glowMin.x) { // оранжевое свечение: пульсирует и плавно гаснет
            const float t = g_revealTime;
            const float fade = std::min(1.0f, t * 4.0f) * std::clamp((kRevealSeconds - t) / 1.5f, 0.0f, 1.0f);
            const float pulse = 0.65f + 0.35f * std::sin(t * 5.0f);
            for (int i = 3; i >= 1; --i) // мягкий ореол
                dl->AddRect(ImVec2(glowMin.x - Px(2.0f * i), glowMin.y - Px(2.0f * i)), ImVec2(glowMax.x + Px(2.0f * i), glowMax.y + Px(2.0f * i)),
                    Orange(fade * pulse * 0.12f * (4 - i)), Px(14 + 2.0f * i), 0, Px(2));
            dl->AddRectFilled(glowMin, glowMax, Orange(fade * pulse * 0.13f), Px(14));
            dl->AddRect(glowMin, glowMax, Orange(fade * (0.55f + 0.35f * pulse)), Px(14), 0, Px(1.6f));
        }
        dl->ChannelsMerge();
        ImGui::SetCursorScreenPos(ImVec2(start.x, cardMax.y + Px(16)));
        ImGui::Dummy(ImVec2(width, 0));
    }

    void LoadBuffers() {
        const Settings s = Config::Get();
        Copy(g_name, s.name);
        Copy(g_assistant, s.assistant);
        Copy(g_spoken, s.spokenName);
        Copy(g_prompt, s.customPrompt);
        Copy(g_city, Config::Value("weather", "city", "Москва"));
        Copy(g_toCollege, Config::Value("weather", "to_college", "08:00"));
        Copy(g_backHome, Config::Value("weather", "back_home", "15:10"));
        Copy(g_token, Config::Value("telegram", "token", ""));
        Copy(g_owner, Config::Value("telegram", "owner", ""));
        Copy(g_loginSecret, Config::Value("telegram", "login_secret", ""));
        Copy(g_discordApp, Config::Value("discord", "app_id", ""));
        Copy(g_cloudUrl, Config::Value("ai", "cloud_url", ""));
        Copy(g_cloudKey, Config::Value("ai", "cloud_key", ""));
        Copy(g_cloudModel, Config::Value("ai", "cloud_model", ""));
        g_cloudCustom = CloudPresetIndex() == kCloudCustom && !Text::Trim(g_cloudUrl).empty();
        g_query[0] = '\0';
        std::lock_guard lock(g_cityMutex);
        if (!g_citySearching) g_cityStatus.clear();
    }
}

void SettingsPage::Open(int section) {
    static const int kFromOldSections[13] = { 0, 6, 3, 3, 3, 0, 4, 5, 2, 2, 0, 0, 0 }; // обучение и проверки зовут разделы старыми номерами
    if (section >= 0 && section < 13) section = kFromOldSections[section];
    if (section >= 0 && section < kSections) {
        g_section = section;
        g_scrollToSection = section;
        g_holdSectionUntil = ImGui::GetTime() + 0.8;
    }
    if (g_open) return;
    LoadBuffers();
    g_open = true;
    g_justOpened = true;
    g_contentAnim = 0.0f;
}

void SettingsPage::Close() { g_open = false; }
bool SettingsPage::IsOpen() { return g_open; }
bool SettingsPage::Covers() { return g_open && g_anim > 0.97f; }
float SettingsPage::Visibility() { return g_anim; }

void SettingsPage::Draw(ImVec2 screen, float top, float dt) {
    g_anim += ((g_open ? 1.0f : 0.0f) - g_anim) * Theme::Ease(dt, g_open ? 13.0f : 24.0f); // старая страница исчезает сразу, не наезжая на новую
    if (g_anim < 0.04f) return;
    const float ease = 1.0f - std::pow(1.0f - g_anim, 3.0f);
    const float slide = (1.0f - ease) * Px(14); // въезжает чуть снизу

    ImGui::SetNextWindowPos(ImVec2(Theme::railWidth + Theme::PageShift(g_open, g_anim, screen.x), top)); // правее панели вкладок
    ImGui::SetNextWindowSize(ImVec2(screen.x - Theme::railWidth, screen.y - top));
    if (g_justOpened) ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ease);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, PageColor(Config::Get().glass ? 0.22f : 0.5f)); // лёгкое затемнение, фон Ани виден целиком
    if (g_reveal >= 0 && (g_revealTime += dt) > kRevealSeconds) g_reveal = -1;
    ImGui::Begin("##settingsPage", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y + slide);
    const ImVec2 size = ImGui::GetWindowSize();
    const float margin = Px(16);

    // разделы слева — плавающая карточка (в узком окне — только значки)
    const float sideDefault = size.x < Px(760) ? Px(54) : Px(224); // «Безопасность и связь» целиком помещается
    const float sideWidth = std::clamp(sideDefault * HomeLayout::Scale("set_side"), Px(54), std::max(Px(54), size.x - Px(40)));
    const float headerY = origin.y + Px(24);

    // колонка настроек — по центру окна, но не наезжая на меню слева
    const float sideRight = origin.x + Px(20) + sideDefault + Px(28);
    float columnWidth = std::min(Px(900), origin.x + size.x - margin - Px(12) - sideRight);
    float left = std::max(origin.x + (size.x - columnWidth) * 0.5f, sideRight);
    columnWidth = std::min(columnWidth, origin.x + size.x - margin - Px(12) - left);
    const float right = left + columnWidth;

    const float titleWidth = Widgets::TextSize(Theme::hero, "Настройки").x;
    float searchWidth = std::min(Px(320), columnWidth - titleWidth - Px(24));
    ImVec2 searchPos(right - searchWidth, headerY);
    float contentTop = headerY + Px(64);
    if (searchWidth < Px(180)) { // не влезает рядом с заголовком — под ним во всю ширину
        searchWidth = right - left;
        searchPos = ImVec2(left, contentTop);
        contentTop += Px(52);
    }

    // «Редактирование»: заголовок, поиск, разделы и карточки двигаются. Пока ничего не трогали — всё как задумано;
    // иначе прокрутка на всю страницу, а заголовок, поиск и разделы рисуются в ней поверх карточек (иначе под прокруткой их не нажать)
    std::string cardIds[kSections];
    bool custom = HomeLayout::Editing() || HomeLayout::Moved("set_title") || HomeLayout::Moved("set_search") || HomeLayout::Moved("set_side");
    float cardsUp = 0.0f; // насколько карточки подняли выше их обычного верха
    for (int s = 0; s < kSections; ++s) {
        cardIds[s] = "set_card_" + std::to_string(s);
        custom = custom || HomeLayout::Moved(cardIds[s].c_str());
        cardsUp = std::min(cardsUp, HomeLayout::Shift(cardIds[s].c_str(), screen).y);
    }
    const ImVec2 pageMin = origin, pageMax(origin.x + size.x, origin.y + size.y);
    bool searched = false;
    auto drawHeader = [&](ImDrawList* dl) {
        HomeLayout::SetArea(pageMin, pageMax);
        const ImVec2 ts = HomeLayout::Shift("set_title", screen);
        const ImVec2 title(left + ts.x, headerY + ts.y);
        Widgets::Text(dl, Theme::hero, title, Theme::kInk, "Настройки");
        HomeLayout::Report("set_title", "заголовок", ImVec2(title.x - Px(8), title.y - Px(6)), ImVec2(title.x + titleWidth + Px(8), title.y + Px(52)), HomeLayout::kFixed);

        const ImVec2 ss = HomeLayout::Shift("set_search", screen);
        const float searchW = std::clamp(searchWidth * HomeLayout::Scale("set_search"), Px(140), std::max(Px(140), size.x - Px(32)));
        const ImVec2 search(searchPos.x + ss.x, searchPos.y + ss.y);
        Tour::Mark("search", search, ImVec2(search.x + searchW, search.y + Px(38)));
        if (DrawSearchField(dl, search, searchW)) searched = true;
        HomeLayout::Report("set_search", "поиск", ImVec2(search.x - Px(4), search.y - Px(4)), ImVec2(search.x + searchW + Px(4), search.y + Px(42)), HomeLayout::kWidth);

        const ImVec2 sd = HomeLayout::Shift("set_side", screen);
        const ImVec2 side(origin.x + Px(20) + sd.x, contentTop + sd.y);
        const ImVec2 sideMax(side.x + sideWidth, side.y + Px(37) * kSections + Px(14));
        Tour::Mark("sidebar", side, sideMax);
        DrawSidebar(dl, side, sideWidth); // верх меню — на одной линии с карточкой настроек
        HomeLayout::Report("set_side", "разделы", side, sideMax, HomeLayout::kWidth);
        HomeLayout::ClearArea();
    };
    Tour::Mark("content", ImVec2(left, contentTop), ImVec2(right, std::min(origin.y + size.y - Px(12), contentTop + Px(520))));
    if (!custom) drawHeader(dl);

    // карточки
    const std::wstring query = Lower(Text::Trim(g_query).c_str());
    const float bodyTop = custom ? origin.y : contentTop;
    ImGui::SetCursorScreenPos(ImVec2(custom ? origin.x : left - Px(4), bodyTop));
    if (g_lastShown == -1) { // новый поисковый запрос — список снова сверху
        ImGui::SetNextWindowScroll(ImVec2(0, 0));
        g_lastShown = 0;
    }
    // прокрутка — до правого края окна (полоса прокрутки у края, как в Astra)
    const float bodyLeft = custom ? origin.x : left - Px(4);
    ImGui::BeginChild("##settingsCards", ImVec2(origin.x + size.x - Px(6) - bodyLeft, origin.y + size.y - bodyTop), ImGuiChildFlags_None,
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* cardsList = ImGui::GetWindowDrawList();
    const int cardsVtx = cardsList->VtxBuffer.Size;
    const ImVec2 bodyPos = ImGui::GetWindowPos(), bodySize = ImGui::GetWindowSize();
    const float cardsTop = std::max(bodyPos.y, contentTop + cardsUp), cardsBottom = bodyPos.y + bodySize.y;
    g_viewTop = cardsTop;
    g_viewBottom = cardsBottom;
    Widgets::SmoothScroll(&g_scrollTo);
    g_contentAnim += (1.0f - g_contentAnim) * Theme::Ease(dt, 13.0f);
    const float appear = 1.0f - std::pow(1.0f - g_contentAnim, 3.0f);
    ImGui::PushClipRect(ImVec2(bodyPos.x, cardsTop), ImVec2(bodyPos.x + bodySize.x, cardsBottom), true);
    // следующий раздел приезжает снизу, предыдущий — сверху
    ImGui::SetCursorPos(ImVec2(left - bodyLeft, contentTop - bodyTop + Px(28) + std::round((1.0f - appear) * Px(70) * g_sectionDir)));
    // сдвинутая карточка оставляет своё место пустым — соседи не прыгают
    HomeLayout::SetArea(ImVec2(bodyPos.x + Px(4), bodyPos.y - ImGui::GetScrollY() + Px(2)), ImVec2(bodyPos.x + bodySize.x - Px(10), 1e6f));
    const float widest = bodySize.x - Px(14);
    bool any = false;
    for (int s = 0; s < kSections; ++s) {
        const bool show = std::any_of(std::begin(kItems), std::end(kItems), [&](const Item& item) {
            return item.section == s && Matches(item, query);
        });
        if (!show) continue;
        any = true;
        const char* id = cardIds[s].c_str();
        const ImVec2 flow = ImGui::GetCursorScreenPos(), shift = HomeLayout::Shift(id, screen);
        const float w = std::min(columnWidth * HomeLayout::Scale(id), widest);
        g_sectionTop[s] = ImGui::GetCursorPosY() + shift.y;
        ImGui::SetCursorScreenPos(ImVec2(flow.x + shift.x, flow.y + shift.y));
        DrawCard(static_cast<Section>(s), query, w);
        const float bottom = ImGui::GetItemRectMin().y - Px(16), next = ImGui::GetCursorScreenPos().y; // DrawCard ставит Dummy под собой
        HomeLayout::Report(id, kInfo[s].name, ImVec2(flow.x + shift.x, flow.y + shift.y), ImVec2(flow.x + shift.x + w, bottom), HomeLayout::kWidth);
        ImGui::SetCursorScreenPos(ImVec2(flow.x, flow.y + next - (flow.y + shift.y)));
        ImGui::Dummy(ImVec2(0, Px(12)));
    }
    HomeLayout::ClearArea();
    if (query.empty()) {
        if (g_scrollToSection >= 0) { g_scrollTo = g_scrollToSection == 0 ? 0.0f : std::max(0.0f, g_sectionTop[g_scrollToSection] - Px(40)); g_scrollToSection = -1; }
        else if (ImGui::GetTime() > g_holdSectionUntil) { // пункт меню следует за прокруткой
            for (int s = 0; s < kSections; ++s) if (g_sectionTop[s] <= ImGui::GetScrollY() + Px(90)) g_section = s;
            if (ImGui::GetScrollMaxY() > 0 && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - Px(4)) g_section = kSections - 1;
        }
    }
    if (!any) {
        const std::string empty = "ничего не нашла по «" + Text::Trim(g_query) + "»";
        ImGui::Dummy(ImVec2(0, Px(40)));
        ImGui::SetCursorPosX(left - bodyLeft + (columnWidth - Widgets::TextSize(Theme::body, empty.c_str()).x) * 0.5f);
        ImGui::TextDisabled("%s", empty.c_str());
    }
    ImGui::Dummy(ImVec2(0, Px(12)));
    ImGui::PopClipRect();
    Theme::FadeEdges(cardsList, cardsVtx, cardsTop, cardsBottom, Px(34)); // у краёв карточки тают, а не обрезаются
    if (custom) { // заголовок, поиск и разделы — поверх карточек и не листаются; размер прокрутки они не меняют
        ImGuiWindow* body = ImGui::GetCurrentWindow();
        const ImVec2 maxPos = body->DC.CursorMaxPos;
        drawHeader(cardsList);
        body->DC.CursorMaxPos = maxPos;
    }
    ImGui::EndChild();
    if (searched) { g_lastShown = -1; g_contentAnim = 0.3f; } // новый запрос — список снова сверху

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(4);
    g_justOpened = false;
    if (g_colorDirty && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) { // палитру закрыли — сохраняем
        Config::Save();
        g_colorDirty = false;
    }
}

// ---------------------------------------------------------------- «где эта настройка?»
namespace {
    // слово → основа: «погоды» и «погода» совпадут
    std::wstring Stem(const std::wstring& w) { return w.substr(0, std::clamp<size_t>(w.size() - 1, 3, 5)); }

    std::vector<std::wstring> Stems(const std::wstring& text) {
        static const std::wstring kSkip[] = { L"где", L"как", L"мне", L"это", L"эта", L"этот", L"там", L"тут", L"аня", L"есть", L"можно",
            L"настройка", L"настройки", L"настройку", L"настроек", L"настройках", L"настроить", L"найти", L"найди", L"покажи",
            L"поменять", L"изменить", L"включить", L"выключить", L"отключить", L"сделать", L"поставить", L"убрать", L"менять",
            L"меняется", L"пункт", L"где-то", L"пожалуйста", L"потерялся", L"потерялась", L"подскажи", L"скажи", L"чтобы", L"что" };
        std::vector<std::wstring> out;
        std::wstring word;
        auto flush = [&] {
            if (word.size() >= 3 && std::find(std::begin(kSkip), std::end(kSkip), word) == std::end(kSkip)) out.push_back(Stem(word));
            word.clear();
        };
        for (wchar_t c : Text::LowerRu(text) + L" ") {
            if (std::iswalnum(c)) word += c;
            else flush();
        }
        return out;
    }

    int Score(const Item& item, const std::vector<std::wstring>& stems) {
        const std::wstring title = Text::LowerRu(Text::ToWide(item.title)), hint = Text::LowerRu(Text::ToWide(item.hint)),
                           section = Text::LowerRu(Text::ToWide(kInfo[item.section].name));
        int score = 0;
        for (const auto& s : stems) {
            if (title.find(s) != std::wstring::npos) score += 3;
            else if (section.find(s) != std::wstring::npos) score += 2;
            else if (hint.find(s) != std::wstring::npos) score += 1;
        }
        return score;
    }

    int FindByWords(const std::string& question, int* bestScore) {
        const auto stems = Stems(Text::ToWide(question));
        int best = -1, top = 0;
        for (int i = 0; i < static_cast<int>(std::size(kItems)); ++i)
            if (const int s = Score(kItems[i], stems); s > top) { top = s; best = i; }
        if (bestScore) *bestScore = top;
        return best;
    }

    // слова не помогли — нейросеть выбирает пункт по смыслу («где сделать её прозрачной» → «Плотность фона»)
    int FindByAi(const std::string& question) {
        std::string list;
        for (int i = 0; i < static_cast<int>(std::size(kItems)); ++i)
            list += std::to_string(i + 1) + ". " + kInfo[kItems[i].section].name + " → " + kItems[i].title + ": " + kItems[i].hint + "\n";
        const std::string answer = Ai::Quiet("Вот пункты настроек программы:\n" + list + "\nПользователь спрашивает: «" + question +
            "». Какой пункт ему нужен? Ответь только номером пункта, без слов. Если ни один не подходит — ответь 0.");
        const int n = std::atoi(Text::Trim(answer).c_str());
        return n >= 1 && n <= static_cast<int>(std::size(kItems)) ? n - 1 : -1;
    }
}

std::optional<SettingsPage::Found> SettingsPage::Reveal(const std::string& question, bool askAi) {
    int score = 0;
    int index = FindByWords(question, &score);
    if (score < (askAi ? 2 : 3)) index = askAi ? FindByAi(question) : -1; // без нейросети — только если слово есть в названии
    if (index < 0) return std::nullopt;
    g_revealRequest = index;
    return Found{ kInfo[kItems[index].section].name, kItems[index].title };
}

bool SettingsPage::PollReveal() {
    const int index = g_revealRequest.exchange(-1);
    if (index < 0) return false;
    Open(kItems[index].section);
    if (IsAdvanced(kItems[index].title)) g_more[kItems[index].section] = true;
    g_query[0] = '\0';
    g_reveal = index;
    g_revealTime = 0.0f;
    g_revealScroll = true;
    g_lastShown = g_section; // прокрутку не сбрасывать наверх — сами доедем
    return true;
}
