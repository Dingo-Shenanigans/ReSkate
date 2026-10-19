#pragma once
// The built-in pack format (Engine/Vfs/builtin_packs.h), as BuiltinPacker writes it at build time
// from a zip in assets/builtin and ReSkate unpacks it: one zstd frame (with its content size and a
// checksum) holding
//   "RSKPACK1", u32 file count,
//   for each file: u16 name length, the name (UTF-8, '/' between folders), u64 size,
//   then every file's bytes in the same order.
// Numbers little-endian.
#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

namespace dingosdk::mods::builtin_pack {
inline constexpr std::array<char, 8> magic{'R', 'S', 'K', 'P', 'A', 'C', 'K', '1'};
// The most a pack may unpack to, and the largest zstd window it may use (zstd's own default
// limit for decoding, 128 MiB).
inline constexpr std::uint64_t max_bytes = 512ull * 1024 * 1024;
inline constexpr int max_window_log = 27;

// A path inside a pack: relative, forward slashes, no "..", plain characters (and spaces inside a
// name, as in a sound's "Body Impact 1.wav", but never at either end of one).
inline bool safe_name(std::string_view name) {
    if (name.empty() || name.size() > 260 || name.front() == '/' || name.find(':') != std::string_view::npos ||
        name.find('\\') != std::string_view::npos)
        return false;
    std::size_t start = 0;
    while (start <= name.size()) {
        const auto end = std::min(name.find('/', start), name.size());
        const auto part = name.substr(start, end - start);
        if (part.empty() || part == "." || part == ".." || part.front() == ' ' || part.back() == ' ') return false;
        if (!std::all_of(part.begin(), part.end(), [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                       c == '-' || c == ' ';
            }))
            return false;
        start = end + 1;
    }
    return true;
}
}
