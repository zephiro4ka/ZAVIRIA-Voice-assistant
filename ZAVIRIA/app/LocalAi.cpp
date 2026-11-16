#include "LocalAi.h"
#include "EcoMode.h"

#include "Lang.h"
#include "Security.h"
#include "Platform.h"
#include "Http.h"
#include "Json.h"
#include "Log.h"
#include "Settings.h"
#include "Sidecar.h"
#include "State.h"
#include "TextUtils.h"
#include "Voice.h"

#include <shlobj.h>
#include <shobjidl.h>

#include "Com.h"
#include "../third_party/nlohmann/json.hpp"

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>


namespace {
    constexpr unsigned short kPort = 18080;     // свой порт, чтобы не мешать другим программам
    constexpr const char* kContextSize = "16384"; // на 2 потока по 8192: хватает и на снимок экрана, и на долгие размышления над задачей
    constexpr Clock kStartupTimeoutMs = 180000; // загрузка модели в видеокарту — обычно 5–20 с

    std::mutex g_mutex;
    std::vector<std::string> g_models;
    std::string g_activeModel;
    HANDLE g_process = nullptr;
    std::atomic<bool> g_vision{ false };      // у модели есть «глаза» (mmproj) и они разрешены в настройках
    std::atomic<bool> g_engineEyes{ false };  // движок сейчас запущен с ними: они занимают ~0,9 ГБ видеопамяти
    std::atomic<bool> g_wantEyes{ false };    // следующий запуск — с «глазами»
    std::atomic<int> g_busy{ 0 };             // сколько запросов выполняется: во время них не выгружаем
    struct BusyScope { BusyScope() { ++g_busy; } ~BusyScope() { --g_busy; } };
    std::atomic<Clock> g_lastUse{ 0 };    // когда нейросеть последний раз отвечала
    std::atomic<bool> g_unloaded{ false }; // выгружена из видеокарты за простоем — проснётся на вопрос
    std::mutex g_startMutex;    // перезапуски идут строго по очереди
    std::atomic<int> g_tuningVersion{ 0 };
    // игра на весь экран -> движок на CPU, VRAM отдаём игре
    std::atomic<bool> g_wantCpu{ false };
    std::atomic<bool> g_engineCpu{ false };
    std::atomic<HWND> g_gameWindow{ nullptr };

    int AiInt(const char* key, const char* fallback) { return std::atoi(Config::Value("ai", key, fallback).c_str()); }

    // ------------------------------------------------------------ настройки ответов
    struct Preset { const char* name; int chat[6]; int task[6]; }; // температура%, top_p%, top_k, min_p%, повторы%, длина
    constexpr Preset kPresets[Ai::kProfiles - 1] = {
        { "Стандарт",  { 70, 90, 40, 5, 100, 600 },  { 60, 95, 20, 5, 100, 5000 } },
        { "Быстро",    { 50, 90, 40, 5, 105, 250 },  { 40, 90, 30, 5, 100, 1200 } },
        { "Умно",      { 50, 90, 40, 5, 100, 900 },  { 60, 95, 20, 5, 100, 8000 } },
        { "Креативно", { 110, 95, 80, 3, 110, 900 }, { 80, 95, 40, 3, 105, 5000 } },
    };
    constexpr const char* kFields[6] = { "temp", "top_p", "top_k", "min_p", "repeat", "tokens" };

    std::string Key(const char* group, const char* field) { return std::string(group) + "_" + field; }

    enum class Kind { Chat, Think, TelegramFast, TelegramDeep, Precise, PreciseCode };

    std::string Hundredths(int value) {
        char text[32];
        snprintf(text, sizeof(text), "%.2f", value / 100.0);
        return text;
    }

    std::string Sampling(Kind kind) {
        if (kind == Kind::Precise || kind == Kind::PreciseCode) {
            const int tokens = std::max(AiInt("chat_tokens", "600"), kind == Kind::PreciseCode ? 1600 : 900);
            return "\"temperature\":" + Hundredths(std::clamp(AiInt("precise_temp", "30"), 0, 200)) + ",\"top_p\":0.90,\"top_k\":20,\"min_p\":0.05,\"max_tokens\":" +
                   std::to_string(tokens) + ",";
        }
        const int profile = Ai::Profile();
        if (profile == 0) { // значения, с которыми программа настраивалась изначально
            switch (kind) {
            case Kind::Think: return "\"temperature\":0.6,\"top_p\":0.9,\"max_tokens\":1500,";
            case Kind::TelegramFast: return "\"temperature\":0.4,\"top_p\":0.9,\"max_tokens\":1400,";
            case Kind::TelegramDeep: return "\"temperature\":0.6,\"top_p\":0.95,\"top_k\":20,\"max_tokens\":5000,";
            default: return "\"temperature\":0.7,\"top_p\":0.9,\"max_tokens\":600,";
            }
        }
        const bool task = kind == Kind::Think || kind == Kind::TelegramDeep;
        const Preset& standard = kPresets[0];
        const int* defaults = task ? standard.task : standard.chat;
        const char* group = task ? "task" : "chat";
        int v[6];
        for (int i = 0; i < 6; ++i) v[i] = AiInt(Key(group, kFields[i]).c_str(), std::to_string(defaults[i]).c_str());
        if (kind == Kind::TelegramFast) v[5] = std::max(v[5], 1400); // письменный ответ длиннее голосового
        return "\"temperature\":" + Hundredths(std::clamp(v[0], 0, 200)) + ",\"top_p\":" + Hundredths(std::clamp(v[1], 0, 100)) +
               ",\"top_k\":" + std::to_string(std::clamp(v[2], 0, 500)) + ",\"min_p\":" + Hundredths(std::clamp(v[3], 0, 100)) +
               ",\"repeat_penalty\":" + Hundredths(std::clamp(v[4], 50, 200)) + ",\"max_tokens\":" + std::to_string(std::clamp(v[5], 16, 32000)) +
               ",\"seed\":" + std::to_string(AiInt("seed", "-1")) + ",";
    }

    int ThinkMode() { return AiInt("think_mode", "0"); } // 0 только по просьбе, 1 и для задач, 2 никогда

    // ------------------------------------------------------------ облачный ИИ (OpenAI-совместимый API)
    std::string CloudBase() {
        std::string url = Text::Trim(Config::Value("ai", "cloud_url", ""));
        while (!url.empty() && url.back() == '/') url.pop_back();
        const std::string tail = "/chat/completions"; // вставили полный адрес — убираем хвост, допишем сами
        if (url.size() > tail.size() && url.compare(url.size() - tail.size(), tail.size(), tail) == 0) url.resize(url.size() - tail.size());
        return url;
    }

    bool CloudReady() { return !CloudBase().empty() && !Text::Trim(Config::Value("ai", "cloud_key", "")).empty(); }

    // Тело для облака: только то, что понимает OpenAI. Параметры llama (top_k, repeat_penalty, seed…) дают там ошибку 400
    std::string CloudBody(const std::string& body) {
        const nlohmann::json in = nlohmann::json::parse(body, nullptr, false);
        if (in.is_discarded() || !in.is_object()) return body;
        nlohmann::json out = nlohmann::json::object();
        for (const char* key : { "messages", "stream", "max_tokens", "temperature", "top_p" })
            if (in.contains(key)) out[key] = in[key];
        out["model"] = Text::Trim(Config::Value("ai", "cloud_model", ""));
        return out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }

