#pragma once

#include <cstdint>

namespace dingosdk::profile_runtime {
using MusicSelectNext = std::int32_t (__fastcall *)(std::uintptr_t queue, std::uintptr_t playlist_asset);
using MusicChannelPlaylistBind = std::uintptr_t (__fastcall *)(std::uintptr_t channel_pointer,
                                                               std::uintptr_t playlist_id_pointer);

// Validate the supported native selector/setter contracts and seed the lock-free
// preference read used by the audio selection hook.
bool initialize_music_playback(std::uintptr_t base, bool shuffle) noexcept;
void activate_music_playback() noexcept;
void deactivate_music_playback() noexcept;
bool music_playback_available() noexcept;
MusicSelectNext& music_select_next_original() noexcept;
// The game's read of one song's number for its scorer, kept within the asset's songs.
using MusicSongScore = void (__fastcall *)(std::uint32_t place, std::uint32_t* out, std::uintptr_t** context);
MusicSongScore& music_song_score_original() noexcept;
void __fastcall music_song_score_hook(std::uint32_t place, std::uint32_t* out, std::uintptr_t** context);
std::int32_t __fastcall music_select_next_hook(std::uintptr_t queue, std::uintptr_t playlist_asset);
MusicChannelPlaylistBind& music_channel_playlist_bind_original() noexcept;
std::uintptr_t __fastcall music_channel_playlist_bind_hook(std::uintptr_t channel_pointer,
                                                            std::uintptr_t playlist_id_pointer);

bool music_shuffle_enabled() noexcept;
void set_music_shuffle_enabled(bool enabled) noexcept;
} // namespace dingosdk::profile_runtime
