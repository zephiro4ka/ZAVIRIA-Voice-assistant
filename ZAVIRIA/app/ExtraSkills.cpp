#include "ExtraSkills.h"

#include <winsock2.h>
#include <iphlpapi.h>

#include "Platform.h"
#include "EcoMode.h"
#include "Launcher.h"
#include "MediaNow.h"
#include "Pc.h"
#include "PowerMode.h"
#include "Rates.h"
#include "TextUtils.h"

#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <cwctype>
#include <vector>

#pragma comment(lib, "iphlpapi.lib")

namespace {
    int FirstNumber(const std::wstring& text) {
        for (size_t i = 0; i < text.size(); ++i)
            if (iswdigit(text[i])) return _wtoi(text.c_str() + i);
        return -1;
    }

    const char* Currency(const std::wstring& text) {
        if (text.find(L"доллар") != std::wstring::npos) return "USD";
        if (text.find(L"евро") != std::wstring::npos) return "EUR";
        if (text.find(L"рубл") != std::wstring::npos) return "RUB";
        return nullptr;
    }

    std::string Amount(double value) {
        char text[32];
        snprintf(text, sizeof(text), value < 100 ? "%.2f" : "%.0f", value);
        return text;
    }

    // Число цифрами или словами: «25», «twenty five», «a hundred and twelve», «3 thousand». Сдвигает i.
    std::optional<double> EnglishNumber(const std::vector<std::string>& words, size_t& i) {
        static const char* const kSmall[] = { "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "eleven", "twelve",
                                              "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen" };
        static const char* const kTens[] = { "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety" };
        const auto value = [&](const std::string& w) -> std::optional<double> {
            for (int n = 0; n < 20; ++n) if (w == kSmall[n]) return n;
            for (int n = 0; n < 8; ++n) if (w == kTens[n]) return 20 + n * 10;
            return std::nullopt;
        };
        double total = 0, part = 0;
        bool any = false;
        while (i < words.size()) {
            const std::string& w = words[i];
            if (std::isdigit(static_cast<unsigned char>(w[0]))) {
                char* end = nullptr;
                const double v = std::strtod(w.c_str(), &end);
                if (any || *end) break; // «2 3» — два разных числа
                part = v; any = true; ++i;
            } else if (const auto v = value(w)) { part += *v; any = true; ++i; }
            else if (w == "a" && i + 1 < words.size() && (words[i + 1] == "hundred" || words[i + 1] == "thousand" || words[i + 1] == "million") && !any) { part = 1; any = true; ++i; }
            else if (w == "hundred" && any) { part *= 100; ++i; }
            else if ((w == "thousand" || w == "million") && any) { total += part * (w == "thousand" ? 1e3 : 1e6); part = 0; ++i; }
            else if (w == "and" && any && i + 1 < words.size() && value(words[i + 1])) ++i;
            else break;
        }
        if (!any) return std::nullopt;
        return total + part;
    }

    // «what is 25 times 4», «how much is twelve divided by three», «15 + 7». Не пример — nullopt.
    std::optional<std::string> EnglishMath(std::string text) {
        for (size_t i = 1; i + 1 < text.size(); ++i) // «twenty-five» → «twenty five», но «5-3» остаётся вычитанием
            if (text[i] == '-' && std::isalpha(static_cast<unsigned char>(text[i - 1])) && std::isalpha(static_cast<unsigned char>(text[i + 1]))) text[i] = ' ';
        std::string expr = " ";
        for (char c : text) { // знаки — отдельными словами, остальная пунктуация — пробел
            if (std::strchr("+-*/", c)) expr += std::string(" ") + c + " ";
            else if (std::isalnum(static_cast<unsigned char>(c)) || c == '.') expr += c;
            else expr += ' ';
        }
        expr += " ";
        for (auto [from, to] : { std::pair{ " multiplied by ", " * " }, { " times ", " * " }, { " x ", " * " }, { " divided by ", " / " }, { " over ", " / " },
                                 { " plus ", " + " }, { " minus ", " - " } })
            for (size_t pos; (pos = expr.find(from)) != std::string::npos;) expr.replace(pos, std::strlen(from), to);
        std::vector<std::string> words;
        for (size_t at = 0; at < expr.size();) {
            const size_t start = expr.find_first_not_of(' ', at);
            if (start == std::string::npos) break;
            const size_t end = expr.find(' ', start);
            words.push_back(expr.substr(start, end - start));
            at = end;
        }
        std::vector<double> numbers;
        std::vector<char> ops;
        bool negative = false;
        for (size_t i = 0; i < words.size();) {
            if (auto n = EnglishNumber(words, i)) {
                if (numbers.size() == ops.size()) numbers.push_back(negative ? -*n : *n);
                negative = false;
                continue;
            }
            const std::string& w = words[i++];
            if (w.size() != 1 || !std::strchr("+-*/", w[0])) continue;
            if (numbers.size() == ops.size() + 1) ops.push_back(w[0]);
            else if (w[0] == '-' && numbers.empty()) negative = true; // «minus five plus three»
        }
        if (numbers.size() < 2 || numbers.size() != ops.size() + 1) return std::nullopt;
        for (size_t i = 0; i < ops.size();) { // сначала умножение и деление
            if (ops[i] != '*' && ops[i] != '/') { ++i; continue; }
            if (ops[i] == '/' && numbers[i + 1] == 0) return std::string("You can't divide by zero");
            numbers[i] = ops[i] == '*' ? numbers[i] * numbers[i + 1] : numbers[i] / numbers[i + 1];
            numbers.erase(numbers.begin() + i + 1);
            ops.erase(ops.begin() + i);
        }
        double result = numbers[0];
        for (size_t i = 0; i < ops.size(); ++i) result = ops[i] == '+' ? result + numbers[i + 1] : result - numbers[i + 1];
        char out[64];
        snprintf(out, sizeof(out), std::fabs(result - std::round(result)) < 1e-9 ? "%.0f" : "%.4f", result);
        std::string number = out;
        if (number.find('.') != std::string::npos) { while (number.back() == '0') number.pop_back(); if (number.back() == '.') number.pop_back(); }
        return "That's " + number;
    }

