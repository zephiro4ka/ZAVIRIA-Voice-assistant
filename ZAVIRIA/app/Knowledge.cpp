#include "Knowledge.h"

#include "Platform.h"
#include "Fuzzy.h"
#include "Http.h"
#include "Json.h"
#include "Lang.h"
#include "LocalAi.h"
#include "Log.h"
#include "Settings.h"
#include "State.h"
#include "TextUtils.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <initializer_list>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <sstream>
#include <string_view>
#include <vector>

// Файлы рядом с программой (%LOCALAPPDATA%\ZAVIRIA):
//   knowledge.tsv — что Аня узнала сама: «вид \t ключ \t ответ \t источники \t день записи \t день последнего ответа».
//                   Не больше kMaxBaseBytes: при переполнении забываются записи, которые дольше всех не спрашивали.
//   dict.tsv      — словарь «e|r \t слово \t перевод; перевод» (WikDict, CC BY-SA), строки по алфавиту. Его может и не быть.

namespace {
    constexpr size_t kMaxBaseBytes = 4u << 20; // 4 МБ — десятки тысяч ответов
    constexpr int kFactDays = 30;              // факты о новом устаревают: через месяц ищем заново
    constexpr float kSame = 0.86f;             // «похоже» для нечёткого совпадения

    std::string L(const char* ru, const char* en) { return Lang::IsEnglish() ? en : ru; }
    int Today() { return static_cast<int>(std::time(nullptr) / 86400); }
    std::filesystem::path Here(const wchar_t* file) { return std::filesystem::path(Text::ExecutableDirectory()) / file; }

    bool HasCyrillic(const std::wstring& w) { return std::any_of(w.begin(), w.end(), [](wchar_t c) { return c >= 0x400 && c <= 0x4FF; }); }
    bool HasLatin(const std::wstring& w) { return std::any_of(w.begin(), w.end(), [](wchar_t c) { return (c | 32) >= 'a' && (c | 32) <= 'z'; }); }

    // Нижний регистр, только буквы, цифры, дефис и пробелы: «Переведи, пожалуйста, „Bank“!» → «переведи пожалуйста bank»
    std::wstring Simplify(const std::string& text) {
        std::wstring out;
        for (wchar_t c : Text::LowerRu(Text::ToWide(text))) {
            const bool keep = IsCharAlphaNumericW(c) || ((c == L'-' || c == L'\'' || c == L'+' || c == L'.') && !out.empty() && out.back() != L' ');
            if (keep) out.push_back(c);
            else if (!out.empty() && out.back() != L' ') out.push_back(L' ');
        }
        while (!out.empty() && (out.back() == L' ' || out.back() == L'-' || out.back() == L'.')) out.pop_back();
        return out;
    }
    std::string Key(const std::string& text) { return Text::ToUtf8(Simplify(text)); }
    std::string NoTabs(std::string s) { for (char& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' '; return s; }
    bool Contains(const std::wstring& padded, const wchar_t* part) { return padded.find(part) != std::wstring::npos; }

    // ------------------------------------------------------------ своя база
    struct Entry { char kind = 'f'; std::string key, answer, sources; int saved = 0, used = 0; };
    std::mutex g_mutex;
    std::vector<Entry> g_base;
    size_t g_baseBytes = 0;
    bool g_loaded = false;

    size_t LineBytes(const Entry& e) { return e.key.size() + e.answer.size() + e.sources.size() + 24; }

    void LoadBase() { // под g_mutex
        if (g_loaded) return;
        g_loaded = true;
        std::ifstream in(Here(L"knowledge.tsv"), std::ios::binary);
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::vector<std::string> f;
            std::stringstream parts(line);
            for (std::string part; std::getline(parts, part, '\t');) f.push_back(part);
            if (f.size() < 6 || f[0].size() != 1) continue;
            g_base.push_back({ f[0][0], f[1], f[2], f[3], std::atoi(f[4].c_str()), std::atoi(f[5].c_str()) });
            g_baseBytes += LineBytes(g_base.back());
        }
    }

