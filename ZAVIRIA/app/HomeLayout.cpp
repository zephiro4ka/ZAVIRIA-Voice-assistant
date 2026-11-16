#include "HomeLayout.h"
#include "Tour.h"

#include "Lang.h"
#include "Settings.h"
#include "Theme.h"
#include "Widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

using Theme::Px;

namespace {
    struct Entry {
        float x = 0, y = 0, s = 1;     // куда хотят (x, y — доли окна)
        float shownX = 0, shownY = 0, shownS = 1; // где нарисован сейчас — плавно едет к цели
        // где блок был в прошлом кадре и где ему можно стоять — чтобы после смены размера окна он не уехал за край
        bool seen = false;
        int frame = -1; // сглаживаем раз в кадр, сколько бы раз ни спросили
        ImVec2 boxMin, boxMax, boxShift, areaMin, areaMax, lastShift;
    };
    std::unordered_map<std::string, Entry> g_entries;
    bool g_editing = false;

    bool g_hasArea = false;
    ImVec2 g_areaMin, g_areaMax;

    struct Box { std::string id, label; ImVec2 min, max; int resize; ImVec2 areaMin, areaMax; };
    std::vector<Box> g_boxes, g_lastBoxes;

    struct Child { std::string parent; int index; ImVec2 min, max; HomeLayout::ChildHooks hooks; };
    std::vector<Child> g_children;
    struct ChildDrag { std::string parent; int index = -1; ImVec2 mouse; bool moved = false; };
    ChildDrag g_childDrag;
    std::string g_menuParent;  // для какой части открыто меню
    int g_menuIndex = -1;
    HomeLayout::ChildHooks g_menuHooks{};

    struct Drag { std::string id; bool resize = false; ImVec2 mouse; float x = 0, y = 0, s = 1; ImVec2 min, max; };
    Drag g_drag;

    std::string Key(const std::string& id, const char* what) { return id + "_" + what; }

    // чья это страница: по приставке имени
    std::string PageOf(const std::string& id) {
        for (const char* prefix : { "proc_", "set_" })
            if (id.rfind(prefix, 0) == 0) return prefix;
        return "";
    }

    Entry& Get(const std::string& id) {
        auto [it, fresh] = g_entries.try_emplace(id);
        if (fresh) {
            Entry& e = it->second;
            e.x = std::atoi(Config::Value("layout", Key(id, "x").c_str(), "0").c_str()) / 10000.0f;
            e.y = std::atoi(Config::Value("layout", Key(id, "y").c_str(), "0").c_str()) / 10000.0f;
            e.s = std::clamp(std::atoi(Config::Value("layout", Key(id, "s").c_str(), "1000").c_str()) / 1000.0f, 0.4f, 2.5f);
            e.shownX = e.x, e.shownY = e.y, e.shownS = e.s;
        }
        return it->second;
    }

    void Store(const std::string& id, const Entry& e) {
        Config::SetValue("layout", Key(id, "x").c_str(), std::to_string(std::lround(e.x * 10000.0f)));
        Config::SetValue("layout", Key(id, "y").c_str(), std::to_string(std::lround(e.y * 10000.0f)));
        Config::SetValue("layout", Key(id, "s").c_str(), std::to_string(std::lround(e.s * 1000.0f)));
    }

    void Smooth(Entry& e) {
        const float k = Theme::Ease(ImGui::GetIO().DeltaTime, 8.7f);
        e.shownX += (e.x - e.shownX) * k, e.shownY += (e.y - e.shownY) * k, e.shownS += (e.s - e.shownS) * k;
        if (std::fabs(e.x - e.shownX) < 1e-4f) e.shownX = e.x;
        if (std::fabs(e.y - e.shownY) < 1e-4f) e.shownY = e.y;
        if (std::fabs(e.s - e.shownS) < 1e-4f) e.shownS = e.s;
    }

    // сдвиг, при котором отрезок [base + shift, base + shift + size] лежит внутри [lo, hi]; не влезает — прижат к началу
    float Fit(float shift, float base, float size, float lo, float hi) {
        if (size >= hi - lo) return lo - base;
        return std::clamp(shift, lo - base, hi - size - base);
    }

    void MenuBody() {
        bool on = HomeLayout::Editing();
        if (Widgets::Check("Редактирование", &on)) { HomeLayout::SetEditing(on); ImGui::CloseCurrentPopup(); }
        ImGui::TextDisabled("%s", Lang::Tr("двигать и растягивать блоки страницы"));
    }

