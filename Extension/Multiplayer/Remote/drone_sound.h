#pragma once
#include <cstdint>
#include <string>

// Other players' FPV drones heard where they fly. Each drone's motor is synthesised here
// (Skater/drone_audio.h, at its estimated throttle and speed: drone_beacon.h) and played through
// the game's positioned voice sound, the one proximity chat uses (Voice/native_voice.h), so the
// game places it, muffles it behind walls and gives it the level's reverb.
namespace dingosdk::multiplayer {
// The client update thread, every frame (in a level or not: sounds stop when `ready` or `show`
// goes false). `show`: other players' drones are shown (DRONE page, LOBBY tab). The
// dronemodeltest prop, while it runs, is heard as a drone too.
void tick_drone_sounds(std::uintptr_t base, std::uintptr_t client, bool ready, bool show) noexcept;
std::string drone_sounds_status();
} // namespace dingosdk::multiplayer
