#include "replay_settings.h"
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>

namespace dingosdk::replay_keyframes {
namespace {
std::string_view trimmed(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return text;
}
template <class T> bool number(std::string_view text, T& out) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), out);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
bool flag(std::string_view text, bool& out) {
    if (text != "0" && text != "1") return false;
    out = text == "1";
    return true;
}
} // namespace

std::string to_text(const Settings& settings) {
    return std::format("on={}\namount={:g}\n", settings.on ? 1 : 0, settings.amount);
}

Settings from_text(std::string_view text) {
    Settings settings;
    while (!text.empty()) {
        const auto end = text.find('\n');
        const auto line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        const auto key = trimmed(line.substr(0, eq)), value = trimmed(line.substr(eq + 1));
        if (key == "on") (void)flag(value, settings.on);
        else if (key == "amount") {
            float amount{};
            if (number(value, amount) && std::isfinite(amount) && amount >= 0 && amount <= 1) settings.amount = amount;
        }
    }
    return settings;
}
} // namespace dingosdk::replay_keyframes
