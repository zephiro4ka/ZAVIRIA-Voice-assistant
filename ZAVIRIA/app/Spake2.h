#pragma once

#include <string>
#include <vector>

// SPAKE2 (PAKE) в группе RFC 3526 №14 (2048 бит): код с экрана компьютера не передаётся по сети,
// а подсмотревший обмен не может подбирать код у себя — только по одной попытке на настоящем подключении.
namespace Spake2 {
    // Сторона компьютера. xHex — сообщение телефона; на выходе — ответ yHex и общий ключ (32 байта)
    bool Respond(const std::string& code, const std::string& xHex, std::string& yHex, std::vector<unsigned char>& key);
    bool SelfTest(); // одна и та же сторона дважды: ключи совпадают
    // Для проверки: сторона телефона с известным x
    std::string StartForTest(const std::string& code, const std::string& xHex32, std::string& xOut);
    std::vector<unsigned char> FinishForTest(const std::string& code, const std::string& xHex32, const std::string& xMsg, const std::string& yMsg);
}
