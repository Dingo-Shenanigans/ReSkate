#pragma once
#include <cstdint>
#include <string>

namespace dingosdk::bmx {
// Explicit development-only recovery: publishes the authored initial-player event
// once after validating a loaded solo world with one unspawned local human player.
bool request_player_startup() noexcept;
void tick_player_startup(std::uintptr_t base, std::uintptr_t client, bool gameplay_ready) noexcept;
void stop_player_startup() noexcept;
std::string player_startup_status();
}
