#pragma once
#include <cstdint>

// The game can stream 20480 textures at once and hangs when a map and its mods need more
// (Engine/Game/Build/<build>/texture_streaming.h). This raises the limit to 32764, the most a
// texture's handle can hold, by moving the manager's tables into a longer allocation before the
// game makes the manager; and it tells the player when the limit, raised or not, is close.
namespace dingosdk::texture_handle_limit {
// Checks every instruction this build is known by and changes them; once, before the game has
// made its texture streaming manager. False when the build differs or the manager is already
// there: the game keeps its own limit.
bool install(std::uintptr_t base) noexcept;

struct Use {
    bool known{};          // the manager is there
    std::uint32_t limit{}; // 20480, or 32764 when raised
    std::uint32_t taken{}; // slots the manager has handed out at its two ends
};
Use use() noexcept;
// Client thread, every tick: a notice when the textures loaded come close to the limit.
void tick() noexcept;
}
