"""Словарь RU<->EN для Ани из WikDict (https://www.wikdict.com, данные Викисловаря, CC BY-SA 3.0).

    python make-dict.py en-ru.sqlite3 ru-en.sqlite3 dict.tsv

Строки «e|r <TAB> слово <TAB> перевод; перевод», по алфавиту (байты UTF-8) — Knowledge.cpp ищет в них двоичным поиском.
"""
import sqlite3
import sys


def rows(path, direction):
    db = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    best = {}
    for word, importance, translations in db.execute(
            "SELECT written_rep, rel_importance, trans_list FROM simple_translation"):
        key = " ".join(word.lower().replace("ё", "е").split())
        if not key or "\t" in key or len(key) > 60:
            continue
        items = []
        for t in translations.replace("́", "").split(" | "):  # «ко́шка» → «кошка»: ударения не нужны ни поиску, ни голосу
            t = t.strip()
            if t and "\t" not in t and t not in items:
                items.append(t)
        items = items[:5]
        if items and (key not in best or (importance or 0) > best[key][0]):
            best[key] = (importance or 0, "; ".join(items))
    return [f"{direction}\t{key}\t{value}\n" for key, (_, value) in best.items()]


def main():
    en_ru, ru_en, out = sys.argv[1:4]
    lines = rows(en_ru, "e") + rows(ru_en, "r")
    lines.sort(key=lambda line: line.encode("utf-8"))
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.writelines(lines)
    print(f"{len(lines)} words -> {out}")


if __name__ == "__main__":
    main()