    int CloudRequest(const std::string& body, int timeoutMs, const Http::OnData& onData) {
        const std::string key = Text::Trim(Config::Value("ai", "cloud_key", ""));
        const std::wstring headers = L"Authorization: Bearer " + std::wstring(key.begin(), key.end()) + L"\r\n"
                                     L"HTTP-Referer: https://github.com/zephiro4ka/ZAVIRIA-Voice-assistant\r\nX-Title: ZAVIRIA\r\n"; // OpenRouter просит назвать приложение
        std::string head; // начало ответа — для журнала, если облако ответило ошибкой
        const int code = Http::Remote(CloudBase() + "/chat/completions", L"POST", CloudBody(body), headers, timeoutMs, [&](const std::string& chunk) {
            if (head.size() < 400) head += chunk.substr(0, 400 - head.size());
            return onData(chunk);
        });
        if (code != 200) Log::Write("облачный ИИ: код " + std::to_string(code) + " " + head.substr(0, 300));
        return code;
    }

    // ------------------------------------------------------------ HTTP
    int Request(const wchar_t* method, const wchar_t* path, const std::string& body, int timeoutMs, const Http::OnData& onData) {
        if (Ai::CloudOn() && wcscmp(path, L"/v1/chat/completions") == 0) return CloudRequest(body, timeoutMs, onData);
        return Http::Local(kPort, method, path, body, timeoutMs, onData);
    }

    bool Healthy() {
        return Request(L"GET", L"/health", "", 2000, [](const std::string&) { return true; }) == 200;
    }

