#pragma once
#include <cstdint>

// The most reported crash of 2.0.8 (Sentry, 2026-10-13: Skate.exe+0x3f86da6 on some thirty
// machines within the hour): as the game prepares its shaders on a job worker, it reads the
// first compiled permutation of each without checking there is one
// (Engine/Game/Build/<build>/shader_permutations.h). A shader whose permutation never came to
// be (a material that names a shader program the game could not build) holds null there. The
// guard passes such a shader over: it is not prepared, and what it would have drawn may be
// missing, where the game would have closed.
namespace dingosdk::shader_permutation_guard {
// Checks this build's function and hooks it; once. False when the build differs (no guard).
bool install(std::uintptr_t base) noexcept;
// How many times a shader was passed over since the game started.
std::uint64_t skipped() noexcept;
}
