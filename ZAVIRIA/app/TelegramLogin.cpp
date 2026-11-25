#include <winsock2.h> // раньше windows.h
#include <ws2tcpip.h>

#include "TelegramLogin.h"

#include "Http.h"
#include "Lang.h"
#include "Log.h"
#include "Settings.h"
#include "Telegram.h"
#include "TextUtils.h"

#include "../third_party/nlohmann/json.hpp"

#include <bcrypt.h>
#include <shellapi.h>

#include <atomic>
#include <cctype>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")

using nlohmann::json;

namespace {
    constexpr const wchar_t* kAuthHost = L"oauth.telegram.org";
    constexpr const char* kIssuer = "https://oauth.telegram.org";
    constexpr const char* kCallbackPath = "/telegram-login";
    constexpr ULONGLONG kWaitMs = 10 * 60 * 1000; // окно входа живёт 10 минут

    std::mutex g_mutex;
    TelegramLogin::State g_state;
    std::atomic<int> g_generation{ 0 };

    void SetState(int generation, TelegramLogin::Phase phase, const std::string& message = {}) {
        std::lock_guard lock(g_mutex);
        if (generation != g_generation.load()) return; // вход уже отменён или начат заново
        g_state = { phase, message };
    }

    std::string Random(size_t bytes) {
        std::string out(bytes, '\0');
        if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(out.data()), static_cast<ULONG>(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return {};
        return out;
    }

    std::string Sha256(const std::string& data) {
        BCRYPT_ALG_HANDLE alg = nullptr;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
        std::string out(32, '\0');
        const NTSTATUS status = BCryptHash(alg, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()),
                                           reinterpret_cast<PUCHAR>(out.data()), 32);
        BCryptCloseAlgorithmProvider(alg, 0);
        return status == 0 ? out : std::string();
    }

    std::string Base64(const std::string& data, bool url) {
        const char* abc = url ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_" : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        size_t i = 0;
        for (; i + 2 < data.size(); i += 3) {
            const unsigned v = (static_cast<unsigned char>(data[i]) << 16) | (static_cast<unsigned char>(data[i + 1]) << 8) | static_cast<unsigned char>(data[i + 2]);
            out += abc[v >> 18]; out += abc[(v >> 12) & 63]; out += abc[(v >> 6) & 63]; out += abc[v & 63];
        }
        if (i < data.size()) {
            const bool two = i + 1 < data.size();
            const unsigned v = (static_cast<unsigned char>(data[i]) << 16) | (two ? static_cast<unsigned char>(data[i + 1]) << 8 : 0);
            out += abc[v >> 18]; out += abc[(v >> 12) & 63];
            if (two) out += abc[(v >> 6) & 63];
            if (!url) out += two ? "=" : "==";
        }
        return out;
    }

    bool Base64UrlDecode(const std::string& in, std::string& out) {
        out.clear();
        unsigned buffer = 0;
        int bits = 0;
        for (char c : in) {
            int v;
            if (c >= 'A' && c <= 'Z') v = c - 'A';
            else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
            else if (c >= '0' && c <= '9') v = c - '0' + 52;
            else if (c == '-' || c == '+') v = 62;
            else if (c == '_' || c == '/') v = 63;
            else if (c == '=') break;
            else return false;
            buffer = (buffer << 6) | static_cast<unsigned>(v);
            bits += 6;
            if (bits >= 8) { bits -= 8; out += static_cast<char>((buffer >> bits) & 0xFF); }
        }
        return true;
    }