    // ------------------------------------------------------------ поиск движка и моделей
    bool FileExists(const std::wstring& path) {
        const DWORD a = GetFileAttributesW(path.c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }

    // llama-server.exe: рядом с exe (копируется при сборке) или в third_party проекта
    std::wstring FindServer() {
        std::wstring dir = Text::ExecutableDirectory();
        for (int level = 0; level < 5; ++level) {
            for (const wchar_t* sub : { L"\\llama.cpp", L"\\third_party\\llama.cpp", L"\\ZAVIRIA\\third_party\\llama.cpp" }) {
                const std::wstring path = dir + sub + L"\\llama-server.exe";
                if (FileExists(path)) return path;
            }
            const size_t slash = dir.find_last_of(L"\\/");
            if (slash == std::wstring::npos) break;
            dir.resize(slash);
        }
        return {};
    }

    void CollectGguf(const std::wstring& dir, std::vector<std::string>& out, int depth = 0) {
        if (depth > 4) return;
        WIN32_FIND_DATAW data;
        const HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &data);
        if (find == INVALID_HANDLE_VALUE) return;
        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring path = dir + L"\\" + name;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                CollectGguf(path, out, depth + 1);
                continue;
            }
            const std::wstring lower = Text::LowerRu(name);
            const bool gguf = lower.size() > 5 && lower.compare(lower.size() - 5, 5, L".gguf") == 0;
            const bool projector = lower.rfind(L"mmproj", 0) == 0; // «глаза» модели для картинок — не сама модель
            if (gguf && !projector) out.push_back(Text::ToUtf8(path));
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }

    // Модели из LM Studio (~/.lmstudio/models) и из папки models проекта
    std::vector<std::string> DiscoverModels() {
        std::vector<std::string> models;
        PWSTR profile = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profile))) {
            CollectGguf(std::wstring(profile) + L"\\.lmstudio\\models", models);
            CoTaskMemFree(profile);
        }
        std::wstring dir = Text::ExecutableDirectory();
        for (int level = 0; level < 4; ++level) {
            CollectGguf(dir + L"\\models", models, 3);
            const size_t slash = dir.find_last_of(L"\\/");
            if (slash == std::wstring::npos) break;
            dir.resize(slash);
        }
        std::sort(models.begin(), models.end());
        models.erase(std::unique(models.begin(), models.end()), models.end());
        return models;
    }

    std::string PickModel(const std::vector<std::string>& models, const std::string& wanted) {
        if (!wanted.empty() && std::find(models.begin(), models.end(), wanted) != models.end()) return wanted;
        for (const char* preferred : { "Qwen3.5-4B", "Qwen3-4B", "qwen" })
            for (const auto& m : models)
                if (Text::Contains(m, preferred)) return m;
        return models.empty() ? std::string() : models.front();
    }

    std::string FileName(const std::string& path) {
        const size_t slash = path.find_last_of("\\/");
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    // «Глаза» модели: mmproj-*.gguf в той же папке
    std::wstring FindProjector(const std::string& model) {
        const std::wstring wide = Text::ToWide(model);
        const std::wstring dir = wide.substr(0, wide.find_last_of(L"\\/"));
        WIN32_FIND_DATAW data;
        const HANDLE find = FindFirstFileW((dir + L"\\mmproj*.gguf").c_str(), &data);
        if (find == INVALID_HANDLE_VALUE) return {};
        FindClose(find);
        return dir + L"\\" + data.cFileName;
    }

    uint64_t FileSize(const std::string& path) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(Text::ToWide(path).c_str(), GetFileExInfoStandard, &data)) return 0;
        return (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    }

    // 9B на CPU еле ползёт, так что в игре берём модель полегче (обычно 4B)
    std::string CpuModel(const std::vector<std::string>& models, const std::string& chosen) {
        constexpr uint64_t kLight = 3500ULL * 1024 * 1024;
        if (FileSize(chosen) <= kLight) return chosen;
        std::string best;
        uint64_t bestSize = 0;
        for (const auto& m : models)
            if (const uint64_t size = FileSize(m); size <= kLight && size > bestSize) { best = m; bestSize = size; }
        return best.empty() ? chosen : best;
    }

    // ------------------------------------------------------------ запуск движка
    bool LaunchServer(const std::wstring& server, const std::string& model) {
        const std::wstring found = Config::Value("ai", "vision", "1") == "1" ? FindProjector(model) : std::wstring();
        g_vision = !found.empty();
        const std::wstring projector = g_wantEyes.load() ? found : std::wstring(); // без снимка «глаза» не загружаем
        g_engineEyes = !projector.empty();
        const bool cpu = g_wantCpu.load();
        g_engineCpu = cpu;
        // игре тоже нужны ядра
        const int threads = cpu ? std::clamp(AiInt("game_threads", "6"), 1, 64) : AiInt("threads", "0");
        const std::string extra = Text::Trim(Config::Value("ai", "engine_args", ""));
        const std::wstring arguments = L"-m \"" + Text::ToWide(model) + L"\"" +
            (projector.empty() ? L"" : L" --mmproj \"" + projector + L"\"") + // картинки: снимок экрана с задачей
            L" --host 127.0.0.1 --port " + std::to_wstring(kPort) +
            L" -c " + std::to_wstring(std::clamp(AiInt("context", kContextSize), 512, 262144)) +
            L" -np " + std::to_wstring(std::clamp(AiInt("parallel", "2"), 1, 8)) + // два запроса сразу: Аня отвечает голосом, пока решает задачу для телеграма
            // 99 = сколько влезет: без -ngl движок раскладывает сам (--fit); дефолтный запас 1 ГБ для 6 ГБ карты великоват
            (cpu ? std::wstring(L" -ngl 0") :
            AiInt("gpu_layers", "99") >= 99 ? L" -fitt " + std::to_wstring(std::clamp(AiInt("gpu_reserve", "512"), 128, 8192))
                                             : L" -ngl " + std::to_wstring(std::clamp(AiInt("gpu_layers", "99"), 0, 999))) +
            (threads > 0 ? L" -t " + std::to_wstring(threads) : L"") +
            L" -fa on -ctk q8_0 -ctv q8_0" + // сжатый кэш контекста: −400 МБ видеопамяти, качество то же
            L" --load-mode none" +          // без отображения файла в память: 0,7 ГБ ОЗУ вместо 2,8
            L" --jinja" +   // шаблон чата из самой модели
            (extra.empty() ? L"" : L" " + Text::ToWide(extra)); // свои параметры из настроек — последними, чтобы могли всё переопределить
        Log::Write("нейросеть: параметры движка — контекст " + std::to_string(AiInt("context", kContextSize)) + ", слоёв " + std::to_string(AiInt("gpu_layers", "99")));
        if (const auto changed = Security::ChangedFiles(); !changed.empty()) { // файлы могли подменить уже после запуска программы
            Log::Write("нейросеть: не запускаю, изменён файл " + changed.front());
            return false;
        }
        g_process = Sidecar::Launch(server, arguments, L"llama-server.log");
        if (g_process && cpu) SetPriorityClass(g_process, BELOW_NORMAL_PRIORITY_CLASS);
        if (cpu) Log::Write("нейросеть: идёт игра, запускаю на процессоре");
        return g_process != nullptr;
    }

    void StartBlocking(bool waking = false) {
        std::lock_guard startLock(g_startMutex);
        if (!waking) { // проснуться — не значит пропасть: пока модель грузится, вопросы ждут её, а не получают отказ
            g_app.aiReady = false;
            g_app.aiChecked = false;
        }

        const auto models = DiscoverModels();
        std::string model = PickModel(models, Config::Get().model);
        if (g_wantCpu.load()) model = CpuModel(models, model);
        {
            std::lock_guard lock(g_mutex);
            g_models = models;
            g_activeModel = model;
        }
        const std::wstring server = FindServer();
        if (model.empty() || server.empty()) {
            if (model.empty()) Chat::Add(Role::System, "Не нашла модель нейросети (.gguf) — ни в LM Studio, ни в папке models.");
            if (server.empty()) Chat::Add(Role::System, "Не нашла движок нейросети (third_party/llama.cpp/llama-server.exe).");
            g_app.aiChecked = true;
            return;
        }

        Sidecar::Stop(g_process);
        if (!LaunchServer(server, model)) {
            Chat::Add(Role::System, "Не получилось запустить нейросеть.");
            g_app.aiChecked = true;
            return;
        }

        // ждём, пока модель загрузится в видеокарту
        const Clock deadline = NowMs() + kStartupTimeoutMs;
        while (g_app.running && NowMs() < deadline) {
            if (Sidecar::HasExited(g_process)) {
                Chat::Add(Role::System, "Нейросеть закрылась при запуске — подробности в llama-server.log рядом с Аней.");
                break;
            }
            if (Healthy()) {
                g_app.aiReady = true;
                break;
            }
            Sleep(400);
        }
        if (waking && !Healthy()) g_app.aiReady = false;
        g_app.aiChecked = true;
    }

    // Запуск Ани: только находим модель. В видеокарту она загрузится при первом вопросе
    void PrepareLazy() {
        const auto models = DiscoverModels();
        const std::string model = PickModel(models, Config::Get().model);
        if (Ai::CloudOn()) { // отвечает облако: своя модель не нужна, даже если её нет
            std::lock_guard lock(g_mutex);
            g_models = models;
            g_activeModel = model;
            g_unloaded = true;
            g_app.aiReady = true;
            g_app.aiChecked = true;
            return;
        }
        if (model.empty() || FindServer().empty()) { StartBlocking(); return; } // скажет, чего не хватает
        {
            std::lock_guard lock(g_mutex);
            g_models = models;
            g_activeModel = model;
        }
        g_vision = Config::Value("ai", "vision", "1") == "1" && !FindProjector(model).empty();
        g_unloaded = true;
        g_app.aiReady = true;
        g_app.aiChecked = true;
    }

    // ------------------------------------------------------------ разговор
    std::string StripThinking(const std::string& s) {
        std::string out;
        size_t pos = 0;
        for (;;) {
            const size_t open = s.find("<think>", pos);
            if (open == std::string::npos) return out + s.substr(pos);
            out += s.substr(pos, open - pos);
            const size_t close = s.find("</think>", open);
            if (close == std::string::npos) return out; // ещё думает — ничего не показываем
            pos = close + 8;
        }
    }

    // Конец ближайшего готового куска речи (позиция после него) или npos.
    // Нейросеть пишет быстрее, чем Аня говорит, поэтому отдаём голосу по одному предложению:
    // короткие куски синтезируются, пока звучит предыдущий, — без пауз между ними.
    // Самый первый кусок и слишком длинные предложения режем ещё и по запятой.
    size_t FindSpeakableCut(const std::string& s, size_t from, bool firstPiece) {
        const size_t commaAfter = firstPiece ? 40 : 120; // байт UTF-8: ≈ 20 и 60 русских букв
        for (size_t i = from; i + 1 < s.size(); ++i) {
            const char c = s[i], next = s[i + 1];
            const bool gap = next == ' ' || next == '\n';
            if ((c == '.' || c == '!' || c == '?' || c == '\n') && gap) return i + 1;
            if (c == ',' && gap && i - from > commaAfter) return i + 1;
        }
        return std::string::npos;
    }

    constexpr const char* kCommandTag = "КОМАНДА:";

    // Как Аня разговаривает — по выбранному характеру
    std::string Persona(const Settings& cfg) {
        switch (cfg.personality) {
        case Personality::Strict:
            return "Характер: строгая, холодная и немногословная. Отвечай сухо и только по делу: одно-два коротких предложения, "
                   "без шуток, ласковых слов, восклицаний и эмоций.";
        case Personality::Sweet:
            return "Характер: милая, нежная и очень преданная. Ты искренне заботишься о пользователе и радуешься ему, "
                   "можешь ласково похвалить, подбодрить и немножко пококетничать. Отвечай тепло и коротко: одно-три предложения.";
        case Personality::Custom:
            if (!Text::Trim(cfg.customPrompt).empty()) return "Характер, который задал пользователь: " + cfg.customPrompt;
            [[fallthrough]];
        default:
            return "Характер: дружелюбная и спокойная — вежливая и тёплая, но без лишних нежностей, иногда с лёгким юмором. "
                   "Отвечай коротко и по делу: обычно одно-три предложения.";
        }
    }

    // Модель сама не знает, какой сейчас день: сообщаем в каждом запросе
    std::string CurrentMoment() {
        static const char* kDays[] = { "воскресенье", "понедельник", "вторник", "среда", "четверг", "пятница", "суббота" };
        static const char* kMonths[] = { "января", "февраля", "марта", "апреля", "мая", "июня", "июля", "августа", "сентября", "октября", "ноября", "декабря" };
        SYSTEMTIME t{};
        GetLocalTime(&t);
        char clock[16];
        snprintf(clock, sizeof(clock), "%02u:%02u", t.wHour, t.wMinute);
        return std::string("Сейчас ") + kDays[t.wDayOfWeek] + ", " + std::to_string(t.wDay) + " " + kMonths[t.wMonth - 1] + " " + std::to_string(t.wYear) + " года, время " + clock + ".";
    }

    std::string SystemPrompt(const std::string& userName) {
        std::string friends;
        for (const auto& [spoken, handle] : Config::Contacts()) friends += (friends.empty() ? "" : ", ") + spoken;
        if (friends.empty()) friends = "(пока никого)";

        const std::string aboutMe = Text::Trim(Config::Value("ai", "about_me", ""));
        const std::string extra = Text::Trim(Config::Value("ai", "extra_prompt", "")) + (aboutMe.empty() ? "" : " Что важно знать о пользователе: " + aboutMe);
        return std::string(Lang::ReplyEnglish() && !Lang::AutoSpeech() ? "Always reply in English, even though these instructions are in Russian.\n\n" : "") + (Text::Trim(extra).empty() ? std::string() : "Особые указания пользователя (важнее всего остального, кроме формата команд): " + extra + "\n\n") +
               "Ты — " + Config::Get().assistant + ", голосовая помощница-девушка. Пользователя зовут " + userName + ". " + Persona(Config::Get()) + " "
               "Никогда не говори, что не знаешь дату или время — они указаны в конце этих указаний. Время называй как человек: «восемь сорок девять вечера», а не «двадцать часов сорок девять минут». Не предлагай зайти к кому-то в чат или открыть браузер, если об этом не просили. "
               "Тебя создал Зефир (его ещё зовут Зефирка или Мара) — он твой создатель и разработчик программы ZAVIRIA. Если спрашивают о создателе, называй его по имени "
               "и говори о нём тепло и с благодарностью, в своём характере. Тот, кто сейчас с тобой говорит, может быть другим человеком. "
               "Ты живёшь на его компьютере и умеешь им управлять через свои команды.\n\n"
               "Если пользователь просит что-то СДЕЛАТЬ на компьютере — открыть программу или сайт, закрыть, зайти к кому-то в чат, "
               "поставить паузу, поменять громкость, напечатать текст, поставить таймер, найти в интернете — не отвечай словами. "
               "Ответь только строкой вида\n"
               "КОМАНДА: <команда>\n"
               "(несколько действий — несколько таких строк). Команды:\n"
               "открой <программа или сайт>\n"
               "закрой <программа>\n"
               "зайди в чат с <имя> — личные сообщения в Discord\n"
               "открой видео <название> — нажать на видео или ссылку, которые сейчас видны в браузере\n"
               "какая сегодня погода | какая будет погода | когда будет дождь\n"
               "пролистай ниже | пролистай выше | прошлая вкладка | следующая вкладка | открой закрытую вкладку | скрой браузер | покажи браузер\n"
               "какие вкладки открыты | перейди на вкладку <название> | громкость на <число> | сколько заряда | загрузка компьютера | прочитай выделенное\n"
               "пауза | продолжи | следующий трек | предыдущий трек | громче | тише | выключи звук\n"
               "напечатай <текст>\n"
               "сверни все | сделай скриншот | заблокируй компьютер | почисти память\n"
               "таймер на <число> секунд | таймер на <число> минут | напомни через <число> минут <о чём> | выключи таймер (секунды и минуты не путай)\n"
               "найди <запрос>\n"
               "Речь распознаётся с ошибками: «дискорт», «открать», «прощал» — догадайся, что имелось в виду. "
               "Имя друга пиши в начальной форме; известные друзья: " + friends + " — если имя похоже на кого-то из них, выбери его. "
               "Никогда не говори, что не можешь управлять компьютером.\n\n"
               "На обычные вопросы и разговор отвечай по-русски, коротко и в своём характере. "
               "Если просят помочь с учёбой или задачей — объясняй понятно и по шагам, но без лишней воды. "
               "Ответ будет озвучен, поэтому без markdown, списков, эмодзи и кода. Склоняй числа и даты правильно: «тридцать первое октября», «в тысяча девятьсот сорок первом году».\n\n"
               // язык фразы — ближе к концу: русский промпт остаётся тем же и не ломает кэш
               + (Lang::ReplyEnglish() && Lang::AutoSpeech() ? " The user is speaking English right now: always reply in English, even though these instructions are in Russian. " : "")
               // время в конце, иначе каждую минуту ломается кэш промпта
               + CurrentMoment();
    }

    // ------------------------------------------------------------ учёба, факты, код
    bool HasAny(const std::string& prompt, std::initializer_list<const char*> markers) {
        for (const char* marker : markers)
            if (Text::Contains(prompt, marker)) return true;
        return false;
    }

    bool IsCode(const std::string& prompt) {
        // голое «программ» не ищем: «открой программу» — команда
        return HasAny(prompt, { "код", "программирова", "напиши программу", "программу на", "функци", "скрипт", "python", "питон", "c++", "плюсах", "javascript",
                                "джаваскрипт", "java", "html", "css", "sql", "алгоритм", "компил", "массив", "цикл ", "класс ", "регулярн", "git ", "rust", "golang", "c#" });
    }

    bool IsKnowledge(const std::string& prompt) {
        return IsCode(prompt) || HasAny(prompt, {
            "что такое", "кто так", "кто был", "кто написал", "кто открыл", "кто изобрёл", "кто изобрел", "когда был", "когда произош", "в каком году", "почему", "зачем", "объясни",
            "расскажи про", "расскажи о", "чем отлича", "в чём разниц", "в чем разниц", "как работает", "как устроен", "как правильно", "определени", "формул",
            "переведи", "перевод", "по-английски", "на английск", "по-немецки", "на немецк", "по-французски", "на французск", "по-испански", "на испанск", "по-китайски", "на китайск", "время глагол", "артикл",
            "правило", "правописан", "запят", "разбор", "морфолог", "синтакси", "орфограф", "пунктуац", "ударени", "склонени", "спряжени", "падеж", "часть речи", "суффикс", "приставк",
            "математ", "алгебр", "геометр", "уравнени", "интеграл", "производн", "логарифм", "дроб", "теорем", "вероятност",
            "физик", "хими", "биолог", "истори", "географ", "обществ", "литератур", "информатик", "астроном", "экономик", "философ", "стихотворени", "произведени", "писател", "столиц" });
    }

    bool PreciseEnabled() { return Config::Value("ai", "precise", "1") == "1"; }

    std::string PreciseRules(bool code) {
        // в задачах итог только после решения, иначе модель сначала угадывает и потом подгоняет
        std::string rules = " Сейчас вопрос на знание: отвечай точно, без воды и без лишних оговорок. На вопрос о факте — сразу прямой ответ, потом одно-два предложения объяснения. "
                            "В задаче — сначала коротко реши по шагам, проверь вычисления и только в конце назови итог. "
                            "Не выдумывай даты, имена, цифры и правила. Правила русского и других языков называй так, как в школьном учебнике, и приводи пример.";
        if (code)
            rules += " Это вопрос про программирование. Напиши правильный, рабочий и полный код в блоке ``` с названием языка — он появится на экране, "
                     "озвучен не будет, поэтому здесь код можно. До кода — одно короткое предложение, что он делает, после — при необходимости одно-два предложения пояснения. "
                     "Учитывай крайние случаи, не придумывай несуществующие функции и библиотеки.";
        return rules;
    }

    // код читать вслух бессмысленно. Результат только дописывается в конец, поэтому его можно озвучивать по кускам
    std::string SpeakablePart(const std::string& visible) {
        std::string out;
        size_t pos = 0;
        for (;;) {
            const size_t open = visible.find("```", pos);
            if (open == std::string::npos) return out + visible.substr(pos);
            out += visible.substr(pos, open - pos);
            const size_t close = visible.find("```", open + 3);
            if (close == std::string::npos) return out;
            out += Lang::ReplyEnglish() ? " The code is on the screen. " : " Код — на экране. ";
            pos = close + 3;
        }
    }

    // Начало ответа — это «КОМАНДА:»? Пока ответ короче метки, решить нельзя — ждём следующих слов.
    enum class Reply { Undecided, Command, Speech };
    Reply Classify(const std::string& visible) {
        const std::wstring text = Text::LowerRu(Text::ToWide(Text::Trim(visible)));
        const std::wstring tag = Text::LowerRu(Text::ToWide(kCommandTag));
        if (text.size() < tag.size()) return tag.rfind(text, 0) == 0 ? Reply::Undecided : Reply::Speech;
        return text.rfind(tag, 0) == 0 ? Reply::Command : Reply::Speech;
    }

    // «КОМАНДА: открой discord\nКОМАНДА: зайди в чат с прыщавый» → { "открой discord", "зайди в чат с прыщавый" }
    std::vector<std::string> ExtractCommands(const std::string& text) {
        std::vector<std::string> commands;
        const std::wstring wide = Text::ToWide(text);
        const std::wstring lower = Text::LowerRu(wide);
        const std::wstring tag = Text::LowerRu(Text::ToWide(kCommandTag));
        for (size_t pos = lower.find(tag); pos != std::wstring::npos; pos = lower.find(tag, pos + tag.size())) {
            const size_t start = pos + tag.size();
            const size_t end = std::min(wide.find(L'\n', start), lower.find(tag, start));
            std::wstring line = wide.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
            std::erase_if(line, [](wchar_t ch) { return ch == L'<' || ch == L'>'; }); // модель иногда копирует «<имя>» из шаблона
            const std::string command = Text::Trim(Text::ToUtf8(line));
            if (!command.empty()) commands.push_back(command);
        }
        return commands;
    }

    // Задача или домашка — решать по шагам «вслух» (быстро и верно: 2–6 секунд)
    bool IsTask(const std::string& prompt) {
        for (const char* marker : { "реши", "решить", "уравнени", "задач", "домашк", "докажи", "контрольн", "посчитай", "вычисли", "сколько будет",
                                    "найди корн", "найди x", "найди икс", "найди значение", "найди площадь", "найди периметр" })
            if (Text::Contains(prompt, marker)) return true;
        return false;
    }

    // Долгие размышления — только когда просят прямо: «подумай хорошо», «подумай как следует»
    bool WantsDeepThinking(const std::string& prompt) {
        for (const char* marker : { "подумай хорошо", "подумай как следует", "подумай подольше", "хорошенько подумай", "подумай внимательно" })
            if (Text::Contains(prompt, marker)) return true;
        return false;
    }

    // Формат OpenAI: его понимает llama-server. Размышления модели включаем только для задач —
    // они идут в отдельное поле reasoning_content и не озвучиваются; на всё остальное ответ приходит сразу.
    enum class Mode { Quick, Think, Hurry };
    enum class Topic { Talk, Knowledge, Code };

    Topic TopicOf(const std::string& prompt) {
        if (!PreciseEnabled()) return Topic::Talk;
        if (IsCode(prompt)) return Topic::Code;
        return IsKnowledge(prompt) || IsTask(prompt) ? Topic::Knowledge : Topic::Talk;
    }

    std::string BuildChatBody(const std::string& userName, Mode mode, Topic topic = Topic::Talk) {
        const bool think = mode == Mode::Think;
        std::string system = SystemPrompt(userName);
        if (mode == Mode::Hurry) // задача — решаем «вслух»: коротко по шагам и с итогом
            system += " Сейчас отвечай сразу, без долгих раздумий: коротко покажи решение по шагам и обязательно назови итоговый ответ.";
        if (topic != Topic::Talk) system += PreciseRules(topic == Topic::Code);
        const Kind kind = think ? Kind::Think : topic == Topic::Code ? Kind::PreciseCode : topic == Topic::Knowledge ? Kind::Precise : Kind::Chat;
        std::string body = std::string("{\"stream\":true,") + Sampling(kind) +
                           "\"chat_template_kwargs\":{\"enable_thinking\":" + (think ? "true" : "false") + "},\"messages\":[";
        body += "{\"role\":\"system\",\"content\":\"" + Json::Escape(system) + "\"}";
        for (const auto& m : Conversation::Recent())
            body += ",{\"role\":\"" + m.role + "\",\"content\":\"" + Json::Escape(m.content) + "\"}";
        return body + "]}";
    }

    // Игра или видео на весь экран: окно закрывает монитор целиком
    bool FullscreenAppInFront() {
        const HWND window = GetForegroundWindow();
        if (!window || window == GetShellWindow()) return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid == GetCurrentProcessId()) return false;
        wchar_t name[64] = {};
        GetClassNameW(window, name, 64);
        if (wcscmp(name, L"WorkerW") == 0 || wcscmp(name, L"Progman") == 0) return false; // рабочий стол
        MONITORINFO info{ sizeof(info) };
        RECT rect{};
        if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info) || !GetWindowRect(window, &rect)) return false;
        return rect.left <= info.rcMonitor.left && rect.top <= info.rcMonitor.top && rect.right >= info.rcMonitor.right && rect.bottom >= info.rcMonitor.bottom;
    }

    // 0 выгружать, 1 на CPU, 2 не трогать
    int GameMode() {
        const std::string mode = Config::Value("ai", "game_mode", "");
        if (!mode.empty()) return std::clamp(std::atoi(mode.c_str()), 0, 2);
        return Config::Value("ai", "game_unload", "1") == "1" ? 1 : 2; // со старых версий
    }

    bool CoversMonitor(HWND window) {
        MONITORINFO info{ sizeof(info) };
        RECT rect{};
        if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info) || !GetWindowRect(window, &rect)) return false;
        return rect.left <= info.rcMonitor.left && rect.top <= info.rcMonitor.top && rect.right >= info.rcMonitor.right && rect.bottom >= info.rcMonitor.bottom;
    }

    // после alt+tab игра либо свёрнута, либо (borderless) всё ещё на весь экран.
    // браузер после полноэкранного видео просто развёрнут — его отпускаем
    bool GameRunning() {
        if (FullscreenAppInFront()) {
            g_gameWindow = GetForegroundWindow();
            return true;
        }
        const HWND game = g_gameWindow.load();
        if (game && IsWindow(game) && (IsIconic(game) || CoversMonitor(game))) return true;
        g_gameWindow = nullptr;
        return false;
    }

    bool WantCpu() { return GameMode() == 1 && GameRunning(); }

    // idle_minutes простоя -> выгрузка. В игре сразу уходим с GPU, после игры уходим с CPU
    void IdleWatcher() {
        for (;;) {
            Sleep(5000);
            const bool game = GameRunning(); // до проверок ниже, иначе пропустим свёрнутую игру
            if (g_unloaded.load() || !g_app.aiReady || g_app.thinking || g_busy.load() > 0) continue;
            // в режиме экономии нейросеть не держим в памяти: минута тишины — и выгружаем
            const int minutes = EcoMode::Active() ? 1 : std::atoi(Config::Value("ai", "idle_minutes", "10").c_str());
            const Clock idle = NowMs() - g_lastUse.load();
            const bool idleLong = minutes > 0 && idle >= static_cast<Clock>(minutes) * 60000;
            const int mode = GameMode();
            const char* reason = nullptr;
            if (idleLong) reason = "простой";
            else if (game && mode == 0 && idle >= 60000) reason = "открыта полноэкранная программа";
            else if (game && mode == 1 && !g_engineCpu.load()) reason = "игра — освобождаю видеокарту";
            else if (!game && g_engineCpu.load() && idle >= 15000) reason = "игра закрыта — следующий ответ снова на видеокарте";
            if (!reason) continue;
            std::lock_guard startLock(g_startMutex);
            if (g_busy.load() > 0) continue;
            Sidecar::Stop(g_process); // g_app.aiReady оставляем: нейросеть доступна, просто спит
            g_unloaded = true;
            g_engineCpu = false;
            Log::Write(idleLong ? "нейросеть: выгружена после " + std::to_string(minutes) + " мин простоя" : std::string("нейросеть: выгружена — ") + reason);
        }
    }

    // Перед вопросом: выгружена — загрузить снова (5–15 с)
    void EnsureAwake(bool eyes = false) {
        static std::mutex s_wakeMutex;
        std::lock_guard wake(s_wakeMutex); // загрузка уже идёт (начата заранее) — дождаться её
        g_lastUse = NowMs();
        if (Ai::CloudOn()) { g_app.aiReady = true; return; } // облаку просыпаться не нужно
        const bool cpu = WantCpu();
        const bool asleep = g_unloaded.exchange(false);
        const bool switchPlace = !asleep && cpu != g_engineCpu.load();
        if (!asleep && !switchPlace && !(eyes && g_vision.load() && !g_engineEyes.load())) {
            std::lock_guard wait(g_startMutex); // модель как раз загружается — дождаться
            return;
        }
        g_wantEyes = eyes;
        g_wantCpu = cpu;
        Caption::Set("просыпаюсь…", true);
        Log::Write(cpu ? "нейросеть: загружаю на процессор (игра)" : "нейросеть: загружаю обратно");
        StartBlocking(true);
        g_lastUse = NowMs();
    }
}

