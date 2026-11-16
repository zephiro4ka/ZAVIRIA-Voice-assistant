// Окна режима экономии: «что будет ограничено» (перед первым включением и по кнопке в настройках)
// и итог выключения со списком закрытых программ. Только ImGui — переносится вместе с интерфейсом.
#include "EcoMode.h"

#include "Lang.h"
#include "SettingsPage.h"
#include "Theme.h"
#include "Widgets.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <thread>

using Theme::Px;

namespace {
    enum class View { None, Preview, Report };
    View g_view = View::None;
    bool g_openNow = false;
    bool g_closeNow = false;
    float g_spin = 0.0f;

    std::string Join(const std::vector<std::string>& names) {
        std::string out;
        for (const auto& name : names) { if (!out.empty()) out += ", "; out += name; }
        return out;
    }

    void Wrapped(const char* text, ImU32 color) {
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }

    // Заголовок пункта жирным, пояснение — приглушённо
    void Row(const char* title, const std::string& detail) {
        ImGui::Dummy(ImVec2(0, Px(4)));
        ImGui::PushFont(Theme::semibold);
        Wrapped(Lang::Tr(title), Theme::kInk);
        ImGui::PopFont();
        Wrapped(detail.c_str(), Theme::kInkMuted);
    }

    bool AccentButton(const char* label, ImVec2 size) {
        ImGui::PushStyleColor(ImGuiCol_Button, Theme::Accent().WithAlpha(0.85f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Accent().Mix({ 255, 255, 255 }, 0.2f).WithAlpha(1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::Accent().WithAlpha(1.0f));
        // тёмный любимый цвет — светлый текст, светлый — тёмный
        const Theme::Color c = Theme::Accent();
        const float luma = c.r * 0.299f + c.g * 0.587f + c.b * 0.114f;
        const bool light = luma > 150.0f; // каналы 0…255
        ImGui::PushStyleColor(ImGuiCol_Text, light ? IM_COL32(18, 11, 29, 255) : IM_COL32(255, 255, 255, 245));
        const bool pressed = ImGui::Button(label, size);
        ImGui::PopStyleColor(4);
        return pressed;
    }

    void DrawPreview(float height) {
        const auto p = EcoMode::GetPreview();
        ImGui::PushFont(Theme::title);
        ImGui::TextUnformatted(Lang::Tr("Игровой режим"));
        ImGui::PopFont();
        Wrapped(Lang::Tr("Пока режим включён, все ресурсы уходят игре. Вот что я сделаю — выключишь режим, и всё вернётся как было."), Theme::kInkMuted);
        ImGui::Dummy(ImVec2(0, Px(6)));

        const float buttonsH = Px(54);
        ImGui::BeginChild("##ecoList", ImVec2(0, height - ImGui::GetCursorPosY() - buttonsH - Px(18)), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollWithMouse);
        Widgets::SmoothScroll();
        if (!p.ready) {
            g_spin += ImGui::GetIO().DeltaTime;
            const char* dots[] = { "", ".", "..", "..." };
            Wrapped((std::string(Lang::Tr("смотрю, что сейчас запущено")) + dots[static_cast<int>(g_spin * 3) % 4]).c_str(), Theme::kInkMuted);
        } else {
            const bool en = Lang::IsEnglish();
            Row("Питание", p.powerScheme ? Lang::Tr("Схема «Высокая производительность» (или режим «Максимальная производительность»), процессору — полная частота.")
                                         : Lang::Tr("Windows не даёт поменять режим питания — оставлю как есть."));
            std::string brightness;
            if (p.brightness < 0) brightness = Lang::Tr("У этого экрана нет управления яркостью — не трогаю.");
            else if (p.brightness <= p.targetBrightness) brightness = en ? "Already " + std::to_string(p.brightness) + "% — I'll leave it." : "Уже " + std::to_string(p.brightness) + "% — оставлю.";
            else brightness = en ? "From " + std::to_string(p.brightness) + "% to " + std::to_string(p.targetBrightness) + "%."
                                 : "С " + std::to_string(p.brightness) + "% до " + std::to_string(p.targetBrightness) + "%.";
            Row("Игра", Lang::Tr("Окну на весь экран — высокий приоритет и полная частота процессора; переключишься на другую игру — перейдёт к ней."));
            Row("Сама ZAVIRIA", Lang::Tr("Фон замрёт, оверлеи спрячутся, нейросеть выгрузится из видеокарты (на вопрос загрузится снова), опросы железа — реже."));
            const std::string none = Lang::Tr("ничего");
            Row("Замедлю: EcoQoS и низкий приоритет", p.throttle.empty() ? none : (std::to_string(p.throttle.size()) + " — " + Join(p.throttle)));
            Row("Приостановлю до выключения режима", p.suspend.empty() ? none : Join(p.suspend));
            Row("Закрою (покажу список, запустить снова — кнопкой)", p.close.empty() ? none : Join(p.close));
            Row("Не трогаю", Lang::Tr("Службы и всё из папки Windows, саму ZAVIRIA, окно впереди и открытые на экране окна, программы, у которых играет звук, и белый список."));
        }
        ImGui::EndChild();

        ImGui::Dummy(ImVec2(0, Px(6)));
        const ImVec2 button(Px(150), Px(40));
        if (AccentButton(Lang::Tr(EcoMode::Active() ? "Уже включён" : "Включить"), button) && !EcoMode::Active()) {
            EcoMode::MarkPreviewSeen();
            std::thread([] { EcoMode::Enable(); }).detach();
            ImGui::CloseCurrentPopup();
            g_view = View::None;
        }
        ImGui::SameLine();
        if (ImGui::Button(Lang::Tr("Настроить списки"), ImVec2(Px(170), button.y))) {
            EcoMode::MarkPreviewSeen();
            SettingsPage::Reveal("Не трогать в игровом режиме", false);
            ImGui::CloseCurrentPopup();
            g_view = View::None;
        }
        ImGui::SameLine();
        if (ImGui::Button(Lang::Tr("Отмена"), ImVec2(Px(110), button.y)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
            g_view = View::None;
        }
    }

    void DrawReport(float height) {
        const auto r = EcoMode::LastReport();
        ImGui::PushFont(Theme::title);
        ImGui::TextUnformatted(Lang::Tr("Игровой режим выключен"));
        ImGui::PopFont();
        Wrapped(Lang::Tr("Приоритеты и режим питания вернула, приостановленные программы снова работают."), Theme::kInkMuted);
        ImGui::Dummy(ImVec2(0, Px(6)));
        const float buttonsH = Px(54);
        ImGui::BeginChild("##ecoClosed", ImVec2(0, height - ImGui::GetCursorPosY() - buttonsH - Px(18)));
        Row("Закрыты во время игрового режима", r.closedNames.empty() ? std::string(Lang::Tr("ничего")) : Join(r.closedNames));
        Wrapped(Lang::Tr("Сама я их не запускаю — нажми кнопку, если они нужны."), Theme::kInkMuted);
        ImGui::EndChild();
        ImGui::Dummy(ImVec2(0, Px(6)));
        if (!r.closedNames.empty() && AccentButton(Lang::Tr("Запустить снова"), ImVec2(Px(170), Px(40)))) {
            std::thread([] { EcoMode::RelaunchClosed(); }).detach();
            ImGui::CloseCurrentPopup();
            g_view = View::None;
            return;
        }
        ImGui::SameLine();
        if (ImGui::Button(Lang::Tr("Не нужно"), ImVec2(Px(130), Px(40))) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            EcoMode::DismissReport();
            ImGui::CloseCurrentPopup();
            g_view = View::None;
        }
    }
}

void EcoMode::OpenPreview() {
    StartPreview();
    g_view = View::Preview;
    g_openNow = true;
}

void EcoMode::ClosePopup() { if (g_view != View::None) g_closeNow = true; }

void EcoMode::RequestToggle() {
    if (Busy() || !Active()) return; // игровой режим убран: можно только выключить, если остался включённым
    if (false) { OpenPreview(); return; } // первый раз — сначала показать, что будет
    std::thread([] { Toggle(); }).detach();
}

void EcoMode::Draw(float screenW, float screenH, float) {
    if (g_view == View::None && !LastReport().shown) { g_view = View::Report; g_openNow = true; } // выключили — показать, что закрывали
    if (g_view == View::None) return;
    if (g_openNow) { ImGui::OpenPopup("##ecoMode"); g_openNow = false; }

    const float width = std::min(Px(640), screenW - Px(60));
    const float height = g_view == View::Report ? std::min(Px(320), screenH - Px(60)) : std::min(Px(600), screenH - Px(60));
    ImGui::SetNextWindowPos(ImVec2(std::round((screenW - width) * 0.5f), std::round((screenH - height) * 0.5f)));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Px(22));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(26), Px(22)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, Theme::Surface(1.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, Theme::Accent().WithAlpha(0.45f));
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, IM_COL32(6, 3, 12, 150));
    if (ImGui::BeginPopupModal("##ecoMode", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        if (g_view == View::Preview) DrawPreview(height);
        else DrawReport(height);
        if (g_closeNow) { g_closeNow = false; EcoMode::DismissReport(); ImGui::CloseCurrentPopup(); g_view = View::None; }
        ImGui::EndPopup();
    } else {
        g_view = View::None; // закрыли чем-то ещё
    }
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
}
