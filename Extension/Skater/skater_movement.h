#pragma once
#include "Extension/Plugins/plugin_skater_movement.h"
#include <cstdint>
#include <optional>

// The local skater's movement, read from the game (reskate.skater.movement; the support rays of
// placement physics). Client tick.
namespace dingosdk::skater_movement {
// The client the camera is read through for `stick` (Runtime/client_tick.cpp, every tick).
void set_client(std::uintptr_t client) noexcept;
// Nothing without a local skater or when its velocity does not read as a velocity.
std::optional<plugins::SkaterMovement> read(std::uintptr_t base);
}
