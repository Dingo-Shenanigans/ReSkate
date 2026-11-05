#pragma once
#include <cstdint>

// Two level loaders walk the references of the assets a list names, and the level exit walks them
// again; all three crashed on an empty one (Skate.exe+0x557dc3, +0x891e07, +0x5591ed: a cosmetic
// pack's asset whose reference resolves to nothing; Engine/Game/Build/<build>/reference_lists.h).
// The guard takes empty references out of each asset's array before the loaders walk it, and logs
// the asset's name once.
namespace dingosdk::reference_list_guard {
// Checks this build's functions and hooks them; once. False when the build differs (no guard).
bool install(std::uintptr_t base) noexcept;
}
