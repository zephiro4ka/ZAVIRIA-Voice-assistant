#include "SpeechHealth.h"

#include "Lang.h"
#include "Log.h"
#include "Scenarios.h"
#include "Settings.h"
#include "State.h"
#include "TextUtils.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <system_error>
#include <vector>

namespace {
    using SpeechHealth::Issue;

    struct Message {
        const char* head;    // в чат и уведомление; detail — после него
        const char* tail;    // после detail, может быть пустым
        const char* caption; // короткая подпись под сферой
    };
    // Строки — ключи перевода (LangEn.inc)
    constexpr std::array<Message, static_cast<size_t>(Issue::Count)> kMessages = { {
        { "Не нашла рабочий микрофон. Подключи его, выбери устройство ввода в настройках звука или открой доступ к микрофону в настройках конфиденциальности — я подхвачу его сама. Пока можно писать текстом.", "", "микрофон не найден — можно писать текстом" },
        { "Микрофон отключился. Как только он вернётся, я снова буду слышать. Пока можно писать текстом.", "", "микрофон отключился — можно писать текстом" },
        { "Микрофон молчит: от него приходит полная тишина. Проверь, не выключен ли он кнопкой и открыт ли доступ к микрофону в настройках конфиденциальности.", "", "микрофон молчит — проверь, не выключен ли он" },
        { "Не нашла ни одной рабочей модели распознавания. Скачай её в настройках: «ИИ и голос» → «Модель распознавания». Пока можно писать текстом.", "", "нет модели распознавания — можно писать текстом" },
        { "Модель распознавания не прошла проверку, переключилась на запасную:", "Выбрать другую можно в настройках: «ИИ и голос» → «Модель распознавания».", "сменила модель распознавания" },
        { "Распознавание раз за разом не разбирало речь, переключилась на запасную модель:", "Вернуть прежнюю можно в настройках: «ИИ и голос» → «Модель распознавания».", "сменила модель распознавания" },
        { "Распознавание плохо разбирает речь, а запасной модели нет. Скачай другую в настройках: «ИИ и голос» → «Модель распознавания».", "", "плохо слышу — попробуй другую модель распознавания" },
    } };

    constexpr Clock kRepeatAfterMs = 30 * 60 * 1000;
    constexpr size_t kMaxBroken = 8; // запись в ini ограничена, старые вытесняются

    std::mutex g_mutex;
    std::array<bool, static_cast<size_t>(Issue::Count)> g_shown{};
    std::array<Clock, static_cast<size_t>(Issue::Count)> g_shownAt{};

    bool IsMicrophone(Issue issue) { return issue == Issue::NoMicrophone || issue == Issue::MicrophoneLost || issue == Issue::MicrophoneSilent; }

    // «имя*размер*время» — у Windows в именах файлов не бывает ни «*», ни «|»
    std::string Fingerprint(const std::filesystem::path& model) {
        std::error_code error;
        const auto size = std::filesystem::file_size(model, error);
        if (error) return {};
        const auto time = std::filesystem::last_write_time(model, error);
        if (error) return {};
        return Text::ToUtf8(model.filename().wstring()) + '*' + std::to_string(size) + '*' + std::to_string(time.time_since_epoch().count());
    }

    std::vector<std::string> LoadBroken() {
        std::vector<std::string> out;
        const std::string stored = Config::Value("ears", "broken", "");
        for (size_t start = 0; start < stored.size();) {
            const size_t end = std::min(stored.find('|', start), stored.size());
            if (end > start) out.push_back(stored.substr(start, end - start));
            start = end + 1;
        }
        return out;
    }

    void SaveBroken(const std::vector<std::string>& entries) {
        std::string stored;
        for (const auto& entry : entries) stored += (stored.empty() ? "" : "|") + entry;
        Config::SetValue("ears", "broken", stored);
    }
}

void SpeechHealth::Report(Issue issue, const std::string& detail) {
    const size_t index = static_cast<size_t>(issue);
    if (index >= kMessages.size()) return;
    {
        std::lock_guard lock(g_mutex);
        const Clock now = NowMs();
        if (g_shown[index] && now - g_shownAt[index] < kRepeatAfterMs) return;
        g_shown[index] = true;
        g_shownAt[index] = now;
    }
    const Message& message = kMessages[index];
    Log::Write(std::string("слух: ") + message.head + (detail.empty() ? "" : " " + detail));

    std::string text = Lang::Tr(message.head);
    if (!detail.empty()) text += " " + detail + ".";
    if (*message.tail) text += std::string(" ") + Lang::Tr(message.tail);
    Chat::Add(Role::System, text);
    Caption::Set(Lang::Tr(message.caption));
    Scenarios::SystemNotify("ZAVIRIA", text); // окно может быть свёрнуто в трей
}

void SpeechHealth::Resolved(Issue issue) {
    const size_t index = static_cast<size_t>(issue);
    if (index >= kMessages.size()) return;
    bool wasShown = false;
    {
        std::lock_guard lock(g_mutex);
        wasShown = g_shown[index];
        g_shown[index] = false;
    }
    if (!wasShown || !IsMicrophone(issue)) return;
    Log::Write("слух: микрофон снова работает");
    Chat::Add(Role::System, Lang::Tr("Микрофон снова работает."));
    Caption::Set(Lang::Tr("микрофон снова работает"));
}

bool SpeechHealth::IsBroken(const std::filesystem::path& model) {
    const std::string key = Fingerprint(model);
    if (key.empty()) return false;
    const auto broken = LoadBroken();
    return std::find(broken.begin(), broken.end(), key) != broken.end();
}

void SpeechHealth::MarkBroken(const std::filesystem::path& model, const std::string& reason) {
    const std::string key = Fingerprint(model);
    if (key.empty()) return;
    Log::Write("слух: модель " + Text::ToUtf8(model.filename().wstring()) + " не прошла проверку — " + reason);
    std::lock_guard lock(g_mutex);
    auto broken = LoadBroken();
    const std::string name = key.substr(0, key.find('*') + 1);
    std::erase_if(broken, [&](const std::string& entry) { return entry.rfind(name, 0) == 0; }); // старая версия этого же файла
    broken.push_back(key);
    if (broken.size() > kMaxBroken) broken.erase(broken.begin(), broken.end() - kMaxBroken);
    SaveBroken(broken);
}

void SpeechHealth::Forget(const std::filesystem::path& model) {
    const std::string name = Text::ToUtf8(model.filename().wstring()) + '*';
    std::lock_guard lock(g_mutex);
    auto broken = LoadBroken();
    if (std::erase_if(broken, [&](const std::string& entry) { return entry.rfind(name, 0) == 0; }) > 0) SaveBroken(broken);
}
