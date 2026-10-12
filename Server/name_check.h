#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace dingosdk::server::names {
inline std::size_t decode(std::string_view text, std::size_t at, char32_t &cp) {
    const auto lead = static_cast<unsigned char>(text[at]);
    std::size_t length = lead < 0x80 ? 1 : (lead >> 5) == 6 ? 2 : (lead >> 4) == 14 ? 3 : (lead >> 3) == 30 ? 4 : 0;
    if (!length || at + length > text.size()) {
        cp = 0xFFFD;
        return 1;
    }
    cp = length == 1 ? lead : length == 2 ? lead & 0x1F : length == 3 ? lead & 0x0F : lead & 0x07;
    for (std::size_t i = 1; i < length; ++i) {
        const auto next = static_cast<unsigned char>(text[at + i]);
        if ((next & 0xC0) != 0x80) {
            cp = 0xFFFD;
            return 1;
        }
        cp = (cp << 6) | (next & 0x3F);
    }
    return length;
}

inline bool invisible(char32_t cp) {
    return cp == 0x00AD || cp == 0x034F || cp == 0x061C || cp == 0x115F || cp == 0x1160 || cp == 0x17B4 || cp == 0x17B5 ||
           cp == 0x180E || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x206F) ||
           cp == 0x2800 || cp == 0x3164 || (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0xFEFF || cp == 0xFFA0 || cp == 0xFFFC ||
           cp == 0xFFFD || (cp >= 0xE0000 && cp <= 0xE0FFF) || (cp >= 0x1D173 && cp <= 0x1D17A);
}

inline bool hides(std::string_view text) {
    for (std::size_t at = 0; at < text.size();) {
        char32_t cp{};
        at += decode(text, at, cp);
        if (invisible(cp) && cp != 0x200D && cp != 0xFE0E && cp != 0xFE0F) return true;
    }
    return false;
}

inline std::string strip_invisible(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t at = 0; at < text.size();) {
        char32_t cp{};
        const auto length = decode(text, at, cp);
        if (!invisible(cp)) out.append(text.substr(at, length));
        at += length;
    }
    return out;
}

inline char lookalike(char32_t cp) {
    struct Pair { char32_t from; char to; };
    static constexpr std::array<Pair, 52> table{{
        {0x0430, 'a'}, {0x0432, 'b'}, {0x0435, 'e'}, {0x043A, 'k'}, {0x043C, 'm'}, {0x043D, 'h'}, {0x043E, 'o'}, {0x0440, 'p'},
        {0x0441, 'c'}, {0x0442, 't'}, {0x0443, 'y'}, {0x0445, 'x'}, {0x0455, 's'}, {0x0456, 'i'}, {0x0458, 'j'}, {0x04BB, 'h'},
        {0x0501, 'd'}, {0x0410, 'a'}, {0x0412, 'b'}, {0x0415, 'e'}, {0x041A, 'k'}, {0x041C, 'm'}, {0x041D, 'h'}, {0x041E, 'o'},
        {0x0420, 'p'}, {0x0421, 'c'}, {0x0422, 't'}, {0x0423, 'y'}, {0x0425, 'x'}, {0x0405, 's'}, {0x0406, 'i'}, {0x0408, 'j'},
        {0x03B1, 'a'}, {0x03BA, 'k'}, {0x03B9, 'i'}, {0x03BD, 'v'}, {0x03BF, 'o'}, {0x03C1, 'p'}, {0x03C4, 't'}, {0x0391, 'a'},
        {0x0392, 'b'}, {0x0395, 'e'}, {0x0396, 'z'}, {0x0397, 'h'}, {0x0399, 'i'}, {0x039A, 'k'}, {0x039C, 'm'}, {0x039D, 'n'},
        {0x039F, 'o'}, {0x03A1, 'p'}, {0x03A4, 't'}, {0x0131, 'i'},
    }};
    for (const auto &pair : table)
        if (pair.from == cp) return pair.to;
    if (cp >= 0xFF01 && cp <= 0xFF5E) return static_cast<char>(cp - 0xFEE0);
    return 0;
}

inline std::string fold(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t at = 0; at < text.size();) {
        char32_t cp{};
        const auto length = decode(text, at, cp);
        const auto chunk = text.substr(at, length);
        at += length;
        if (invisible(cp) || (cp >= 0x0300 && cp <= 0x036F)) continue;
        char c = cp < 0x80 ? static_cast<char>(cp) : lookalike(cp);
        if (!c) {
            out.append(chunk);
            continue;
        }
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        switch (c) {
        case '0': c = 'o'; break;
        case '1': case 'i': case '|': case '!': c = 'l'; break;
        case '3': c = 'e'; break;
        case '4': case '@': c = 'a'; break;
        case '5': case '$': c = 's'; break;
        case '7': c = 't'; break;
        case '8': c = 'b'; break;
        default: break;
        }
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out += c;
    }
    return out;
}

inline constexpr std::array<std::string_view, 14> staff_words{
    "admin", "administrator", "mod", "moderator", "owner", "dev", "developer", "staff", "server", "reskate", "helper", "official", "host", "console",
};
inline constexpr std::array<std::string_view, 8> staff_names{
    "admin", "administrator", "moderator", "staff", "server", "reskate", "console", "developer",
};

inline bool same_word(std::string_view folded, std::string_view word) {
    const auto f = fold(word);
    return !f.empty() && (folded == f || (folded.size() == f.size() + 1 && folded.starts_with(f) && folded.back() == 's'));
}

inline bool staff_word(std::string_view folded, std::span<const std::string> tags) {
    if (folded.empty()) return false;
    for (const auto word : staff_words)
        if (same_word(folded, word)) return true;
    for (const auto &tag : tags)
        if (same_word(folded, tag)) return true;
    return false;
}

inline bool staff_name(std::string_view folded) {
    return std::any_of(staff_names.begin(), staff_names.end(), [&](std::string_view word) { return same_word(folded, word); });
}

inline std::string strip_fake_tags(std::string_view name, std::span<const std::string> tags) {
    const auto trim = [](std::string_view text) {
        while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
        while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
        return text;
    };
    std::string_view rest = trim(name);
    for (unsigned round = 0; round < 4 && !rest.empty(); ++round) {
        const char open = rest.front();
        const char close = open == '[' ? ']' : open == '(' ? ')' : open == '{' ? '}' : open == '<' ? '>' : 0;
        if (!close) break;
        const auto end = rest.find(close, 1);
        if (end == std::string_view::npos || !staff_word(fold(rest.substr(1, end - 1)), tags)) break;
        rest = trim(rest.substr(end + 1));
        while (!rest.empty() && (rest.front() == '|' || rest.front() == '-' || rest.front() == ':')) rest = trim(rest.substr(1));
    }
    return std::string(rest);
}
}
