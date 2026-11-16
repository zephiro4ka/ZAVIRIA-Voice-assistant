#pragma once
// Вкладка «Сценарии»: список сценариев слева, справа — цепочка блоков «Слушать → действия → Конец».
// Только ImGui — переносится вместе с интерфейсом.

#include "../ImGui/imgui.h"
#include "Scenarios.h"

namespace ScenariosPage {
    void Open();
    void Close();
    bool IsOpen();
    bool Covers();
    float Visibility();
    void Draw(ImVec2 screen, float top, float dt);

    // Новый сценарий, который собрала Аня (из любого потока): сохранить и показать во вкладке
    void Add(const Scenarios::Scenario& scenario);
    bool PollAdded(); // из потока интерфейса, раз в кадр (true — открыла вкладку)

    // Для DevProbe: вид холста (0 кривые Безье, 1 минимализм, 2 изломы 90°, 3 кибер) и режим «Рисую сама»
    void SetLook(int look);
    void SetDrawMode(bool on);
    bool KeepsEscape(); // Esc нужен самой странице (режим рисования, открытое меню) — не закрывать её
}
