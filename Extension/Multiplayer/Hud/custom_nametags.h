#pragma once
#include "Extension/UI/Overlay/overlay.h"
#include "Extension/Multiplayer/developer_identity.h"
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// ReSkate's own nametags (drawn by the overlay, Extension/UI/Overlay/nametag_overlay.cpp)
// in place of the game's nametag and compass arrows; the pause map's player icons stay.
namespace dingosdk::multiplayer {
// One line in a player's bubble stack: the text (masked when the chat filter is on), the line
// as sent when the filter changed it (so emote names survive), how far its pop-in has come
// (0 just arrived, 1 settled) and how opaque it still is (1, down to 0 as it goes).
struct NametagBubble {
    std::string text, raw;
    float appear{1}, fade{1};
};
struct NametagPlayer {
    std::array<float, 3> head{}; // world position above the skater
    std::string name;
    std::uint32_t color{0xffffffffU}; // IM_COL32 layout
    std::string tag;                  // role badge before the name, as in chat ("" for none)
    bool talking{};
    // The player's recent chat lines to show as bubbles above the head, oldest first. Empty
    // draws no bubbles.
    std::vector<NametagBubble> bubbles;
    // The local player's own tag: only ever draws bubbles, never a name or dot.
    bool self{};
    // Another player whose name is not shown (friends-only nametags): bubbles only, like `self`.
    bool nameless{};
};
// Role colours; friends are the viewer's own Steam friends.
inline constexpr std::uint32_t nametag_white = 0xffffffffU, nametag_developer = 0xffff78b4U, // purple
                               nametag_creator = 0xff6464ffU,                            // red
                               nametag_homie = 0xff7ae2ffU,                              // gold
                               nametag_centrix = 0xffff7b1fU,                            // blue (#1F7BFF)
                               nametag_staff = 0xff71cc2eU,                              // green (#2ECC71)
                               nametag_admin = 0xffc674ffU,                              // pink
                               nametag_host = 0xffffc85eU,                               // blue
                               nametag_friend = 0xff8ae07bU,                             // green
                               nametag_server = 0xffff5c8eU,                             // violet (#8E5CFF)
                               nametag_server_text = 0xffffc8d9U;                        // lavender (#D9C8FF), its lines' text
// Developers, staff, content creators, homies and Centrix shimmer from these to their role
// colour (Centrix between white and its blue).
inline constexpr std::uint32_t nametag_developer_start = 0xffff206eU, // #6E20FF, IM_COL32 layout
                               nametag_creator_start = 0xff2e10c8U,   // #C8102E
                               nametag_homie_start = 0xff008ae0U,     // #E08A00
                               nametag_centrix_start = 0xffffffffU,   // white
                               nametag_staff_start = 0xff3a6b0bU;     // #0B6B3A

// Client thread, once per rendered frame: everyone to label and the local skater's position
// (for distances). Also reads whether the game is hiding its own nametags right now.
// `show_names` draws the name, distance and role badge; `show_bubbles` draws chat bubbles
// within `bubble_distance` metres. Names show within `name_distance` metres; past it, and for
// players off screen, a dot when `dots` is on.
void publish_custom_nametags(std::uintptr_t base, std::vector<NametagPlayer> players,
                             std::optional<std::array<float, 3>> local, bool show_names,
                             bool show_bubbles, float bubble_distance, float name_distance = 120.f,
                             bool dots = true) noexcept;
void set_custom_nametags_enabled(bool enabled) noexcept;
bool custom_nametags_enabled() noexcept;
// The overlay's feed (any thread): nothing while off, stale, or while the game hides nametags.
overlay::Nametags custom_nametags();
} // namespace dingosdk::multiplayer
