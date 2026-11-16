#include "Autostart.h"

#include "Platform.h"

#include <string>

namespace {
    constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    // Отметка «разрешено» из диспетчера задач → «Автозагрузка». Новые сборки Windows 11 без неё запись Run не выполняют
    constexpr const wchar_t* kApprovedKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
    constexpr const wchar_t* kValue = L"Anya";

    std::wstring Command() {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        return L"\"" + std::wstring(path) + L"\" " + Autostart::kFlag;
    }

    // true — в диспетчере задач автозапуск выключен (первый байт нечётный: 03, 07…)
    bool DisabledInTaskManager() {
        BYTE data[12] = {};
        DWORD size = sizeof(data);
        if (RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, kValue, RRF_RT_REG_BINARY, nullptr, data, &size) != ERROR_SUCCESS) return false;
        return (data[0] & 1) != 0;
    }

    void Approve(bool enabled) {
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kApprovedKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
        if (enabled) {
            const BYTE data[12] = { 2 }; // 02 00 … — «Включено», как ставит сам диспетчер задач
            RegSetValueExW(key, kValue, 0, REG_BINARY, data, sizeof(data));
        } else {
            RegDeleteValueW(key, kValue);
        }
        RegCloseKey(key);
    }

    bool Approved() {
        BYTE data[12] = {};
        DWORD size = sizeof(data);
        return RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, kValue, RRF_RT_REG_BINARY, nullptr, data, &size) == ERROR_SUCCESS;
    }
}

bool Autostart::Enabled() {
    wchar_t value[1024] = {};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValue, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) return false;
    return Command() == value && !DisabledInTaskManager(); // запись есть, ведёт именно на эту программу и не выключена в диспетчере задач
}

void Autostart::Migrate() {
    wchar_t value[1024] = {};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValue, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) return;
    const std::wstring command = value;
    if (command.find(L"\\Anya.exe") != std::wstring::npos || command.find(L"\\ASTRIX.exe") != std::wstring::npos) { Set(true); return; } // запись от старого Anya.exe / ASTRIX.exe
    if (command == Command() && !Approved()) Approve(true); // включали до этой версии — отметки «разрешено» ещё нет
}

void Autostart::Set(bool enabled) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (enabled) {
        const std::wstring command = Command();
        RegSetValueExW(key, kValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kValue);
    }
    RegCloseKey(key);
    Approve(enabled);
}
