#pragma once
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The materials of items worn on top (outfit_overlay::set_layers). The game's skater loader
// (characters/lua/skaterlualoaderasset) builds each recipe slot with that slot's own function, which
// puts the item's appearance shader preset on a material named for the slot's kind: "Top_mat" for the
// top, "Bottom_mat" for the bottoms, "Outfit_Top_mat" for the outfit. An item worn on top sits in
// another kind's slot, so its preset lands on a material its mesh does not use, and the mesh draws
// with no textures (white). The preset lookup guard (preset_lookup_guard.cpp) puts such a preset on
// the item's own kind's material too; this says which presets those are, and that material's name.
namespace dingosdk::layer_materials {
// The material an item's own builder puts its preset of `type` on (AssetPathTypeEnum: 3
// AppearanceShaderPreset, 16 AppearanceShaderPreset2, 19 to 26 a costume's), by the item's catalog
// folder ("cust_bottoms"); null when there is none. The names live as long as the process.
const char* material_of(std::string_view folder, int type) noexcept;
// The presets of the items worn on top now, each with its material: replaced as a whole. Any thread.
void set(std::vector<std::pair<std::string, const char*>> presets);
// The material for a preset by its name (any case, its folders left out), or null. Any thread.
const char* material_for(std::string_view preset) noexcept;
// The name hash the game keys an item's materials by (AppearanceItem:addMaterial): djb2 over the
// name with A-Z as a-z, each step xor.
constexpr unsigned name_hash(std::string_view name) noexcept {
    unsigned hash = 0x1505;
    for (const char c : name) {
        const auto byte = static_cast<unsigned char>(c);
        hash = (hash * 0x21) ^ (byte + (byte - 'A' < 26u ? 0x20u : 0u));
    }
    return hash;
}
} // namespace dingosdk::layer_materials
