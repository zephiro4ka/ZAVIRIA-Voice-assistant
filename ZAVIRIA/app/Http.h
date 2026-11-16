#pragma once
// HTTP для Ани (WinHTTP): локальные помощники (нейросеть, голос) и интернет (погода).

#include <functional>
#include <string>

namespace Http {
    // Ответ читается кусками: onData(кусок) → false = прервать. Возвращает HTTP-код или 0, если сервер недоступен.
    using OnData = std::function<bool(const std::string& chunk)>;

    // Запрос к своему локальному серверу на 127.0.0.1:port
    int Local(unsigned short port, const wchar_t* method, const wchar_t* path, const std::string& body, int timeoutMs, const OnData& onData);

    // Запрос по полному адресу http(s)://host[:port]/path (облачный ИИ). extraHeaders — строки «Имя: значение\r\n».
    // Ответ читается кусками, как у Local. Возвращает HTTP-код или 0, если сервер недоступен.
    int Remote(const std::string& url, const wchar_t* method, const std::string& body, const std::wstring& extraHeaders, int timeoutMs,
               const OnData& onData);

    // GET по HTTPS (например, прогноз погоды). Тело ответа или пустая строка.
    std::string HttpsGet(const wchar_t* host, const std::wstring& path, int timeoutMs = 8000);

    // POST по HTTPS (телеграм): тело и его тип («application/json», «multipart/form-data; boundary=…»). Ответ или пусто.
    std::string HttpsPost(const wchar_t* host, const std::wstring& path, const std::string& body, const std::string& contentType,
                          int timeoutMs = 20000);

    // GET по полному адресу http(s)://… (страницы и API для поиска ответов). Тело (не больше ~2 МБ) или пусто.
    std::string Get(const std::string& url, int timeoutMs = 8000);
}
