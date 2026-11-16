#pragma once

#include <string>

// Автообновление: скачивает новый установщик с GitHub, проверяет его контрольную сумму
// и ставит при следующем запуске программы (или сразу по кнопке).
namespace Updater {
    inline constexpr const char* kVersion = "1.5";

    bool ApplyPendingAtStartup(); // true — запущена установка, программе надо закрыться
    void Start();                 // фоновая проверка, если включены автообновления
    bool Installed();             // программа стоит через установщик, а не запущена из папки сборки
    bool HasPending();
    void InstallNow();            // запустить установку и закрыть программу
    std::string Status();
}
