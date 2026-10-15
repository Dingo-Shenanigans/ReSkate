#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace dingosdk {

// A path as narrow text.
//
// std::filesystem::path::string() converts to the process's ANSI code page and
// throws when a character has no mapping there: "No mapping for the Unicode
// character exists in the target multi-byte code page". A folder named in
// Cyrillic or CJK is enough, and so is an invisible one -- a directory called
// U+200E (left-to-right mark) has been seen in the wild. The failure lands far
// from the cause, because the path is usually only being written into a log or
// an error message at the time.
//
// UTF-8 can represent every path Windows allows, so every path that becomes a
// narrow string goes through here instead.
inline std::string path_utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

// A file name stem that Windows keeps for a device: CON, PRN, AUX, NUL, COM0-COM9 or LPT0-LPT9, in any ASCII case.
[[nodiscard]] inline bool windows_device_name(std::string_view stem) noexcept {
    if (stem.size() != 3 && stem.size() != 4) return false;
    char text[4]{};
    for (std::size_t i = 0; i < stem.size(); ++i) text[i] = stem[i] >= 'A' && stem[i] <= 'Z' ? static_cast<char>(stem[i] - 'A' + 'a') : stem[i];
    const std::string_view name(text, stem.size());
    if (name.size() == 3) return name == "con" || name == "prn" || name == "aux" || name == "nul";
    return (name.starts_with("com") || name.starts_with("lpt")) && name[3] >= '0' && name[3] <= '9';
}

} // namespace dingosdk
