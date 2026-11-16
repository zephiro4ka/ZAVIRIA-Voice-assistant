#include "LookFile.h"

#include "HomeLayout.h"
#include "Lang.h"
#include "Platform.h"
#include "Settings.h"
#include "State.h"
#include "TextUtils.h"
#include "Theme.h"

#include <shobjidl.h>

#include <algorithm>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace {
    // что входит во «внешний вид»; обои — путь на чужом компьютере, их не берём
    struct Key { const char* section; const char* name; };
    constexpr Key kKeys[] = {
        { "anya", "look" }, { "anya", "look_color" }, { "anya", "accent" }, { "anya", "accent_color" },
        { "anya", "backdrop" }, { "anya", "backdrop_speed" }, { "anya", "background_color" }, { "anya", "animation_color" },
        { "anya", "glass" }, { "anya", "button_plates" }, { "anya", "window_effect" }, { "anya", "window_opacity" },
        { "anya", "wallpaper_dim" },
        { "look", "font" }, { "look", "font_scale" }, { "look", "mode" }, { "look", "style" },
        { "cozy", "enabled" }, { "cozy", "pattern" }, { "cozy", "dark" },
    };
    constexpr const char* kHeader = "ZAVIRIA look 1";

    std::mutex g_mutex;
    std::string g_status;
    bool g_dialogOpen = false;
    std::wstring g_pending;   // выбранный в окне файл: применяем в потоке интерфейса
    bool g_pendingSave = false;

    void SetStatus(const std::string& text) {
        std::lock_guard lock(g_mutex);
        g_status = text;
    }

    // места блоков из «Редактирования»: [layout] id_x / id_y / id_s
    std::vector<std::string> LayoutKeys() {
        std::vector<std::string> keys;
        for (const std::string& id : HomeLayout::Ids())
            for (const char* what : { "_x", "_y", "_s" }) keys.push_back(id + what);
        return keys;
    }

    bool Known(const std::string& section, const std::string& name) {
        if (section == "layout") {
            const auto keys = LayoutKeys();
            return std::find(keys.begin(), keys.end(), name) != keys.end();
        }
        for (const Key& k : kKeys)
            if (section == k.section && name == k.name) return true;
        return false;
    }

    // «section.key=value» по строкам; чужое и лишнее пропускаем — файл могли написать руками
    void Apply(const std::string& text) {
        Config::SaveIfDirty(true);
        std::istringstream in(text);
        for (std::string line; std::getline(in, line);) {
            line = Text::Trim(line);
            const size_t dot = line.find('.'), eq = line.find('=');
            if (dot == std::string::npos || eq == std::string::npos || dot > eq) continue;
            const std::string section = line.substr(0, dot), name = Text::Trim(line.substr(dot + 1, eq - dot - 1));
            const std::string value = Text::Trim(line.substr(eq + 1));
            if (!Known(section, name) || value.size() > 16) continue;
            Config::SetValue(section.c_str(), name.c_str(), value);
        }
        HomeLayout::Reload();
        Config::Load();
        Theme::ReloadOptions();
    }

    std::wstring FileName(const std::wstring& path) {
        const size_t slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? path : path.substr(slash + 1);
    }

    struct Preset { const char* name; const char* values; };
    const Preset kPresets[] = {
        { "Как после установки",
          "anya.look=0\nanya.accent=0\nanya.backdrop=0\nanya.backdrop_speed=100\nanya.background_color=-1\nanya.animation_color=-1\nanya.glass=1\n"
          "anya.button_plates=0\nanya.window_effect=2\nanya.window_opacity=70\nlook.font=0\nlook.font_scale=100\nlook.mode=0\nlook.style=0\ncozy.enabled=0\ncozy.dark=0" },
        { "Мягкие пилюли",
          "anya.background_color=-1\nanya.glass=1\nlook.font=0\nlook.mode=0\nlook.style=1\nlook.atmosphere=1\ncozy.enabled=0" },
        { "Ночной контур",
          "anya.background_color=-1\nanya.glass=1\nlook.font=0\nlook.mode=0\nlook.style=3\nlook.atmosphere=3\ncozy.enabled=0" },
        { "Светлое утро",
          "anya.look=3\nanya.accent=2\nanya.background_color=-1\nanya.glass=0\nlook.font=11\nlook.mode=1\nlook.style=1\ncozy.enabled=0" },
        { "Строгий графит",
          "anya.background_color=-1\nanya.glass=0\nlook.font=0\nlook.mode=0\nlook.style=2\nlook.atmosphere=0\ncozy.enabled=0" },
        { "Розовый уют",
          "anya.background_color=-1\nanya.glass=1\nlook.font=0\nlook.mode=1\nlook.style=1\nlook.atmosphere=2\nlook.cozy_accent=1\ncozy.enabled=0" },
    };

    template <typename Dialog>
    void RunDialog(const CLSID& clsid, bool save) {
        {
            std::lock_guard lock(g_mutex);
            if (g_dialogOpen) return;
            g_dialogOpen = true;
        }
        std::thread([clsid, save] {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            std::wstring chosen;
            {
                Dialog* dialog = nullptr;
                if (SUCCEEDED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
                    const COMDLG_FILTERSPEC filter[] = { { L"ZAVIRIA look", L"*.zavirialook" } };
                    dialog->SetFileTypes(1, filter);
                    dialog->SetDefaultExtension(L"zavirialook");
                    if (save) dialog->SetFileName(L"мой вид ZAVIRIA");
                    IShellItem* item = nullptr;
                    PWSTR path = nullptr;
                    if (SUCCEEDED(dialog->Show(FindWindowW(L"AnyaWindow", nullptr))) && SUCCEEDED(dialog->GetResult(&item)) &&
                        SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                        chosen = path;
                        CoTaskMemFree(path);
                    }
                    if (item) item->Release();
                    dialog->Release();
                }
            }
            CoUninitialize();
            std::lock_guard lock(g_mutex);
            g_dialogOpen = false;
            if (!chosen.empty()) { g_pending = chosen; g_pendingSave = save; }
        }).detach();
    }
}

