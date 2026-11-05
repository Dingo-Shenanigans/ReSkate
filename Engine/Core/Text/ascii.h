#pragma once

#include <string>
#include <string_view>

namespace dingosdk {

// Case folding for ASCII letters only, A to Z. Asset names, mod folders, command names and
// the like are compared without case, and they are ASCII or UTF-8. std::tolower would follow
// the process locale, which in some code pages also rewrites bytes above 0x7f and so breaks
// UTF-8; folding only A to Z leaves every other byte as it is.
constexpr char ascii_lower(char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

inline std::string ascii_lower(std::string_view text) {
    std::string result(text);
    for (auto& c : result) c = ascii_lower(c);
    return result;
}

// Wide text kept to ASCII: every character past 0x7f becomes '?'. For text that must stay
// plain ASCII wherever it goes (trace ids, module names in a report), not for text shown to
// a person, which wants UTF-8.
inline std::string ascii_narrow(std::wstring_view text) {
    std::string result;
    result.reserve(text.size());
    for (const auto c : text) result.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return result;
}

} // namespace dingosdk
