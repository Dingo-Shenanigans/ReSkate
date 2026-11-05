#pragma once
#include "Engine/Core/Platform/clock.h"
#include <cstdint>

namespace dingosdk::multiplayer {
// Microseconds on the steady clock (steady_us). Session, audio and animation timings share it,
// under this name.
inline std::uint64_t now_us() noexcept { return steady_us(); }
} // namespace dingosdk::multiplayer