// ---------------------------------------------------------------------
void Ai::Start() {
    g_lastUse = NowMs();
    if (Config::Value("ai", "preload", "0") == "1" && !CloudOn()) std::thread([] { StartBlocking(); }).detach();
    else std::thread(PrepareLazy).detach();
    static bool s_watching = false;
    if (!s_watching) { s_watching = true; std::thread(IdleWatcher).detach(); }
}

void Ai::Unload() {
    std::thread([] {
        if (g_unloaded.load() || g_app.thinking || g_busy.load() > 0) return;
        std::lock_guard startLock(g_startMutex);
        if (!g_process) return;
        Sidecar::Stop(g_process); // g_app.aiReady оставляем: при вопросе модель загрузится снова
        g_unloaded = true;
        Log::Write("нейросеть: выгружена — включён режим экономии");
    }).detach();
}

void Ai::Prewarm() {
    if (g_unloaded.load() && !CloudOn()) std::thread([] { EnsureAwake(); }).detach();
}

void Ai::Restart() {
    g_lastUse = NowMs();
    if (CloudOn()) { CloudChanged(); return; } // модель выбрали, но отвечает облако — загрузится, когда облако выключат
    g_unloaded = false; // запуск сам загрузит модель
    g_wantEyes = false;
    g_wantCpu = WantCpu();
    std::thread([] { StartBlocking(); }).detach();
}

