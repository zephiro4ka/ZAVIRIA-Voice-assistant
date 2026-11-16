#include "QrCode.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>

namespace {
    constexpr int kDataBytes = 108, kEccBytes = 26, N = QrCode::kSize;

    uint8_t Mul(uint8_t a, uint8_t b) { // умножение в поле GF(256)
        uint8_t out = 0;
        for (int i = 7; i >= 0; --i) {
            out = static_cast<uint8_t>((out << 1) ^ ((out >> 7) * 0x1D));
            if ((b >> i) & 1) out ^= a;
        }
        return out;
    }

    std::array<uint8_t, kEccBytes> ReedSolomon(const std::vector<uint8_t>& data) {
        std::array<uint8_t, kEccBytes> divisor{};
        divisor[kEccBytes - 1] = 1;
        uint8_t root = 1;
        for (int i = 0; i < kEccBytes; ++i) {
            for (int j = 0; j < kEccBytes; ++j) {
                divisor[j] = Mul(divisor[j], root);
                if (j + 1 < kEccBytes) divisor[j] ^= divisor[j + 1];
            }
            root = Mul(root, 2);
        }
        std::array<uint8_t, kEccBytes> remainder{};
        for (uint8_t byte : data) {
            const uint8_t factor = byte ^ remainder[0];
            for (int i = 0; i + 1 < kEccBytes; ++i) remainder[i] = remainder[i + 1];
            remainder[kEccBytes - 1] = 0;
            for (int i = 0; i < kEccBytes; ++i) remainder[i] ^= Mul(divisor[i], factor);
        }
        return remainder;
    }
}

std::vector<bool> QrCode::Encode(const std::string& text) {
    if (text.size() > 106) return {};
    std::vector<bool> bits;
    const auto put = [&](unsigned value, int count) { for (int i = count - 1; i >= 0; --i) bits.push_back((value >> i) & 1); };
    put(4, 4);
    put(static_cast<unsigned>(text.size()), 8);
    for (unsigned char c : text) put(c, 8);
    put(0, std::min(4, kDataBytes * 8 - static_cast<int>(bits.size())));
    while (bits.size() % 8) bits.push_back(false);
    for (bool odd = false; bits.size() < kDataBytes * 8; odd = !odd) put(odd ? 0x11 : 0xEC, 8);
    std::vector<uint8_t> bytes(kDataBytes);
    for (size_t i = 0; i < bits.size(); ++i) bytes[i / 8] = static_cast<uint8_t>(bytes[i / 8] | (bits[i] << (7 - i % 8)));
    const auto ecc = ReedSolomon(bytes);
    bytes.insert(bytes.end(), ecc.begin(), ecc.end());

    std::vector<bool> dark(N * N, false), fixed(N * N, false);
    const auto set = [&](int x, int y, bool value) { dark[y * N + x] = value; fixed[y * N + x] = true; };
    const auto finder = [&](int cx, int cy) {
        for (int dy = -4; dy <= 4; ++dy)
            for (int dx = -4; dx <= 4; ++dx) {
                const int x = cx + dx, y = cy + dy, d = std::max(std::abs(dx), std::abs(dy));
                if (x >= 0 && y >= 0 && x < N && y < N) set(x, y, d != 2 && d != 4);
            }
    };
    finder(3, 3); finder(N - 4, 3); finder(3, N - 4);
    for (int i = 8; i < N - 8; ++i) { set(i, 6, i % 2 == 0); set(6, i, i % 2 == 0); }
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx) set(30 + dx, 30 + dy, std::max(std::abs(dx), std::abs(dy)) != 1);

    const unsigned format = 0b111011111000100; // коррекция L, маска 0
    const auto bit = [&](int i) { return ((format >> i) & 1) != 0; };
    for (int i = 0; i <= 5; ++i) set(8, i, bit(i));
    set(8, 7, bit(6)); set(8, 8, bit(7)); set(7, 8, bit(8));
    for (int i = 9; i < 15; ++i) set(14 - i, 8, bit(i));
    for (int i = 0; i < 8; ++i) set(N - 1 - i, 8, bit(i));
    for (int i = 8; i < 15; ++i) set(8, N - 15 + i, bit(i));
    set(8, N - 8, true);

    size_t index = 0;
    for (int right = N - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int step = 0; step < N; ++step)
            for (int j = 0; j < 2; ++j) {
                const int x = right - j;
                const bool upward = ((right + 1) & 2) == 0;
                const int y = upward ? N - 1 - step : step;
                if (fixed[y * N + x]) continue;
                bool value = false;
                if (index < bytes.size() * 8) { value = (bytes[index >> 3] >> (7 - (index & 7))) & 1; ++index; }
                dark[y * N + x] = value ^ ((x + y) % 2 == 0); // маска 0
            }
    }
    return dark;
}
