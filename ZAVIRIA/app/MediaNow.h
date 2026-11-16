#pragma once
// «Сейчас играет»: трек из любого плеера, который показывает себя в Windows (Яндекс Музыка, Spotify, браузер с YouTube…),
// и кнопки «назад / пауза / вперёд». Опрос раз в секунду в фоне; Snapshot() мгновенный.

#include <string>

namespace MediaNow {
    struct Track {
        bool present = false;   // что-то открыто в плеере
        bool playing = false;
        std::string title, artist, app;
        double position = 0, duration = 0; // секунды (0 — плеер не сообщает)
    };

    void Start();
    Track Snapshot();
    // Что играет на телефоне (его присылает приложение ZAVIRIA) — для статуса в Discord, когда на ПК тихо
    void SetPhone(const Track& track);
    Track PhoneSnapshot(); // present = false, если телефон давно ничего не присылал

    enum class Action { PlayPause, Next, Previous };
    void Control(Action action); // в фоне, не блокирует
}
