#pragma once
// Навыки Ани — всё, что она делает сама, без ИИ:
// программы, медиа, громкость, окна, горячие клавиши, печать под диктовку, таймеры, счёт, время.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Skills {
    // Если фраза — команда, выполнить её и вернуть, что сказать в ответ (пустая строка — молча).
    std::optional<std::string> Handle(const std::string& command);

    // Режим диктовки: всё сказанное печатается в активное окно
    bool IsDictating();
    void HandleDictation(const std::string& spoken);

    // Короткая справка «что ты умеешь»
    std::string Help();

    // Идущие таймеры и напоминания (для островка): что, когда кончится и сколько длится, по NowMs()
    struct Countdown { std::string about; uint64_t end = 0, length = 0; };
    std::vector<Countdown> Countdowns();
    void CancelCountdown(uint64_t end); // снять один таймер (крестик в островке)
}
