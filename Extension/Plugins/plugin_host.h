#pragma once
#include <cstdint>

namespace dingosdk::plugins {
// Called once per client tick with the validated local client and skater identity (0 when none).
// Loads Plugins\*.dll on first use, runs each plugin's tick, and puts the board back when needed.
// Never throws.
void tick(std::uintptr_t base, std::uintptr_t client, std::uintptr_t skater) noexcept;
}
