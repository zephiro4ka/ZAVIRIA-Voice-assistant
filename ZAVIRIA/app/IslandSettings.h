#pragma once
// Отдельное окно настроек островка (не главное окно ZAVIRIA): открывается шестерёнкой в самом островке.
// Это тот же ZAVIRIA.exe, запущенный с ключом --island-settings, — своё маленькое окно со своим ImGui.

#include <windows.h>

namespace IslandSettings {
    constexpr const wchar_t* kFlag = L"--island-settings";
    constexpr UINT kChangedMessage = WM_APP + 11; // окну Ани: настройки островка поменялись — перечитать
    int Run(HINSTANCE instance);                  // своё окно и цикл; возвращает код выхода процесса
    void Open();                                  // запустить окно (или показать уже открытое)
}
