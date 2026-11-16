#pragma once

#include <cstdint>
#include <string>

namespace dingosdk {
// DLSS frame generation keeps its resources when the game turns it off for a frame. Without this,
// each of the game's one-frame offs (in bursts after every map load, now and then while riding)
// freed and remade them, and every one stalled the game for about 220 ms
// (Engine/Game/Build/20260929/frame_generation.h).
// The off still happens; only nothing is thrown away. This holds only while frame generation's mode
// is on: turning it off in the settings frees its video memory as before.
// Installs before executable entry, while the game is suspended.
bool start_frame_generation_retain(std::uintptr_t base, std::string& error);
} // namespace dingosdk
