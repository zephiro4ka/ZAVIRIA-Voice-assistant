#pragma once
// Здоровье слуха: микрофон и модель распознавания.
// Сообщает о проблемах (чат, подпись, уведомление системы) и помнит модели, которые не прошли проверку.

#include <filesystem>
#include <string>

namespace SpeechHealth {
    enum class Issue {
        NoMicrophone,     // рабочего устройства ввода нет
        MicrophoneLost,   // было и пропало (выдернули, отключили)
        MicrophoneSilent, // устройство есть, но присылает полную тишину: выключено кнопкой или закрыт доступ
        NoModel,          // ни одной модели, которая загрузилась бы и прошла проверку
        ModelReplaced,    // выбранная модель не прошла проверку, работает запасная (detail — её файл)
        ModelUnreliable,  // модель раз за разом не разбирает речь, переключились на запасную (detail — её файл)
        NoBetterModel,    // модель работает плохо, а запасной нет
        Count
    };

    // Одно и то же повторно не показываем, пока проблема не ушла (или полчаса)
    void Report(Issue issue, const std::string& detail = {});
    // Проблема ушла. Для микрофона, если о ней сообщали, — короткое «снова работает»
    void Resolved(Issue issue);

    // Сломанные модели. Запись привязана к размеру и дате файла: обновил модель — её снова пробуем
    bool IsBroken(const std::filesystem::path& model);
    void MarkBroken(const std::filesystem::path& model, const std::string& reason);
    void Forget(const std::filesystem::path& model); // выбрали её вручную в настройках — дать ещё шанс
}
