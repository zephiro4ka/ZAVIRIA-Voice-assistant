#pragma once
// Режим экономии ресурсов: батарея ноутбука садится как можно медленнее, фоновые программы не мешают.
// Пока режим включён:
//   • фоновые программы (без окна на экране или свёрнутые) получают EcoQoS (Windows сама держит их на экономных ядрах
//     и низкой частоте) и пониженный приоритет;
//   • программы из списка «приостановить» замирают (NtSuspendProcess), из списка «закрыть» — закрываются;
//   • схема электропитания — «Экономия энергии» (или режим «Лучшая энергоэффективность»), яркость — ниже;
//   • у самой ZAVIRIA замирает фон, прячутся оверлеи, нейросеть выгружается и опросы идут реже.
// Не трогаем: службы (сеанс 0), всё из папки Windows, саму ZAVIRIA и её помощников, окно на переднем плане
// и программы с открытыми окнами (кроме понижения приоритета у свёрнутых), белый список.
// Выключение возвращает всё как было; закрытые программы только перечисляются — запустить их снова можно кнопкой.
// Состояние пишется в eco_state.json: если ZAVIRIA упадёт, при следующем запуске всё восстановится.

#include <cstdint>
#include <string>
#include <vector>

namespace EcoMode {
    bool Active();

    // Что будет ограничено — для окна перед первым включением и для подсказок
    struct Preview {
        bool ready = false;
        std::vector<std::string> throttle, suspend, close; // понятные названия программ
        bool powerScheme = false;  // есть схема «Экономия энергии» или режим энергоэффективности
        int brightness = -1;       // текущая яркость, -1 — экран без управления яркостью
        int targetBrightness = 40;
    };
    void StartPreview();           // собрать в фоне
    Preview GetPreview();

    // Включить/выключить. Долго (до пары секунд) — само уходит в фоновый поток. Ответ — для голоса и телеграма.
    std::string Enable();
    std::string Disable();
    std::string Toggle();
    bool Busy();                   // включается или выключается прямо сейчас

    // Итог последнего выключения: что закрыто (и можно запустить снова)
    struct Report {
        bool shown = false;        // окно с итогом уже закрыли
        std::vector<std::string> closedNames;
        std::vector<std::wstring> closedPaths;
        int restored = 0;          // скольким процессам вернули приоритет и частоту
        int resumed = 0;           // сколько разморозили
    };
    Report LastReport();
    void DismissReport();
    void RelaunchClosed();         // запустить снова то, что закрыли (только по кнопке)

    // Списки — имена exe через запятую в anya.ini, [eco]
    std::vector<std::string> List(const char* key);  // "whitelist", "suspend", "close"
    void SetList(const char* key, const std::vector<std::string>& items);
    bool PreviewSeen();            // окно «что будет ограничено» уже показывали
    void MarkPreviewSeen();

    void RecoverAfterCrash();      // при запуске: прошлый сеанс упал с включённым режимом — вернуть всё
    void Shutdown();               // выход из программы — выключить, если включён

    // Окно «что будет ограничено» и итог выключения — рисуются поверх всего (Ui::Frame)
    void RequestToggle();          // из интерфейса: первый раз — сначала показать окно
    void OpenPreview();
    void ClosePopup();             // закрыть окно режима (для проверки через DevProbe)
    void Draw(float screenW, float screenH, float dt);
}
