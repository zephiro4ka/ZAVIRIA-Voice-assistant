#include "Security.h"

#include <cstring>

#include "Platform.h"
#include "Settings.h"
#include "TextUtils.h"

#include <wincrypt.h>
#include <dpapi.h>
#include <bcrypt.h>

#include <atomic>
#include <cstdio>
#include <ctime>
#include <mutex>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace {
    const char kPrefix[] = "dpapi:";

    struct KnownFile { const wchar_t* path; const char* sha256; };
    const KnownFile kKnownFiles[] = {
#include "IntegrityHashes.inc"
    };

    std::atomic<bool> g_accepted{ false };

    std::string Sha256File(const std::wstring& path) {
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) return {};
        std::string result;
        BCRYPT_ALG_HANDLE alg = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 && BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
            std::vector<UCHAR> buffer(1 << 20);
            DWORD got = 0;
            bool ok = true;
            while (ok && ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr) && got > 0) ok = BCryptHashData(hash, buffer.data(), got, 0) == 0;
            UCHAR digest[32];
            if (ok && BCryptFinishHash(hash, digest, 32, 0) == 0) {
                char hex[3];
                for (UCHAR byte : digest) { sprintf_s(hex, "%02x", byte); result += hex; }
            }
        }
        if (hash) BCryptDestroyHash(hash);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
        CloseHandle(file);
        return result;
    }

    // ------------------------------------------------------------ код из аутентификатора
    constexpr ULONGLONG kPassMs = 15 * 60 * 1000, kBlockMs = 15 * 60 * 1000;
    constexpr int kMaxFails = 5;
    std::mutex g_mutex;
    ULONGLONG g_passedUntil = 0, g_blockedUntil = 0;
    long long g_lastStep = 0;
    int g_fails = 0;
    std::string g_pending;

    const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

    std::vector<UCHAR> DecodeBase32(const std::string& text) {
        std::vector<UCHAR> out;
        unsigned buffer = 0;
        int bits = 0;
        for (char c : text) {
            const char* at = strchr(kAlphabet, c);
            if (!at || !c) continue;
            buffer = (buffer << 5) | static_cast<unsigned>(at - kAlphabet);
            bits += 5;
            if (bits >= 8) { out.push_back(static_cast<UCHAR>((buffer >> (bits - 8)) & 0xFF)); bits -= 8; buffer &= (1u << bits) - 1; }
        }
        return out;
    }

    std::string TotpCode(const std::string& secret, long long step) {
        std::vector<UCHAR> key = DecodeBase32(secret);
        UCHAR message[8];
        for (int i = 0; i < 8; ++i) message[i] = static_cast<UCHAR>((step >> ((7 - i) * 8)) & 0xFF);
        UCHAR digest[20];
        BCRYPT_ALG_HANDLE alg = nullptr;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) return {};
        const NTSTATUS status = BCryptHash(alg, key.data(), static_cast<ULONG>(key.size()), message, 8, digest, 20);
        BCryptCloseAlgorithmProvider(alg, 0);
        if (status != 0) return {};
        const int offset = digest[19] & 15;
        const unsigned number = ((digest[offset] & 0x7Fu) << 24) | (digest[offset + 1] << 16) | (digest[offset + 2] << 8) | digest[offset + 3];
        char text[8];
        sprintf_s(text, "%06u", number % 1000000u);
        return text;
    }

    long long NowStep() { return static_cast<long long>(std::time(nullptr)) / 30; }

    long long MatchStep(const std::string& secret, const std::string& code, long long after) {
        const long long now = NowStep();
        for (long long step = now - 1; step <= now + 1; ++step)
            if (step > after && TotpCode(secret, step) == code) return step;
        return 0;
    }
}

bool Security::IsProtected(const std::string& stored) { return stored.rfind(kPrefix, 0) == 0; }