Ai::Answer Ai::Ask(const std::string& prompt) {
    BusyScope busy;
    EnsureAwake();
    const int generation = g_app.cancelGeneration.load();
    Conversation::Push("user", prompt);

    const size_t message = Chat::Add(Role::Anya, "…");
    std::string full, pending, error;
    size_t spoken = 0;
    Reply kind = Reply::Undecided; // команда или обычный ответ — решаем по первым словам
    Clock thinkDeadline = 0;       // до какого момента модели можно размышлять (0 — без размышлений)
    bool thinkingTooLong = false;

    // Ответ приходит потоком «data: {...}» — по кусочку текста в каждой строке
    auto onData = [&](const std::string& chunk) {
        if (g_app.cancelGeneration.load() != generation) return false;
        if (thinkDeadline && full.empty() && NowMs() > thinkDeadline) { // всё ещё думает, а время вышло
            thinkingTooLong = true;
            return false;
        }
        pending += chunk;
        for (size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (line.rfind("data:", 0) != 0) continue;
            line.erase(0, 5);
            if (line.find("[DONE]") != std::string::npos) continue;
            if (line.find("\"error\"") != std::string::npos) {
                const auto m = Json::FindStrings(line, "message", 1);
                error = m.empty() ? "ошибка движка" : m[0];
                return false;
            }
            const auto content = Json::FindStrings(line, "content", 1); // reasoning_content сюда не попадает
            if (content.empty()) continue;
            full += content[0];

            const std::string visible = StripThinking(full);
            if (kind == Reply::Undecided) kind = Classify(visible);
            if (kind != Reply::Speech) continue; // команду не озвучиваем и не показываем — её выполнят навыки
            const std::string speakable = SpeakablePart(visible);
            for (size_t cut; (cut = FindSpeakableCut(speakable, spoken, spoken == 0)) != std::string::npos && cut > spoken;) {
                Voice::Speak(speakable.substr(spoken, cut - spoken));
                spoken = cut;
            }
            const std::string shown = Text::Trim(visible).empty() ? "…" : visible;
            Chat::Update(message, shown);
            Caption::Set(shown, true);
        }
        return true;
    };

    const int thinkMode = ThinkMode();
    const bool think = thinkMode != 2 && (WantsDeepThinking(prompt) || (thinkMode == 1 && IsTask(prompt)));
    const Mode mode = think ? Mode::Think : IsTask(prompt) ? Mode::Hurry : Mode::Quick;
    const Topic topic = TopicOf(prompt);
    if (topic != Topic::Talk) Log::Write(topic == Topic::Code ? "нейросеть: вопрос про код — точный режим" : "нейросеть: вопрос на знание — точный режим");
    if (think) { // размышления ограничены по времени, сразу даём понять, что Аня не зависла
        Caption::Set("думаю…", true);
        Voice::Speak("Хорошо, подумаю");
    }
    // Ник «zephiro4ka» модель коверкает («зепhiro4ka»), поэтому называем пользователя так, как имя звучит вслух
    const Settings cfg = Config::Get();
    const std::string userName = cfg.spokenName.empty() ? cfg.name : cfg.spokenName;
    if (think) thinkDeadline = NowMs() + static_cast<Clock>(std::clamp(AiInt("think_seconds", "10"), 3, 300)) * 1000;
    int code = Request(L"POST", L"/v1/chat/completions", BuildChatBody(userName, mode, topic), 180000, onData);

    // Размышления не уложились в бюджет (или съели весь ответ) — просим ответить сразу, коротко и по шагам
    if (think && g_app.cancelGeneration.load() == generation && (thinkingTooLong || Text::Trim(StripThinking(full)).empty())) {
        Log::Write("нейросеть думала слишком долго — прошу ответить сразу");
        Caption::Set("ещё секундочку…", true);
        thinkDeadline = 0;
        full.clear();
        pending.clear();
        kind = Reply::Undecided;
        code = Request(L"POST", L"/v1/chat/completions", BuildChatBody(userName, Mode::Hurry, topic), 120000, onData);
    }
    const std::string visible = StripThinking(full);
    const std::string answer = Text::Trim(visible);

    if (g_app.cancelGeneration.load() != generation) {
        Chat::Update(message, answer.empty() ? "(перебили)" : answer);
        return { true, {} };
    }
    if (code != 200 || answer.empty()) {
        if (code == 0 && !CloudOn()) g_app.aiReady = false;
        Chat::Update(message, Role::System, "Нейросеть не ответила" + (error.empty() ? std::string() : ": " + error));
        return {};
    }

    if (Classify(visible) == Reply::Command) {
        Answer result{ true, ExtractCommands(visible) };
        std::string shown;
        for (const auto& c : result.commands) shown += (shown.empty() ? "⚙ " : " · ") + c;
        Chat::Update(message, Role::System, shown.empty() ? "⚙ (пустая команда)" : shown);
        return result;
    }

    if (const std::string speakable = SpeakablePart(visible); spoken < speakable.size()) Voice::Speak(speakable.substr(spoken));
    Chat::Update(message, answer);
    Caption::Set(answer);
    Conversation::Push("assistant", answer);
    return { true, {} };
}

