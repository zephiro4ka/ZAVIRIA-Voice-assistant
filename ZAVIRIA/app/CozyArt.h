#pragma once
// «Домашний уют»: лист тетради с рисунками от руки и нарисованная сфера.

#include "../ImGui/imgui.h"

#include <string>

namespace CozyArt {
    constexpr int kPatterns = 15;
    const char* PatternName(int index);
    void Background(ImDrawList* dl, ImVec2 size, int pattern);
    // тетрадь с каракулями поверх тёплого фона атмосферы «Домашний уют»: тише и в тон (кофе, карамель, терракота)
    void Doodles(ImDrawList* dl, ImVec2 size, int pattern, bool light, bool notebook = true); // notebook — клетка, поля и тёплые цвета
    // Кто живёт в центре экрана
    constexpr int kMascots = 8, kMascotLogo = 6, kMascotCustom = 7;
    const char* MascotName(int index);
    int Mascot();
    void SetMascot(int index);
    void PickCustom();                        // открыть выбор файла
    bool TakePicked(const std::string& path); // файл выбран для персонажа?
    void Orb(ImDrawList* dl, ImVec2 center, float radius, float time);
}
