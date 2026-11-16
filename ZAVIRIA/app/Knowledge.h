#pragma once
// База знаний Ани: перевод RU↔EN и проверенные ответы из интернета.
// Что уже известно (своя база, словарь dict.tsv) — отвечает мгновенно и без сети и нейросети.
// Чего нет — ищет в интернете, сверяет источники, сохраняет в knowledge.tsv рядом с программой.
// «Не то» после ответа — переспрашивает, ищет заново и перезаписывает запись.

#include <cstddef>
#include <optional>
#include <string>

namespace Knowledge {
    std::optional<std::string> Followup(const std::string& command); // «не то» и уточнение после него — раньше навыков
    std::optional<std::string> Handle(const std::string& command);   // перевод и вопросы о новом — после навыков

    struct Stats { size_t entries = 0, bytes = 0, dictionaryWords = 0; };
    Stats GetStats();
    void Clear(); // забыть всё, что Аня узнала сама (словарь остаётся)
}
