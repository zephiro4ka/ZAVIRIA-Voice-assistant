#pragma once

#include <optional>
#include <string>

// Навыки, которым не нужна нейросеть: отвечают мгновенно
namespace ExtraSkills {
    std::optional<std::string> Handle(const std::string& command);
    std::optional<std::string> HandleEnglish(const std::string& command); // команды в английском режиме
}
