#pragma once
#include <cstdint>

// Leaving some custom maps crashed the game: as a level unloads, the game takes each mesh
// variation database's entries out of its registry by the entry's mesh, and reads the mesh
// without checking it is still there (Engine/Game/Build/<build>/mesh_variations.h). A map whose
// meshes unload before their databases leaves entries without one. The guard runs that one
// function under an exception handler and, for that one read alone, has it go on with the next
// entry: the function finishes, lock and all. Every other fault is left to the crash reporter.
namespace dingosdk::mesh_unload_guard {
// Checks this build's function and hooks it; once. False when the build differs (no guard).
bool install(std::uintptr_t base) noexcept;
// How many entries were passed over since the game started.
std::uint64_t skipped() noexcept;
}