    bool FreeRightClick() {
        return ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !ImGui::IsAnyItemHovered() &&
            !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    }
}

bool HomeLayout::Editing() { return g_editing; }

void HomeLayout::SetEditing(bool on) {
    g_editing = on;
    g_drag = {};
    g_childDrag = {};
}

ImVec2 HomeLayout::Shift(const char* id, ImVec2 screen) {
    Entry& e = Get(id);
    if (e.frame != ImGui::GetFrameCount()) { e.frame = ImGui::GetFrameCount(); Smooth(e); }
    ImVec2 s(e.shownX * screen.x, e.shownY * screen.y);
    if (e.seen) { // окно стало меньше или крупнее масштаб — подвинуть блок обратно в окно (сохранённое место не трогаем)
        const ImVec2 base(e.boxMin.x - e.boxShift.x, e.boxMin.y - e.boxShift.y), size(e.boxMax.x - e.boxMin.x, e.boxMax.y - e.boxMin.y);
        s.x = Fit(s.x, base.x, size.x, e.areaMin.x, e.areaMax.x);
        s.y = Fit(s.y, base.y, size.y, e.areaMin.y, e.areaMax.y);
    }
    e.lastShift = s;
    return s;
}

float HomeLayout::Scale(const char* id) { return Get(id).shownS; }

bool HomeLayout::Moved(const char* id) {
    const Entry& e = Get(id);
    return e.x != 0.0f || e.y != 0.0f || e.s != 1.0f;
}

void HomeLayout::SetArea(ImVec2 min, ImVec2 max) { g_hasArea = true, g_areaMin = min, g_areaMax = max; }
void HomeLayout::ClearArea() { g_hasArea = false; }

void HomeLayout::Report(const char* id, const char* label, ImVec2 min, ImVec2 max, int resize) {
    Tour::Mark(id, min, max);
    Entry& e = Get(id);
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    e.seen = true;
    e.boxMin = min, e.boxMax = max, e.boxShift = e.lastShift;
    e.areaMin = g_hasArea ? g_areaMin : ImVec2(0, 0);
    e.areaMax = g_hasArea ? g_areaMax : screen;
    if (Editing()) g_boxes.push_back({ id, label, min, max, resize, e.areaMin, e.areaMax });
}

void HomeLayout::ReportChild(const char* parent, int index, ImVec2 min, ImVec2 max, ChildHooks hooks) {
    if (Editing()) g_children.push_back({ parent, index, min, max, hooks });
}

bool HomeLayout::HitsBlock(float x, float y) {
    if (!Editing()) return false;
    for (const Box& b : g_lastBoxes)
        if (x >= b.min.x && x <= b.max.x && y >= b.min.y && y <= b.max.y) return true;
    return false;
}

void HomeLayout::Reset(const char* page) {
    for (auto& [id, e] : g_entries) {
        if (PageOf(id) != page) continue;
        e.x = e.y = 0.0f;
        e.s = 1.0f;
        Store(id, e);
    }
    g_drag = {};
}

const std::vector<std::string>& HomeLayout::Ids() {
    static const std::vector<std::string> ids = [] {
        std::vector<std::string> v = { "nav", "history", "orb", "dock", "weather", "widgets", "footer",
            "proc_title", "proc_tools", "proc_metrics", "proc_memory", "proc_list", "set_title", "set_search", "set_side" };
        for (int i = 0; i < 16; ++i) v.push_back("set_card_" + std::to_string(i));
        return v;
    }();
    return ids;
}

void HomeLayout::Reload() {
    g_entries.clear();
    g_drag = {};
}

void HomeLayout::ContextMenu(bool allowed) {
    if (allowed && !Editing() && FreeRightClick() && ImGui::GetIO().MousePos.y > 0) ImGui::OpenPopup("##editMenu");
    if (Widgets::BeginPopupAnimated("##editMenu")) {
        MenuBody();
        Widgets::EndPopupAnimated();
    }
}

