#pragma once

#include <string>

// Перевод области экрана: сочетание клавиш → выделяешь мышкой место на экране → текст распознаётся (OCR Windows,
// если не вышло — «глазами» нейросети) и переводится на язык из настроек. Перевод — в окошке рядом с выделением.
// [translate] hotkey = «моды|k|код клавиши» (как у кругового меню), по умолчанию Shift+T (её выбор); [translate] lang — код языка.
namespace ScreenTranslate {
    void Start();
    void Reload();                      // поменяли сочетание — перерегистрировать
    void Run();                         // начать выделение сейчас (кнопка в настройках)
    struct Language { const char* code; const char* name; const char* english; };
    const Language* Languages(int& count);
}
