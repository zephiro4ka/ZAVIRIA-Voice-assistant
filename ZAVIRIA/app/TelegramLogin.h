#pragma once
// Вход через Telegram — как «Войти с Google»: OpenID Connect (код + PKCE), https://core.telegram.org/bots/telegram-login
// Браузер → oauth.telegram.org (подтверждение в Telegram) → страница-переходник kRedirect → 127.0.0.1 → обмен кода
// на id_token с проверкой подписи (RS256, ключи oauth.telegram.org). Вошедший аккаунт попадает в «Кому можно».
// Нужны: токен бота (Client ID — число до «:») и Client Secret из @BotFather → Login Widget, а kRedirect — в его Allowed URLs.

#include <string>

namespace TelegramLogin {
    constexpr const char* kRedirect = "https://zephiro4ka.github.io/ZAVIRIA-Voice-assistant/telegram-login/";

    enum class Phase { Idle, Waiting, Checking, Failed };
    struct State {
        Phase phase = Phase::Idle;
        std::string message; // причина ошибки (по-русски, для Lang::Tr)
    };

    bool Begin(std::string& error); // откроет вход в браузере; false — не хватает настроек (error — почему)
    void Cancel();
    State Current();
    std::string Account();          // «Имя (@ник)», пусто — не вошли
    void SignOut();                 // убрать вошедший аккаунт из «Кому можно»
}
