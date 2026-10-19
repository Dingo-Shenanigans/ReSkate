// Reviewed native contract for supported_build::game_sha256 (September 29 2026).
// The game's fatal handlers catch most crashes themselves (ReSkate's last-chance filter never
// sees them) and write their own minidump, of thread stacks only, in-process: three callers
// (0x18e3ea1 "Frostbite MiniDump", 0x2d28c9d, 0x2d2bfee) through one import thunk.
#pragma once
#include <array>
#include <cstdint>

namespace dingosdk::game::build::v20260929::game_fatal_dump {
// MiniDumpWriteDump's import slot (api-ms-win-core-debug-minidump-l1-1-0.dll).
inline constexpr std::uintptr_t import_slot = 0x05feb2e0;
// The thunk every caller goes through: jmp qword ptr [import_slot].
inline constexpr std::uintptr_t thunk = 0x0546dbc6;
inline constexpr std::array<std::uint8_t, 6> thunk_bytes{0xff, 0x25, 0x14, 0xd7, 0xb7, 0x00};
} // namespace dingosdk::game::build::v20260929::game_fatal_dump
