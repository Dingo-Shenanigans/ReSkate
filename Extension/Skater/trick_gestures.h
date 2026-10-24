#pragma once
#include <array>
#include <filesystem>
#include <string_view>
#include <vector>

// The flick-it gesture behind each flip trick, read from the installed game (its
// DelMarGameplayGesture assets, gameplay/input/gestures/fliptricks/), so a plugin can draw the
// stick diagram for a trick the way skate.'s own S.K.A.T.E. does. Nothing of the game's
// is shipped: it is read once, in the background, the first time it is asked for.
namespace dingosdk::tricks {
// A stick path in the gesture's own space: x to the left, y down (pulled back), the stick's gate
// the unit circle; a regular stance (mirror x for goofy).
using StickPath = std::vector<std::array<float, 2>>;
// Starts the reading (once); nothing until it is done.
void prepare_trick_gestures() noexcept;
// Reads them now from a game folder (the one with Skate.exe), for tests; how many were found.
// Throws when they cannot be read.
std::size_t load_trick_gestures(const std::filesystem::path &game_root);
// The path for one trick as skate. names it ("Kickflip", "Nollie Heelflip", "360 Pop Shove-it");
// empty for a trick with no flick (grabs, grinds, manuals) or one not known.
StickPath trick_gesture(std::string_view trick);
} // namespace dingosdk::tricks
