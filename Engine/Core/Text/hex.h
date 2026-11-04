#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace dingosdk {

// Bytes as lower-case hex, two digits each and nothing between: how hashes (SHA-1 and the like)
// are written into keys, file names and logs.
inline std::string hex_bytes(std::span<const std::byte> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const auto value : bytes) {
        const auto byte = static_cast<unsigned>(value);
        text += digits[byte >> 4];
        text += digits[byte & 15];
    }
    return text;
}

// A number as "0x" and exactly `width` lower-case digits: zeros in front, and digits past
// `width` dropped, so columns of addresses and hashes line up in research dumps.
inline std::string hex_number(std::uint64_t value, int width) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out(static_cast<std::size_t>(width), '0');
    for (int i = width - 1; i >= 0; --i, value >>= 4) out[static_cast<std::size_t>(i)] = digits[value & 15];
    return "0x" + out;
}

} // namespace dingosdk
