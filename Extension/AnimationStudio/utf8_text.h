#pragma once
#include <cstddef>
#include <string>
#include <string_view>

namespace dingosdk::anim_studio {
// The first `max_bytes` of UTF-8 text, never ending inside a character: a cut in the middle of one is not UTF-8, and
// writing it as JSON throws (every later save failed while a name like that was set).
inline std::string utf8_prefix(std::string_view text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) return std::string(text);
    std::size_t end = max_bytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    return std::string(text.substr(0, end));
}
} // namespace dingosdk::anim_studio