std::string Security::Protect(const std::string& plain) {
    DATA_BLOB in{ static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data())) }, out{};
    if (!CryptProtectData(&in, L"ZAVIRIA", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    DWORD size = 0;
    const DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    std::string encoded;
    if (CryptBinaryToStringA(out.pbData, out.cbData, flags, nullptr, &size)) {
        encoded.resize(size);
        if (CryptBinaryToStringA(out.pbData, out.cbData, flags, encoded.data(), &size)) encoded.resize(size);
        else encoded.clear();
    }
    LocalFree(out.pbData);
    return encoded.empty() ? std::string() : kPrefix + encoded;
}

std::string Security::Unprotect(const std::string& stored) {
    if (!IsProtected(stored)) return {};
    const std::string encoded = stored.substr(sizeof(kPrefix) - 1);
    DWORD size = 0;
    if (!CryptStringToBinaryA(encoded.c_str(), 0, CRYPT_STRING_BASE64, nullptr, &size, nullptr, nullptr)) return {};
    std::vector<BYTE> raw(size);
    if (!CryptStringToBinaryA(encoded.c_str(), 0, CRYPT_STRING_BASE64, raw.data(), &size, nullptr, nullptr)) return {};
    DATA_BLOB in{ size, raw.data() }, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    std::string plain(reinterpret_cast<char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return plain;
}

std::string Security::FileSha256(const std::wstring& path) { return Sha256File(path); }

void Security::HardenDllSearch() {
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
    SetDllDirectoryW(L""); // текущая папка не участвует в поиске
}

std::vector<std::string> Security::ChangedFiles() {
    std::vector<std::string> changed;
    if (g_accepted) return changed;
    const std::wstring base = Text::ExecutableDirectory() + L"\\";
    for (const KnownFile& known : kKnownFiles) {
        const std::wstring path = base + known.path;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue; // необязательный файл не поставлен
        if (Sha256File(path) != known.sha256) changed.push_back(Text::ToUtf8(known.path));
    }
    return changed;
}

void Security::AcceptChangedFiles() { g_accepted = true; }

bool Security::TwoFactorEnabled() {
    return Config::Value("telegram", "totp_on", "0") == "1" && !Config::Value("telegram", "totp_secret", "").empty();
}

std::string Security::NewTwoFactorSecret() {
    UCHAR bytes[20];
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return {};
    std::string out;
    unsigned buffer = 0;
    int bits = 0;
    for (UCHAR byte : bytes) {
        buffer = (buffer << 8) | byte;
        bits += 8;
        while (bits >= 5) { out += kAlphabet[(buffer >> (bits - 5)) & 31]; bits -= 5; }
        buffer &= (1u << bits) - 1;
    }
    if (bits > 0) out += kAlphabet[(buffer << (5 - bits)) & 31];
    return out;
}

bool Security::EnableTwoFactor(const std::string& secret, const std::string& code) {
    const long long step = MatchStep(secret, Text::Trim(code), 0);
    if (step == 0) return false;
    Config::SetValue("telegram", "totp_secret", secret);
    Config::SetValue("telegram", "totp_on", "1");
    std::lock_guard lock(g_mutex);
    g_lastStep = step;
    g_passedUntil = 0;
    return true;
}

void Security::DisableTwoFactor() {
    Config::SetValue("telegram", "totp_on", "0");
    Config::SetValue("telegram", "totp_secret", "");
    std::lock_guard lock(g_mutex);
    g_passedUntil = 0;
    g_pending.clear();
}

bool Security::Passed() {
    std::lock_guard lock(g_mutex);
    return GetTickCount64() < g_passedUntil;
}

bool Security::CheckCode(const std::string& code, std::string& message) {
    const std::string secret = Config::Value("telegram", "totp_secret", "");
    std::lock_guard lock(g_mutex);
    const ULONGLONG now = GetTickCount64();
    if (now < g_blockedUntil) { message = "Слишком много ошибок. Подожди " + std::to_string((g_blockedUntil - now) / 60000 + 1) + " мин."; return false; }
    const long long step = secret.empty() ? 0 : MatchStep(secret, Text::Trim(code), g_lastStep);
    if (step == 0) {
        if (++g_fails >= kMaxFails) { g_fails = 0; g_blockedUntil = now + kBlockMs; g_pending.clear(); }
        message = "Неверный код.";
        return false;
    }
    g_lastStep = step;
    g_fails = 0;
    g_passedUntil = now + kPassMs;
    return true;
}

std::string Security::Base64(const unsigned char* data, size_t size) {
    DWORD length = 0;
    const DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    if (!CryptBinaryToStringA(data, static_cast<DWORD>(size), flags, nullptr, &length)) return {};
    std::string out(length, '\0');
    if (!CryptBinaryToStringA(data, static_cast<DWORD>(size), flags, out.data(), &length)) return {};
    out.resize(length);
    return out;
}

namespace {
    std::mutex g_phoneMutex;
    std::string g_phoneCode;
    ULONGLONG g_phoneUntil = 0;
    int g_phoneFails = 0;
    constexpr ULONGLONG kPhoneCodeMs = 5 * 60 * 1000;
}

std::string Security::OpenPhoneCode() {
    static const char kChars[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789"; // без похожих букв и цифр
    UCHAR bytes[8];
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return {};
    std::string code;
    for (UCHAR byte : bytes) code += kChars[byte % (sizeof(kChars) - 1)];
    std::lock_guard lock(g_phoneMutex);
    g_phoneCode = code;
    g_phoneUntil = GetTickCount64() + kPhoneCodeMs;
    g_phoneFails = 0;
    return code;
}

std::string Security::PhoneCode() {
    std::lock_guard lock(g_phoneMutex);
    if (GetTickCount64() >= g_phoneUntil) g_phoneCode.clear();
    return g_phoneCode;
}

bool Security::ConsumePhoneCode(const std::string& code, std::string& message) {
    std::lock_guard lock(g_phoneMutex);
    if (g_phoneCode.empty() || GetTickCount64() >= g_phoneUntil) { message = "Код устарел. Получи новый на компьютере."; return false; }
    if (code != g_phoneCode) {
        if (++g_phoneFails >= 5) g_phoneCode.clear();
        message = "Неверный код.";
        return false;
    }
    g_phoneCode.clear(); // одноразовый
    return true;
}

namespace {
    // AES-256-GCM: nonce(12) | шифротекст | метка(16). encrypt=false — расшифровка с проверкой метки
    bool Gcm(const unsigned char* key32, bool encrypt, const std::vector<UCHAR>& in, std::vector<UCHAR>& out) {
        UCHAR nonce[12], tag[16];
        const UCHAR* data = in.data();
        size_t size = in.size();
        if (encrypt) { if (BCryptGenRandom(nullptr, nonce, 12, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return false; }
        else {
            if (size < 28) return false;
            std::memcpy(nonce, data, 12);
            std::memcpy(tag, data + size - 16, 16);
            data += 12;
            size -= 28;
        }
        BCRYPT_ALG_HANDLE alg = nullptr;
        BCRYPT_KEY_HANDLE key = nullptr;
        bool ok = false;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) == 0 &&
            BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)), sizeof(BCRYPT_CHAIN_MODE_GCM), 0) == 0 &&
            BCryptGenerateSymmetricKey(alg, &key, nullptr, 0, const_cast<PUCHAR>(key32), 32, 0) == 0) {
            BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
            BCRYPT_INIT_AUTH_MODE_INFO(info);
            info.pbNonce = nonce; info.cbNonce = 12; info.pbTag = tag; info.cbTag = 16;
            std::vector<UCHAR> result(size ? size : 1);
            ULONG written = 0;
            const NTSTATUS status = encrypt
                ? BCryptEncrypt(key, const_cast<PUCHAR>(data), static_cast<ULONG>(size), &info, nullptr, 0, result.data(), static_cast<ULONG>(size), &written, 0)
                : BCryptDecrypt(key, const_cast<PUCHAR>(data), static_cast<ULONG>(size), &info, nullptr, 0, result.data(), static_cast<ULONG>(size), &written, 0);
            if (status == 0) {
                result.resize(size);
                if (encrypt) { out.assign(nonce, nonce + 12); out.insert(out.end(), result.begin(), result.end()); out.insert(out.end(), tag, tag + 16); }
                else out = result;
                ok = true;
            }
        }
        if (key) BCryptDestroyKey(key);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
        return ok;
    }
}

