#pragma once

#include "Engine/Core/Platform/user_data_paths.h"

#include <Windows.h>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>

namespace dingosdk::local_settings_path {

enum class Status { accepted, invalid_encoding, relative, outside_selection, unresolved };

inline const char* status_name(Status status) noexcept {
    switch (status) {
    case Status::accepted: return "accepted";
    case Status::invalid_encoding: return "invalid_encoding";
    case Status::relative: return "relative_path";
    case Status::outside_selection: return "outside_selected_root";
    case Status::unresolved: return "unresolved_path";
    }
    return "unresolved_path";
}

// The native storage path is UTF-8. Check its actual Windows destination rather
// than assuming the engine's persistent alias still names the selected profile.
inline Status validate(std::string_view text, const std::filesystem::path& root) noexcept {
    try {
        if (text.empty() || text.find('\0') != std::string_view::npos ||
            text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
            return Status::invalid_encoding;
        const auto length = static_cast<int>(text.size());
        const auto characters = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            text.data(), length, nullptr, 0);
        if (characters <= 0) return Status::invalid_encoding;
        std::wstring wide(static_cast<std::size_t>(characters), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length,
            wide.data(), characters) != characters) return Status::invalid_encoding;
        const std::filesystem::path supplied(wide);
        if (!supplied.is_absolute()) return Status::relative;
        if (root.empty() || !root.is_absolute()) return Status::unresolved;
        if (!user_data::detail::within(supplied.lexically_normal(), root))
            return Status::outside_selection;
        std::error_code error;
        const auto resolved = std::filesystem::weakly_canonical(supplied, error);
        if (error || resolved.empty()) return Status::unresolved;
        return user_data::detail::within(resolved, root) ? Status::accepted : Status::outside_selection;
    } catch (...) {
        return Status::unresolved;
    }
}

} // namespace dingosdk::local_settings_path
