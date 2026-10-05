#pragma once
#include <cstdint>

namespace dingosdk::vr {
// Client update thread, after the multiplayer tick: puts the Feet Only costume on the
// local skater while VR runs and a "You see" choice is Feet only, and takes it off after.
void tick_costume(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept;
// Whether the FeetOnly mod's costume is in the game's catalog (false until a skater is seen).
bool feet_only_installed() noexcept;
} // namespace dingosdk::vr
