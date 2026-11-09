#pragma once
#include "Engine/Core/Platform/user_data_paths.h"

namespace dingosdk::news::patch_notes_cache {
// The worker asks before its read/fetch try block, so a refused root must disable its disk cache
// without throwing out of the thread or falling back to the player's normal patch notes.
inline std::filesystem::path file() noexcept {
    try { return user_data::root() / L"news" / L"releases.json"; }
    catch (...) { return {}; }
}
}
