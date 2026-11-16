#pragma once

// Язык программы: как в системе, русский или английский
#include <string>

namespace Lang {
    void Reload();           // прочитать настройку и применить
    bool IsEnglish();
    const char* Tr(const char* text); // перевод целой строки
    std::string Latin(const std::string& text); // кириллицу — латиницей (названия городов)
    std::string TranslateText(const std::string& text); // фразы бота и пульта внутри любого текста
    const char* Code();      // "ru" или "en": для распознавания и озвучки

    // Язык каждой фразы: [ears] autolang=1 — распознавание само выбирает русский или английский,
    // и навыки, ответ нейросети и озвучка идут на языке сказанного, а не интерфейса
    bool AutoSpeech();
    bool IsEnglishText(const std::string& text); // латиница без кириллицы → английский (при autolang); иначе — язык интерфейса
    void SetReplyEnglish(bool english);          // язык текущей фразы — на нём отвечает нейросеть
    bool ReplyEnglish();
    const char* SpeechCode(const std::string& text); // "ru" / "en" для озвучки этого текста
}
