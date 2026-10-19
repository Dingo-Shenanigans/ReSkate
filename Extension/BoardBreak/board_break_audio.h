#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>

// Board Break's sounds: wave files out of a deck pack's `sounds` folder, over each other and at the
// loudness asked for, so a crack, the board's metal and the body's hits of one bail can all be heard.
// In a level they play through the game's own positioned sound, where the skater is, so the game's
// volume, mix and echo apply (16 kHz mono, as that sound takes). Elsewhere, or until the game's
// sound is ready, straight to the speakers: XAudio2 on a thread of its own, and without it (no
// sound device) the cracks one at a time with PlaySound.
namespace dingosdk::board_break::audio {
enum class Kind { light, medium, heavy, metal, body };
inline constexpr std::size_t kinds = 5;
// The wave files of a kind (16-bit or float PCM, any rate and channels). Any thread; a kind is
// given its files once, later calls for it change nothing.
void load(Kind kind, const std::vector<std::filesystem::path>& files) noexcept;
bool has(Kind kind) noexcept;
// One of them, at random, at `volume` (0 to 1). Any thread; returns at once.
void play(Kind kind, float volume) noexcept;
// Client thread, every frame (Runtime/client_tick.cpp): plays what play() gave the game's sound,
// at the local skater. `ready`: a level is loaded and the skater can be read.
void tick_game_sound(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept;
}