std::string LookFile::Save(const std::wstring& path) {
    std::ostringstream out;
    out << kHeader << "\n";
    for (const Key& k : kKeys) {
        const std::string value = Config::Value(k.section, k.name, "");
        if (!value.empty()) out << k.section << "." << k.name << "=" << value << "\n";
    }
    for (const std::string& key : LayoutKeys()) {
        const std::string value = Config::Value("layout", key.c_str(), "");
        if (!value.empty()) out << "layout." << key << "=" << value << "\n";
    }
    std::ofstream file(path, std::ios::binary);
    if (!(file << out.str())) return "не получилось сохранить файл";
    SetStatus("сохранено: " + Text::ToUtf8(FileName(path)));
    return {};
}

std::string LookFile::Load(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    if (!file || !(text << file.rdbuf())) return "не получилось открыть файл";
    const std::string content = text.str();
    if (content.size() > 64 * 1024 || content.rfind(kHeader, 0) != 0) return "это не файл внешнего вида ZAVIRIA";
    Apply(content);
    SetStatus("загружено: " + Text::ToUtf8(FileName(path)));
    return {};
}

void LookFile::SaveWithDialog() { RunDialog<IFileSaveDialog>(CLSID_FileSaveDialog, true); }
void LookFile::OpenWithDialog() { RunDialog<IFileOpenDialog>(CLSID_FileOpenDialog, false); }

void LookFile::OnDrop(const std::wstring& path) {
    const std::string error = Load(path);
    if (!error.empty()) SetStatus(error);
    Chat::Add(Role::System, error.empty() ? "Внешний вид загружен из файла «" + Text::ToUtf8(FileName(path)) + "»" : error);
}

int LookFile::PresetCount() { return static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0])); }
const char* LookFile::PresetName(int index) { return kPresets[std::clamp(index, 0, PresetCount() - 1)].name; }

void LookFile::ApplyPreset(int index) {
    const Preset& preset = kPresets[std::clamp(index, 0, PresetCount() - 1)];
    Apply(preset.values);
    SetStatus(std::string("готовый вариант: ") + Lang::Tr(preset.name));
}

std::string LookFile::Status() {
    std::wstring path;
    bool save = false;
    {
        std::lock_guard lock(g_mutex);
        path.swap(g_pending);
        save = g_pendingSave;
    }
    if (!path.empty()) {
        const std::string error = save ? Save(path) : Load(path);
        if (!error.empty()) SetStatus(error);
    }
    std::lock_guard lock(g_mutex);
    return g_status;
}