    std::string UrlEncode(const std::string& text) {
        static const char* hex = "0123456789ABCDEF";
        std::string out;
        for (unsigned char c : text) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
            else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
        }
        return out;
    }

    std::string UrlDecode(const std::string& text) {
        std::string out;
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '+') { out += ' '; continue; }
            if (text[i] == '%' && i + 2 < text.size() && isxdigit(static_cast<unsigned char>(text[i + 1])) && isxdigit(static_cast<unsigned char>(text[i + 2]))) {
                out += static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16));
                i += 2;
                continue;
            }
            out += text[i];
        }
        return out;
    }

    std::string QueryValue(const std::string& query, const char* key) {
        const std::string prefix = std::string(key) + "=";
        for (size_t at = 0; at <= query.size();) {
            size_t end = query.find('&', at);
            if (end == std::string::npos) end = query.size();
            if (query.compare(at, prefix.size(), prefix) == 0) return UrlDecode(query.substr(at + prefix.size(), end - at - prefix.size()));
            at = end + 1;
        }
        return {};
    }

    bool SameSecret(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        unsigned char diff = 0;
        for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
        return diff == 0;
    }

    // Имя для экрана: без управляющих символов и не длиннее 64 байт (не режем букву UTF-8 пополам)
    std::string Clean(const std::string& text) {
        std::string out;
        for (unsigned char c : text) if (c >= 0x20 && c != 0x7F) out += static_cast<char>(c);
        if (out.size() > 64) {
            size_t cut = 64;
            while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
            out.resize(cut);
        }
        return Text::Trim(out);
    }

    std::string ClientId() {
        const std::string token = Text::Trim(Config::Value("telegram", "token", ""));
        const size_t colon = token.find(':');
        if (colon == std::string::npos || colon == 0 || colon > 20) return {};
        for (size_t i = 0; i < colon; ++i) if (token[i] < '0' || token[i] > '9') return {};
        return token.substr(0, colon);
    }

    bool VerifyRs256(std::string modulus, const std::string& exponent, const std::string& signed_, const std::string& signature) {
        while (!modulus.empty() && modulus[0] == '\0') modulus.erase(0, 1);
        if (modulus.size() < 256 || modulus.size() > 1024 || exponent.empty() || exponent.size() > 8 || signature.size() != modulus.size()) return false;
        std::vector<unsigned char> blob(sizeof(BCRYPT_RSAKEY_BLOB) + exponent.size() + modulus.size());
        auto* header = reinterpret_cast<BCRYPT_RSAKEY_BLOB*>(blob.data());
        header->Magic = BCRYPT_RSAPUBLIC_MAGIC;
        header->BitLength = static_cast<ULONG>(modulus.size() * 8);
        header->cbPublicExp = static_cast<ULONG>(exponent.size());
        header->cbModulus = static_cast<ULONG>(modulus.size());
        std::memcpy(blob.data() + sizeof(BCRYPT_RSAKEY_BLOB), exponent.data(), exponent.size());
        std::memcpy(blob.data() + sizeof(BCRYPT_RSAKEY_BLOB) + exponent.size(), modulus.data(), modulus.size());
        BCRYPT_ALG_HANDLE alg = nullptr;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_RSA_ALGORITHM, nullptr, 0) != 0) return false;
        BCRYPT_KEY_HANDLE key = nullptr;
        if (BCryptImportKeyPair(alg, nullptr, BCRYPT_RSAPUBLIC_BLOB, &key, blob.data(), static_cast<ULONG>(blob.size()), 0) != 0) { BCryptCloseAlgorithmProvider(alg, 0); return false; }
        const std::string digest = Sha256(signed_);
        BCRYPT_PKCS1_PADDING_INFO padding{ BCRYPT_SHA256_ALGORITHM };
        const bool ok = digest.size() == 32 &&
                        BCryptVerifySignature(key, &padding, reinterpret_cast<PUCHAR>(const_cast<char*>(digest.data())), 32,
                                              reinterpret_cast<PUCHAR>(const_cast<char*>(signature.data())), static_cast<ULONG>(signature.size()), BCRYPT_PAD_PKCS1) == 0;
        BCryptDestroyKey(key);
        BCryptCloseAlgorithmProvider(alg, 0);
        return ok;
    }

    struct Person { long long id = 0; std::string label; };

    // id_token: подпись ключом с oauth.telegram.org, издатель, получатель (наш бот) и срок
    bool CheckIdToken(const std::string& token, const std::string& clientId, Person& person, std::string& error) {
        error = "Telegram прислал неверный ответ";
        const size_t a = token.find('.'), b = a == std::string::npos ? a : token.find('.', a + 1);
        if (b == std::string::npos || token.find('.', b + 1) != std::string::npos) return false;
        std::string headerText, payloadText, signature;
        if (!Base64UrlDecode(token.substr(0, a), headerText) || !Base64UrlDecode(token.substr(a + 1, b - a - 1), payloadText) ||
            !Base64UrlDecode(token.substr(b + 1), signature)) return false;
        const json header = json::parse(headerText, nullptr, false), payload = json::parse(payloadText, nullptr, false);
        if (!header.is_object() || !payload.is_object()) return false;
        if (header.value("alg", "") != "RS256") { error = "В @BotFather → Login Widget выбери подпись RS256"; return false; }

        const json keys = json::parse(Http::HttpsGet(kAuthHost, L"/.well-known/jwks.json", 10000), nullptr, false);
        if (!keys.is_object() || !keys.contains("keys") || !keys["keys"].is_array()) { error = "Нет связи с Telegram"; return false; }
        const std::string kid = header.value("kid", "");
        bool verified = false;
        for (const auto& key : keys["keys"]) {
            if (!key.is_object() || key.value("kty", "") != "RSA" || (!kid.empty() && key.value("kid", "") != kid)) continue;
            std::string n, e;
            if (Base64UrlDecode(key.value("n", ""), n) && Base64UrlDecode(key.value("e", ""), e) && VerifyRs256(n, e, token.substr(0, b), signature)) { verified = true; break; }
        }
        if (!verified) { error = "Подпись Telegram не сошлась"; return false; }

        auto text = [](const json& v) { return v.is_string() ? v.get<std::string>() : v.is_number_integer() ? std::to_string(v.get<long long>()) : std::string(); };
        bool forUs = false;
        if (payload.contains("aud")) {
            const json& aud = payload["aud"];
            if (aud.is_array()) { for (const auto& item : aud) forUs = forUs || text(item) == clientId; }
            else forUs = text(aud) == clientId;
        }
        const long long now = static_cast<long long>(std::time(nullptr));
        const long long exp = payload.contains("exp") && payload["exp"].is_number() ? payload["exp"].get<long long>() : 0;
        const long long iat = payload.contains("iat") && payload["iat"].is_number() ? payload["iat"].get<long long>() : 0;
        if (payload.value("iss", "") != kIssuer || !forUs) return false;
        if (exp < now - 60 || iat > now + 300) { error = "Вход устарел. Проверь часы на компьютере и попробуй ещё раз"; return false; }

        const std::string id = payload.contains("id") ? text(payload["id"]) : payload.contains("sub") ? text(payload["sub"]) : std::string();
        person.id = id.empty() || id.size() > 19 || id.find_first_not_of("0123456789") != std::string::npos ? 0 : std::atoll(id.c_str());
        if (person.id <= 0) return false;
        const std::string name = Clean(payload.value("name", payload.value("given_name", ""))), nick = Clean(payload.value("preferred_username", ""));
        person.label = name.empty() ? (nick.empty() ? std::string("Telegram") : "@" + nick) : nick.empty() ? name : name + " (@" + nick + ")";
        error.clear();
        return true;
    }

    std::string ExchangeError(const json& answer) {
        const std::string code = answer.is_object() ? answer.value("error", "") : std::string();
        if (code == "invalid_client" || code == "unauthorized_client") return "Неверный Client Secret или токен бота";
        if (code == "invalid_grant") return "Вход устарел. Нажми «Войти через Telegram» ещё раз";
        if (code == "invalid_request" || code == "redirect_uri_mismatch") return "Добавь адрес входа в @BotFather → Login Widget → Allowed URLs";
        return code.empty() ? "Нет связи с Telegram" : "Telegram отклонил вход";
    }

    bool Exchange(const std::string& code, const std::string& verifier, const std::string& clientId, Person& person, std::string& error) {
        const std::string secret = Config::Value("telegram", "login_secret", "");
        const std::string body = "grant_type=authorization_code&code=" + UrlEncode(code) + "&redirect_uri=" + UrlEncode(TelegramLogin::kRedirect) +
                                 "&client_id=" + clientId + "&code_verifier=" + UrlEncode(verifier);
        const std::string basic = Base64(clientId + ":" + secret, false);
        const std::string response = Http::HttpsPost(kAuthHost, L"/token", body, "application/x-www-form-urlencoded", 15000,
                                                     L"Authorization: Basic " + std::wstring(basic.begin(), basic.end()) + L"\r\nAccept: application/json\r\n");
        const json answer = json::parse(response, nullptr, false);
        if (!answer.is_object() || !answer.contains("id_token") || !answer["id_token"].is_string()) { error = ExchangeError(answer); return false; }
        return CheckIdToken(answer["id_token"].get<std::string>(), clientId, person, error);
    }

    void Remember(const Person& person) {
        const std::string id = std::to_string(person.id);
        if (!Telegram::IsAllowedId(person.id)) {
            const std::string list = Text::Trim(Config::Value("telegram", "owner", ""));
            Config::SetValue("telegram", "owner", list.empty() ? id : list + ", " + id);
        }
        Config::SetValue("telegram", "account_id", id);
        Config::SetValue("telegram", "account_name", person.label);
        Log::Write("телеграм: вход выполнен");
        Telegram::Reload();
    }

    void SendPage(SOCKET client, int status, const char* title, const char* text) {
        const std::string html =
            std::string("<!doctype html><html lang=\"") + Lang::Code() + "\"><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<title>ZAVIRIA</title><style>"
            ":root{--bg:#f4f3f0;--ink:#1d1c1a;--muted:#6b6862;--line:#e2dfd8;--card:#fbfaf8}"
            "@media(prefers-color-scheme:dark){:root{--bg:#141416;--ink:#ecebe8;--muted:#9a9893;--line:#2a2a2e;--card:#1b1b1e}}"
            "body{margin:0;min-height:100vh;display:grid;place-items:center;background:var(--bg);color:var(--ink);font:16px/1.5 system-ui,sans-serif}"
            "main{max-width:420px;margin:24px;padding:40px;border:1px solid var(--line);border-radius:28px;background:var(--card)}"
            "h1{margin:0 0 8px;font-size:28px;letter-spacing:-.02em}p{margin:0;color:var(--muted)}</style>"
            "<main><h1>" + Lang::Tr(title) + "</h1><p>" + Lang::Tr(text) + "</p></main></html>";
        const std::string response = "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : " Bad Request") +
                                     "\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " + std::to_string(html.size()) +
                                     "\r\nContent-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'none'"
                                     "\r\nCache-Control: no-store\r\nReferrer-Policy: no-referrer\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n" + html;
        send(client, response.data(), static_cast<int>(response.size()), 0);
    }

    // Ждём один правильный ответ браузера: чужие и случайные запросы (favicon, сканеры) просто отбиваем
    void Listen(int generation, SOCKET listener, std::string state, std::string verifier, std::string clientId) {
        const ULONGLONG until = GetTickCount64() + kWaitMs;
        std::string code, denied;
        bool got = false;
        while (!got && generation == g_generation.load() && GetTickCount64() < until) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(listener, &set);
            timeval wait{ 0, 500 * 1000 };
            if (select(0, &set, nullptr, nullptr, &wait) <= 0) continue;
            SOCKET client = accept(listener, nullptr, nullptr);
            if (client == INVALID_SOCKET) continue;
            const DWORD timeout = 5000;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            std::string request;
            char buffer[2048];
            while (request.size() < 8192 && request.find("\r\n\r\n") == std::string::npos) {
                const int read = recv(client, buffer, sizeof(buffer), 0);
                if (read <= 0) break;
                request.append(buffer, read);
            }
            const size_t lineEnd = request.find("\r\n");
            const std::string line = request.substr(0, lineEnd == std::string::npos ? 0 : lineEnd);
            const size_t space = line.find(' ', 4);
            const std::string target = line.rfind("GET ", 0) == 0 && space != std::string::npos ? line.substr(4, space - 4) : std::string();
            const size_t question = target.find('?');
            const std::string query = question == std::string::npos ? std::string() : target.substr(question + 1);
            if (target.substr(0, question) == kCallbackPath && SameSecret(QueryValue(query, "state"), state)) {
                code = QueryValue(query, "code");
                denied = QueryValue(query, "error");
                got = true;
                if (code.empty()) SendPage(client, 400, "Вход отменён", "Вернись в ZAVIRIA и попробуй ещё раз.");
                else SendPage(client, 200, "Почти готово", "Можно закрыть вкладку и вернуться в ZAVIRIA.");
            } else {
                SendPage(client, 400, "Ссылка устарела", "Нажми «Войти через Telegram» в ZAVIRIA ещё раз.");
            }
            shutdown(client, SD_BOTH);
            closesocket(client);
        }
        closesocket(listener);
        if (generation != g_generation.load()) return;
        if (!got) { SetState(generation, TelegramLogin::Phase::Failed, "Время на вход вышло. Попробуй ещё раз"); return; }
        if (code.empty() || code.size() > 2048) { SetState(generation, TelegramLogin::Phase::Failed, denied.empty() ? "Telegram прислал неверный ответ" : "Вход отменён"); return; }
        SetState(generation, TelegramLogin::Phase::Checking);
        Person person;
        std::string error;
        bool ok = false;
        try { ok = Exchange(code, verifier, clientId, person, error); } catch (...) { error = "Telegram прислал неверный ответ"; } // поля не того типа
        if (!ok) {
            Log::Write("телеграм: вход не удался — " + error);
            SetState(generation, TelegramLogin::Phase::Failed, error);
            return;
        }
        if (generation != g_generation.load()) return;
        Remember(person);
        SetState(generation, TelegramLogin::Phase::Idle);
    }
}

