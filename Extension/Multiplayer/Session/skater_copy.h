#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// A copy of the local skater, drawn somewhere else: the same body in the same outfit, in the pose the
// other players would be sent now, turned and moved as one piece so that its root stands at a place.
// It is made as another player's skater is (Extension/Multiplayer/Remote/native_skater.h: the actor,
// its outfit, its pose each frame), in a slot no player has, and is this game's alone: nothing is
// sent, it has no name, sound or collision. For a picture of the skater where the skater cannot be:
// the game does not draw the local skater far from where it stands (seen 2026-11-16: its pose drawn
// in the menu's character room, 270 m off, showed nothing), and its own menu shows a copy too.
// Only outside a session: in one, every slot may hold a player.
namespace dingosdk::multiplayer {
struct SkaterCopy {
    std::array<float, 3> position{};                 // where its root stands
    std::optional<std::array<float, 4>> rotation;    // which way its root faces (as the skater does, without)
    bool board{};                                    // whether its board is drawn with it
};
// Any thread. `owner` (a plugin's id) has the copy; nullopt lets it go. Why not, or an empty text.
std::string set_skater_copy(std::string_view owner, std::optional<SkaterCopy> copy);
// Whether the copy is drawn now.
[[nodiscard]] bool skater_copy_shown() noexcept;
} // namespace dingosdk::multiplayer
