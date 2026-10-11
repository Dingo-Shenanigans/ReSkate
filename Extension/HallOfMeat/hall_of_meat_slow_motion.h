#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

// The slow motion after a break: the game run below its own speed for a while, smoothly. skate.
// steps its simulation at a fixed rate on a clock its time scale slows
// (Engine/Game/Settings/simulation_time.h): at 0.3 alone only 18 steps come each real second, and the
// camera, moving every frame, follows a skater that jumps 18 times a second. So the rate goes up as
// the time scale goes down, keeping as many steps each real second as before, each the shorter
// (measured in play: at 0.3 with a rate of 200 the slow motion runs smooth). The three
// native settings are changed as a session override (Extension/Settings/named_settings.h).
//
// Every break's slow motion runs the same speeds, whatever the game ran at, but never faster than
// that: with the trainer's game speed (PRACTICE) below 1 it only takes the part of the curve that is
// slower still. What it found is written back at the end, each setting only while it still holds
// what the slow motion wrote; one changed from elsewhere meanwhile (the trainer, the console) is
// theirs, and ends this slow motion.
//
// The local skater goes along: skate. gives a skater its physics step length when its core is built
// (at a spawn and at every teleport) and never touches it again (measured in play; Ghidra
// build_pose_object, world_get_physics_step_length). A rate changed live would leave the skater
// stepping out of line, and one teleported meanwhile would keep the changed step. So while the rate is
// held the skater's step is the held rate's, and once the rate is given back it gets the game's own back.
//
// Never during a multiplayer session: the local simulation must keep the others' pace, so a session
// that starts while it runs ends it. Other skaters (AI skaters) are not followed. Game update thread only.
namespace dingosdk::hall_of_meat {
inline constexpr float slowest_speed = 0.05f;              // what the steps a second can follow
inline constexpr std::uint32_t highest_simulation_rate = 1200; // steps a second of game time, at most

// The simulation rate (steps a second of game time) that keeps `base_rate` steps each real second at `speed`.
inline std::uint32_t slow_motion_rate(std::uint32_t base_rate, float speed) noexcept {
    const float clamped = std::isfinite(speed) ? std::clamp(speed, slowest_speed, 1.0f) : 1.0f;
    const float rate = std::round(static_cast<float>(base_rate) / clamped);
    return static_cast<std::uint32_t>(std::clamp(rate, 1.0f, static_cast<float>(highest_simulation_rate)));
}
// The time scale the slow motion runs at `speed` when the game ran at `found`; nothing when the game
// already runs as slowly (or the speed is broken).
inline std::optional<float> slow_motion_time_scale(float found, float speed) noexcept {
    if (!std::isfinite(found) || !std::isfinite(speed)) return {};
    const float scale = std::clamp(speed, slowest_speed, 1.0f);
    if (!(scale < found)) return {};
    return scale;
}

class SlowMotion {
public:
    // Runs the game at `speed` while that is slower than the speed it found (1 or more ends the slow
    // motion). False when it does not now (no slower than the game already runs, a session, no local
    // skater, the settings not ready, or changed from elsewhere during this slow motion): then the
    // game runs as it was.
    bool set(float speed);
    // Gives back what was found, and the local skater its own step once it can be reached.
    void release();
    // Nothing of the game's is held: its settings and the local skater's step are as they were.
    bool idle() const noexcept { return !held_ && !skater_changed_; }

private:
    // The three settings as found, and what the slow motion last wrote over each (empty: nothing yet).
    struct Held {
        std::array<std::string, 3> found, ours;
    };
    bool still_ours() const;

    std::optional<Held> held_;
    bool yielded_{};                         // a setting changed from elsewhere: none until the next slow motion
    std::optional<std::uint32_t> base_rate_; // the game's own simulation rate, while it matters
    bool skater_changed_{};                  // the local skater's step is ours, to be given back
};
}
