#pragma once
#include <cstdint>

namespace dingosdk::replay_export {
// The replay editor steps the replay by one frame of video for every frame the
// game renders, but the engine encodes at most one frame per world update and
// drops the rest. Rendering faster than it updates (an uncapped frame rate),
// an export came out with a fraction of its frames and played that much too
// fast. Every rendered frame of a replay export is encoded instead.
// The export dialog's 8/12/18 Mbps choice is also raised to 100 Mbps H.264.
bool start(std::uintptr_t base) noexcept;
}
