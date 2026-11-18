#pragma once
#include "Engine/Core/Platform/user_data_paths.h"

namespace dingosdk::overlay::detail::store_icon_paths {
// Both kinds of picture share the launcher's checked root. An unusable requested root disables
// the disk cache rather than letting a worker read or write the player's normal cache.
inline std::filesystem::path folder(bool news) noexcept {
    try {
        const auto root = user_data::root();
        return news ? root / L"news" / L"pictures" : root / L"thunderstore" / L"icons";
    } catch (...) { return {}; }
}
}
