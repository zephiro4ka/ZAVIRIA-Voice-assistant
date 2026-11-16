#include "Updater.h"

#include "Http.h"
#include "Log.h"
#include "Platform.h"
#include "Security.h"
#include "Settings.h"
#include "TextUtils.h"

#include "../third_party/nlohmann/json.hpp"

#include <urlmon.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#pragma comment(lib, "urlmon.lib")

using json = nlohmann::json;

namespace {
    std::mutex g_mutex;
    std::string g_status;

    void SetStatus(const std::string& text) {
        std::lock_guard lock(g_mutex);
        g_status = text;
    }

    std::filesystem::path Folder() {
        const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
        return local ? std::filesystem::path(local) / L"ZAVIRIA-update" : std::filesystem::path();
    }

    std::vector<int> Numbers(const std::string& version) {
        std::vector<int> out;
        std::stringstream in(version.rfind('v', 0) == 0 ? version.substr(1) : version);
        std::string part;
        while (std::getline(in, part, '.')) out.push_back(std::atoi(part.c_str()));
        out.resize(4);
        return out;
    }

    bool IsNewer(const std::string& version) { return Numbers(version) > Numbers(Updater::kVersion); }

    std::string ReadText(const std::filesystem::path& path) {
        std::ifstream in(path);
        std::string text;
        std::getline(in, text);
        return Text::Trim(text);
    }

    void Clear() {
        std::error_code error;
        std::filesystem::remove_all(Folder(), error);
    }

    bool PendingValid() {
        const auto folder = Folder();
        if (folder.empty()) return false;
        const std::string tag = ReadText(folder / L"tag.txt"), sha = ReadText(folder / L"sha256.txt");
        return !tag.empty() && !sha.empty() && IsNewer(tag) && Security::FileSha256((folder / L"ZAVIRIA-Setup.exe").wstring()) == sha;
    }

    bool Launch() {
        const auto setup = Folder() / L"ZAVIRIA-Setup.exe";
        std::ofstream(Folder() / L"attempted.txt") << "1"; // при следующем запуске не повторять, даже если установка сорвалась
        const std::wstring args = L"--update --dir \"" + Text::ExecutableDirectory() + L"\"";
        return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", setup.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL)) > 32;
    }

    void CheckOnce() {
        const json release = json::parse(Http::HttpsGet(L"api.github.com", L"/repos/zephiro4ka/ZAVIRIA-Voice-assistant/releases/latest", 15000), nullptr, false);
        if (!release.is_object()) { SetStatus("не удалось проверить обновления"); return; }
        const std::string tag = release.value("tag_name", "");
        if (!IsNewer(tag)) { SetStatus("у тебя последняя версия"); return; }
        if (PendingValid() && ReadText(Folder() / L"tag.txt") == tag) { SetStatus("версия " + tag + " скачана, установится при следующем запуске"); return; }
        std::string url, sha;
        for (const json& asset : release.value("assets", json::array()))
            if (asset.value("name", "") == "ZAVIRIA-Setup.exe") {
                url = asset.value("browser_download_url", "");
                const std::string digest = asset.value("digest", "");
                if (digest.rfind("sha256:", 0) == 0) sha = digest.substr(7);
            }
        if (url.empty() || sha.empty()) { SetStatus("вышла версия " + tag + ", скачай её со страницы релизов"); return; } // без контрольной суммы сами не ставим
        SetStatus("скачиваю версию " + tag + "…");
        Clear();
        std::error_code error;
        std::filesystem::create_directories(Folder(), error);
        const auto target = Folder() / L"ZAVIRIA-Setup.exe";
        if (URLDownloadToFileW(nullptr, Text::ToWide(url).c_str(), target.c_str(), 0, nullptr) != S_OK || Security::FileSha256(target.wstring()) != sha) {
            Clear();
            Log::Write("обновление: файл не скачался или не совпала контрольная сумма");
            SetStatus("не удалось скачать обновление");
            return;
        }
        std::ofstream(Folder() / L"tag.txt") << tag;
        std::ofstream(Folder() / L"sha256.txt") << sha;
        Log::Write("обновление: скачана версия " + tag);
        SetStatus("версия " + tag + " скачана, установится при следующем запуске");
    }
}

bool Updater::Installed() {
    return GetFileAttributesW((Text::ExecutableDirectory() + L"\\unins000.exe").c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool Updater::HasPending() { return Installed() && PendingValid(); }

bool Updater::ApplyPendingAtStartup() {
    const auto folder = Folder();
    std::error_code error;
    if (folder.empty() || !std::filesystem::exists(folder, error)) return false;
    if (std::filesystem::exists(folder / L"attempted.txt", error) || !Installed() || !PendingValid()) { Clear(); return false; }
    return Launch();
}

void Updater::Start() {
    std::thread([] {
        Sleep(30000);
        for (;;) {
            if (Installed() && Config::Value("update", "auto", "0") == "1") CheckOnce();
            Sleep(6 * 60 * 60 * 1000);
        }
    }).detach();
}

void Updater::InstallNow() {
    if (HasPending() && Launch()) ExitProcess(0);
}

std::string Updater::Status() {
    std::lock_guard lock(g_mutex);
    return g_status;
}
