#pragma once
#include "vr.h"

namespace dingosdk::vr {
// Called from the game-thread tick once the local profile is readable: loads
// the saved VR settings the first time, then saves changes (debounced).
void profile_tick() noexcept;
}
