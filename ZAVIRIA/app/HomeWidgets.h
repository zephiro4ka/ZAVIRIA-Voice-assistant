#pragma once
// Карточки справа на главном экране: «Система» (мини-диспетчер задач) и «Сейчас играет».
// Только ImGui — переносится на Linux вместе с интерфейсом.

#include "../ImGui/imgui.h"

namespace HomeWidgets {
    float Width();
    void Bounds(ImVec2& min, ImVec2& max); // где карточки нарисованы в последнем кадре
    // right — правый край карточек; top/bottom — свободная полоса; appear 0…1 — анимация появления
    // minX — левее заходить нельзя (там сфера); туда встаёт второй столбец карточек
    void Draw(ImDrawList* dl, float right, float top, float bottom, float dt, float appear, float minX);
    bool Visible();
    bool AnyMoved();          // какую-то карточку двигали в «Редактировании»           // главный выключатель виджетов
}