void HomeLayout::DrawEditor(ImVec2 screen, float top, const char* page) {
    std::vector<Box> all;
    all.swap(g_boxes);
    std::vector<Child> children;
    children.swap(g_children);
    if (!page) { // на этой странице править нечего (там правый клик занят своим)
        if (Editing()) SetEditing(false);
        g_lastBoxes.clear();
        return;
    }
    // только блоки открытой страницы (страница ещё выезжает — блоки главной уже не трогаем); меню в шапке — на всех
    std::vector<Box> boxes;
    for (Box& b : all)
        if (PageOf(b.id) == page || b.id == "nav") boxes.push_back(std::move(b));
    if (*page) children.erase(std::remove_if(children.begin(), children.end(), [](const Child& c) { return c.parent != "nav"; }), children.end());
    g_lastBoxes = boxes;

    static float s_anim = 0.0f;
    s_anim += ((Editing() ? 1.0f : 0.0f) - s_anim) * Theme::Ease(ImGui::GetIO().DeltaTime, 6.2f);
    if (!Editing() || s_anim < 0.01f) { if (!Editing()) g_drag = {}; return; }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) { SetEditing(false); return; }
    const float ease = 1.0f - std::pow(1.0f - s_anim, 3.0f);

    ImGui::SetNextWindowPos(ImVec2(0, 0)); // с самого верха — меню в шапке тоже можно двигать
    ImGui::SetNextWindowSize(screen);
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) ImGui::SetNextWindowFocus(); // поверх всей страницы, пока правим
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##layoutEditor", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    dl->AddRectFilled(ImVec2(0, top), screen, IM_COL32(0, 0, 0, static_cast<int>(70 * ease))); // лёгкое затемнение: видно, что это режим правки

    // панель «что происходит и как выйти» — первой: мышь в ImGui достаётся тому, кто объявлен раньше, а блоки могут оказаться под ней.
    // Рисуем её в верхнем слое, рамки блоков — в нижнем
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    {
        const char* hint = Lang::Tr("Редактирование: тяни блок мышью, уголок — размер");
        const char* resetLabel = Lang::Tr("вернуть как было");
        const char* doneLabel = Lang::Tr("готово");
        const ImGuiStyle& style = ImGui::GetStyle();
        const float hintW = Widgets::TextSize(Theme::semibold, hint).x;
        const float resetW = ImGui::CalcTextSize(resetLabel).x + style.FramePadding.x * 2, doneW = ImGui::CalcTextSize(doneLabel).x + style.FramePadding.x * 2 + Px(24);
        const bool narrow = hintW + resetW + doneW + Px(70) > screen.x - Px(24); // узкое окно — без подсказки
        const float barW = (narrow ? 0.0f : hintW + Px(12)) + resetW + doneW + Px(48), barH = Px(46);
        // сверху, а если там блоки — снизу или посередине: где панель закрывает меньше всего
        const float x0 = std::round(screen.x * 0.5f - barW * 0.5f);
        const float spots[] = { top + Px(40), screen.y - barH - Px(56), (top + screen.y - barH) * 0.5f };
        float barY = spots[0], least = 1e30f;
        for (float y : spots) {
            float covered = 0.0f;
            for (const Box& b : boxes) {
                const float w = std::min(b.max.x, x0 + barW) - std::max(b.min.x, x0), h = std::min(b.max.y, y + barH) - std::max(b.min.y, y);
                if (w > 0 && h > 0) covered += w * h;
            }
            if (covered < least - 1.0f) least = covered, barY = y;
        }
        static float s_barY = -1.0f; // переезжает плавно, а не скачком
        if (s_barY < 0) s_barY = barY;
        else if (g_drag.id.empty()) s_barY += (barY - s_barY) * Theme::Ease(io.DeltaTime, 6.0f); // пока тянут блок — панель стоит на месте
        const ImVec2 bar(x0, std::round(s_barY - (1.0f - ease) * Px(16)));
        Theme::Plate(dl, bar, ImVec2(bar.x + barW, bar.y + barH), Theme::Surface(0.96f), barH * 0.5f);
        dl->AddRect(bar, ImVec2(bar.x + barW, bar.y + barH), Theme::Accent().WithAlpha(0.5f * ease), barH * 0.5f, 0, Px(1.2f));
        float x = bar.x + Px(20);
        if (!narrow) {
            Widgets::Text(dl, Theme::semibold, ImVec2(x, bar.y + (barH - Theme::semibold->LegacySize) * 0.5f), Theme::kInk, hint);
            x += hintW + Px(12);
        }
        ImGui::SetCursorScreenPos(ImVec2(x, bar.y + (barH - ImGui::GetFrameHeight()) * 0.5f));
        if (ImGui::Button(resetLabel)) Reset(page);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, Theme::Accent().WithAlpha(0.9f));
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(18, 11, 29, 255));
        if (ImGui::Button(doneLabel, ImVec2(doneW, 0))) SetEditing(false);
        ImGui::PopStyleColor(2);
    }
    dl->ChannelsSetCurrent(0);

    // части внутри блоков — раньше блоков: рамка блока их накрывает
    for (const Child& c : children) {
        ImGui::PushID(c.parent.c_str());
        ImGui::PushID(c.index);
        ImGui::SetCursorScreenPos(c.min);
        ImGui::InvisibleButton("##child", ImVec2(std::max(1.0f, c.max.x - c.min.x), std::max(1.0f, c.max.y - c.min.y)),
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) g_childDrag = { c.parent, c.index, io.MousePos, false };
        const bool dragging = g_childDrag.parent == c.parent && g_childDrag.index == c.index; // и в кадр отпускания — тогда и переставляем
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            g_menuParent = c.parent, g_menuIndex = c.index, g_menuHooks = c.hooks;
            ImGui::OpenPopup("##childMenu");
        }
        ImGui::PopID();
        ImGui::PopID();
        const float glow = dragging ? 0.35f : hovered ? 0.2f : 0.0f;
        if (glow > 0) dl->AddRectFilled(c.min, c.max, Theme::Accent().WithAlpha(glow * ease), (c.max.y - c.min.y) * 0.5f);
        dl->AddRect(c.min, c.max, Theme::Accent().WithAlpha((hovered || dragging ? 0.8f : 0.35f) * ease), (c.max.y - c.min.y) * 0.5f, 0, Px(1));
        if (hovered && !dragging) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (dragging) {
            // куда встанет: ближайший сосед по горизонтали
            int target = c.index;
            float best = 1e9f;
            for (const Child& o : children) {
                if (o.parent != c.parent) continue;
                const float dist = std::fabs((o.min.x + o.max.x) * 0.5f - io.MousePos.x);
                if (dist < best) best = dist, target = o.index;
            }
            const bool travelled = std::fabs(io.MousePos.x - g_childDrag.mouse.x) + std::fabs(io.MousePos.y - g_childDrag.mouse.y) > Px(6);
            if (travelled) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            for (const Child& o : children)
                if (o.parent == c.parent && o.index == target && target != c.index) {
                    const float x = target > c.index ? o.max.x : o.min.x;
                    dl->AddLine(ImVec2(x, o.min.y - Px(3)), ImVec2(x, o.max.y + Px(3)), Theme::Accent().WithAlpha(1.0f), Px(3)); // сюда встанет
                }
            g_childDrag.moved = g_childDrag.moved || travelled;
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                if (target != c.index && c.hooks.move) c.hooks.move(c.index, target);
                else if (!g_childDrag.moved && hovered && c.hooks.click) c.hooks.click(c.index); // просто нажали — открыть вкладку
            }
        }
    }
    bool snappedCenter = false;
    for (const Box& box : boxes) {
        Entry& e = Get(box.id);
        const bool dragged = g_drag.id == box.id;
        ImGui::PushID(box.id.c_str());
        ImGui::SetCursorScreenPos(box.min);
        ImGui::InvisibleButton("##box", ImVec2(std::max(1.0f, box.max.x - box.min.x), std::max(1.0f, box.max.y - box.min.y)));
        const bool hovered = ImGui::IsItemHovered();
        const float handle = Px(18);
        const bool nearCorner = box.resize != HomeLayout::kFixed && io.MousePos.x > box.max.x - handle && io.MousePos.y > box.max.y - handle;
        if (ImGui::IsItemActivated()) g_drag = { box.id, nearCorner, io.MousePos, e.x, e.y, e.s, box.min, box.max };
        if (dragged && ImGui::IsItemActive()) {
            ImVec2 d(io.MousePos.x - g_drag.mouse.x, io.MousePos.y - g_drag.mouse.y);
            const float w = std::max(1.0f, g_drag.max.x - g_drag.min.x), h = std::max(1.0f, g_drag.max.y - g_drag.min.y);
            const float areaW = box.areaMax.x - box.areaMin.x, areaH = box.areaMax.y - box.areaMin.y;
            if (g_drag.resize && box.resize == HomeLayout::kWidth) {
                const float most = std::max(g_drag.s, g_drag.s * (areaW - Px(8)) / w); // шире области не тянем
                e.s = std::clamp(g_drag.s * (w + d.x) / w, 0.5f, std::min(2.2f, most));
            } else if (g_drag.resize) {
                const float most = std::max(g_drag.s, g_drag.s * std::min(areaW / w, areaH / h));
                e.s = std::clamp(g_drag.s * (w + h + d.x + d.y) / (w + h), 0.5f, std::min(2.2f, most));
            } else {
                // не уводить за край области (блок крупнее области — его верх и левый край остаются в ней)
                d.x = Fit(d.x, g_drag.min.x, w, box.areaMin.x, box.areaMax.x);
                d.y = Fit(d.y, g_drag.min.y, h, box.areaMin.y, box.areaMax.y);
                const float center = (g_drag.min.x + g_drag.max.x) * 0.5f + d.x;
                if (std::fabs(center - screen.x * 0.5f) < Px(10)) { d.x += screen.x * 0.5f - center; snappedCenter = true; } // прилипает к середине
                e.x = g_drag.x + d.x / screen.x;
                e.y = g_drag.y + d.y / screen.y;
            }
            e.shownX = e.x, e.shownY = e.y, e.shownS = e.s; // пока тянешь — без задержки
        }
        if (dragged && ImGui::IsItemDeactivated()) { Store(box.id, e); g_drag = {}; }
        if (hovered || dragged)
            ImGui::SetMouseCursor(nearCorner || (dragged && g_drag.resize) ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeAll);
        ImGui::PopID();

        const ImU32 accent = Theme::Accent().WithAlpha((dragged ? 0.95f : hovered ? 0.8f : 0.5f) * ease);
        dl->AddRectFilled(box.min, box.max, Theme::Accent().WithAlpha((dragged ? 0.16f : hovered ? 0.1f : 0.05f) * ease), Px(16));
        dl->AddRect(box.min, box.max, accent, Px(16), 0, Px(dragged ? 2.0f : 1.4f));
        // подпись блока
        const char* labelText = Lang::Tr(box.label.c_str());
        const ImVec2 label = Widgets::TextSize(Theme::semibold, labelText);
        const bool small = box.max.y - box.min.y < Px(70); // в низком блоке подпись не помещается — под ним (или над ним у нижнего края)
        float chipY = small ? box.max.y + Px(4) : box.min.y + Px(8);
        if (small && chipY + label.y + Px(8) > screen.y) chipY = box.min.y - label.y - Px(12);
        const ImVec2 chip(box.min.x + Px(10), chipY);
        dl->AddRectFilled(chip, ImVec2(chip.x + label.x + Px(16), chip.y + label.y + Px(8)), Theme::Accent().WithAlpha(0.85f * ease), Px(8));
        Widgets::Text(dl, Theme::semibold, ImVec2(chip.x + Px(8), chip.y + Px(4)), IM_COL32(255, 255, 255, static_cast<int>(255 * ease)), labelText);
        if (box.resize != HomeLayout::kFixed) { // уголок для размера
            for (int i = 1; i <= 3; ++i)
                dl->AddLine(ImVec2(box.max.x - Px(5.0f * i) - Px(3), box.max.y - Px(3)), ImVec2(box.max.x - Px(3), box.max.y - Px(5.0f * i) - Px(3)), accent, Px(1.6f));
            if (dragged && g_drag.resize) {
                char text[32];
                snprintf(text, sizeof(text), "%d%%", static_cast<int>(std::lround(e.s * 100.0f)));
                Widgets::Text(dl, Theme::semibold, ImVec2(box.max.x - Px(54), box.max.y - Px(30)), IM_COL32(255, 255, 255, 230), text);
            }
        }
    }
    dl->ChannelsMerge();
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) g_childDrag = {};
    if (Widgets::BeginPopupAnimated("##childMenu")) {
        if (g_menuHooks.menu && g_menuIndex >= 0) g_menuHooks.menu(g_menuIndex);
        Widgets::EndPopupAnimated();
    }
    // правый клик мимо блоков — то же меню, что и вне режима: им же и выйти
    if (FreeRightClick() && ImGui::IsWindowHovered()) ImGui::OpenPopup("##editMenu");
    if (Widgets::BeginPopupAnimated("##editMenu")) {
        MenuBody();
        Widgets::EndPopupAnimated();
    }

    if (snappedCenter) dl->AddLine(ImVec2(screen.x * 0.5f, top), ImVec2(screen.x * 0.5f, screen.y), Theme::Accent().WithAlpha(0.5f), Px(1));
    ImGui::End();
}