std::string Security::Seal(const unsigned char* key32, const std::string& text) {
    std::vector<UCHAR> out;
    if (!Gcm(key32, true, std::vector<UCHAR>(text.begin(), text.end()), out)) return {};
    return Base64(out.data(), out.size());
}

bool Security::Open(const unsigned char* key32, const std::string& sealed, std::string& text) {
    DWORD size = 0;
    if (sealed.empty() || !CryptStringToBinaryA(sealed.c_str(), static_cast<DWORD>(sealed.size()), CRYPT_STRING_BASE64, nullptr, &size, nullptr, nullptr)) return false;
    std::vector<UCHAR> raw(size);
    if (!CryptStringToBinaryA(sealed.c_str(), static_cast<DWORD>(sealed.size()), CRYPT_STRING_BASE64, raw.data(), &size, nullptr, nullptr)) return false;
    raw.resize(size);
    std::vector<UCHAR> out;
    if (!Gcm(key32, false, raw, out)) return false;
    text.assign(out.begin(), out.end());
    return true;
}

std::string Security::SealForPhone(const std::string& text) {
    const std::string keyText = "zaviria-url" + Config::Value("phone", "key", "");
    UCHAR aes[32];
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(keyText.data())), static_cast<ULONG>(keyText.size()), aes, 32) != 0) return {};
    return Seal(aes, text);
}

bool Security::PhoneCodeFailed() {
    std::lock_guard lock(g_phoneMutex);
    if (++g_phoneFails >= 5) g_phoneCode.clear(); // пять неверных попыток — код сгорает
    return g_phoneCode.empty();
}

void Security::ClosePhoneCode() {
    std::lock_guard lock(g_phoneMutex);
    g_phoneCode.clear();
}

void Security::RememberCommand(const std::string& text) {
    std::lock_guard lock(g_mutex);
    g_pending = text;
}

bool Security::AwaitingCode() {
    std::lock_guard lock(g_mutex);
    return !g_pending.empty();
}

std::string Security::TakeCommand() {
    std::lock_guard lock(g_mutex);
    std::string text;
    text.swap(g_pending);
    return text;
}
