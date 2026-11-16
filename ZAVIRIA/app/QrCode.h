#pragma once

#include <string>
#include <vector>

// QR-код версии 5, коррекция L: до 106 байт. Пусто — текст не поместился.
namespace QrCode {
    constexpr int kSize = 37;
    std::vector<bool> Encode(const std::string& text); // kSize * kSize, true — тёмный модуль
}