    void SaveBase() { // под g_mutex. Переполнение — забываем то, что дольше всех не спрашивали
        while (g_baseBytes > kMaxBaseBytes && !g_base.empty()) {
            const auto oldest = std::min_element(g_base.begin(), g_base.end(), [](const Entry& a, const Entry& b) { return a.used < b.used; });
            g_baseBytes -= LineBytes(*oldest);
            g_base.erase(oldest);
        }
        const auto file = Here(L"knowledge.tsv"), temp = Here(L"knowledge.tsv.tmp");
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            for (const Entry& e : g_base)
                out << e.kind << '\t' << e.key << '\t' << e.answer << '\t' << e.sources << '\t' << e.saved << '\t' << e.used << '\n';
            if (!out) return;
        }
        std::error_code ec;
        std::filesystem::rename(temp, file, ec);
    }

    // Точное совпадение ключа, а если его нет — самое похожее (≥ kSame). Свежие факты — не старше kFactDays.
    std::optional<Entry> Find(char kind, const std::string& key) {
        std::lock_guard lock(g_mutex);
        LoadBase();
        const std::wstring wkey = Text::ToWide(key);
        Entry* best = nullptr;
        float bestScore = kSame;
        for (Entry& e : g_base) {
            if (e.kind != kind || (kind == 'f' && Today() - e.saved > kFactDays)) continue;
            if (e.key == key) { best = &e; break; }
            const std::wstring other = Text::ToWide(e.key);
            if (std::abs(static_cast<int>(other.size()) - static_cast<int>(wkey.size())) > 3) continue;
            if (const float score = Fuzzy::Similarity(wkey, other); score >= bestScore) { bestScore = score; best = &e; }
        }
        if (!best) return std::nullopt;
        if (best->used != Today()) { best->used = Today(); SaveBase(); }
        return *best;
    }

    void Put(char kind, const std::string& key, const std::string& answer, const std::string& sources = "") {
        if (key.empty() || answer.empty() || key.size() > 300) return;
        std::lock_guard lock(g_mutex);
        LoadBase();
        Entry fresh{ kind, NoTabs(key), NoTabs(answer), NoTabs(sources), Today(), Today() };
        const auto old = std::find_if(g_base.begin(), g_base.end(), [&](const Entry& e) { return e.kind == kind && e.key == fresh.key; });
        if (old != g_base.end()) { g_baseBytes -= LineBytes(*old); g_base.erase(old); }
        g_baseBytes += LineBytes(fresh);
        g_base.push_back(std::move(fresh));
        SaveBase();
        Log::Write(std::string("база знаний: запомнила «") + key + "»");
    }

    // ------------------------------------------------------------ словарь
    std::string g_dict;                // весь dict.tsv
    std::vector<uint32_t> g_en, g_ru;  // начала строк «e\t…» и «r\t…» (по алфавиту)
    std::once_flag g_dictOnce;

    void LoadDictionary() {
        std::call_once(g_dictOnce, [] {
            std::ifstream in(Here(L"dict.tsv"), std::ios::binary);
            g_dict.assign(std::istreambuf_iterator<char>(in), {});
            for (size_t at = 0; at + 2 < g_dict.size();) {
                const size_t end = g_dict.find('\n', at);
                if (g_dict[at + 1] == '\t') (g_dict[at] == 'e' ? g_en : g_ru).push_back(static_cast<uint32_t>(at));
                if (end == std::string::npos) break;
                at = end + 1;
            }
            if (!g_dict.empty()) Log::Write("словарь: " + std::to_string(g_en.size() + g_ru.size()) + " слов");
        });
    }
    std::string_view DictWord(uint32_t at) { return std::string_view(g_dict).substr(at + 2, g_dict.find('\t', at + 2) - at - 2); }
    std::string DictValue(uint32_t at) {
        const size_t start = g_dict.find('\t', at + 2) + 1, end = g_dict.find_first_of("\r\n", start);
        return g_dict.substr(start, end == std::string::npos ? std::string::npos : end - start);
    }

    // Перевод из словаря; guessed — если слово нашлось только похожее (оговорка распознавания или опечатка)
    std::optional<std::string> FromDictionary(const std::string& key, bool fromRussian, std::string& guessed) {
        LoadDictionary();
        const auto& index = fromRussian ? g_ru : g_en;
        const auto it = std::lower_bound(index.begin(), index.end(), key, [](uint32_t at, const std::string& k) { return DictWord(at) < k; });
        if (it != index.end() && DictWord(*it) == key) return DictValue(*it);
        const std::wstring wkey = Text::ToWide(key);
        if (wkey.size() < 4 || key.find(' ') != std::string::npos) return std::nullopt;
        uint32_t best = 0;
        float bestScore = kSame;
        for (uint32_t at : index) {
            const std::string_view word = DictWord(at);
            if (word.size() + 4 < key.size() || word.size() > key.size() + 4) continue; // байты: быстрый отсев
            const float score = Fuzzy::Similarity(wkey, Text::ToWide(std::string(word)));
            if (score > bestScore) { bestScore = score; best = at; }
        }
        if (bestScore <= kSame) return std::nullopt;
        guessed = std::string(DictWord(best));
        return DictValue(best);
    }

    // ------------------------------------------------------------ перевод в интернете (MyMemory, без ключа)
    std::vector<std::string> WebTranslations(const std::string& phrase, bool toEnglish) {
        const std::string body = Http::Get("https://api.mymemory.translated.net/get?q=" + Text::UrlEncode(phrase) +
                                           (toEnglish ? "&langpair=ru%7Cen" : "&langpair=en%7Cru"), 8000);
        auto found = Json::FindStrings(body, "translatedText", 1);
        for (auto& more : Json::FindStrings(body, "translation", 20)) found.push_back(std::move(more));
        std::vector<std::string> out;
        std::vector<std::string> keys{ Key(phrase) };
        for (std::string& text : found) {
            text = Text::Trim(text);
            if (phrase.find('.') == std::string::npos && !text.empty() && text.back() == '.') text.pop_back();
            const std::wstring wide = Text::ToWide(text);
            if (text.empty() || text.find("MYMEMORY") != std::string::npos || text.size() > 400) continue;
            if (toEnglish ? HasCyrillic(wide) : !HasCyrillic(wide)) continue; // перевод — на другом языке
            const std::string key = Key(text);
            if (std::find(keys.begin(), keys.end(), key) != keys.end()) continue;
            keys.push_back(key);
            out.push_back(Text::Trim(text));
            if (out.size() >= 6) break;
        }
        return out;
    }

    std::string Join(const std::vector<std::string>& items, size_t limit, const char* separator) {
        std::string out;
        for (size_t i = 0; i < items.size() && i < limit; ++i) out += (i ? separator : "") + items[i];
        return out;
    }

    // «банк; берег; запас» → «банк, берег, запас» (вслух — не больше трёх)
    std::string Spoken(const std::string& stored) {
        std::vector<std::string> items;
        std::stringstream parts(stored);
        for (std::string part; std::getline(parts, part, ';');) if (!Text::Trim(part).empty()) items.push_back(Text::Trim(part));
        return Join(items, 3, ", ");
    }

    // ------------------------------------------------------------ разбор просьбы о переводе
    struct Translation { std::string phrase; bool toEnglish = true; };

    std::wstring StripEdges(std::wstring s, std::initializer_list<const wchar_t*> front, std::initializer_list<const wchar_t*> back) {
        for (bool changed = true; changed;) {
            changed = false;
            s = L" " + Text::ToWide(Text::Trim(Text::ToUtf8(s))) + L" ";
            for (const wchar_t* w : front)
                if (s.rfind(std::wstring(L" ") + w + L" ", 0) == 0) { s = s.substr(wcslen(w) + 1); changed = true; }
            for (const wchar_t* w : back) {
                const std::wstring tail = std::wstring(L" ") + w + L" ";
                if (s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0) { s.resize(s.size() - tail.size() + 1); changed = true; }
            }
        }
        return Text::ToWide(Text::Trim(Text::ToUtf8(s)));
    }

    std::optional<Translation> ParseTranslation(const std::string& command) {
        std::wstring w = L" " + Simplify(command) + L" ";
        for (const wchar_t* other : { L"немец", L"француз", L"испан", L"итальян", L"китай", L"япон", L"корей", L"german", L"french", L"spanish", L"chinese", L"japanese" })
            if (Contains(w, other)) return std::nullopt; // другие языки — к нейросети

        int target = 0; // 1 — на английский, -1 — на русский
        for (const wchar_t* m : { L" по-английски ", L" по английски ", L" на английский язык ", L" на английский ", L" на английском языке ", L" на английском ",
                                  L" in english ", L" into english ", L" to english ", L" с русского " })
            for (size_t at; (at = w.find(m)) != std::wstring::npos;) { w.replace(at, wcslen(m), L" "); target = 1; }
        for (const wchar_t* m : { L" по-русски ", L" по русски ", L" на русский язык ", L" на русский ", L" на русском языке ", L" на русском ",
                                  L" in russian ", L" into russian ", L" to russian ", L" с английского " })
            for (size_t at; (at = w.find(m)) != std::wstring::npos;) { w.replace(at, wcslen(m), L" "); target = -1; }

        bool asked = false, meaning = false;
        for (const wchar_t* verb : { L" как переводится ", L" переведи ", L" перевести ", L" перевод слова ", L" перевод фразы ", L" перевод ", L" как будет ",
                                     L" как сказать ", L" как пишется ", L" translate ", L" how do you say ", L" how to say ", L" what's ", L" what is " }) {
            const size_t at = w.find(verb);
            if (at == std::wstring::npos) continue;
            const bool question = wcscmp(verb, L" what's ") == 0 || wcscmp(verb, L" what is ") == 0;
            if (question && !target) continue; // «what is …» — перевод, только если назван язык
            w = L" " + w.substr(at + wcslen(verb));
            asked = true;
            break;
        }
        if (!asked)
            for (const wchar_t* verb : { L" что значит ", L" что означает ", L" what does " }) {
                const size_t at = w.find(verb);
                if (at == std::wstring::npos) continue;
                w = L" " + w.substr(at + wcslen(verb));
                asked = meaning = true;
                break;
            }
        if (!asked && target && w.rfind(L" как ", 0) == 0) { w = w.substr(4); asked = true; } // «как по-английски кошка»
        if (!asked) return std::nullopt;

        w = StripEdges(w, { L"пожалуйста", L"мне", L"слово", L"слова", L"фразу", L"фраза", L"выражение", L"будет", L"по", L"а", L"ну", L"the word", L"the phrase", L"word", L"please" },
                       { L"пожалуйста", L"будет", L"значит", L"означает", L"переводится", L"please", L"mean", L"means", L"in" });
        if (w.empty() || std::count(w.begin(), w.end(), L' ') > 11) return std::nullopt;
        if (meaning && (Lang::IsEnglish() ? !HasCyrillic(w) : !HasLatin(w))) return std::nullopt; // «что значит эмпатия» — объяснение, не перевод
        const bool toEnglish = target ? target > 0 : !HasLatin(w);
        return Translation{ Text::ToUtf8(w), toEnglish };
    }

    std::string TranslationReply(const Translation& t, const std::string& stored, const std::string& guessed = "") {
        const std::string said = guessed.empty() ? t.phrase : guessed;
        std::string reply = guessed.empty() ? "" : L("Наверное, ты про «", "I think you mean \"") + guessed + L("». ", "\". ");
        if (Lang::IsEnglish()) return reply + "\"" + said + "\" in " + (t.toEnglish ? "English" : "Russian") + " is " + Spoken(stored);
        return reply + "«" + said + "» " + (t.toEnglish ? "по-английски" : "по-русски") + " — " + Spoken(stored);
    }

    // ------------------------------------------------------------ что ответили последним — для «не то»
    struct Last { char kind = 0; std::string key, question; bool toEnglish = true; Clock at = 0; };
    Last g_last;
    bool g_awaiting = false; // спросили «что именно?» — следующая фраза будет уточнением

    void Remember(char kind, const std::string& key, const std::string& question, bool toEnglish = true) {
        g_last = { kind, key, question, toEnglish, NowMs() };
        g_awaiting = false;
    }

    std::optional<std::string> Translate(const Translation& t) {
        const char kind = t.toEnglish ? 'r' : 'e'; // по языку исходного слова, как в словаре
        const std::string key = Key(t.phrase);
        Remember(kind, key, t.phrase, t.toEnglish);
        if (auto known = Find(kind, key)) {
            Log::Write("перевод «" + key + "» → из базы знаний");
            return TranslationReply(t, known->answer, known->key == key ? "" : known->key);
        }
        std::string guessed;
        if (auto word = FromDictionary(key, t.toEnglish, guessed)) {
            Log::Write("перевод «" + key + "» → из словаря" + (guessed.empty() ? "" : " (похожее: " + guessed + ")"));
            if (!guessed.empty()) g_last.key = guessed;
            return TranslationReply(t, *word, guessed);
        }
        Caption::Set(L("ищу перевод в интернете…", "looking up the translation…"), true);
        const auto found = WebTranslations(t.phrase, t.toEnglish);
        if (found.empty()) return L("Не нашла перевод «", "I couldn't find a translation for \"") + t.phrase + L("» ни в словаре, ни в интернете", "\" in the dictionary or online");
        const std::string stored = Join(found, 4, "; ");
        Log::Write("перевод «" + key + "» → из интернета: " + stored);
        Put(kind, key, stored, "mymemory.translated.net");
        return TranslationReply(t, stored);
    }

    // ------------------------------------------------------------ факты из интернета
    struct Intent { const char* ru; const char* en; std::vector<const wchar_t*> hints; };

    // О чём вопрос: от этого — что дописать в поисковый запрос и какие слова искать в текстах
    Intent IntentOf(const std::wstring& w) {
        const auto any = [&](std::initializer_list<const wchar_t*> parts) { return std::any_of(parts.begin(), parts.end(), [&](const wchar_t* p) { return Contains(w, p); }); };
        if (any({ L"весит", L"весить", L"вес игры", L"размер", L"сколько места", L"гигабайт", L"how big", L"how much space", L"file size", L"download size", L"storage", L"how many gb" }))
            return { "системные требования", "PC storage size", { L"гб", L"gb", L"гигабайт", L"gigabyte", L"storage", L"места", L"ssd", L"tb", L"тб" } };
        if (any({ L"требовани", L"requirement", L"specs" }))
            return { "системные требования", "system requirements", { L"гб", L"gb", L"rtx", L"gtx", L"radeon", L"озу", L"ram", L"процессор", L"cpu", L"i5", L"i7", L"ryzen" } };
        if (any({ L"когда выйдет", L"когда выход", L"дата выхода", L"релиз", L"вышла ли", L"вышел ли", L"выйдет ли", L"release", L"come out", L"coming out", L"launch" }))
            return { "дата выхода", "release date", { L"выход", L"выйдет", L"вышл", L"релиз", L"release", L"launch", L"202" } };
        if (any({ L"сколько стоит", L"цена", L"стоимост", L"how much does", L"price", L"cost" }))
            return { "цена", "price", { L"руб", L"$", L"€", L"цен", L"стоит", L"price", L"usd", L"бесплат", L"free" } };
        if (any({ L"верси", L"обновлени", L"патч", L"version", L"update", L"patch" }))
            return { "обновление", "update", { L"верси", L"version", L"обновлен", L"update", L"patch", L"патч" } };
        return { "", "", {} };
    }

    std::string NameOf(const std::string& question);

    bool IsFactQuestion(const std::wstring& w, const std::string& original) {
        const auto any = [&](std::initializer_list<const wchar_t*> parts) { return std::any_of(parts.begin(), parts.end(), [&](const wchar_t* p) { return Contains(w, p); }); };
        if (any({ L" узнай в интернете ", L" проверь в интернете ", L" посмотри в интернете ", L" поищи в интернете и скажи ", L" что пишут ",
                  L" search the internet ", L" look it up ", L" check online ", L" look up " })) return true;
        if (any({ L" напиши ", L" реши ", L" код ", L" открой ", L" включи ", L" запусти ", L" посчитай ", L" write ", L" solve ", L" open " })) return false;
        const bool question = any({ L" сколько ", L" когда ", L" какой ", L" какая ", L" какие ", L" какое ", L" кто ", L" вышел ", L" вышла ", L" выйдет ", L" есть ли ",
                                    L" how ", L" when ", L" what ", L" which ", L" who ", L" is ", L" does ", L" did " });
        if (!question) return false;
        if (NameOf(original).empty()) return false; // без названия искать в интернете нечего — ответит нейросеть
        return *IntentOf(w).ru || any({ L" что нового ", L" новост", L" последн", L" анонс", L" вышл", L" выйд", L" news ", L" latest ", L" new " });
    }

    bool IsQuestionWord(const std::wstring& w) {
        for (const wchar_t* q : { L"сколько", L"когда", L"какой", L"какая", L"какие", L"какое", L"будет", L"есть", L"что", L"кто", L"где", L"как", L"про",
                                  L"для", L"это", L"how", L"what", L"when", L"who", L"the", L"does", L"will" })
            if (w == q) return true;
        return false;
    }

    struct Source { std::string site, url, text; };

    std::string Site(const std::string& url) {
        size_t start = url.find("://");
        start = start == std::string::npos ? 0 : start + 3;
        std::string host = url.substr(start, url.find('/', start) - start);
        if (host.rfind("www.", 0) == 0) host = host.substr(4);
        return host;
    }

    std::string UrlDecode(const std::string& s) {
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size()) { out.push_back(static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16))); i += 2; }
            else out.push_back(s[i] == '+' ? ' ' : s[i]);
        }
        return out;
    }

    // HTML → текст: без скриптов, стилей и тегов, с расшифровкой &amp; &#1234; и т.п. Старые сайты в windows-1251 — перекодируем.
    std::string HtmlToText(std::string html) {
        if (html.size() > 800'000) html.resize(800'000);
        const std::string head = Text::ToUtf8(Text::LowerRu(Text::ToWide(html.substr(0, 3000))));
        if (head.find("windows-1251") != std::string::npos) {
            std::wstring wide(html.size(), L'\0');
            wide.resize(MultiByteToWideChar(1251, 0, html.data(), static_cast<int>(html.size()), wide.data(), static_cast<int>(wide.size())));
            html = Text::ToUtf8(wide);
        }
        std::string out;
        for (size_t i = 0; i < html.size();) {
            if (html[i] == '<') {
                const size_t end = html.find('>', i);
                if (end == std::string::npos) break;
                std::string tag = html.substr(i + 1, std::min<size_t>(end - i - 1, 10));
                for (char& c : tag) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                for (const char* skip : { "script", "style", "noscript", "svg", "nav", "footer" })
                    if (tag.rfind(skip, 0) == 0) {
                        const size_t close = html.find(std::string("</") + skip, end);
                        i = close == std::string::npos ? html.size() : close;
                        break;
                    }
                if (i < html.size() && html[i] == '<') {
                    const size_t close = html.find('>', i);
                    i = close == std::string::npos ? html.size() : close + 1;
                }
                out += (tag.rfind("p", 0) == 0 || tag.rfind("br", 0) == 0 || tag.rfind("li", 0) == 0 || tag.rfind("h", 0) == 0 || tag.rfind("div", 0) == 0 ||
                        tag.rfind("td", 0) == 0 || tag.rfind("/", 0) == 0) ? "\n" : " ";
                continue;
            }
            if (html[i] == '&') {
                const size_t end = html.find(';', i);
                if (end != std::string::npos && end - i < 10) {
                    const std::string name = html.substr(i + 1, end - i - 1);
                    wchar_t ch = 0;
                    if (!name.empty() && name[0] == '#') ch = static_cast<wchar_t>(name.size() > 1 && (name[1] == 'x' || name[1] == 'X') ? std::strtol(name.c_str() + 2, nullptr, 16) : std::atoi(name.c_str() + 1));
                    else for (auto [n, c] : { std::pair{ "amp", L'&' }, { "lt", L'<' }, { "gt", L'>' }, { "quot", L'"' }, { "apos", L'\'' }, { "nbsp", L' ' },
                                              { "laquo", L'«' }, { "raquo", L'»' }, { "mdash", L'—' }, { "ndash", L'–' }, { "hellip", L'…' }, { "thinsp", L' ' } })
                        if (name == n) ch = c;
                    if (ch) { out += Text::ToUtf8(std::wstring(1, ch)); i = end + 1; continue; }
                }
            }
            out.push_back(html[i] == '\t' || html[i] == '\r' ? ' ' : html[i]);
            ++i;
        }
        return out;
    }

    // Самые нужные предложения источника: где есть название и слова о сути вопроса. Пусто — источник не о том.
    std::string Excerpt(const std::string& text, const std::vector<std::wstring>& names, const Intent& intent) {
        const std::wstring all = Text::LowerRu(Text::ToWide(text));
        const bool about = std::any_of(names.begin(), names.end(), [&](const std::wstring& n) { return all.find(n) != std::wstring::npos; });
        if (!about) return {};
        struct Piece { size_t order; int score; std::string text; };
        std::vector<Piece> pieces;
        std::string sentence;
        const auto flush = [&] {
            const std::string s = Text::Trim(sentence);
            sentence.clear();
            if (s.size() < 25 || s.size() > 500) return;
            const std::wstring lw = Text::LowerRu(Text::ToWide(s));
            int score = 0;
            for (const auto& n : names) if (lw.find(n) != std::wstring::npos) score += 1;
            for (const wchar_t* h : intent.hints) if (lw.find(h) != std::wstring::npos) score += 2;
            if (score > 0) pieces.push_back({ pieces.size(), score, s });
        };
        for (size_t i = 0; i < text.size(); ++i) {
            const char c = text[i];
            if (c == '\n') { flush(); continue; }
            sentence.push_back(c);
            if ((c == '.' || c == '!' || c == '?') && i + 1 < text.size() && text[i + 1] == ' ') flush();
        }
        flush();
        std::stable_sort(pieces.begin(), pieces.end(), [](const Piece& a, const Piece& b) { return a.score > b.score; });
        if (pieces.empty() || (!intent.hints.empty() && pieces[0].score < 2)) return {}; // о названии есть, а о сути вопроса — ни слова
        pieces.resize(std::min<size_t>(pieces.size(), 4));
        std::sort(pieces.begin(), pieces.end(), [](const Piece& a, const Piece& b) { return a.order < b.order; });
        std::string out;
        for (const auto& p : pieces) if (out.size() + p.text.size() < 900) out += (out.empty() ? "" : " … ") + p.text;
        return out;
    }

    Source Wikipedia(const char* lang, const std::string& query) {
        const std::string host = std::string("https://") + lang + ".wikipedia.org/w/api.php?format=json&formatversion=2&utf8=1&action=query";
        const auto titles = Json::FindStrings(Http::Get(host + "&list=search&srlimit=1&srsearch=" + Text::UrlEncode(query)), "title", 1);
        if (titles.empty()) return {};
        const auto text = Json::FindStrings(Http::Get(host + "&prop=extracts&explaintext=1&redirects=1&titles=" + Text::UrlEncode(titles[0])), "extract", 1);
        return { std::string(lang) + ".wikipedia.org", "https://" + std::string(lang) + ".wikipedia.org/wiki/" + Text::UrlEncode(titles[0]), text.empty() ? "" : text[0] };
    }

    // Новости по запросу (RSS Bing News): заголовок и анонс, адрес статьи у самого издания
    std::vector<Source> News(const std::string& query, const char* market) {
        if (Text::Trim(query).empty()) return {};
        const std::string rss = Http::Get("https://www.bing.com/news/search?format=rss&mkt=" + std::string(market) + "&q=" + Text::UrlEncode(query));
        std::vector<Source> out;
        const auto between = [](const std::string& s, const char* a, const char* b) {
            const size_t start = s.find(a);
            if (start == std::string::npos) return std::string();
            const size_t from = start + strlen(a), end = s.find(b, from);
            return s.substr(from, end == std::string::npos ? std::string::npos : end - from);
        };
        for (size_t at = rss.find("<item>"); at != std::string::npos; at = rss.find("<item>", at + 6)) {
            const std::string item = rss.substr(at, rss.find("</item>", at) - at);
            std::string link = HtmlToText(between(item, "<link>", "</link>"));
            if (const size_t u = link.find("url="); u != std::string::npos) link = UrlDecode(link.substr(u + 4, link.find('&', u) - u - 4));
            if (link.rfind("http", 0) != 0) continue;
            out.push_back({ Site(link), link, HtmlToText(between(item, "<title>", "</title>")) + ". " + HtmlToText(between(item, "<description>", "</description>")) + "\n" });
        }
        return out;
    }

    // Название, о котором вопрос: слова латиницей («ANANTA», «GTA 6») или с заглавной буквы не в начале фразы («Ведьмак 4»).
    // По нему ищем и проверяем, что источник о том же. Пусто — названия нет.
    std::string NameOf(const std::string& question) {
        static const wchar_t* kCommon[] = { L"how", L"what", L"when", L"is", L"the", L"a", L"of", L"does", L"did", L"big", L"much", L"space", L"size", L"file",
            L"download", L"release", L"date", L"come", L"out", L"price", L"cost", L"latest", L"version", L"update", L"will", L"be", L"take", L"on", L"pc",
            L"system", L"requirements", L"news", L"about", L"search", L"internet", L"look", L"up", L"check", L"online", L"for", L"many", L"gb", L"it", L"in",
            L"which", L"who", L"there", L"weigh", L"storage", L"need", L"do", L"i", L"you", L"tell", L"me", L"please", L"game", L"new", L"coming", L"launch",
            L"anya", L"is", L"are", L"has", L"have", L"come", L"comes", L"out", L"specs" };
        std::string out;
        std::wstringstream in(Text::ToWide(question));
        bool first = true;
        for (std::wstring raw; in >> raw; first = false) {
            std::wstring word;
            for (wchar_t c : raw) if (IsCharAlphaNumericW(c) || c == L'-') word.push_back(c);
            if (word.empty()) continue;
            const std::wstring lower = Text::LowerRu(word);
            const bool latin = HasLatin(word) && !HasCyrillic(word);
            const bool capital = !first && IsCharUpperW(word[0]) && HasCyrillic(word);
            const bool number = iswdigit(word[0]) && !out.empty();
            if (!(latin || capital || number)) continue;
            if (latin && std::any_of(std::begin(kCommon), std::end(kCommon), [&](const wchar_t* c) { return lower == c; })) continue;
            if (Text::Contains(Text::ToUtf8(word), Config::Get().assistant.c_str())) continue; // «Аня»
            out += (out.empty() ? "" : " ") + Text::ToUtf8(word);
        }
        return out;
    }

    // Вопрос без служебных слов — для поиска и для ключа в базе
    std::string QuestionCore(const std::string& question) {
        return Text::ToUtf8(StripEdges(Simplify(question), { L"аня", L"скажи", L"подскажи", L"пожалуйста", L"а", L"узнай", L"в интернете", L"проверь", L"посмотри",
                                                             L"tell me", L"please", L"search the internet", L"look up", L"check online" },
                                       { L"пожалуйста", L"please" }));
    }

    std::optional<std::string> AnswerFact(const std::string& question, const std::string& saveKey) {
        const std::wstring w = L" " + Simplify(question) + L" ";
        const Intent intent = IntentOf(w);
        const std::string name = NameOf(question), core = QuestionCore(question);
        std::vector<std::wstring> names;
        {
            std::wstringstream in(Text::LowerRu(Text::ToWide(name.empty() ? core : name)));
            for (std::wstring word; in >> word;)
                if (word.size() >= 3 && (!name.empty() || !(IntentOf(L" " + word + L" ").ru[0] || IsQuestionWord(word))))
                    names.push_back(name.empty() ? word.substr(0, std::min<size_t>(word.size(), 5)) : word); // русские слова — по основе
        }
        if (names.empty()) return std::nullopt;

        Caption::Set(L("ищу в интернете и сверяю источники…", "searching the web and comparing sources…"), true);
        g_app.thinking = true;
        const std::string ruQuery = name.empty() ? core : name + " " + intent.ru, enQuery = name.empty() ? "" : name + " " + intent.en;
        auto wikiEn = std::async(std::launch::async, Wikipedia, "en", name.empty() ? core : enQuery);
        auto wikiRu = std::async(std::launch::async, Wikipedia, "ru", name.empty() ? core : name);
        auto newsRu = std::async(std::launch::async, News, ruQuery, "ru-RU");
        auto newsEn = std::async(std::launch::async, News, enQuery, "en-US");

        // из новостей — по одной статье с каждого сайта, по очереди русские и английские, всего до пяти
        std::vector<Source> candidates;
        {
            const auto ru = newsRu.get(), en = newsEn.get();
            std::vector<std::string> sites;
            for (size_t i = 0; i < std::max(ru.size(), en.size()) && candidates.size() < 5; ++i)
                for (const auto* list : { &ru, &en })
                    if (i < list->size() && candidates.size() < 5 && std::find(sites.begin(), sites.end(), (*list)[i].site) == sites.end()) {
                        sites.push_back((*list)[i].site);
                        candidates.push_back((*list)[i]);
                    }
        }
        std::vector<std::future<std::string>> pages;
        for (const auto& c : candidates) pages.push_back(std::async(std::launch::async, [url = c.url] { return HtmlToText(Http::Get(url, 7000)); }));
        for (size_t i = 0; i < candidates.size(); ++i) candidates[i].text += pages[i].get();
        candidates.push_back(wikiEn.get());
        candidates.push_back(wikiRu.get());

        std::vector<Source> sources;
        for (auto& c : candidates)
            if (std::string excerpt = Excerpt(c.text, names, intent); !excerpt.empty() && sources.size() < 6) sources.push_back({ c.site, c.url, excerpt });
        Log::Write("поиск фактов «" + core + "»: источников с ответом " + std::to_string(sources.size()) + " из " + std::to_string(candidates.size()));
        g_app.thinking = false;
        if (sources.empty()) return L("Поискала в интернете, но надёжного ответа не нашла", "I searched online but couldn't find a reliable answer");

        std::string sites, excerpts;
        for (size_t i = 0; i < sources.size(); ++i) {
            if (sites.find(sources[i].site) == std::string::npos) sites += (sites.empty() ? "" : ", ") + sources[i].site;
            excerpts += "[" + std::to_string(i + 1) + "] " + sources[i].site + ": " + sources[i].text + "\n";
        }
        if (!g_app.aiReady) // без нейросети — честно читаем лучший отрывок
            return L("Нейросеть ещё не готова, поэтому просто прочитаю, что пишет ", "The AI isn't ready, so here's what ") + sources[0].site +
                   L(": ", " says: ") + sources[0].text.substr(0, 400);

        g_app.thinking = true;
        char today[16];
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &now);
        std::strftime(today, sizeof(today), "%Y-%m-%d", &local);
        const std::string language = Lang::IsEnglish() ? "in English" : "по-русски";
        const std::string json = Ai::AskJson(
            "Ты проверяешь факты по выдержкам из нескольких источников. Отвечай только по этим выдержкам, ничего не добавляй от себя. "
            "Сравни, что пишут разные источники, и учти дату: более новые данные важнее. Верни JSON: "
            "{\"answer\": \"короткий ответ на вопрос в 1–2 предложениях " + language + ", с точными числами из источников\", "
            "\"agree\": \"yes\" — если два или больше источника подтверждают одно и то же; \"no\" — если источники называют разные значения "
            "(тогда в answer перечисли значения и какие источники что пишут); \"single\" — если ответ есть только в одном источнике; "
            "\"none\" — если ответа в выдержках нет, \"sources\": [номера источников, на которых основан ответ]}",
            "Сегодня " + std::string(today) + ". Вопрос: " + core + "\n\n" + excerpts);
        g_app.thinking = false;
        const auto answers = Json::FindStrings(json, "answer", 1), agrees = Json::FindStrings(json, "agree", 1);
        std::string answer = answers.empty() ? "" : Text::Trim(answers[0]);
        const std::string agree = agrees.empty() ? "none" : agrees[0];
        for (size_t at; (at = answer.find('[')) != std::string::npos;) { // «источник [2]» → «goha.ru»: так понятнее и вслух
            const size_t end = answer.find(']', at);
            if (end == std::string::npos) break;
            const int n = std::atoi(answer.c_str() + at + 1);
            answer.replace(at, end - at + 1, n >= 1 && n <= static_cast<int>(sources.size()) ? sources[n - 1].site : "");
        }
        Log::Write("поиск фактов: сверка «" + agree + "» — " + answer);

        std::string cited; // сайты, на которые опирается ответ (номера из «sources»)
        if (const size_t at = json.find("\"sources\""); at != std::string::npos)
            for (size_t i = at, end = json.find(']', at); i < end && end != std::string::npos; ++i)
                if (isdigit(static_cast<unsigned char>(json[i]))) {
                    const int n = std::atoi(json.c_str() + i);
                    while (i + 1 < end && isdigit(static_cast<unsigned char>(json[i + 1]))) ++i;
                    if (n >= 1 && n <= static_cast<int>(sources.size()) && cited.find(sources[n - 1].site) == std::string::npos)
                        cited += (cited.empty() ? "" : ", ") + sources[n - 1].site;
                }
        if (cited.empty()) cited = sites;

        // первая строка — то, что Аня говорит вслух; источники — второй строкой, только в чате
        if (answer.empty() || agree == "none") return L("Поискала, но точного ответа в источниках нет.\nСмотрела: ", "I looked, but the sources don't say exactly.\nI checked: ") + sites;
        std::string reply = answer;
        if (agree == "no") reply = L("Тут источники расходятся. ", "The sources disagree here. ") + reply;
        if (agree == "single") reply += L(" Но это пишет только один источник, так что лучше перепроверить.", " But only one source says this, so it's worth double-checking.");
        reply += L("\nИсточники: ", "\nSources: ") + cited;
        if (agree == "yes") Put('f', saveKey, answer, cited);
        return reply;
    }

    bool IsCorrection(const std::wstring& w) {
        if (std::count(w.begin(), w.end(), L' ') > 7) return false;
        for (const wchar_t* p : { L" не то ", L" неправильно ", L" не правильно ", L" неверно ", L" ошиблась ", L" ошибаешься ", L" это не так ", L" не тот ",
                                  L" не та ", L" ответ не тот ", L" wrong ", L" not right ", L" incorrect ", L" that's not it ", L" not that " })
            if (Contains(w, p)) return true;
        return false;
    }

    std::optional<std::string> Clarify(const std::string& clarification) {
        const Last last = g_last;
        if (last.kind == 'f') {
            auto reply = AnswerFact(last.question + " " + clarification, last.key);
            Remember('f', last.key, last.question + " " + clarification);
            return reply;
        }
        // перевод: из найденных вариантов — тот, что похож на уточнение; иначе запоминаем то, что сказали
        const std::string said = Text::ToUtf8(StripEdges(Simplify(clarification), { L"это", L"нет", L"правильно", L"правильный перевод", L"значит", L"переводится как",
                                                                                     L"в смысле", L"как", L"it's", L"it is", L"it means", L"meaning", L"no", L"the right one is" }, {}));
        if (said.empty()) return std::nullopt;
        std::string answer;
        float bestScore = 0.7f;
        for (const std::string& option : WebTranslations(last.question, last.toEnglish)) {
            const std::string key = Key(option);
            const float score = key.find(said) != std::string::npos || said.find(key) != std::string::npos ? 1.0f : Fuzzy::Similarity(Text::ToWide(key), Text::ToWide(said));
            if (score >= bestScore) { bestScore = score; answer = option; }
        }
        const bool saidInTarget = last.toEnglish ? !HasCyrillic(Text::ToWide(said)) : HasCyrillic(Text::ToWide(said));
        if (answer.empty() && saidInTarget) answer = said;
        if (answer.empty()) { // объяснили значение на исходном языке: «лук — овощ» → переводим вместе с подсказкой
            const auto found = WebTranslations(last.question + " (" + said + ")", last.toEnglish);
            if (!found.empty()) answer = found[0];
        }
        if (answer.empty()) return L("Не получилось найти другой перевод. Скажи правильный — я запомню", "I couldn't find another translation. Tell me the right one and I'll remember it");
        Put(last.kind, last.key, answer, "уточнение");
        Remember(last.kind, last.key, last.question, last.toEnglish);
        return L("Поняла, запомнила: «", "Got it, I'll remember: \"") + last.question + L("» — ", "\" — ") + answer;
    }
}

