#pragma once
#include <cstdint>
#include <string>

namespace dingosdk::bmx {
// Optional, passthrough-only diagnostic. Installs only for an isolated data
// root with RESKATE_BMX_FLOW_DIAGNOSTICS=1 and the exact supported executable.
// Initialization failure is nonfatal. Call during the normal hook setup.
bool initialize_spawn_diagnostics(std::uintptr_t base) noexcept;
// Passive diagnostic; call on the verified native game-update thread.
std::string spawn_diagnostics(std::uintptr_t base);
}
