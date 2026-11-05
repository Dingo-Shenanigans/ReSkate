#pragma once

// UTF-8 to and from Windows' UTF-16 wide text. Windows only: nothing the Linux server builds may
// include this.
#ifndef _WIN32
#error "wide_text.h is Windows only"
#endif

#include <Windows.h>
#include <string>
#include <string_view>

namespace dingosdk {

// UTF-8 as wide text. Bytes that are not valid UTF-8 become U+FFFD rather than failing: the
// text is going to a window, a URL or a file name, where a replaced character beats nothing.
inline std::wstring utf8_to_wide(std::string_view text) {
    if (text.empty()) return {};
    const auto size = static_cast<int>(text.size());
    const auto length = MultiByteToWideChar(CP_UTF8, 0, text.data(), size, nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), size, result.data(), length);
    return result;
}

// Wide text as UTF-8. A lone surrogate becomes U+FFFD in the same way.
inline std::string wide_to_utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const auto size = static_cast<int>(text.size());
    const auto length = WideCharToMultiByte(CP_UTF8, 0, text.data(), size, nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), size, result.data(), length, nullptr, nullptr);
    return result;
}

} // namespace dingosdk
