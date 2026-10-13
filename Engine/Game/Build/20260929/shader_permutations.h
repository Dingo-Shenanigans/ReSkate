#pragma once
#include "Engine/Game/Build/fingerprint.h"
#include <array>
#include <cstdint>

// The game's expression shaders (Extension/World/shader_permutation_guard.cpp).
namespace dingosdk::game::build::v20260929::shader_permutations {
// prepare(shader, renderer, state, bool, bool): run on a job worker (from 0x3f72b90, for every
// shader each view has registered, and from 0x3f79b70 for one). The shader keeps its compiled
// permutations as an array of pointers (begin at +0x50, end at +0x58). The function asks the
// first one for its kind (its first virtual function: ExpressionShaderRasterPermutation or
// ExpressionShaderRaytracePermutation) without checking it is there, then hands each to
// 0x46aa370 or 0x46aac20:
//   mov rax,[rcx+0x50]; ...; mov rcx,[rax]; mov rax,[rcx]   <- 0x3f86da6, rcx null
inline constexpr Fingerprint prepare{0x3f86d80, {
    0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x6c,0x24,0x20,0x56,0x57,0x41,0x54,0x48,0x83,
    0xec,0x40,0x48,0x8b,0x41,0x50,0x48,0x8b,0xf1,0x45,0x0f,0xb6,0xe1,0x49,0x8b,0xd8}};
inline constexpr std::uintptr_t permutations_begin = 0x50, permutations_end = 0x58;
}
