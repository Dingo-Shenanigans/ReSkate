#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>
#include "Extension/UI/Overlay/scripted_pad.h"

// The game's stick gestures for the ground flip tricks, read from its data (Gameplay/Input/Gestures/FlipTricks).
namespace dingosdk::style {
// Angles are degrees clockwise from up; `wait` is seconds after the part before (0: no limit).
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
// A gesture's Value as a FlipTrickType: 1 to 63, else none.
[[nodiscard]] std::optional<std::uint8_t> flip_trick_type(std::int64_t value) noexcept;
// A stick position from -1 to 1 as an XInput axis value. NaN and infinity give the middle.
[[nodiscard]] std::int16_t stick_axis(float value) noexcept;
// Recordings run on game ticks, one per client update at this rate while recording, so a take is the same at any frame rate.
inline constexpr std::uint32_t tick_hz = 60;
[[nodiscard]] inline constexpr std::uint32_t to_ticks(std::uint32_t ms) noexcept { return (ms * tick_hz + 500) / 1000; }
[[nodiscard]] inline constexpr std::uint64_t to_ms(std::uint64_t ticks) noexcept { return (ticks * 1000 + tick_hz / 2) / tick_hz; }
inline constexpr std::uint32_t max_flick_ms = 1000;
// Pad steps in game ticks: a crouch, then the flick over `flick_ms`. Empty when the gesture has no arcs.
[[nodiscard]] std::vector<overlay::PadStep> flick(const TrickGesture &gesture, std::uint32_t flick_ms);
} // namespace dingosdk::style