bool TelegramLogin::Begin(std::string& error) {
    const std::string clientId = ClientId();
    if (clientId.empty()) { error = "Сначала вставь токен бота"; return false; }
    if (Text::Trim(Config::Value("telegram", "login_secret", "")).empty()) { error = "Сначала вставь Client Secret из @BotFather"; return false; }

    static const bool network = [] { WSADATA data; return WSAStartup(MAKEWORD(2, 2), &data) == 0; }();
    SOCKET listener = network ? socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) : INVALID_SOCKET;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // только этот компьютер
    address.sin_port = 0;                              // любой свободный порт
    int size = sizeof(address);
    if (listener != INVALID_SOCKET) {
        const BOOL exclusive = TRUE;
        setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    }
    if (listener == INVALID_SOCKET || bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size) != 0 || listen(listener, 8) != 0) {
        if (listener != INVALID_SOCKET) closesocket(listener);
        error = "Не получилось подготовить вход";
        return false;
    }

    const std::string verifierBytes = Random(32), stateBytes = Random(24);
    if (verifierBytes.empty() || stateBytes.empty()) { closesocket(listener); error = "Не получилось подготовить вход"; return false; }
    const std::string verifier = Base64(verifierBytes, true);
    const std::string state = std::to_string(ntohs(address.sin_port)) + "." + Base64(stateBytes, true); // порт нужен переходнику
    const std::string challenge = Base64(Sha256(verifier), true);
    const std::string url = std::string("https://oauth.telegram.org/auth?client_id=") + clientId + "&redirect_uri=" + UrlEncode(kRedirect) +
                            "&response_type=code&scope=" + UrlEncode("openid profile telegram:bot_access") + "&state=" + UrlEncode(state) +
                            "&code_challenge=" + challenge + "&code_challenge_method=S256";

    int generation;
    {
        std::lock_guard lock(g_mutex);
        generation = ++g_generation;
        g_state = { Phase::Waiting, {} };
    }
    std::thread(Listen, generation, listener, state, verifier, clientId).detach();
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", Text::ToWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
        Cancel();
        error = "Не получилось открыть браузер";
        return false;
    }
    return true;
}

