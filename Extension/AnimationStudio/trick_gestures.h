#pragma once
#include "Extension/UI/Overlay/scripted_pad.h"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// The game's stick gestures for the ground flip tricks, read from its data
// (Gameplay/Input/Gestures/FlipTricks), and the pad script that does one. From PR #230 (Norms).
namespace dingosdk::anim_studio::gestures {
// Angles are degrees clockwise from up; `wait` is seconds after the part before (0: no limit).
struct Arc {
    float from{}, to{}, low{}, high{}, wait{};
};
struct Gesture {
    std::uint8_t trick{}; // the game's FlipTrickType
    bool nollie{};
    std::string name;     // the gesture's own name, as gesture_fliptrick_kickflip
    // The crouch: the stick held between these angles at this magnitude or more.
    float hold_from{}, hold_to{}, hold_magnitude{};
    // The flick in time order, from the first arc pattern of the gesture. XInput axes: the data's are turned half a circle.
    std::vector<Arc> arcs;
};
// One gesture for each ground flip trick. Throws if the data has none.
std::vector<Gesture> read(const std::filesystem::path& game_root);
// A stick position from -1 to 1 as an XInput axis value. NaN and infinity give the middle.
[[nodiscard]] std::int16_t stick_axis(float value) noexcept;
// Pad steps in game ticks (60 a second): the crouch for `crouch_ticks`, then the flick over `flick_ms`. The pop builds
// while the stick is held down, so the crouch's length is the pop's height (3-5 ticks low, 7-11 medium, 13 and up
// high). Empty when the gesture has no arcs.
[[nodiscard]] std::vector<overlay::PadStep> flick(const Gesture& gesture, std::uint32_t flick_ms, std::uint32_t crouch_ticks = 18);
} // namespace dingosdk::anim_studio::gestures
