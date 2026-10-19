#pragma once
#include <cstdint>
namespace dingosdk::trick_names {
// Verified client thread, while the game is in a world. No simulation or
// scoring changes: observe native ticker rows produced for the local player.
void tick(std::uintptr_t base, bool world_ready) noexcept;
}
