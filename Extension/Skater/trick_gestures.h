#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>

// The game's stick gestures for the ground flip tricks, read from its data (Gameplay/Input/Gestures/FlipTricks).
namespace dingosdk::style {
// One part of a flick: the stick passes between these angles (degrees clockwise from up) at a magnitude in this range,
// within `wait` seconds of the part before (0: no limit).
struct GestureArc {
    float from{}, to{}, low{}, high{}, wait{};
};
struct TrickGesture {
    std::uint8_t trick{}; // FlipTrickType
    bool nollie{};
    // The crouch: the stick held between these angles at this magnitude or more.
    float hold_from{}, hold_to{}, hold_magnitude{};
    // The flick in time order, from the first arc pattern of the gesture. XInput axes: the data's are turned half a circle.
    std::vector<GestureArc> arcs;
};
// One gesture for each ground flip trick. Throws if the data has none.
std::vector<TrickGesture> read_trick_gestures(const std::filesystem::path &game_root);
} // namespace dingosdk::style
