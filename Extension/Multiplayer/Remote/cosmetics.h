#pragma once
#include <array>
#include <cstdint>
#include <string_view>
#include <string>
#include <vector>

namespace dingosdk::multiplayer {
inline constexpr std::uint32_t skater_recipe_key = 2759515148U, board_recipe_key = 1583459055U;
inline constexpr std::size_t max_cosmetic_slots = 64, max_cosmetic_scalars = 64,
                             max_cosmetic_parameters = 128, max_cosmetic_asset = 255;
// The largest encoded outfit packet. Well under the packet limit, so an outfit a host accepts
// always fits again with the reference marker a relayed copy carries.
inline constexpr std::size_t max_appearance_bytes = 16384;
struct CosmeticSlot {
    std::uint32_t slot{};
    std::string asset;
    std::vector<std::uint32_t> parameters;
    bool operator==(const CosmeticSlot &) const = default;
};
struct CosmeticRecipe {
    std::uint32_t key{}, version{};
    std::vector<std::uint32_t> scalars;
    std::vector<CosmeticSlot> items;
    bool operator==(const CosmeticRecipe &) const = default;
};
struct PlayerCard {
    std::uint32_t background{}, emblem{}, title{};
    bool operator==(const PlayerCard &) const = default;
};
// How one of a listed player's marked cosmetics is coloured (developer_identity.h), chosen on
// the menu's Special page and carried in their Appearance, so everyone sees the same.
enum class MarkMode : std::uint8_t { standard, off, gradient, solid };
struct MarkStyle {
    MarkMode mode{};                          // standard: what the player's list gives
    std::array<std::uint8_t, 3> from{}, to{}; // as picked on screen. gradient: between the two; solid: `from`
    std::uint8_t speed{};                     // 0 normal, 1 slow, 2 fast
    bool operator==(const MarkStyle &) const = default;
};
// The marked cosmetics, in this order: whatever the skater has in each of these slots, then the
// parts of whatever board they ride.
inline constexpr std::array<std::string_view, 12> mark_item_names{
    "Top", "Bottoms", "Shoes", "Socks", "Hat", "Glasses", "Outfit", "Costume", "Deck", "Grip tape", "Trucks", "Wheels"};
inline constexpr std::size_t mark_items = mark_item_names.size(), skater_mark_items = 8;
using MarkStyles = std::array<MarkStyle, mark_items>;
// The rainbow, as a style a developer or a member of staff picks (a developer's standard is
// the rainbow already). Players on builds from before it could be picked must be able to read
// the style, so it travels as one they know: a solid colour, with this in the second colour a
// solid does not use. They show the solid colour; nobody else's item takes the rainbow from it.
inline constexpr std::array<std::uint8_t, 3> rainbow_marker{0x52, 0x42, 0x57}; // "RBW"
inline bool rainbow_style(const MarkStyle &style) noexcept { return style.mode == MarkMode::solid && style.to == rainbow_marker; }
inline bool valid_mark_style(const MarkStyle &style) noexcept {
    return style.mode <= MarkMode::solid && style.speed <= 2;
}
struct Appearance {
    CosmeticRecipe skater, board;
    PlayerCard card;
    // The player has turned off what the ReSkate backend gives them (developer_identity.h):
    // nobody shows their tag, or nobody animates their hoodie and board.
    bool hide_tag{}, hide_items{};
    MarkStyles marks{};
    bool operator==(const Appearance &) const = default;
};
// Stand-ins for a peer's cosmetic that this PC does not have (a player's
// cosmetics mod): each replaces an item in a slot of its own category. Asset
// keys stay the same between seasons. Slots with none of these (outfits,
// costumes, hats, glasses, socks, stickers, tattoos...) are left empty.
inline constexpr std::array<std::string_view, 8> default_cosmetic_items{
    "Own_TopShirt_Gen_TshirtRelaxed_00007",         // Black I Heart Tee
    "Own_BottomPants_Gen_ChinoLoose_00001",         // Olive Chinos
    "Own_ShoeSneaker_Gen_SlipOnSkateboarding_00002", // Black Slip-ons
    "Own_DeckGraphic_Gen_Popsicle_00001",           // Natural Blank deck
    "Own_DeckGripCutout_Gen_Popsicle_00001",        // Plain grip pattern
    "Own_DeckGripColor_Gen_Popsicle_00001",         // Black grip
    "Own_Truck_Gen_Default_00001",                  // Basic trucks
    "Own_WheelColor_Gen_Classic_00009",             // White wheels
};
inline std::uint32_t cosmetic_asset_hash(std::string_view value) {
    if (value.empty())
        return 0;
    std::uint32_t hash = 5381;
    for (unsigned char c : value)
        hash = hash * 33 ^ c;
    return hash;
}
// Values are opaque native parameter bits; no addresses, model handles,
// inventory flags or saved-profile identifiers cross the connection.
bool valid_appearance(const Appearance &) noexcept;
} // namespace dingosdk::multiplayer