bool Ai::LooksLikeTask(const std::string& prompt) { return IsTask(prompt); }

std::string Ai::Quiet(const std::string& prompt, const std::string& imageBase64, bool deepRequested) {
    const bool deep = deepRequested && ThinkMode() != 2;
    BusyScope busy;
    EnsureAwake(!imageBase64.empty());
    if (!g_app.aiReady) return {};
    const Settings cfg = Config::Get();
    const std::string userName = cfg.spokenName.empty() ? cfg.name : cfg.spokenName;
    std::string system = SystemPrompt(userName) +
        " Сейчас ты отвечаешь в телеграме: пиши обычным текстом, без команд. Не используй LaTeX, знаки $ и Markdown — "
        "формулы пиши как в тетради: x², √(x+1), a·b, (a+1)/(b-2), D = b² − 4ac.";
    if (!imageBase64.empty())
        system += " Тебе прислали снимок экрана. Если на нём задача — реши её по шагам и в конце напиши строку «Ответ: …».";
    if (deep) // как настоящий решатель: сначала подумать, потом коротко и чисто записать
        system += " Это задача: хорошо подумай, выбери метод (для задач с параметром — разложение на множители, замена или графический анализ), "
                  "проверь ответ подстановкой. В ответе — только чистое решение по шагам без перебора неудачных попыток, и в конце «Ответ: …».";
    std::string user = "\"" + Json::Escape(prompt) + "\"";
    if (!imageBase64.empty() && (CloudOn() ? Config::Value("ai", "cloud_vision", "1") == "1" : g_vision.load()))
        user = "[{\"type\":\"text\",\"text\":\"" + Json::Escape(prompt) + "\"},{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64," +
               imageBase64 + "\"}}]";
    const Topic topic = TopicOf(prompt);
    if (topic != Topic::Talk) {
        system += PreciseRules(false);
        if (topic == Topic::Code) system += " Это вопрос про программирование: напиши правильный, рабочий и полный код обычным текстом с отступами, без выдуманных функций.";
    }
    // режим размышлений — с настройками, которые рекомендуют для Qwen: температура 0.6, top_p 0.95, top_k 20
    std::string sampling = Sampling(deep ? Kind::TelegramDeep : Kind::TelegramFast);
    if (!deep && topic != Topic::Talk) { // в телеграме ответ длиннее голосового
        sampling = Sampling(topic == Topic::Code ? Kind::PreciseCode : Kind::Precise);
        const size_t at = sampling.find("\"max_tokens\":");
        if (at != std::string::npos) {
            const int tokens = std::max(std::atoi(sampling.c_str() + at + 13), 1400);
            sampling = sampling.substr(0, at) + "\"max_tokens\":" + std::to_string(tokens) + ",";
        }
    }
    const std::string body = "{\"stream\":false," + sampling +
                             "\"chat_template_kwargs\":{\"enable_thinking\":" + (deep ? "true" : "false") + "},\"messages\":["
                             "{\"role\":\"system\",\"content\":\"" + Json::Escape(system) + "\"},{\"role\":\"user\",\"content\":" + user + "}]}";
    std::string response;
    const int code = Request(L"POST", L"/v1/chat/completions", body, deep ? 420000 : 180000, [&](const std::string& chunk) { response += chunk; return true; });
    if (code != 200) {
        Log::Write("тихий ответ: код " + std::to_string(code) + " " + response.substr(0, 200));
        return {};
    }
    const auto content = Json::FindStrings(response, "content", 1);
    const std::string answer = content.empty() ? std::string() : Text::PrettyMath(Text::Trim(StripThinking(content[0])));
    if (deep && answer.empty()) { // размышления не уложились в предел — отвечаем сразу, по шагам, но отвечаем
        Log::Write("тихий ответ: размышления не уложились — решаю сразу");
        return Quiet(prompt, imageBase64, false);
    }
    return answer;
}

