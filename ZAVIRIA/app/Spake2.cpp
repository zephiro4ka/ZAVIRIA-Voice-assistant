#include "Spake2.h"

#include "Platform.h"

#include <bcrypt.h>

#include <array>
#include <cstdint>
#include <cstring>

#pragma comment(lib, "bcrypt.lib")

namespace {
    constexpr int N = 64; // 64 × 32 бита = 2048 бит
    using Num = std::array<uint32_t, N>;

    const char* kPrime =
        "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B139B22514A08798E3404DD"
        "EF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
        "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F"
        "83655D23DCA3AD961C62F356208552BB9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
        "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF6955817183995497CEA956AE515D2261898FA0510"
        "15728E5A8AACAA68FFFFFFFFFFFFFFFF";

    Num FromBytes(const unsigned char* data, size_t size) { // big-endian → младшие слова первыми
        Num out{};
        for (size_t i = 0; i < size && i < N * 4; ++i) out[i / 4] |= static_cast<uint32_t>(data[size - 1 - i]) << (8 * (i % 4));
        return out;
    }

    bool FromHex(const std::string& hex, Num& out) {
        if (hex.empty() || hex.size() > N * 8) return false;
        std::vector<unsigned char> bytes((hex.size() + 1) / 2);
        const size_t offset = bytes.size() * 2 - hex.size();
        for (size_t i = 0; i < hex.size(); ++i) {
            const char c = hex[i];
            const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (v < 0) return false;
            const size_t at = i + offset;
            bytes[at / 2] |= static_cast<unsigned char>(at % 2 == 0 ? v << 4 : v);
        }
        out = FromBytes(bytes.data(), bytes.size());
        return true;
    }

    std::string ToHex(const Num& a) {
        static const char* d = "0123456789abcdef";
        std::string out;
        for (int i = N - 1; i >= 0; --i)
            for (int s = 28; s >= 0; s -= 4) out += d[(a[i] >> s) & 15];
        return out; // всегда 512 знаков: так обе стороны хэшируют одинаковые строки
    }

    bool Less(const uint32_t* a, const Num& b) {
        for (int i = N - 1; i >= 0; --i) { if (a[i] != b[i]) return a[i] < b[i]; }
        return false;
    }

    void SubInPlace(uint32_t* a, const Num& b) {
        int64_t borrow = 0;
        for (int i = 0; i < N; ++i) {
            const int64_t d = static_cast<int64_t>(a[i]) - b[i] - borrow;
            a[i] = static_cast<uint32_t>(d);
            borrow = d < 0 ? 1 : 0;
        }
    }

    struct Group {
        Num p{}, r2{}, one{}, q{};
        uint32_t pinv = 0;

        Group() {
            FromHex(kPrime, p);
            uint32_t inv = 1; // p⁻¹ mod 2³² методом Ньютона
            for (int i = 0; i < 5; ++i) inv *= 2 - p[0] * inv;
            pinv = static_cast<uint32_t>(0u - inv);
            Num x{};
            x[0] = 1;
            for (int i = 0; i < 2 * 32 * N; ++i) { // R² mod p удвоениями
                uint32_t carry = 0;
                for (int j = 0; j < N; ++j) { const uint32_t next = x[j] >> 31; x[j] = (x[j] << 1) | carry; carry = next; }
                if (carry || !Less(x.data(), p)) SubInPlace(x.data(), p);
            }
            r2 = x;
            one[0] = 1;
            q = p; // q = (p − 1) / 2
            q[0] -= 1;
            for (int i = 0; i < N; ++i) q[i] = (q[i] >> 1) | (i + 1 < N ? q[i + 1] << 31 : 0);
        }

        Num Mul(const Num& a, const Num& b) const { // умножение Монтгомери
            uint32_t t[N + 2] = {};
            for (int i = 0; i < N; ++i) {
                uint64_t c = 0;
                for (int j = 0; j < N; ++j) { const uint64_t s = static_cast<uint64_t>(t[j]) + static_cast<uint64_t>(a[j]) * b[i] + c; t[j] = static_cast<uint32_t>(s); c = s >> 32; }
                uint64_t s = static_cast<uint64_t>(t[N]) + c;
                t[N] = static_cast<uint32_t>(s);
                t[N + 1] = static_cast<uint32_t>(s >> 32);
                const uint32_t m = t[0] * pinv;
                uint64_t s2 = static_cast<uint64_t>(t[0]) + static_cast<uint64_t>(m) * p[0];
                c = s2 >> 32;
                for (int j = 1; j < N; ++j) { s2 = static_cast<uint64_t>(t[j]) + static_cast<uint64_t>(m) * p[j] + c; t[j - 1] = static_cast<uint32_t>(s2); c = s2 >> 32; }
                s2 = static_cast<uint64_t>(t[N]) + c;
                t[N - 1] = static_cast<uint32_t>(s2);
                t[N] = t[N + 1] + static_cast<uint32_t>(s2 >> 32);
                t[N + 1] = 0;
            }
            if (t[N] || !Less(t, p)) SubInPlace(t, p);
            Num out{};
            std::memcpy(out.data(), t, sizeof(uint32_t) * N);
            return out;
        }

