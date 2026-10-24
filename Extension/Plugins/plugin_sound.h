#pragma once
#include "plugin_wave.h"
#include "Extension/Skater/motor_synth.h"
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// Plugins' sounds in the game (reskate.audio): their own WAVE files, or a synthesised motor, played
// through the game's positioned voice sound (Multiplayer/Voice/native_voice.h, as players' voices
// are), so the level's mix, echo and walls apply. In a level only. Client thread.
namespace dingosdk::plugin_sound {
struct Voice {
    std::string owner;
    std::shared_ptr<const plugins::Wave> wave; // empty: the motor
    float volume{1}, pitch{1};
    float range{40};                           // metres to silence
    float throttle{}, speed{};                 // the motor's
    bool loop{};
    std::optional<std::array<float, 3>> position; // empty: at the local skater
};
// Its id, or 0 when there is no room (at most 8 a plugin, 48 in all).
std::uint64_t start(Voice voice);
// The voice of `owner`, to change while it plays; null once it has finished.
Voice* find(std::uint64_t id, std::string_view owner);
void stop(std::uint64_t id, std::string_view owner);
void stop_all(std::string_view owner);
// Each client tick: plays them, where they are, as loud as they are from the camera.
void tick(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept;
}
