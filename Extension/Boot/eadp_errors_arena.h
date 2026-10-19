#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace dingosdk {
// The arena EA's error reporting allocates from is 80 MiB, made at startup and kept in RAM, of
// which about 10 MB was in use: reporting is off (ea_service_block, and the EadpErrorsData
// settings in client_tick.cpp), so its reports are never built. It is made before those settings
// can be changed, so its size is capped where the game reads it instead. A smaller configured
// size is left as it is.
inline constexpr std::uint32_t eadp_errors_arena_bytes = 32u << 20;

// Installs before executable entry, while the game is suspended.
bool start_eadp_errors_arena(std::uintptr_t base, std::string& error);
} // namespace dingosdk
