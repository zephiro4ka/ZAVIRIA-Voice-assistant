#pragma once
// Пульт и удалённый рабочий стол в Telegram Mini App.
// Локальный сервер (только 127.0.0.1) + туннель cloudflared; работает, пока включён пульт, адрес при запуске новый.
// Вход — только с подписью Telegram (initData) и только для владельца. На заблокированном компьютере не работает.

#include <functional>
#include <string>

namespace RemoteDesktop {
    struct Result {
        std::string url;   // https-адрес Mini App
        std::string error; // понятная причина, если не получилось
    };

    // Долгая (ждёт туннель, до минуты) — звать не из потока интерфейса. Уже запущен — сразу вернёт адрес.
    Result Start(const std::string& botToken, long long owner, std::function<void(const std::string&)> onClosed);
    void Stop();

    // Телефон: компьютер сам объявляет себя в домашней сети, бот для этого не нужен
    void StartPhoneBeacon();
    void KeepForPhone();          // при запуске: телефон привязан — связь нужна и без бота
    std::string BeaconStatus();
    std::string BeaconUrl();
    std::string TunnelUrl();
    std::string LanUrl();
    void ShareCode(bool on);    // рассылать по Wi-Fi и код привязки (пока на экране QR «установить на телефон»)       // адрес в домашней сети (http://192.168…:47821), пусто — нет сети    // адрес туннеля (пусто — не поднят)
    void StartForInstall();     // поднять туннель ради QR-кода «скачать приложение» (телефон ещё не привязан)      // адрес для QR-кода, пусто — связь ещё запускается
    // Команды защиты телефона прямо из настроек
    void PhoneTask(const std::string& name, const std::string& arg = "");
    bool PhoneOnline();
    std::string PhoneReports();   // последние ответы телефона, новые сверху
    std::string PhoneLastPlace(); // «широта,долгота» или пусто
    // Отключение телефона: согласие нужно с обеих сторон
    struct PhoneUnlink { bool phone = false, pc = false, done = false; };
    PhoneUnlink PhoneUnlinkState();
    void ApprovePhoneUnlink(); // согласие компьютера
    void CancelPhoneUnlink();
}
