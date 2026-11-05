#pragma once
#include "Extension/Multiplayer/developer_identity.h"
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
// How one of a player's marked cosmetics is coloured, chosen on the Skater page's STYLE tab and
// carried in their Appearance, so everyone sees the same. Anyone may give an item a solid
// colour; the rest comes with a list of the backend's (developer_identity.h,
// developer_hoodie_material.h: item_animation).
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
// Which list's tag a player shows (Appearance::tag) travels the same way, so that builds from
// before it still read the appearance it is in: the packet has no room of its own for it, and
// an older build turns down a packet with anything more. It goes in the second colour of a
// style that does not use it (one standard, off, or a solid colour that is not the rainbow),
// as these two bytes and the list's number. Every build ignores that colour there.
inline constexpr std::array<std::uint8_t, 2> tag_marker{0x54, 0x47}; // "TG"
inline bool carries_tag(const MarkStyle &style) noexcept {
    return style.mode != MarkMode::gradient && !rainbow_style(style) && style.to[0] == tag_marker[0] && style.to[1] == tag_marker[1];
}
// The styles as sent: the choice in the first free second colour from the end (the board's
// parts first, which the fewest players colour), and no other one that reads as a choice.
// When every style uses its second colour the choice is left out, and the usual tag shows.
inline MarkStyles carry_tag(MarkStyles styles, std::uint8_t tag) noexcept {
    for (auto &style : styles)
        if (carries_tag(style)) style.to[1] ^= 1;
    if (tag == usual_tag) return styles;
    for (auto style = styles.rbegin(); style != styles.rend(); ++style)
        if (style->mode != MarkMode::gradient && !rainbow_style(*style)) {
            style->to = {tag_marker[0], tag_marker[1], tag};
            break;
        }
    return styles;
}
// The choice the styles carry (usual_tag for none), taken out: that colour reads as nothing.
inline std::uint8_t carried_tag(MarkStyles &styles) noexcept {
    for (auto &style : styles)
        if (carries_tag(style)) {
            const auto tag = style.to[2];
            style.to = {};
            return tag;
        }
    return usual_tag;
}
struct Appearance {
    CosmeticRecipe skater, board;
    PlayerCard card;
    // The player has turned off what the ReSkate backend gives them (developer_identity.h):
    // nobody shows their tag, or nobody animates their hoodie and board.
    bool hide_tag{}, hide_items{};
    MarkStyles marks{};
    // Which of their lists' tags shows, for a player on more than one (developer_identity.h:
    // identity_tag); usual_tag for the usual. In the styles on the wire (carry_tag).
    std::uint8_t tag = usual_tag;
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
