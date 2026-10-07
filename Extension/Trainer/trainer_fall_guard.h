#pragma once
// The fall-through guard's decision, kept free of the game so it can be tested on its own
// (Test/fall_guard_tests.cpp). trainer.cpp feeds it the skater's height and speed and asks
// the physics world whether anything is under them before it rescues anyone.
#include <optional>

namespace dingosdk::trainer::fall_guard {
constexpr float falling_speed = -10.0f; // metres per second, downward
constexpr float deep_below = 25.0f;     // under the last solid spot
constexpr float void_height = -2000.0f; // no map goes this low: always a fall through it
constexpr unsigned check_every_ms = 100, safe_every_ms = 500, settle_ms = 5000, after_rescue_ms = 3000;

// A fast fall far below the last place the skater stood, or a height no map reaches. Only a
// suspicion: a long drop off a real ledge looks the same until the ray finds nothing below.
inline bool suspect(float y, float vertical, std::optional<float> last_safe_y) noexcept {
    if (y <= void_height) return true;
    return vertical < falling_speed && last_safe_y && y < *last_safe_y - deep_below;
}
// Standing or rolling on something: a spot to come back to.
inline bool steady(float vertical, bool airborne) noexcept { return !airborne && vertical > -1.0f && vertical < 1.0f; }
} // namespace dingosdk::trainer::fall_guard
