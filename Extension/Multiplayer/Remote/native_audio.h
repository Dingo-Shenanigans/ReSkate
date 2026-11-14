#pragma once
#include "native_skater.h"
namespace dingosdk::multiplayer {
void prepare_audio_capture(std::uintptr_t base, const NativeFrame &) noexcept;
// Installs the skater sound hook now (client thread) instead of when a session starts.
void prepare_remote_audio(std::uintptr_t base) noexcept;
std::vector<AudioSample> drain_audio_capture(std::uint64_t now);
void update_remote_audio(std::uintptr_t base, const NativeFrame &, const Pose &, const AudioState &) noexcept;
void stop_remote_audio() noexcept;
void reset_audio() noexcept;
std::string native_audio_status();
std::uint64_t captured_audio_frames() noexcept;
std::uint64_t played_audio_frames() noexcept;
// The local skater's surface sound: the material words in `fields` of its sound updates (bit i:
// audio_material_offsets[i]) take this word's material and property slots, keeping their own flags
// (0: each surface its own sound). Others in a session hear it too: they hear what the skater's
// sound updates say.
void set_surface_sound(std::uint32_t word, std::uint32_t fields = ~0U) noexcept;
} // namespace dingosdk::multiplayer