        Num To(const Num& a) const { return Mul(a, r2); }
        Num From(const Num& a) const { return Mul(a, one); }

        Num Pow(const Num& base, const Num& exponent) const { // base и результат — обычные числа
            Num result = To(one), b = To(base);
            for (int i = N - 1; i >= 0; --i)
                for (int bit = 31; bit >= 0; --bit) {
                    result = Mul(result, result);
                    if ((exponent[i] >> bit) & 1) result = Mul(result, b);
                }
            return From(result);
        }

        Num Times(const Num& a, const Num& b) const { return From(Mul(To(a), To(b))); }
    };

    const Group& G() { static const Group group; return group; }

    std::vector<unsigned char> Sha256(const std::string& text) {
        std::vector<unsigned char> out(32);
        BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())), static_cast<ULONG>(text.size()), out.data(), 32);
        return out;
    }

    // Точка, у которой никто не знает дискретный логарифм: квадрат числа из хэша
    Num Element(const char* label) {
        std::vector<unsigned char> bytes;
        for (int i = 0; i < 8; ++i) { const auto part = Sha256(std::string(label) + std::to_string(i)); bytes.insert(bytes.end(), part.begin(), part.end()); }
        bytes[0] &= 0x7F; // меньше p
        const Num h = FromBytes(bytes.data(), bytes.size());
        return G().Times(h, h);
    }

    Num Password(const std::string& code) {
        const auto hash = Sha256("ZAVIRIA-SPAKE2-w" + code);
        return FromBytes(hash.data(), hash.size()); // 256 бит < q
    }

    Num QMinus(const Num& w) { Num out = G().q; SubInPlace(out.data(), w); return out; }

    std::vector<unsigned char> SessionKey(const std::string& x, const std::string& y, const Num& k) {
        return Sha256("ZAVIRIA-SPAKE2|" + x + "|" + y + "|" + ToHex(k));
    }

    bool Valid(const Num& a) { // 1 < a < p − 1
        Num pm1 = G().p;
        pm1[0] -= 1;
        Num two{};
        two[0] = 1;
        return Less(two.data(), a) && Less(a.data(), pm1);
    }
}

bool Spake2::Respond(const std::string& code, const std::string& xHex, std::string& yHex, std::vector<unsigned char>& key) {
    Num X{};
    if (!FromHex(xHex, X) || !Valid(X)) return false;
    Num unit{};
    unit[0] = 1;
    if (G().Pow(X, G().q) != unit) return false; // только элементы подгруппы порядка q: иначе утекают биты ключа
    unsigned char random[32];
    if (BCryptGenRandom(nullptr, random, 32, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return false;
    const Num y = FromBytes(random, 32), w = Password(code);
    Num two{};
    two[0] = 2;
    const Num Y = G().Times(G().Pow(two, y), G().Pow(Element("ZAVIRIA-SPAKE2-N"), w)); // Y = g^y · N^w
    const Num K = G().Pow(G().Times(X, G().Pow(Element("ZAVIRIA-SPAKE2-M"), QMinus(w))), y); // K = (X · M^−w)^y
    yHex = ToHex(Y);
    Num normalized{};
    FromHex(xHex, normalized);
    key = SessionKey(ToHex(normalized), yHex, K);
    return true;
}

std::string Spake2::StartForTest(const std::string& code, const std::string& xHex32, std::string& xOut) {
    Num x{};
    FromHex(xHex32, x);
    Num two{};
    two[0] = 2;
    xOut = ToHex(G().Times(G().Pow(two, x), G().Pow(Element("ZAVIRIA-SPAKE2-M"), Password(code))));
    return xOut;
}

std::vector<unsigned char> Spake2::FinishForTest(const std::string& code, const std::string& xHex32, const std::string& xMsg, const std::string& yMsg) {
    Num x{}, Y{};
    FromHex(xHex32, x);
    FromHex(yMsg, Y);
    const Num K = G().Pow(G().Times(Y, G().Pow(Element("ZAVIRIA-SPAKE2-N"), QMinus(Password(code)))), x);
    return SessionKey(xMsg, yMsg, K);
}

bool Spake2::SelfTest() {
    std::string xMsg, yMsg;
    StartForTest("ABCD2345", "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef", xMsg);
    std::vector<unsigned char> server;
    if (!Respond("ABCD2345", xMsg, yMsg, server)) return false;
    const auto phone = FinishForTest("ABCD2345", "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef", xMsg, yMsg);
    const auto wrong = FinishForTest("WRONG234", "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef", xMsg, yMsg);
    return phone == server && wrong != server;
}
