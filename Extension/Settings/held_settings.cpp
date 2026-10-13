#include "Extension/Settings/named_settings.h"
#include "Engine/Core/Console/command_registry.h"
#include "Engine/Core/Log/logging.h"
#include <format>
#include <utility>

namespace dingosdk {
bool still_held(std::string_view current, std::string_view held) noexcept {
    const auto a = console::parse_value(console::Type::number, current), b = console::parse_value(console::Type::number, held);
    return a && b && *a == *b;
}

HeldSettings::HeldSettings(Reader read, Writer write) noexcept : read_(read), write_(write) {}

bool HeldSettings::hold(std::span<const std::pair<std::string_view, std::string_view>> settings, std::string &error) {
    for (const auto &[name, value] : settings) {
        const auto before = read_(name);
        if (!before || before->empty()) {
            error = std::format("the game's {} setting could not be read", name);
            return false;
        }
        if (still_held(*before, value)) continue;
        if (const auto result = write_(name, value); result.starts_with("error")) {
            error = std::format("the game's {} could not be set to {} ({})", name, value, result);
            return false;
        }
        held_.push_back({std::string(name), *before, std::string(value)});
    }
    return true;
}

// True when the setting was put back, or another feature changed it meanwhile.
bool HeldSettings::put_back(const Held &setting) const noexcept {
    try {
        const auto now = read_(setting.name);
        if (!now || now->empty()) return false;
        if (!still_held(*now, setting.value)) return true;
        return !write_(setting.name, setting.before).starts_with("error");
    } catch (...) {
        return false;
    }
}

std::vector<std::string> HeldSettings::release() noexcept {
    std::vector<std::string> left;
    for (auto &setting : std::exchange(held_, {})) {
        if (put_back(setting)) continue;
        logging::log(logging::Level::warning, logging::Channel::settings, "Settings: {} was left at {}; it could not be put back to {}.",
                     setting.name, setting.value, setting.before);
        try {
            left.push_back(std::move(setting.name));
        } catch (...) {}
    }
    return left;
}
} // namespace dingosdk
