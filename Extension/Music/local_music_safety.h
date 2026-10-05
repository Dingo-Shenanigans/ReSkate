#pragma once
#include <cstdint>

namespace dingosdk::profile_runtime {
// Installs the in-place playlist-lookup crash guard described in
// Engine/Game/Build/20260929/local_music.h. Verifies the 13 contract bytes at
// `playlist_lookup_safety_rva` before writing, so an unsupported image is left
// untouched. Never throws; a failed install is logged and reported as false.
bool install_playlist_lookup_safety(std::uintptr_t base) noexcept;
}
