#include "Uninstall.h"

#include "Autostart.h"
#include "Log.h"
#include "TextUtils.h"
#include "Updater.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <objbase.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace {
    std::wstring ExePath() {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        return path;
    }

    std::wstring Quote(const std::wstring& path) { // строка для PowerShell в одинарных кавычках
        std::wstring out = L"'";
        for (wchar_t c : path) out += c == L'\'' ? std::wstring(L"''") : std::wstring(1, c);
        return out + L"'";
    }

    // Скрипт PowerShell (UTF-8 с BOM — русские пути не ломаются) работает уже после того, как Аня закрылась
    void RunHiddenScript(const std::wstring& body) {
        wchar_t temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp);
        const std::wstring script = std::wstring(temp) + L"anya-cleanup.ps1";
        {
            std::ofstream out(std::filesystem::path(script), std::ios::binary);
            out << "\xEF\xBB\xBF" << Text::ToUtf8(body + L"\r\nRemove-Item -LiteralPath $PSCommandPath -Force -ErrorAction SilentlyContinue\r\n");
        }
        std::wstring command = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" + script + L"\"";
        STARTUPINFOW si{ sizeof(si) };
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB, nullptr, temp, &si, &pi) ||
            CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, temp, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }

    constexpr const wchar_t* kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ZAVIRIA";

    std::wstring KnownFolder(REFKNOWNFOLDERID id) {
        PWSTR raw = nullptr;
        std::wstring out;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &raw))) out = raw;
        CoTaskMemFree(raw);
        return out;
    }
    std::wstring StartMenuLink() { return KnownFolder(FOLDERID_Programs) + L"\\ZAVIRIA.lnk"; }
    std::wstring DesktopLink() { return KnownFolder(FOLDERID_Desktop) + L"\\ZAVIRIA.lnk"; }

    bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

    // Ярлык на ZAVIRIA.exe; уже есть и ведёт сюда же — не трогаем
    void MakeLink(const std::wstring& link, const std::wstring& target) {
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        IShellLinkW* shell = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell)))) {
            IPersistFile* file = nullptr;
            bool same = false;
            if (Exists(link) && SUCCEEDED(shell->QueryInterface(IID_PPV_ARGS(&file)))) {
                wchar_t current[MAX_PATH] = {};
                if (SUCCEEDED(file->Load(link.c_str(), STGM_READ)) && SUCCEEDED(shell->GetPath(current, MAX_PATH, nullptr, 0)))
                    same = _wcsicmp(current, target.c_str()) == 0;
                file->Release();
                file = nullptr;
            }
            if (!same) {
                const std::wstring dir = target.substr(0, target.find_last_of(L'\\'));
                shell->SetPath(target.c_str());
                shell->SetWorkingDirectory(dir.c_str());
                shell->SetIconLocation(target.c_str(), 0);
                shell->SetDescription(L"ZAVIRIA - Voice assistant");
                if (SUCCEEDED(shell->QueryInterface(IID_PPV_ARGS(&file)))) { file->Save(link.c_str(), TRUE); file->Release(); }
            }
            shell->Release();
        }
        if (SUCCEEDED(init)) CoUninitialize();
    }

    void SetString(HKEY key, const wchar_t* name, const std::wstring& value) {
        RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    }
    void SetDword(HKEY key, const wchar_t* name, DWORD value) { RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value)); }

    // Размер папки в КБ — для строки в «Установленных приложениях»
    DWORD FolderSizeKb(const std::wstring& dir) {
        std::error_code ec;
        unsigned long long total = 0;
        for (auto it = std::filesystem::recursive_directory_iterator(dir, std::filesystem::directory_options::skip_permission_denied, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec)) total += it->file_size(ec);
        }
        return static_cast<DWORD>(std::min<unsigned long long>(total / 1024, 0xFFFFFFFFull));
    }

    void RemoveRegistration() {
        RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
        for (const auto& link : { StartMenuLink(), DesktopLink() }) {
            if (!Exists(link)) continue;
            // свой ли ярлык: ведёт на ZAVIRIA.exe из этой папки
            const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            IShellLinkW* shell = nullptr;
            bool mine = false;
            if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell)))) {
                IPersistFile* file = nullptr;
                if (SUCCEEDED(shell->QueryInterface(IID_PPV_ARGS(&file)))) {
                    wchar_t current[MAX_PATH] = {};
                    if (SUCCEEDED(file->Load(link.c_str(), STGM_READ)) && SUCCEEDED(shell->GetPath(current, MAX_PATH, nullptr, 0)))
                        mine = _wcsicmp(current, ExePath().c_str()) == 0;
                    file->Release();
                }
                shell->Release();
            }
            if (SUCCEEDED(init)) CoUninitialize();
            if (mine) DeleteFileW(link.c_str());
        }
    }
}

std::wstring Uninstall::Folder() { return Text::ExecutableDirectory(); }

bool Uninstall::SafeToDeleteFolder() {
    const std::filesystem::path dir(Folder());
    if (dir.empty() || dir == dir.root_path() || !dir.has_parent_path() || dir.parent_path() == dir.root_path().parent_path()) return false;
    std::wstring name = Text::LowerRu(dir.filename().wstring());
    static const wchar_t* kShared[] = { L"desktop", L"рабочий стол", L"downloads", L"загрузки", L"documents", L"документы", L"program files",
                                        L"program files (x86)", L"windows", L"users", L"пользователи", L"onedrive" };
    if (std::any_of(std::begin(kShared), std::end(kShared), [&](const wchar_t* s) { return name == s; })) return false;
    wchar_t profile[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) && std::filesystem::path(profile) == dir) return false;
    return true;
}

