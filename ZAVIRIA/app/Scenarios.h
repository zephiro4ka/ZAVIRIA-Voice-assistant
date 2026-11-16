#pragma once
// Сценарии: «Аня, уютный вечер» → цепочка блоков: открыть кино на втором мониторе, запустить игру, убавить звук…
// Сценарий начинается с триггера (фраза, горячая клавиша, время) и идёт по стрелкам от блока к блоку.
// Хранятся в scenarios.json рядом с Аней.

#include <optional>
#include <string>
#include <vector>

namespace Scenarios {
    // Номера не меняем: они записаны в scenarios.json. Новые — только в конец
    enum class Step {
        Launch,     // открыть приложение (text — название, как в «Пуске»)
        OpenLink,   // открыть ссылку в браузере (text — адрес)
        Place,      // поставить окно: text — программа (пусто — из прошлого блока), monitor, position
        Volume,     // громкость number %
        Media,      // number: 0 — пауза/плей, 1 — следующий трек, 2 — предыдущий
        Wait,       // задержка number секунд
        Say,        // произнести text вслух
        // триггеры
        Phrase,     // text — фразы через запятую, после «Аня» или в чате
        Hotkey,     // text — сочетание, «Ctrl+Alt+N»
        // управление
        Condition,  // text position extra: «{ответ} содержит дождь» → выход 0 «да», 1 «нет»
        Loop,       // number раз выход 0, потом выход 1
        Switch,     // text сравнить с вариантами extra через «|»: выход по номеру, последний — «иначе»
        SetVar,     // text — имя, extra — значение
        Nothing,    // просто передаёт ход дальше
        // действия
        Mute,       // position: 0 выключить звук, 1 включить, 2 переключить
        Notify,     // уведомление Windows: text, extra — заголовок
        ChatReply,  // text — сообщение Ани в чате
        Screenshot, // снимок в «Изображения\ZAVIRIA», путь в {ответ}
        Play,       // text — файл или адрес, открыть в своей программе
        Command,    // text — как будто сказали Ане: её навыки и нейросеть
        kCount
    };
    // Положение окна для блока Place
    enum class Position { Maximize = 0, LeftHalf = 1, RightHalf = 2, Center = 3, FullScreen = 4 /* развернуть и F11 — для видео в браузере */ };

    struct Block {
        Step step = Step::Launch;
        std::string text;
        int monitor = 1;   // 1 — основной (слева направо по порядку)
        int position = 0;
        int number = 0;
        // холст: место блока, куда идёт дальше (номера блоков), свой цвет (0xRRGGBB, -1 — по типу) и своё название
        int id = 0;
        float x = 0, y = 0;
        std::vector<int> next;                // выход 0
        int color = -1;
        std::string title;
        std::string extra;                    // второе поле: значение, время, заголовок
        std::vector<std::vector<int>> alt;    // выходы 1, 2… у развилок
        int sides = -1;                       // стороны на холсте: -1 сами; иначе бит 1 — вход справа, бит 2 — выходы слева
        std::vector<float> shape;             // нарисован от руки: контур x0,y0,x1,y1… в долях рамки блока (0…1)
        bool outline = true;                  // показывать нарисованный контур вместо обычной карточки
    };

    struct Scenario {
        std::string name = "Новый сценарий";
        std::string phrase;        // старый формат: фраза «Слушать» — при загрузке становится блоком «Текстовая фраза»
        bool enabled = true;
        std::vector<Block> blocks;
        float startX = 0, startY = 0;   // старый формат: где стоял «Слушать» и куда вёл
        std::vector<int> startNext;
        int linkStyle = 0;              // линии: 0 кривые, 1 ровные, 2 как у перфекциониста
        bool hasLayout = false; // старый сценарий (цепочкой) — расставим на холсте сами
        bool relayout = false;  // из прошлой версии: блоки стояли под старые стрелки — разложить заново при открытии
    };

    bool IsTrigger(Step step);
    int PortCount(const Block& block);              // сколько выходов
    std::vector<int>& Port(Block& block, int port);
    const std::vector<int>& Port(const Block& block, int port);
    std::vector<std::string> Cases(const std::string& extra); // варианты «Переключателя» через «|»

    // Раздать блокам номера и места на холсте, старый «Слушать» превратить в триггер
    void Normalize(Scenario& scenario);

    std::vector<Scenario> Load();
    void Save(const std::vector<Scenario>& scenarios);

    // Фраза совпала с каким-то сценарием — запустить его в фоне и вернуть, что ответить
    std::optional<std::string> TryRun(const std::string& command);
    // в фоне; trigger — номер блока-триггера, -1 — от всех триггеров сразу
    void Run(const Scenario& scenario, int trigger = -1);

    // Горячие клавиши и уведомления — свой поток, перечитывает сценарии после Save
    void StartTriggers();
    void StopTriggers();

    const char* StepName(Step step);
    int MonitorCount();
}
