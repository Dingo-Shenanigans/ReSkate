#pragma once
#include <string>
#include <string_view>

// Keyframe smoothing's settings as the player left them, kept between game runs
// (%LOCALAPPDATA%\ReSkate\KeySmooth.settings, a line each: on=1, amount=1).
namespace dingosdk::replay_keyframes {
struct Settings {
    bool on = false;     // off: the game's own curve, whatever the rest says
    float amount = 1.0f; // smoothness σ when on, 0..1
};
std::string to_text(const Settings& settings);
// Lines it doesn't know, or values out of range, keep the defaults.
Settings from_text(std::string_view text);
} // namespace dingosdk::replay_keyframes