void TelegramLogin::Cancel() {
    std::lock_guard lock(g_mutex);
    ++g_generation;
    g_state = {};
}

TelegramLogin::State TelegramLogin::Current() {
    std::lock_guard lock(g_mutex);
    return g_state;
}

std::string TelegramLogin::Account() {
    const long long id = std::atoll(Config::Value("telegram", "account_id", "").c_str());
    if (id <= 0 || !Telegram::IsAllowedId(id)) return {}; // убрали из «Кому можно» руками — значит, вышли
    const std::string name = Config::Value("telegram", "account_name", "");
    return name.empty() ? std::string("Telegram") : name;
}

void TelegramLogin::SignOut() {
    const std::string id = Text::Trim(Config::Value("telegram", "account_id", ""));
    if (!id.empty()) {
        const std::string list = Config::Value("telegram", "owner", "");
        std::string kept;
        for (size_t at = 0; at < list.size();) {
            size_t end = list.find_first_of(",; ", at);
            if (end == std::string::npos) end = list.size();
            const std::string item = list.substr(at, end - at);
            if (!item.empty() && item != id) kept += (kept.empty() ? "" : ", ") + item;
            at = end + 1;
        }
        Config::SetValue("telegram", "owner", kept);
    }
    Config::SetValue("telegram", "account_id", "");
    Config::SetValue("telegram", "account_name", "");
    Telegram::Reload();
}
