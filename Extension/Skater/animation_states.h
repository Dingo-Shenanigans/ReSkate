#pragma once
#include "Engine/Game/Skater/animation_values.h"
#include <cstdint>
#include <optional>
#include <string_view>

// Every named animation value of the local rider's, not only the ones his animation graph lists
// (Engine/Game/Skater/animation_values.h): the game's descriptors are found by a search of its memory, on a thread of
// their own, and read like the listed ones.
namespace dingosdk::animation_states {
// The rider's value by its full name ("Animation/Dingo/Enum.Intent.Manual"): nothing when no descriptor of that name has
// been found, the rider does not hold the value now, or it does not read as its kind.
std::optional<animation_values::Value> read(std::uintptr_t base, std::uintptr_t core, std::string_view name);
// Whether a descriptor of that name has been found (one found and not held now needs no search).
bool known(std::string_view name);
// Called when names were looked up: `missing` says one of them has no descriptor found. Starts a search when one is due
// (animation_values::SearchRule) and forgets what was found on another level.
void wanted(std::uintptr_t base, std::string_view level, bool missing, std::uint64_t now_ms);
}
