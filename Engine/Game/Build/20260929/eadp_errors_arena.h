// Reviewed native contract for supported_build::game_sha256 (September 29 2026).
// The arena EA's error reporting (EadpErrorsData) allocates from: 80 MiB, made
// about seven seconds into startup, before the named settings can be changed.
#pragma once

#include <array>
#include <cstdint>

namespace dingosdk::game::build::v20260929::eadp_errors_arena {
// In the arena's factory (0x02318660): the load of its size from the settings
// object (EadpErrorsData.EadpErrorsDataPlatformSpecificMemPoolSize) and the
// vtable load after it, the two complete instructions moved into the stub.
//   mov eax, dword ptr [rax + 0x7c]
//   lea rcx, [rip + 0x03fffd17]        ; 0x063183d8
// No branch in the factory lands inside them; the factory goes on at resume_rva.
inline constexpr std::uintptr_t size_load_rva = 0x023186b7;
inline constexpr std::array<std::uint8_t, 10> size_load{
    0x8b, 0x40, 0x7c, 0x48, 0x8d, 0x0d, 0x17, 0xfd, 0xff, 0x03};
inline constexpr std::uintptr_t vtable_rva = 0x063183d8;
inline constexpr std::uintptr_t resume_rva = 0x023186c1;
} // namespace dingosdk::game::build::v20260929::eadp_errors_arena
