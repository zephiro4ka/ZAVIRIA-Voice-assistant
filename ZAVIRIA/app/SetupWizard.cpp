#include "SetupWizard.h"

#include "Autostart.h"
#include "DiscordRpc.h"
#include "Platform.h"
#include "Settings.h"
#include "Telegram.h"
#include "TextUtils.h"
#include "Theme.h"
#include "Lang.h"
#include "Weather.h"
#include "Widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>

#include <shellapi.h>

using Theme::Px;

namespace {
    bool g_open = false;
    float g_anim = 0.0f;   // появление всего окна
    int g_step = 0;
    float g_stepAnim = 1.0f; // смена шага: карточка въезжает сбоку
    int g_direction = 1;
    constexpr int kSteps = 4;
    float g_contentHeight[kSteps] = {}, g_shownHeight = 0; // окно подстраивается под содержимое шага

    char g_assistant[64] = "", g_name[64] = "", g_spoken[64] = "", g_city[96] = "";
    char g_token[128] = "", g_owner[256] = "", g_discord[32] = "";
    bool g_wake = true, g_autostart = false, g_discordOn = true;

    template <size_t N> void Copy(char (&buffer)[N], const std::string& text) { snprintf(buffer, N, "%s", text.c_str()); }

    void Load() {
        const Settings s = Config::Get();
        Copy(g_assistant, s.assistant);
        Copy(g_name, s.name == "друг" ? std::string() : s.name);
        Copy(g_spoken, s.spokenName);
        Copy(g_city, Config::Value("weather", "city", ""));
        Copy(g_token, Config::Value("telegram", "token", ""));
        Copy(g_owner, Config::Value("telegram", "owner", ""));
        Copy(g_discord, Config::Value("discord", "app_id", ""));
        g_wake = s.wakeWord;
        g_autostart = Autostart::Enabled();
        g_discordOn = Config::Value("discord", "enabled", "1") == "1";
    }

    void Finish() {
        const std::string assistant = Text::Trim(g_assistant), name = Text::Trim(g_name), spoken = Text::Trim(g_spoken);
        Config::Update([&](Settings& s) {
            s.assistant = assistant.empty() ? "Аня" : assistant;
            s.name = name.empty() ? "друг" : name;
            s.spokenName = spoken;
            s.wakeWord = g_wake;
        });
        Autostart::Set(g_autostart);
        const std::string token = Text::Trim(g_token), owner = Text::Trim(g_owner), discord = Text::Trim(g_discord);
        Config::SetValue("telegram", "token", token);
        Config::SetValue("telegram", "owner", owner);
        Config::SetValue("telegram", "enabled", token.empty() || owner.empty() ? "0" : "1");
        if (!discord.empty()) Config::SetValue("discord", "app_id", discord); // пусто — встроенное приложение ZAVIRIA
        Config::SetValue("discord", "enabled", g_discordOn ? "1" : "0");
        Telegram::Reload();
        DiscordRpc::Reload();
        if (const std::string city = Text::Trim(g_city); !city.empty() && city != Config::Value("weather", "city", ""))
            std::thread([city] { if (const auto found = Weather::FindCity(city)) Weather::SetCity(*found); }).detach(); // поиск города — по сети
        Config::SetValue("anya", "setup", "1");
        g_open = false;
    }

    void Go(int step) {
        g_direction = step > g_step ? 1 : -1;
        g_step = std::clamp(step, 0, kSteps - 1);
        g_stepAnim = 0.0f;
    }

    // поле с подписью и подсказкой
    bool Field(const char* label, const char* id, char* buffer, size_t size, const char* hint, float width, ImGuiInputTextFlags flags = 0) {
        ImGui::PushFont(Theme::semibold, Theme::semibold->LegacySize);
        ImGui::TextColored(ImColor(Theme::kInkMuted), "%s", label);
        ImGui::PopFont();
        ImGui::SetNextItemWidth(width);
        const bool changed = ImGui::InputTextWithHint(id, hint, buffer, size, flags);
        ImGui::Dummy(ImVec2(0, Px(10)));
        return changed;
    }

    void ToggleRow(const char* label, const char* id, bool* value, float width) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::PushFont(Theme::semibold, Theme::semibold->LegacySize);
        ImGui::TextUnformatted(label);
        ImGui::PopFont();
        Widgets::Toggle(id, ImVec2(at.x + width - Px(44), at.y - Px(2)), value, Theme::Accent().WithAlpha(1.0f));
        ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + Px(34)));
        ImGui::Dummy(ImVec2(0, 0));
    }
}

bool SetupWizard::Needed() { return Config::Value("anya", "setup", "0") != "1"; }

void SetupWizard::Open() {
    Load();
    g_open = true;
    g_step = 0;
    g_stepAnim = 1.0f;
}

void SetupWizard::OpenAt(int step) {
    Open();
    g_step = std::clamp(step, 0, kSteps - 1);
}

bool SetupWizard::IsOpen() { return g_open || g_anim > 0.01f; }

