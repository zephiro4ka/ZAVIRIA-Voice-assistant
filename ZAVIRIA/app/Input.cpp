#include "Input.h"

#include "TextUtils.h"

#include <vector>

namespace {
    INPUT KeyEvent(WORD key, bool up) {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = key;
        input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
        // стрелки, Win и т.п. — «расширенные» клавиши
        switch (key) {
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_LWIN: case VK_RWIN:
        case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT: case VK_INSERT: case VK_DELETE:
            input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
            break;
        }
        return input;
    }

    INPUT UnicodeEvent(wchar_t ch, bool up) {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = ch;
        input.ki.dwFlags = KEYEVENTF_UNICODE | (up ? KEYEVENTF_KEYUP : 0);
        return input;
    }

    void Send(std::vector<INPUT>& inputs) {
        if (!inputs.empty()) SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    }
}

void Input::TypeText(const std::string& utf8) {
    std::vector<INPUT> inputs;
    for (wchar_t ch : Text::ToWide(utf8)) {
        if (ch == L'\n') {
            inputs.push_back(KeyEvent(VK_RETURN, false));
            inputs.push_back(KeyEvent(VK_RETURN, true));
            continue;
        }
        inputs.push_back(UnicodeEvent(ch, false));
        inputs.push_back(UnicodeEvent(ch, true));
    }
    Send(inputs);
}

void Input::Chord(std::initializer_list<WORD> keys) {
    std::vector<INPUT> inputs;
    for (WORD key : keys) inputs.push_back(KeyEvent(key, false));
    for (auto it = keys.end(); it != keys.begin();) inputs.push_back(KeyEvent(*--it, true));
    Send(inputs);
}

void Input::Tap(WORD key, int times) {
    std::vector<INPUT> inputs;
    for (int i = 0; i < times; ++i) {
        inputs.push_back(KeyEvent(key, false));
        inputs.push_back(KeyEvent(key, true));
    }
    Send(inputs);
}

bool Input::ParseChord(const std::string& text, std::vector<WORD>& keys) {
    static const std::pair<const wchar_t*, WORD> kNames[] = {
        { L"ctrl", VK_CONTROL }, { L"control", VK_CONTROL }, { L"контрол", VK_CONTROL }, { L"alt", VK_MENU }, { L"альт", VK_MENU },
        { L"shift", VK_SHIFT }, { L"шифт", VK_SHIFT }, { L"win", VK_LWIN }, { L"вин", VK_LWIN },
        { L"enter", VK_RETURN }, { L"ввод", VK_RETURN }, { L"esc", VK_ESCAPE }, { L"escape", VK_ESCAPE }, { L"tab", VK_TAB }, { L"таб", VK_TAB },
        { L"space", VK_SPACE }, { L"пробел", VK_SPACE }, { L"backspace", VK_BACK }, { L"delete", VK_DELETE }, { L"del", VK_DELETE }, { L"insert", VK_INSERT },
        { L"up", VK_UP }, { L"down", VK_DOWN }, { L"left", VK_LEFT }, { L"right", VK_RIGHT }, { L"вверх", VK_UP }, { L"вниз", VK_DOWN }, { L"влево", VK_LEFT }, { L"вправо", VK_RIGHT },
        { L"home", VK_HOME }, { L"end", VK_END }, { L"pgup", VK_PRIOR }, { L"pageup", VK_PRIOR }, { L"pgdn", VK_NEXT }, { L"pagedown", VK_NEXT },
        { L"printscreen", VK_SNAPSHOT }, { L"prtsc", VK_SNAPSHOT }, { L"capslock", VK_CAPITAL }, { L"menu", VK_APPS },
        { L"play", VK_MEDIA_PLAY_PAUSE }, { L"next", VK_MEDIA_NEXT_TRACK }, { L"prev", VK_MEDIA_PREV_TRACK }, { L"mute", VK_VOLUME_MUTE },
        { L"volup", VK_VOLUME_UP }, { L"voldown", VK_VOLUME_DOWN },
    };
    keys.clear();
    std::wstring word;
    const std::wstring all = Text::LowerRu(Text::ToWide(text)) + L"+";
    for (wchar_t ch : all) {
        if (ch != L'+' && ch != L' ') { word += ch; continue; }
        if (word.empty()) continue;
        WORD key = 0;
        for (const auto& [name, vk] : kNames) if (word == name) key = vk;
        if (!key && word.size() == 1 && ((word[0] >= L'a' && word[0] <= L'z') || (word[0] >= L'0' && word[0] <= L'9'))) key = static_cast<WORD>(towupper(word[0]));
        if (!key && word.size() >= 2 && word[0] == L'f') {
            const int n = _wtoi(word.c_str() + 1);
            if (n >= 1 && n <= 24) key = static_cast<WORD>(VK_F1 + n - 1);
        }
        if (!key) return false;
        keys.push_back(key);
        word.clear();
    }
    return !keys.empty();
}