std::optional<std::string> Knowledge::Followup(const std::string& command) {
    if (g_awaiting && NowMs() - g_last.at < 60000) {
        g_awaiting = false;
        return Clarify(command);
    }
    g_awaiting = false;
    if (!g_last.kind || NowMs() - g_last.at > 180000 || !IsCorrection(L" " + Simplify(command) + L" ")) return std::nullopt;
    g_awaiting = true;
    g_last.at = NowMs();
    g_app.followUpUntil = NowMs() + 20000; // уточнение можно сказать без «Аня»
    if (g_last.kind == 'f') return L("Уточни, пожалуйста, точное название или что именно узнать — поищу заново", "Tell me the exact name or what exactly to look up, and I'll search again");
    return L("Уточни, какое значение «", "Which meaning of \"") + g_last.question + L("» ты имеешь в виду, или скажи правильный перевод", "\" do you mean? Or tell me the right translation");
}

std::optional<std::string> Knowledge::Handle(const std::string& command) {
    if (const auto t = ParseTranslation(command)) return Translate(*t);
    const std::wstring w = L" " + Simplify(command) + L" ";
    if (!IsFactQuestion(w, command)) return std::nullopt;
    const std::string key = Key(QuestionCore(command));
    if (key.empty()) return std::nullopt;
    if (auto known = Find('f', key)) {
        Log::Write("факт «" + key + "» → из базы знаний");
        Remember('f', known->key, command);
        return L("Я это уже узнавала: ", "I already looked this up: ") + known->answer + L("\nИсточники: ", "\nSources: ") + known->sources;
    }
    Remember('f', key, command);
    return AnswerFact(command, key);
}

Knowledge::Stats Knowledge::GetStats() {
    std::lock_guard lock(g_mutex);
    LoadBase();
    return { g_base.size(), g_baseBytes, g_en.size() + g_ru.size() };
}

void Knowledge::Clear() {
    std::lock_guard lock(g_mutex);
    LoadBase();
    g_base.clear();
    g_baseBytes = 0;
    SaveBase();
}
