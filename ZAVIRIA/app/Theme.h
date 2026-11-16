#pragma once
// Внешний вид: масштаб под DPI, шрифты, цвета, стиль ImGui, палитры настроений.

#include "../ImGui/imgui.h"

#include <cmath>

struct Settings;

namespace Theme {
    // Настроение Ани — от него зависят цвета сферы, фона и акцентов
    enum class Mood { Idle, Listening, Thinking, Speaking };

    struct Color {
        float r, g, b;
        ImU32 WithAlpha(float a) const;
        Color Mix(const Color& to, float t) const;
    };

    extern float dpi;          // 1.0 = 96 DPI
    extern ImFont* body;       // обычный текст
    extern ImFont* semibold;   // подписи, кнопки
    extern ImFont* display;    // заголовки
    extern ImFont* hero;       // крупное приветствие
    extern ImFont* plain;      // обычный чёткий шрифт: коды должны читаться при любом оформлении
    extern ImFont* title;      // названия настроек и карточек — жирный
    extern ImFont* poster;     // крупная надпись на главной — очень жирный
    extern ImFont* mono;       // поле ввода и мелкие подписи на главной — моноширинный

    extern ImU32 kInk, kInkSoft, kInkMuted; // цвет текста зависит от темы: светлый на тёмном, чернила на бумаге
    constexpr ImU32 kOk = IM_COL32(110, 231, 183, 255);
    constexpr ImU32 kBad = IM_COL32(251, 113, 133, 255);

    void Init(float dpiScale);      // шрифты + стиль
    void LoadFonts();               // загрузить шрифты по настройкам
    float Px(float value);          // значение в пикселях с учётом DPI
    // Доля пути за кадр для плавного сближения: одинаково мягко при любой частоте кадров
    inline float Ease(float dt, float speed) { return 1.0f - std::exp(-std::fmin(dt, 1.0f / 40.0f) * speed); } // долгий кадр не даёт рывка
    inline float railWidth = 0.0f;  // ширина панели вкладок слева: страницы начинаются правее неё
    inline int pageDirection = 1;   // куда листаем вкладки: 1 — вправо, -1 — влево
    // Смещение страницы при переходе: новая въезжает со стороны своей вкладки, старая уезжает в другую
    inline float PageShift(bool open, float anim, float width) {
        const float rest = std::pow(1.0f - std::fmin(anim, 1.0f), 3.0f);
        return std::round((open ? 1.0f : -1.0f) * pageDirection * rest * width * 0.28f);
    }

    // Плавно перетекающая палитра из трёх цветов
    void UpdatePalette(Mood mood, float dt);
    const Color& Palette(int index);

    // ---- внешний вид из настроек: тема, любимый цвет, стекло
    void Apply(const Settings& settings);  // каждый кадр: дёшево, меняет только цвета
    Color Accent();                               // любимый цвет — кнопки, переключатели, выбор
    Color Background(int index);                  // 0 — верх фона, 1 — низ
    ImU32 Surface(float alpha = 1.0f);            // цвет панелей (со «стеклом» — прозрачнее)
    ImU32 SurfaceSolid();                          // то же, но всегда плотно — для всплывающих меню (стекло не просвечивает)
    ImU32 Overlay(int alpha);
    // подложка кнопок и полей: на тёмном — светлая дымка (как Overlay), на светлой теме — белая, как в Windows 11
    ImU32 Control(int level);                     // тонкая линия или подсветка: белая на тёмном, карандашная на бумаге

    // Шрифт и его размер (80–120%, 100% — как задумано)
    int FontCount();
    const char* FontName(int index);
    int FontIndex();
    ImFont* FontPreview(int index);               // этот шрифт для примера в настройках
    int FontScale();
    void SetFont(int index, int scale);

    // Формат «Домашний уют»: бумага и рисунки от руки
    // форма плашек и кнопок: 0 обычная, 1 капли, 2 стикеры
    int Style();
    void SetStyle(int style);
    // нарисовать в выбранной форме; false — форма обычная, рисуй как всегда
    bool Panel(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 fill, ImU32 border, float rounding);
    bool Frame(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 fill, ImU32 border, float rounding);
    // плашка с текстом (виджеты, карточки, блоки): мягкая тень, заливка с переходом, свет по верхнему краю.
    // glow 0..1 — подсветка при наведении или выборе
    void Plate(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 fill, float rounding, float glow = 0.0f);
    // выбранный пункт внутри плашки: мягкое пятно любимого цвета без жёсткой рамки
    void Pick(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, float amount = 1.0f, bool horizontal = false);
    // островок внутри плашки (группа строк, как в настройках Telegram): ровная светлая подложка и тонкий край
    void Island(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding);
    // всё, что нарисовано с vtxStart, плавно растворяется к краям top/bottom (полоса band) — вместо резкого обреза при прокрутке
    void FadeEdges(ImDrawList* dl, int vtxStart, float top, float bottom, float band);
    void ReloadOptions();  // [look]/[cozy] поменяли снаружи (файл вида)
    int Mode();            // 0 тёмная, 1 светлая, 2 как в Windows
    bool Light();          // светлая прямо сейчас
    void SetMode(int mode);
    bool Cozy();
    int CozyPattern();
    void SetCozy(bool on, int pattern);
    bool CozyDark();                              // тёмный вариант: ночная тетрадь
    void SetCozyDark(bool on);
    int DoodleColor();                            // свой цвет рисунков (-1 — разноцветные)
    bool Paper();                                 // светлая бумага

    bool ButtonPlates();                          // подложка под кнопками окна

    struct LookColors { Color top, bottom, surface; };

    // Атмосферы (docs/atmospheres.md): своя палитра, у каждой — светлый и тёмный вариант
    constexpr int kAtmospheres = 5, kAtmosphereCustom = 4; // 0 системная, 1 Airy, 2 домашний уют, 3 Midnight, 4 свои цвета
    struct Atmo { Color top, bottom, surface, accent, ink, shadow; };
    Atmo AtmospherePalette(int atmosphere, bool light);
    const char* AtmosphereName(int atmosphere);
    int Atmosphere();
    int CozyAccent();            // «Домашний уют»: 0 карамель, 1 роза, 2 шалфей, 3 лаванда, 4 черника
    void SetCozyAccent(int index);
    bool CozyAtmosphere(); // «Домашний уют» включён как атмосфера (не тетрадь)
    void SetAtmosphere(int atmosphere);
    LookColors LookPreset(int look, int customRgb);
    constexpr int kAccentPresets = 6;
    Color AccentPreset(int index);
    Color FromRgb(int rgb);
    int ToRgb(const Color& color);
}
