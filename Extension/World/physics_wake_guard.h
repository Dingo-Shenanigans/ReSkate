#pragma once
#include <cstdint>

// Long multiplayer sessions crashed the game in its allocator (Skate.exe+0x3d99e2f, +0x3d9ee43,
// +0x3d9ee47; some 4,000 of 2.1.1's reports): the physics system's wake pass keeps a bitset of
// 20000 owners and sets a bit for any owner index, and indices pass 20000 once enough bodies
// have come and gone (Engine/Game/Build/<build>/physics_wake.h). The one bit past the end lands
// in the next heap block. The guard makes the bitset as long as the world's owner list for each
// pass that needs it, so every index the pass can meet has its bit.
namespace dingosdk::physics_wake_guard {
// Checks this build's function and hooks it; once. False when the build differs (no guard).
bool install(std::uintptr_t base) noexcept;
// The most owners a pass has had to make room for (0 while the game's own size was enough).
std::uint32_t widest() noexcept;
}