std::string Ai::AskJson(const std::string& instruction, const std::string& request) {
    BusyScope busy;
    EnsureAwake();
    if (!g_app.aiReady) return {};
    const std::string body = "{\"stream\":false,\"temperature\":0.2,\"max_tokens\":1200,\"response_format\":{\"type\":\"json_object\"},"
                             "\"chat_template_kwargs\":{\"enable_thinking\":false},\"messages\":["
                             "{\"role\":\"system\",\"content\":\"" + Json::Escape(instruction) + "\"},"
                             "{\"role\":\"user\",\"content\":\"" + Json::Escape(request) + "\"}]}";
    std::string response;
    const int code = Request(L"POST", L"/v1/chat/completions", body, 120000, [&](const std::string& chunk) { response += chunk; return true; });
    if (code != 200) {
        Log::Write("ответ в JSON: код " + std::to_string(code) + " " + response.substr(0, 200));
        return {};
    }
    const auto content = Json::FindStrings(response, "content", 1);
    return content.empty() ? std::string() : Text::Trim(StripThinking(content[0]));
}

const char* Ai::ProfileName(int index) {
    if (index >= 0 && index < kProfiles - 1) return kPresets[index].name;
    return "Свой";
}

int Ai::Profile() { return std::clamp(AiInt("profile", "0"), 0, kProfiles - 1); }

void Ai::SetProfile(int index) {
    index = std::clamp(index, 0, kProfiles - 1);
    Config::SetValue("ai", "profile", std::to_string(index));
    if (index < kProfiles - 1)
        for (int i = 0; i < 6; ++i) {
            Config::SetValue("ai", Key("chat", kFields[i]).c_str(), std::to_string(kPresets[index].chat[i]));
            Config::SetValue("ai", Key("task", kFields[i]).c_str(), std::to_string(kPresets[index].task[i]));
        }
    ++g_tuningVersion;
}

int Ai::TuningVersion() { return g_tuningVersion.load(); }

void Ai::ResetTuning() {
    SetProfile(0);
    const std::pair<const char*, const char*> kDefaults[] = { { "seed", "-1" }, { "think_mode", "0" }, { "think_seconds", "10" }, { "vision", "1" },
                                      { "threads", "0" }, { "context", kContextSize }, { "parallel", "2" }, { "gpu_layers", "99" }, { "engine_args", "" }, { "extra_prompt", "" } };
    for (const auto& [key, value] : kDefaults)
        Config::SetValue("ai", key, value);
    ++g_tuningVersion;
}

bool Ai::CanSee() {
    if (CloudOn()) return Config::Value("ai", "cloud_vision", "1") == "1"; // большинство облачных моделей видят картинки
    return g_vision && (g_app.aiReady || g_unloaded.load());
}

// ---------------------------------------------------------------- облачный ИИ
namespace {
    std::mutex g_cloudMutex;
    std::string g_cloudStatus;
    bool g_cloudGood = false;
    std::atomic<bool> g_cloudTesting{ false };

    void SetCloudStatus(const std::string& text, bool good) {
        std::lock_guard lock(g_cloudMutex);
        g_cloudStatus = text;
        g_cloudGood = good;
    }
}

bool Ai::CloudOn() { return Config::Value("ai", "cloud", "0") == "1" && CloudReady(); }

