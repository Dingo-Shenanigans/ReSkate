#pragma once

#include <string>
#include <string_view>

namespace dingosdk {

// Text for a URL's query or path: ASCII letters, digits and - _ . ~ as they are, every other
// byte as %XX (upper-case hex, RFC 3986). UTF-8 goes byte by byte, as servers expect. The
// letters are tested as ASCII rather than with std::isalnum, which follows the process locale
// and could let a byte above 0x7f through unescaped.
inline std::string url_escaped(std::string_view text) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : text) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                           c == '-' || c == '_' || c == '.' || c == '~';
        if (plain) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(digits[c >> 4]);
            out.push_back(digits[c & 15]);
        }
    }
    return out;
}

} // namespace dingosdk
