#pragma once
#include <filesystem>
#include <vector>

// Board Break's sounds: wave files out of a deck pack's `sounds` folder, played straight to the
// speakers (not through the game's mixer), over each other and at the loudness asked for, so a
// crack, the board's metal and the body's hits of one bail can all be heard. XAudio2 on a thread
// of its own; without it (no sound device) the cracks fall back to PlaySound, one at a time.
namespace dingosdk::board_break::audio {
enum class Kind { light, medium, heavy, metal, body };
inline constexpr std::size_t kinds = 5;
// The wave files of a kind (16-bit or float PCM, any rate and channels). Any thread; a kind is
// given its files once, later calls for it change nothing.
void load(Kind kind, const std::vector<std::filesystem::path>& files) noexcept;
bool has(Kind kind) noexcept;
// One of them, at random, at `volume` (0 to 1). Any thread; returns at once.
void play(Kind kind, float volume) noexcept;
}
