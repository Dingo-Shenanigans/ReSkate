#pragma once
#include <cstdint>

// A load that failed could crash the game instead: to say why, the game describes each part of
// the load's error in turn, and the list can hold parts that were never set
// (Engine/Game/Build/<build>/load_errors.h). The guard looks at the list first and empties every
// part that is not one of the game's own objects, so those read "no error" as an empty part
// does; the description itself runs under an exception handler as well. The failed load is then
// the game's to deal with, as it meant to. What the game wrote is put in the log, so a mod
// whose content does not load can be found.
namespace dingosdk::load_error_guard {
// Checks this build's function and hooks it; once. False when the build differs (no guard).
bool install(std::uintptr_t base) noexcept;
// How many parts were emptied since the game started.
std::uint64_t emptied() noexcept;
// How many failed loads the game has described since it started (0 without the guard).
std::uint64_t described() noexcept;
}
