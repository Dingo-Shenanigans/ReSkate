#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "Engine/Core/Json/json.h"

namespace dingosdk::profile {
// The most items a saved profile keeps owned or described (cosmetics, and Build objects). The
// profile keeps every item it has seen, those of mods since switched off or removed too, so a mod
// comes back owned: a player who has tried many mods passes 8192 in the profile alone, and at
// 8192 every save was refused, leaving the game with no catalog at all (no items in the
// customization menu, no playcard). A bound against a damaged file, not a size a profile should
// reach.
inline constexpr std::size_t saved_item_limit = 65536;
// The most items installed at once that ReSkate reads and publishes: the game's own catalog, the
// catalog handed back to it, the owned list. It was 8192, taken to be the game's limit, and it is
// not one: the game held 8385 (2026-11-19, the base game and about 90 cosmetic packs) and only
// ReSkate refused, so no saved outfit loaded and the skater came up in the game's defaults. A
// bound against a damaged read, like saved_item_limit.
inline constexpr std::size_t installed_item_limit = 65536;

// Exact decoded recipe values. Float parameters retain their IEEE-754 bits;
// resource pointers, allocator state and transient UI flags are never saved.
struct CosmeticSlot {
    std::uint32_t slot{};
    std::string asset;
    std::vector<std::uint32_t> parameter_bits;
    bool operator==(const CosmeticSlot&) const = default;
};
struct CosmeticRecipe {
    std::uint32_t template_key{}, template_version{};
    std::vector<std::uint32_t> scalar_bits;
    std::vector<CosmeticSlot> items;
    bool operator==(const CosmeticRecipe&) const = default;
};
struct CosmeticLoadout {
    std::vector<CosmeticRecipe> recipes;
    bool operator==(const CosmeticLoadout&) const = default;
};
dingosdk::Json encode_loadout(const CosmeticLoadout&);
CosmeticLoadout decode_loadout(const dingosdk::Json&);
void validate_customization(const dingosdk::Json&);
void validate_player_card(const CosmeticLoadout&);
// Cached display metadata only. Caller still enumerates installed assets and
// applies local catalog overrides; this does not determine ownership.
dingosdk::Json item_display_metadata(const std::string& asset, const dingosdk::Json& overrides);
}