    std::string LocalAddress() {
        ULONG size = 0;
        GetIpAddrTable(nullptr, &size, TRUE);
        std::vector<char> buffer(size);
        auto* table = reinterpret_cast<MIB_IPADDRTABLE*>(buffer.data());
        if (!size || GetIpAddrTable(table, &size, TRUE) != NO_ERROR) return {};
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
            const DWORD ip = ntohl(table->table[i].dwAddr);
            if ((ip >> 24) == 192 || (ip >> 24) == 10 || (ip >> 24) == 172) {
                char text[32];
                snprintf(text, sizeof(text), "%lu.%lu.%lu.%lu", (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
                return text;
            }
        }
        return {};
    }
}

std::optional<std::string> ExtraSkills::Handle(const std::string& command) {
    const std::wstring w = Text::LowerRu(Text::ToWide(command));
    const auto has = [&](const wchar_t* part) { return w.find(part) != std::wstring::npos; };

    if (const char* code = Currency(w); code && (has(L"курс") || has(L"сколько стоит") || has(L"почём") || has(L"почем") || (FirstNumber(w) > 0 && (has(L"в сум") || has(L"сколько это"))))) {
        const auto data = Rates::Snapshot();
        for (const auto& item : data.items)
            if (item.code == code) {
                const int count = (std::max)(1, FirstNumber(w));
                return std::to_string(count) + " " + code + " — это " + Amount(item.value * count) + " " + data.base;
            }
        return "Курсы ещё загружаются, спроси через минуту";
    }
    if (has(L"мой ip") || has(L"мой айпи") || has(L"какой айпи") || has(L"ip адрес")) {
        const std::string address = LocalAddress();
        return address.empty() ? "Не вижу подключения к сети" : "Адрес компьютера в сети: " + address;
    }
    if (has(L"сколько работает компьютер") || has(L"время работы компьютера")) {
        const ULONGLONG minutes = GetTickCount64() / 60000;
        return "Компьютер работает " + std::to_string(minutes / 60) + " ч " + std::to_string(minutes % 60) + " мин";
    }
    if (has(L"места на диске") || has(L"свободно на диске")) {
        ULARGE_INTEGER free{};
        GetDiskFreeSpaceExW(L"C:\\", &free, nullptr, nullptr);
        return "На диске C свободно " + std::to_string(free.QuadPart / (1024ULL * 1024 * 1024)) + " гигабайт";
    }
    if (has(L"очисти корзин") || has(L"очистить корзин")) {
        const HRESULT result = SHEmptyRecycleBinW(nullptr, nullptr, SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
        return SUCCEEDED(result) || result == E_UNEXPECTED ? "Корзина очищена" : "Не получилось очистить корзину";
    }
    if (has(L"яркост") && FirstNumber(w) >= 0) {
        const int level = (std::min)(100, FirstNumber(w));
        const std::wstring args = L"-NoProfile -WindowStyle Hidden -Command (Get-CimInstance -Namespace root/WMI -ClassName WmiMonitorBrightnessMethods | Invoke-CimMethod -MethodName WmiSetBrightness -Arguments @{Timeout=1;Brightness=" + std::to_wstring(level) + L"})";
        ShellExecuteW(nullptr, L"open", L"powershell.exe", args.c_str(), nullptr, SW_HIDE);
        return "Ставлю яркость " + std::to_string(level) + " процентов. Работает на ноутбуках и экранах с управлением яркостью";
    }
    return std::nullopt;
}

std::optional<std::string> ExtraSkills::HandleEnglish(const std::string& command) {
    std::string text = Text::Trim(Text::ToUtf8(Text::LowerRu(Text::ToWide(command))));
    while (!text.empty() && (text.back() == '.' || text.back() == '!' || text.back() == '?')) text.pop_back();
    const auto starts = [&](const char* word) { return text.rfind(word, 0) == 0; };
    const auto has = [&](const char* word) { return text.find(word) != std::string::npos; };
    const auto rest = [&](const char* word) { return Text::Trim(text.substr(std::strlen(word))); };

    SYSTEMTIME now{};
    GetLocalTime(&now);
    if (has("what time") || text == "time") {
        char reply[32];
        snprintf(reply, sizeof(reply), "It's %02d:%02d", now.wHour, now.wMinute);
        return reply;
    }
    if (has("what day") || has("what's the date") || has("what is the date") || has("today's date")) {
        static const char* const kDays[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
        static const char* const kMonths[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
        return std::string("Today is ") + kDays[now.wDayOfWeek] + ", " + kMonths[now.wMonth - 1] + " " + std::to_string(now.wDay);
    }
    if (has(" plus ") || has(" minus ") || has(" times ") || has("multiplied") || has("divided") || has(" over ") ||
        text.find_first_of("+*/") != std::string::npos || starts("what is") || starts("what's") || starts("how much is") || starts("calculate"))
        if (auto r = EnglishMath(text)) return r;
    if (has("eco mode") || has("power saving") || has("battery saver") || has("energy saver") || has("saving mode")) {
        if (has("off") || has("disable") || has("stop") || has("exit")) return EcoMode::Disable();
        if (has("is ") && !has("turn")) return std::string(EcoMode::Active() ? "Eco mode is on" : "Eco mode is off");
        return EcoMode::Enable();
    }
    for (const char* word : { "open ", "launch ", "start ", "run " })
        if (starts(word)) {
            const auto result = Launcher::Open(rest(word));
            return result.found ? "Opening " + result.name : "I couldn't find " + rest(word);
        }
    for (const char* word : { "close ", "quit ", "exit " })
        if (starts(word)) {
            const auto result = Launcher::Close(rest(word));
            return result.found ? "Closing " + result.name : rest(word) + " isn't running";
        }
    if (has("volume")) {
        const int level = FirstNumber(Text::ToWide(text));
        if (level >= 0) { Pc::SetVolume((std::min)(100, level)); return "Volume " + std::to_string((std::min)(100, level)) + " percent"; }
    }
    if (has("unmute")) { Pc::SetMute(false); return "Sound is on"; }
    if (text == "mute" || has("mute the sound")) { Pc::SetMute(true); return "Muted"; }
    if (text == "pause" || text == "play" || text == "resume" || has("pause the music") || has("play music")) { MediaNow::Control(MediaNow::Action::PlayPause); return "Done"; }
    if (has("next track") || has("next song") || text == "next") { MediaNow::Control(MediaNow::Action::Next); return "Next track"; }
    if (has("previous track") || has("previous song")) { MediaNow::Control(MediaNow::Action::Previous); return "Previous track"; }
    if (has("lock the computer") || has("lock my computer") || has("lock the pc") || text == "lock") { PowerMode::Lock(); return "Locking the computer"; }
    if (has("cancel shutdown") || has("cancel restart")) { PowerMode::CancelShutdown(); return "Cancelled"; }
    if (has("shut down") || has("turn off the computer")) { PowerMode::Shutdown(15); return "Shutting down in 15 seconds. Say cancel shutdown if you change your mind"; }
    if (has("restart the computer") || has("reboot")) { PowerMode::Restart(15); return "Restarting in 15 seconds"; }
    if (has("my ip") || has("ip address")) { const std::string address = LocalAddress(); return address.empty() ? "I can't see a network connection" : "Your local address is " + address; }
    if (has("uptime") || has("how long has the computer")) { const ULONGLONG minutes = GetTickCount64() / 60000; return "The computer has been on for " + std::to_string(minutes / 60) + " hours and " + std::to_string(minutes % 60) + " minutes"; }
    if (has("disk space") || has("free space")) { ULARGE_INTEGER free{}; GetDiskFreeSpaceExW(L"C:\\", &free, nullptr, nullptr); return "Drive C has " + std::to_string(free.QuadPart / (1024ULL * 1024 * 1024)) + " gigabytes free"; }
    if (has("empty the recycle bin")) { SHEmptyRecycleBinW(nullptr, nullptr, SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND); return "Recycle bin emptied"; }
    for (const auto& [word, code] : { std::pair<const char*, const char*>{ "dollar", "USD" }, { "euro", "EUR" }, { "ruble", "RUB" } })
        if (has(word) && (has("rate") || has("how much"))) {
            const auto data = Rates::Snapshot();
            for (const auto& item : data.items)
                if (item.code == code) return std::string("1 ") + code + " is " + Amount(item.value) + " " + data.base;
            return "Rates are still loading";
        }
    return std::nullopt; // остальное отвечает нейросеть на английском
}
