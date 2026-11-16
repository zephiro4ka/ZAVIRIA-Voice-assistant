#pragma once
// Режим «Редактирование» («Полная кастомизация»): любой блок страницы (меню, сфера, виджеты, поле ввода, нижняя строка,
// разделы и карточки «Настроек», блоки «Процессов») можно перетащить мышью и потянуть за уголок, чтобы поменять размер.
// Места хранятся в [layout] anya.ini в долях окна, поэтому при другом размере окна блоки остаются на своих местах.
// У каждой страницы свои блоки: имена начинаются с её приставки («proc_», «set_»), у главной приставки нет.
// Только ImGui — переносится на Linux вместе с интерфейсом.

#include "../ImGui/imgui.h"

#include <string>
#include <vector>

namespace HomeLayout {
    enum Resize { kFixed = 0, kBoth = 1, kWidth = 2 }; // как тянется уголок: никак, весь блок, только ширина

    bool Editing();              // режим редактирования включён
    void SetEditing(bool on);

    ImVec2 Shift(const char* id, ImVec2 screen);   // на сколько пикселей сдвинут блок (уже с поправкой, чтобы не уехал за край)
    float Scale(const char* id);                    // во сколько раз увеличен (1 — как задумано)
    bool Moved(const char* id);                     // блок двигали — показывать его, даже если «по умолчанию» ему тесно

    // Где могут стоять следующие блоки (по умолчанию — всё окно). Карточкам в прокрутке — область прокрутки.
    void SetArea(ImVec2 min, ImVec2 max);
    void ClearArea();
    // Блок нарисован здесь (уже со сдвигом — то есть где он сейчас на экране); resize — что делает уголок
    void Report(const char* id, const char* label, ImVec2 min, ImVec2 max, int resize);
    // Часть внутри блока (вкладка в меню): её можно перетащить на другое место среди соседей и настроить правым кликом.
    // move(from, to) — переставить; menu(index) — содержимое меню (рисуется внутри открытого всплывающего окна); click — просто нажали
    struct ChildHooks { void (*move)(int from, int to); void (*menu)(int index); void (*click)(int index); };
    void ReportChild(const char* parent, int index, ImVec2 min, ImVec2 max, ChildHooks hooks);
    bool HitsBlock(float x, float y); // в режиме правки точка на каком-то блоке (окно за неё не тянем)

    // Раз в кадр, после всех страниц: рамки, перетаскивание и панель «вернуть как было / готово».
    // page — приставка открытой страницы ("" — главная), nullptr — на этой странице редактировать нечего
    void DrawEditor(ImVec2 screen, float top, const char* page);
    // Правый клик по свободному месту — меню с пунктом «Редактирование» (вне режима; в режиме его открывает сам редактор)
    void ContextMenu(bool allowed);

    void Reset(const char* page); // блоки страницы — на места по умолчанию

    // для файла внешнего вида: имена всех блоков и перечитать места из anya.ini
    const std::vector<std::string>& Ids();
    void Reload();
}