void Uninstall::ResetSettings() {
    const std::wstring ini = Folder() + L"\\anya.ini";
    for (const wchar_t* section : { L"anya", L"metrics", L"limits", L"layout", L"sounds", L"weather", L"ai", L"debug" })
        WritePrivateProfileStringW(section, nullptr, nullptr, ini.c_str()); // раздел целиком
    Log::Write("настройки сброшены по умолчанию — перезапуск");
    const std::wstring restart = std::wstring(L"Start-Sleep -Seconds 2") + L'\n' + L"Start-Process -FilePath " + Quote(ExePath());
    RunHiddenScript(restart);
    ExitProcess(0); // без сохранения — иначе настройки из памяти вернутся в файл
}

void Uninstall::RemoveEverything() {
    Autostart::Set(false);
    RemoveRegistration();
    const std::wstring dir = Folder();
    std::wstring body = L"Start-Sleep -Seconds 3\r\n$dir = " + Quote(dir) + L"\r\n";
    if (SafeToDeleteFolder()) { // движки нейросети и голоса закрываются чуть позже Ани — пробуем несколько раз
        body += L"for ($i = 0; $i -lt 6 -and (Test-Path -LiteralPath $dir); $i++) {\r\n"
                L"  Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue\r\n  Start-Sleep -Seconds 2\r\n}\r\n";
    } else { // общая папка (например, «Загрузки») — только свои файлы
        for (const wchar_t* item : { L"ZAVIRIA.exe", L"Anya.exe", L"anya.ini", L"scenarios.json", L"anya.log", L"llama-server.log", L"devprobe.txt", L"models", L"third_party", L"llama.cpp" })
            body += L"Remove-Item -LiteralPath (Join-Path $dir " + Quote(item) + L") -Recurse -Force -ErrorAction SilentlyContinue\r\n";
    }
    Log::Write("полное удаление программы");
    RunHiddenScript(body);
    ExitProcess(0);
}

void Uninstall::RegisterApp() {
    const std::wstring exe = ExePath(), dir = Folder();
    const std::wstring lower = Text::LowerRu(dir);
    if (lower.find(L"\\x64\\release") != std::wstring::npos || lower.find(L"\\x64\\debug") != std::wstring::npos) return; // dev-сборка
    { // строка от установщика (Inno): если она про эту папку — всё уже сделано; если про старую (ASTRIX, папку перенесли) — убрать её
        constexpr const wchar_t* kInno = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{7E3C1A52-9B46-4D1E-8F27-5A0C6D2B9E11}_is1";
        wchar_t location[MAX_PATH] = {};
        DWORD size = sizeof(location);
        if (RegGetValueW(HKEY_CURRENT_USER, kInno, L"InstallLocation", RRF_RT_REG_SZ, nullptr, location, &size) == ERROR_SUCCESS) {
            std::wstring at = location;
            while (!at.empty() && (at.back() == L'\\' || at.back() == L'/')) at.pop_back();
            if (_wcsicmp(at.c_str(), dir.c_str()) == 0) return; // ставил установщик — у него свои ярлыки и строка удаления
            if (!Exists(at)) RegDeleteTreeW(HKEY_CURRENT_USER, kInno); // папки уже нет — строка «ASTRIX» в списке приложений ведёт в никуда
        }
    }
    MakeLink(StartMenuLink(), exe);
    HKEY key = nullptr;
    DWORD created = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &key, &created) != ERROR_SUCCESS) return;
    if (created == REG_CREATED_NEW_KEY && !Exists(DesktopLink())) MakeLink(DesktopLink(), exe); // на рабочий стол — один раз: удалит, больше не появится
    SetString(key, L"DisplayName", L"ZAVIRIA - Voice assistant");
    SetString(key, L"DisplayIcon", exe + L",0");
    SetString(key, L"DisplayVersion", Text::ToWide(Updater::kVersion));
    SetString(key, L"Publisher", L"ZAVIRIA");
    SetString(key, L"InstallLocation", dir);
    SetString(key, L"UninstallString", L"\"" + exe + L"\" " + kUninstallFlag);
    SetString(key, L"URLInfoAbout", L"https://github.com/zephiro4ka/ZAVIRIA-Voice-assistant");
    SetDword(key, L"NoModify", 1);
    SetDword(key, L"NoRepair", 1);
    RegCloseKey(key);
    std::thread([dir] { // размер считаем в фоне: в папке модели на гигабайты
        HKEY k = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
        SetDword(k, L"EstimatedSize", FolderSizeKb(dir));
        RegCloseKey(k);
    }).detach();
}

int Uninstall::RunUninstall() {
    if (MessageBoxW(nullptr, L"Удалить ZAVIRIA с компьютера?\n\nУдалятся программа, настройки, модели и всё, что лежит в её папке.",
                    L"Удаление ZAVIRIA", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return 0;
    // запущенную ZAVIRIA из этой папки закрываем, иначе файлы заняты
    const std::wstring exe = ExePath();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W e{ sizeof(e) };
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
        if (e.th32ProcessID == GetCurrentProcessId() || _wcsicmp(e.szExeFile, L"ZAVIRIA.exe") != 0) continue;
        if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, e.th32ProcessID)) {
            wchar_t path[MAX_PATH] = {};
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameW(h, 0, path, &size) && _wcsicmp(path, exe.c_str()) == 0) { TerminateProcess(h, 0); WaitForSingleObject(h, 3000); }
            CloseHandle(h);
        }
    }
    CloseHandle(snap);
    RemoveEverything(); // закрывает процесс
    return 0;
}
