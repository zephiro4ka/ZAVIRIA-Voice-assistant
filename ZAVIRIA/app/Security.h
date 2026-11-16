#pragma once

#include <string>
#include <vector>

// Защита программы: шифрование секретов, проверка своих файлов, второй фактор для бота
namespace Security {
    // Секреты в anya.ini шифруются ключом текущего пользователя Windows
    bool IsProtected(const std::string& stored);
    std::string Protect(const std::string& plain);
    std::string Unprotect(const std::string& stored); // пусто — расшифровать нельзя (другой компьютер или пользователь)

    void HardenDllSearch(); // библиотеки только из папки программы и системной
    std::string FileSha256(const std::wstring& path);
    std::vector<std::string> ChangedFiles(); // файлы программы, не совпавшие с контрольными суммами
    void AcceptChangedFiles();                // пользователь решил запустить всё равно

    // Код из приложения-аутентификатора для опасных команд бота
    bool TwoFactorEnabled();
    std::string NewTwoFactorSecret();
    bool EnableTwoFactor(const std::string& secret, const std::string& code);
    void DisableTwoFactor();
    bool Passed();             // код уже вводили в последние 15 минут
    bool CheckCode(const std::string& code, std::string& message);
    void RememberCommand(const std::string& text);
    bool AwaitingCode();
    std::string TakeCommand();

    // Привязка телефона: одноразовый код с экрана компьютера
    std::string OpenPhoneCode();
    std::string PhoneCode();                    // пусто — код не открыт или истёк
    bool ConsumePhoneCode(const std::string& code, std::string& message);
    std::string SealForPhone(const std::string& text); // AES-GCM ключом привязки
    bool PhoneCodeFailed();  // неверная попытка PAKE; true — код сгорел
    void ClosePhoneCode();   // телефон подключён — код больше не нужен
    // AES-256-GCM, Base64(nonce | шифротекст | метка)
    std::string Seal(const unsigned char* key32, const std::string& text);
    bool Open(const unsigned char* key32, const std::string& sealed, std::string& text);
    std::string Base64(const unsigned char* data, size_t size);
}
