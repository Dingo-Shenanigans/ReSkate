#pragma once
#include "Engine/Game/Build/fingerprint.h"
#include <cstdint>

// The game's errors of a load made of several (Extension/World/load_error_guard.cpp).
namespace dingosdk::game::build::v20260929::load_errors {
// describe(errors, text): a virtual of the error that holds one error per part of a load. It
// writes "[i]:" and each part's own description (its virtual at + 8), or "no error" for a null
// one, a line each. The parts are an array of pointers (errors + 0x18) and their count
// (errors + 0x20); the text is the game's fixed text writer, whose first field is its buffer
// with flags in the two low bits.
//
// The game calls it when a load fails, to write "load <address> failed with error: ..."
// (0x1aa75e0), and the array can hold parts that were never set: crash reports of 2026-10-11
// (one player, six times, 2.0.5 and 2.0.6) all end in the call through part 8 after eight that
// read "no error" - once a null vtable, else garbage that was read or run.
inline constexpr Fingerprint describe{0x2e3ab40, {
    0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x33,
    0xff,0x48,0x8b,0xda,0x48,0x8b,0xf1,0x48,0x39,0x79,0x20,0x76,0x6b,0x0f,0x1f,0x00}};
inline constexpr std::uintptr_t parts_offset = 0x18, count_offset = 0x20;
}