void SetupWizard::Draw(ImVec2 screen, float top, float dt) {
    g_anim += ((g_open ? 1.0f : 0.0f) - g_anim) * Theme::Ease(dt, 6.2f);
    if (g_anim < 0.01f) return;
    g_stepAnim += (1.0f - g_stepAnim) * Theme::Ease(dt, 6.8f);
    const float ease = 1.0f - std::pow(1.0f - g_anim, 3.0f);
    const float stepEase = 1.0f - std::pow(1.0f - g_stepAnim, 3.0f);

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(screen);
    ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ease);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::Background(0).WithAlpha(0.72f));
    ImGui::Begin("##setupWizard", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float available = screen.y - top - Px(20);
    const float wanted = g_contentHeight[g_step] > 0 ? g_contentHeight[g_step] + Px(22 + 88) : Px(510);
    if (g_shownHeight <= 0) g_shownHeight = wanted;
    g_shownHeight += (wanted - g_shownHeight) * Theme::Ease(dt, 10.0f);
    const float width = std::min(Px(560), screen.x - Px(40)), height = std::min(g_shownHeight, available);
    const ImVec2 card(screen.x * 0.5f - width * 0.5f + (1.0f - stepEase) * Px(40) * g_direction,
                      std::max(top + Px(10), screen.y * 0.5f - height * 0.5f) + (1.0f - ease) * Px(24));
    const ImVec2 cardMax(card.x + width, card.y + height);
    // окно-островок как диалог Windows 11: плотная подложка (главный экран не просвечивает), «слюда» сверху, полоса кнопок снизу
    dl->AddRectFilled(card, cardMax, Theme::Background(0).WithAlpha(1.0f), Px(20));
    Theme::Plate(dl, card, cardMax, Theme::Surface(0.97f), Px(20), 0.35f);
    const float footer = Px(80);
    dl->AddRectFilled(ImVec2(card.x + Px(1), cardMax.y - footer), ImVec2(cardMax.x - Px(1), cardMax.y - Px(1)), Theme::Paper() ? Theme::Overlay(8) : IM_COL32(0, 0, 0, 46), Px(19), ImDrawFlags_RoundCornersBottom);
    dl->AddLine(ImVec2(card.x + Px(1), cardMax.y - footer), ImVec2(cardMax.x - Px(1), cardMax.y - footer), Theme::Overlay(16), Px(1));

    // шаги — короткие полоски, текущая длиннее и цветом логотипа
    {
        static float s_pill[kSteps] = {};
        float x = cardMax.x - Px(32);
        for (int i = kSteps - 1; i >= 0; --i) {
            s_pill[i] += ((i == g_step ? 1.0f : 0.0f) - s_pill[i]) * Theme::Ease(dt, 10.0f);
            const float w = Px(8) + Px(16) * s_pill[i];
            const ImVec2 a(x - w, card.y + Px(31)), b(x, card.y + Px(37));
            dl->AddRectFilled(a, b, i <= g_step ? Theme::Accent().Mix({ 244, 114, 182 }, 0.3f * s_pill[i]).WithAlpha(1.0f) : Theme::Overlay(40), Px(3));
            x -= w + Px(6);
        }
    }

    const float pad = Px(32), inner = width - pad * 2;
    ImGui::SetCursorScreenPos(ImVec2(card.x + pad, card.y + Px(22)));
    ImGui::PushID(g_step);
    const ImGuiWindowFlags bodyFlags = ImGuiWindowFlags_NoBackground | (wanted > available ? 0 : ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::BeginChild("##body", ImVec2(inner, std::max(Px(40), height - Px(22 + 76))), ImGuiChildFlags_None, bodyFlags);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ease * (0.3f + 0.7f * stepEase));
    static const char* kTitles[] = { "Давай познакомимся", "Погода и запуск", "Телефон и Discord", "Всё готово" };
    ImGui::PushFont(Theme::hero, Theme::hero->LegacySize);
    ImGui::TextUnformatted(kTitles[g_step]);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, Px(6)));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
    ImGui::PushFont(Theme::semibold, Theme::semibold->LegacySize);
    switch (g_step) {
    case 0:
        ImGui::TextDisabled("Как будут звать помощницу и как ей обращаться к тебе. Имя слышно в голосе, в окне, в Discord и телеграме.");
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, Px(12)));
        if (Field("Имя помощницы — на него она отзывается", "##wAssistant", g_assistant, sizeof(g_assistant), "Аня", inner)) {
            const std::string typed = Text::Trim(g_assistant); // имя в шапке окна меняется сразу, пока печатаешь
            Config::Update([&](Settings& x) { x.assistant = typed.empty() ? "Аня" : typed; }, false);
        }
        Field("Как тебя зовут", "##wName", g_name, sizeof(g_name), "например, Саша", inner);
        Field("Как произносить твоё имя вслух (если это ник)", "##wSpoken", g_spoken, sizeof(g_spoken), "необязательно", inner);
        { // оформление с первого запуска: светлая или тёмная, как в Windows (атмосферы — потом в настройках)
            ImGui::Dummy(ImVec2(0, Px(6)));
            ImGui::PushFont(Theme::semibold, Theme::semibold->LegacySize);
            ImGui::TextUnformatted(Lang::Tr("Оформление"));
            ImGui::PopFont();
            static const char* kModes[] = { "тёмная", "светлая", "как в Windows" };
            const int mode = Theme::Mode();
            for (int i = 0; i < 3; ++i) {
                if (i) ImGui::SameLine(0, Px(8));
                if (Widgets::Radio(kModes[i], mode == i)) Theme::SetMode(i);
            }
        }
        break;
    case 1:
        ImGui::TextDisabled("Для прогноза погоды и виджета на главном экране.");
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, Px(12)));
        Field("Твой город", "##wCity", g_city, sizeof(g_city), "например, Москва", inner);
        ImGui::Dummy(ImVec2(0, Px(4)));
        ToggleRow(("Слушать всегда — ждать слово «" + (Text::Trim(g_assistant).empty() ? std::string("Аня") : Text::Trim(g_assistant)) + "»").c_str(),
                  "##wWake", &g_wake, inner);
        ToggleRow("Запускать вместе с Windows (тихо, в трее)", "##wAuto", &g_autostart, inner);
        break;
    case 2:
        ImGui::TextDisabled("Это необязательно! Не знаешь, что это, — смело жми «пропустить». Подробная инструкция по шагам есть в настройках, раздел «Безопасность и связь».");
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, Px(6)));
        if (ImGui::SmallButton("  сделать бота  ")) ShellExecuteW(nullptr, L"open", L"https://t.me/BotFather", nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::SameLine();
        if (ImGui::SmallButton("  узнать свой ID  ")) ShellExecuteW(nullptr, L"open", L"https://t.me/userinfobot", nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::SameLine();
        ImGui::NewLine();
        ImGui::Dummy(ImVec2(0, Px(8)));
        Field("Токен телеграм-бота", "##wToken", g_token, sizeof(g_token), "123456:ABC… от @BotFather", inner, ImGuiInputTextFlags_Password);
        Field("Твой ID в телеграме — бот слушается только тебя", "##wOwner", g_owner, sizeof(g_owner), "например: 1234567890, 0987654321", inner);
        ToggleRow("Показывать активность в Discord", "##wDiscordOn", &g_discordOn, inner); // встроенное приложение ZAVIRIA — ничего вводить не нужно
        break;
    default: {
        const std::string me = Text::Trim(g_assistant).empty() ? "Аня" : Text::Trim(g_assistant);
        ImGui::TextUnformatted(("Скажи «" + me + "» и что нужно — или нажми на сферу. Например:").c_str());
        ImGui::Dummy(ImVec2(0, Px(8)));
        for (const char* example : { "«…, открой Steam»", "«…, какая погода завтра?»", "«…, создай сценарий уютный вечер»", "«…, где настройка голоса?»" }) {
            std::string line = example;
            line.replace(line.find("…"), std::string("…").size(), me);
            ImGui::BulletText("%s", line.c_str());
        }
        ImGui::Dummy(ImVec2(0, Px(8)));
        ImGui::TextDisabled("Всё это можно поменять в настройках, а блоки на главном экране — двигать в «Полной кастомизации».");
        ImGui::PopFont();
        break;
    }
    }
    ImGui::PopTextWrapPos();
    ImGui::PopStyleVar();
    g_contentHeight[g_step] = ImGui::GetCursorPosY();
    ImGui::EndChild();
    ImGui::PopID();

    // кнопки внизу
    const float buttonsY = cardMax.y - Px(40) - ImGui::GetFrameHeight() * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(card.x + pad, buttonsY));
    if (g_step > 0) {
        if (ImGui::Button("  назад  ")) Go(g_step - 1);
    } else if (ImGui::Button("пропустить")) {
        Config::Save(); // что успел вписать (имя помощницы) — остаётся, остальное по умолчанию
        Config::SetValue("anya", "setup", "1");
        g_open = false;
    }
    const char* next = g_step == kSteps - 1 ? "  начать  " : g_step == 2 && !g_token[0] && !g_discord[0] ? "  пропустить  " : "  далее  ";
    const float nextW = ImGui::CalcTextSize(next).x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetCursorScreenPos(ImVec2(cardMax.x - pad - nextW, buttonsY));
    ImGui::PushStyleColor(ImGuiCol_Button, Theme::Accent().WithAlpha(1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Accent().Mix({ 255, 255, 255 }, 0.12f).WithAlpha(1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::Accent().Mix({ 0, 0, 0 }, 0.1f).WithAlpha(1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
    if (ImGui::Button(next) || (ImGui::IsKeyPressed(ImGuiKey_Enter) && g_step != 2)) {
        if (g_step == kSteps - 1) Finish();
        else Go(g_step + 1);
    }
    ImGui::PopStyleColor(4);

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}
