#pragma once
// Внешний вид одним файлом .zavirialook: сохранить, отдать другу, перетащить в окно Ани.

#include <string>

namespace LookFile {
    constexpr const char* kExtension = ".zavirialook";

    std::string Save(const std::wstring& path);   // пусто — получилось, иначе текст ошибки
    std::string Load(const std::wstring& path);
    void SaveWithDialog();                         // окно «Сохранить как» в фоне; итог — в Status()
    void OpenWithDialog();
    void OnDrop(const std::wstring& path);         // файл бросили на окно

    int PresetCount();
    const char* PresetName(int index);
    void ApplyPreset(int index);                   // 0 — как после установки

    std::string Status();                          // что случилось последним: «сохранено …», «загружено …»
}
