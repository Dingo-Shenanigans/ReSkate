#pragma once
#include <cstdint>

// The game's streaming copies a staged read into place without checking that each piece lies in
// its 64 KB staging buffer, and a mod's data whose header asks for more than its chunk has makes
// it read past the buffer (Engine/Game/Build/<build>/streaming_reads.h). The guard empties such a
// piece before the game copies: what it was for keeps what it held before (a mod item may look
// wrong), where the game would have crashed.
namespace dingosdk::streaming_copy_guard {
// Checks this build's function and hooks it; once. False when the build differs (no guard).
bool install(std::uintptr_t base) noexcept;
// How many pieces were emptied since the game started.
std::uint64_t emptied() noexcept;
}