void Ai::CloudChanged() {
    if (CloudOn()) {
        std::thread([] { // своя модель больше не нужна — отдаём видеокарту
            std::lock_guard startLock(g_startMutex);
            if (g_process) Sidecar::Stop(g_process);
            g_unloaded = true;
            g_engineCpu = false;
            g_app.aiReady = true;
            g_app.aiChecked = true;
            Log::Write("нейросеть: отвечает облако, своя модель выгружена");
        }).detach();
        return;
    }
    if (g_unloaded.load() || !g_process) std::thread(PrepareLazy).detach(); // снова своя модель: загрузится при первом вопросе
}

void Ai::TestCloud() {
    if (g_cloudTesting.exchange(true)) return;
    if (!CloudReady()) {
        SetCloudStatus(CloudBase().empty() ? "впиши адрес API" : "впиши ключ API", false);
        g_cloudTesting = false;
        return;
    }
    if (Text::Trim(Config::Value("ai", "cloud_model", "")).empty()) {
        SetCloudStatus("впиши название модели", false);
        g_cloudTesting = false;
        return;
    }
    SetCloudStatus("проверяю…", true);
    std::thread([] {
        const std::string body = "{\"stream\":false,\"max_tokens\":16,\"temperature\":0.2,\"messages\":[{\"role\":\"user\",\"content\":\"Скажи одно слово: привет\"}]}";
        std::string response;
        const int code = CloudRequest(body, 30000, [&](const std::string& chunk) { if (response.size() < 20000) response += chunk; return true; });
        if (code == 200) SetCloudStatus("работает — облако ответило", true);
        else if (code == 0) SetCloudStatus("не получилось связаться — проверь адрес и интернет", false);
        else if (code == 401 || code == 403) SetCloudStatus("ключ не подошёл", false);
        else if (code == 404) SetCloudStatus("нет такой модели или адреса", false);
        else if (code == 402 || code == 429) SetCloudStatus("закончились деньги или лимит запросов", false);
        else {
            const auto message = Json::FindStrings(response, "message", 1);
            SetCloudStatus("облако ответило ошибкой" + (message.empty() ? std::string() : ": " + message[0].substr(0, 160)), false);
        }
        g_cloudTesting = false;
    }).detach();
}

std::string Ai::CloudStatus(bool& good) {
    std::lock_guard lock(g_cloudMutex);
    good = g_cloudGood;
    return g_cloudStatus;
}

// ---------------------------------------------------------------- своя модель
namespace {
    std::atomic<bool> g_importing{ false };
    std::atomic<float> g_importProgress{ 0.0f };
    std::mutex g_importMutex;
    std::string g_importStatus;
    bool g_importGood = false;

    void SetImportStatus(const std::string& text, bool good) {
        std::lock_guard lock(g_importMutex);
        g_importStatus = text;
        g_importGood = good;
    }

    struct CopyProgress { uint64_t done = 0, total = 1; };

    DWORD CALLBACK OnCopy(LARGE_INTEGER, LARGE_INTEGER copied, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE, LPVOID data) {
        const auto* p = static_cast<const CopyProgress*>(data);
        g_importProgress = static_cast<float>(static_cast<double>(p->done + static_cast<uint64_t>(copied.QuadPart)) / static_cast<double>(p->total));
        return g_app.running ? PROGRESS_CONTINUE : PROGRESS_CANCEL;
    }

    std::wstring PickGguf() {
        std::wstring chosen;
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE); // окно выбора файла хочет однопоточный COM
        { // COM-объекты должны освободиться до CoUninitialize
            Com<IFileOpenDialog> dialog;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
                const COMDLG_FILTERSPEC filter[] = { { Lang::IsEnglish() ? L"Model (*.gguf)" : L"Модель (*.gguf)", L"*.gguf" } };
                dialog->SetFileTypes(1, filter);
                dialog->SetTitle(Lang::IsEnglish() ? L"Your own model for ZAVIRIA" : L"Своя модель для ZAVIRIA");
                Com<IShellItem> item;
                PWSTR path = nullptr;
                if (SUCCEEDED(dialog->Show(FindWindowW(L"AnyaWindow", nullptr))) && SUCCEEDED(dialog->GetResult(&item)) &&
                    SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    chosen = path;
                    CoTaskMemFree(path);
                }
            }
        }
        CoUninitialize();
        return chosen;
    }

    void ImportBlocking() {
        const std::wstring source = PickGguf();
        if (source.empty()) return;
        const std::wstring name = source.substr(source.find_last_of(L"\\/") + 1);
        if (Text::LowerRu(name).rfind(L"mmproj", 0) == 0) {
            SetImportStatus("это «глаза» модели (mmproj) — выбери саму модель, они скопируются вместе с ней", false);
            return;
        }
        const std::string sourceUtf8 = Text::ToUtf8(source);
        const std::wstring projector = FindProjector(sourceUtf8);
        const std::wstring stem = name.size() > 5 ? name.substr(0, name.size() - 5) : name;
        const std::wstring folder = Text::ExecutableDirectory() + L"\\models\\custom\\" + stem;
        const std::wstring target = folder + L"\\" + name;
        if (Text::LowerRu(source) == Text::LowerRu(target)) { SetImportStatus("эта модель уже в списке", true); return; }

        CopyProgress progress;
        progress.total = FileSize(sourceUtf8) + (projector.empty() ? 0 : FileSize(Text::ToUtf8(projector)));
        if (progress.total == 0) progress.total = 1;
        SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
        ULARGE_INTEGER free{};
        if (GetDiskFreeSpaceExW(folder.c_str(), &free, nullptr, nullptr) && free.QuadPart < progress.total + 200ULL * 1024 * 1024) {
            SetImportStatus("не хватает места на диске", false);
            return;
        }
        SetImportStatus("копирую…", true);
        Log::Write("своя модель: копирую " + Text::ToUtf8(name));
        BOOL cancel = FALSE;
        bool ok = CopyFileExW(source.c_str(), target.c_str(), OnCopy, &progress, &cancel, 0) != FALSE;
        if (ok && !projector.empty()) {
            progress.done = FileSize(sourceUtf8);
            const std::wstring eyes = folder + projector.substr(projector.find_last_of(L"\\/"));
            ok = CopyFileExW(projector.c_str(), eyes.c_str(), OnCopy, &progress, &cancel, 0) != FALSE;
        }
        if (!ok) {
            Log::Write("своя модель: не скопировалась, ошибка " + std::to_string(GetLastError()));
            DeleteFileW(target.c_str());
            SetImportStatus("не получилось скопировать", false);
            return;
        }
        {
            std::lock_guard lock(g_mutex);
            g_models = DiscoverModels();
        }
        g_importProgress = 1.0f;
        SetImportStatus(projector.empty() ? "готово — модель появилась в списке" : "готово — модель и её «глаза» появились в списке", true);
    }
}

void Ai::ImportModel() {
    if (g_importing.exchange(true)) return;
    g_importProgress = 0.0f;
    std::thread([] {
        ImportBlocking();
        g_importing = false;
    }).detach();
}

bool Ai::Importing(float& progress) {
    progress = g_importProgress.load();
    return g_importing.load();
}

std::string Ai::ImportStatus(bool& good) {
    std::lock_guard lock(g_importMutex);
    good = g_importGood;
    return g_importStatus;
}


std::vector<std::string> Ai::Models() {
    std::lock_guard lock(g_mutex);
    return g_models;
}

std::string Ai::ActiveModel() {
    std::lock_guard lock(g_mutex);
    return FileName(g_activeModel);
}
