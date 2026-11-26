#pragma once
#include <cstdint>

// The game can stream 20480 textures at once and hangs when a map and its mods need more
// (Engine/Game/Build/<build>/texture_streaming.h). This raises the limit to 65532, the most a
// texture's 16-bit handle can hold once the game's checks stop reading bit 15 as "no handle", by
// moving the manager's tables into a longer allocation before the game makes the manager. At the
// limit a new texture goes without a slot (blurry or blank) instead of the game freezing (a map
// loading then may not finish), and the player is told when the limit, raised or not, is close.
namespace dingosdk::texture_handle_limit {
// Checks every instruction this build is known by and changes them; once, before the game has
// made its texture streaming manager. False when the build differs or the manager is already
// there: the game keeps its own limit.
bool install(std::uintptr_t base) noexcept;

struct Use {
    bool known{};          // the manager is there
    std::uint32_t limit{}; // 20480, or 65532 when raised
    std::uint32_t taken{}; // slots the manager has handed out at its two ends
    bool soft_full{};      // at the limit a texture goes without a slot instead of the game freezing
};
Use use() noexcept;
// Client thread, every tick: a notice when the textures loaded come close to the limit.
void tick() noexcept;
}
