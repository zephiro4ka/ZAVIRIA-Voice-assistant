#pragma once

#include <string>
#include <vector>

// Круговое меню быстрого запуска: зажать среднюю кнопку мыши (~0,3 с) в любом месте экрана.
// Короткое нажатие средней кнопки работает как обычно (вкладки в браузере).
// Пункты — дерево любой глубины: программа, папка (открывает следующий круг), сайт, файл/папка с диска. Хранится в pie.json.
namespace PieMenu {
    struct Node {
        std::string kind = "app";     // app | folder | site | path
        std::string name;             // подпись (у программы — название из «Пуска»)
        std::string target;           // сайт — адрес, path — путь на диске
        std::vector<Node> items;      // у папки
    };

    void Start();
    Node Root();                                     // всё меню (копия)
    void SetRoot(const Node& root);                  // сохранить в pie.json
    void Preview();                                  // показать меню у курсора (кнопка в настройках)
    // Картинка круга для настроек: уровень по пути path (номера папок), BGRA сверху вниз, сторона size
    std::vector<unsigned char> RenderPicture(const std::vector<int>& path, int size);
    void Dump(const std::string& file, int hover);   // DevProbe: открыть меню и сохранить кадр в .bmp

    // Чем открывать: [pie] trigger = «моды|m|кнопка» (мышь: 1 левая, 2 правая, 3 колёсико, 4/5 боковые) или «моды|k|код клавиши»;
    // моды — сумма: 1 Ctrl, 2 Alt, 4 Shift, 8 Win. По умолчанию «0|m|3» — зажать колёсико.
    std::string TriggerName(const std::string& trigger); // «Ctrl + колёсико мыши» — для настроек
    void StartCapture(int owner = 0);                 // следующее нажатие (клавиши или кнопки мыши) станет сочетанием; owner — кто просил
    bool Capturing(int owner = 0);
    bool TakeCaptured(std::string& trigger, int owner = 0); // сочетание записано (true один раз); пусто — отменили Esc
    void ReloadTrigger();
}
