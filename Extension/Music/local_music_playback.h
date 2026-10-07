#pragma once

#include <cstdint>

namespace dingosdk::profile_runtime {
using MusicSelectNext = std::int32_t (__fastcall *)(std::uintptr_t queue, std::uintptr_t playlist_asset);

// Validate the supported native selector/setter contracts and seed the lock-free
// preference read used by the audio selection hook.
bool initialize_music_playback(std::uintptr_t base, bool shuffle) noexcept;
void activate_music_playback() noexcept;
void deactivate_music_playback() noexcept;
bool music_playback_available() noexcept;
MusicSelectNext& music_select_next_original() noexcept;
std::int32_t __fastcall music_select_next_hook(std::uintptr_t queue, std::uintptr_t playlist_asset);

bool music_shuffle_enabled() noexcept;
void set_music_shuffle_enabled(bool enabled) noexcept;
} // namespace dingosdk::profile_runtime
