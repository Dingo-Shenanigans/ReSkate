#pragma once
#include "board_break_model.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

// How a board wears and how hard it has to be hit to break, apart from the game: the player's
// settings, the damage each kind of hit does, what a worn board does to its handling, and what kind
// of contact a board makes with a rail. Plain numbers, so the tests can hold them.
namespace dingosdk::board_break {
// ---- the player's settings ----------------------------------------------------------------------
// Each is kept in %LOCALAPPDATA%\ReSkate\BoardBreak.settings and set from the console as
// `boardbreak <name> <value>` (the menus send the same).
struct Settings {
    bool always = false;     // every slam breaks the board, for replay clips
    int sensitivity = 100;   // percent: at 200 half the force breaks a board, at 50 twice as much
    int joined = 50;         // percent of breaks the grip tape holds together
    bool rails = true;       // a hard boardslide or darkslide onto a rail or ledge can break it
    bool wear = true;        // boards wear out, handle worse and break more easily as they do
    int wear_rate = 100;     // percent: how fast they wear
    bool crack_sounds = true;
    bool body_sounds = true; // the body's hits during a bail
};
inline constexpr int sensitivity_min = 10, sensitivity_max = 400, rate_min = 0, rate_max = 500;
struct SettingInfo {
    std::string_view name;
    bool on_off; // else a percent
    int low, high;
};
inline constexpr std::array<SettingInfo, 8> setting_info{{
    {"always", true, 0, 1}, {"sensitivity", false, sensitivity_min, sensitivity_max}, {"joined", false, 0, 100},
    {"rails", true, 0, 1}, {"wear", true, 0, 1}, {"wearrate", false, rate_min, rate_max},
    {"cracks", true, 0, 1}, {"bodysounds", true, 0, 1},
}};
// The setting `name` as a number (on/off as 1/0), or -1 for no such setting.
inline int setting_value(const Settings& s, std::string_view name) noexcept {
    if (name == "always") return s.always;
    if (name == "sensitivity") return s.sensitivity;
    if (name == "joined") return s.joined;
    if (name == "rails") return s.rails;
    if (name == "wear") return s.wear;
    if (name == "wearrate") return s.wear_rate;
    if (name == "cracks") return s.crack_sounds;
    if (name == "bodysounds") return s.body_sounds;
    return -1;
}
// Sets it, kept within its range; false for no such setting.
inline bool set_setting_value(Settings& s, std::string_view name, int value) noexcept {
    for (const auto& info : setting_info) {
        if (info.name != name) continue;
        value = std::clamp(value, info.low, info.high);
        if (name == "always") s.always = value != 0;
        else if (name == "sensitivity") s.sensitivity = value;
        else if (name == "joined") s.joined = value;
        else if (name == "rails") s.rails = value != 0;
        else if (name == "wear") s.wear = value != 0;
        else if (name == "wearrate") s.wear_rate = value;
        else if (name == "cracks") s.crack_sounds = value != 0;
        else s.body_sounds = value != 0;
        return true;
    }
    return false;
}

// ---- how hard a slam has to be ------------------------------------------------------------------
// The slam forces measured in play run from about 2 (a stumble) to 10 and more (a hard slam).
inline constexpr float heavy_slam = 10.0f;
// A worn board takes less: at the end of its life about half the force it took new.
inline constexpr float worn_strength = 0.5f;
// How likely a slam of `force` breaks the board, with the player's sensitivity (percent) and the
// board's wear (0 new, 1 worn out). A new board at 100%: never under 6, half the time at 8, always
// from 10. A bail whose force is not known (0) never breaks it.
inline float slam_chance(float force, int sensitivity, float wear) noexcept {
    if (!(force > 0.0f)) return 0.0f;
    const float s = std::clamp(sensitivity, sensitivity_min, sensitivity_max) / 100.0f;
    const float strength = 1.0f - (1.0f - worn_strength) * std::clamp(wear, 0.0f, 1.0f);
    return break_chance(force, heavy_slam * strength / s);
}

// ---- wear and tear ------------------------------------------------------------------------------
// A board's wear runs from 0 (new) to 1 (worn out). Every hit adds some, at the player's rate
// (percent of these). A fresh board lasts a good while: a few hundred hard landings, or some
// dozens of big drops, or half an hour of boardslides.
// A landing, by how fast the board was coming down as it touched (m/s): nothing up to an ollie off
// a curb's speed, then more and more.
inline constexpr float landing_free = 3.5f;
inline float landing_wear(float down_speed) noexcept {
    const float over = (std::max)(0.0f, down_speed - landing_free);
    return (std::min)(0.0008f * over * over, 0.04f);
}
// Onto a rail or ledge: the board's own hit, harder on a slide (the deck takes it) than on the trucks.
inline float rail_wear(float down_speed, bool slide) noexcept {
    const float hit = landing_wear(down_speed + (slide ? 1.0f : 0.0f));
    return hit + (slide ? 0.001f : 0.0003f);
}
// Every second of grinding or sliding.
inline float grind_wear_per_second(bool slide) noexcept { return slide ? 0.0005f : 0.00015f; }
// A slam, by its force.
inline float slam_wear(float force) noexcept { return std::clamp((force - 4.0f) / 6.0f, 0.0f, 1.0f) * 0.025f; }
inline float add_wear(float wear, float amount, int rate) noexcept {
    return std::clamp(wear + amount * std::clamp(rate, rate_min, rate_max) / 100.0f, 0.0f, 1.0f);
}
// What wear does to the board's handling, as factors on the game's own tuning: the steering answers
// less, the trucks' springs and their damping go soft (the board wobbles and leans further), and
// they twist further. Nothing shows until a board is a fifth worn.
struct Handling {
    float steering = 1, spring = 1, damping = 1, twist = 1;
    bool operator==(const Handling&) const = default;
};
inline Handling handling(float wear) noexcept {
    const float w = std::clamp((wear - 0.2f) / 0.8f, 0.0f, 1.0f);
    const float eased = w * w * (3.0f - 2.0f * w);
    return {1.0f - 0.25f * eased, 1.0f - 0.6f * eased, 1.0f - 0.5f * eased, 1.0f + 0.4f * eased};
}
// What a board's state is called on the menus. A board is "done" from `done_wear` on.
inline constexpr float done_wear = 0.85f;
inline std::string_view condition_name(float wear) noexcept {
    return wear < 0.15f ? "new" : wear < 0.4f ? "used" : wear < 0.65f ? "worn" : wear < done_wear ? "beat" : "done";
}
// A done board gives way under the rider on a landing (the nose goes, and they ride on): from a
// big drop (6.5 m/s coming down) as it becomes done, down to any real drop (3.5 m/s, a curb's)
// when it is worn right out. Never a board less worn, and never on flat ground riding: a board
// breaks from something the rider did. Sensitivity as for a slam.
inline bool gives_way_on_landing(float down_speed, int sensitivity, float wear) noexcept {
    if (wear < done_wear) return false;
    const float s = std::clamp(sensitivity, sensitivity_min, sensitivity_max) / 100.0f;
    const float t = std::clamp((wear - done_wear) / (1.0f - done_wear), 0.0f, 1.0f);
    return down_speed >= (6.5f - 3.0f * t) / s;
}

// ---- the board on a rail ------------------------------------------------------------------------
enum class Contact { grind, slide, darkslide };
constexpr std::array<std::string_view, 3> contact_names{"grind", "slide", "darkslide"};
// From the board's long axis and its up axis in the world, and how it moves: a board lying across
// its way (a boardslide, a lipslide) slides, one on its grip is in a darkslide, the rest grind on
// their trucks. A board that hardly moves grinds.
inline Contact contact_of(Vec along, Vec up, Vec velocity) noexcept {
    if (up.y < -0.35f) return Contact::darkslide;
    const float a = std::hypot(along.x, along.z), v = std::hypot(velocity.x, velocity.z);
    if (a < 0.2f || v < 0.5f) return Contact::grind;
    const float cosine = std::abs((along.x * velocity.x + along.z * velocity.z) / (a * v));
    return cosine < 0.5f ? Contact::slide : Contact::grind;
}
// How likely a board snaps as it comes down onto a rail in a slide, by how fast it was coming down
// (m/s): from 3 m/s up (a hop onto a rail is about 2), certain 3 m/s above that; a darkslide on the
// grip goes sooner. Grinds on the trucks do not snap a board. Sensitivity and wear as for a slam.
inline float rail_chance(float down_speed, Contact contact, int sensitivity, float wear) noexcept {
    if (contact == Contact::grind) return 0.0f;
    const float s = std::clamp(sensitivity, sensitivity_min, sensitivity_max) / 100.0f;
    const float strength = 1.0f - (1.0f - worn_strength) * std::clamp(wear, 0.0f, 1.0f);
    const float start = (contact == Contact::darkslide ? 2.2f : 3.0f) * strength / s;
    const float span = 3.0f * strength / s;
    return std::clamp((down_speed - start) / span, 0.0f, 1.0f);
}

// ---- where a board breaks -----------------------------------------------------------------------
// From how the board came down: nose first it breaks at the nose, tail first at the tail, flat (or
// across a rail) in the middle. `slope` is how far the nose points down (the board's long axis'
// downward part, -1 to 1); `roll` is 0 to 1, chance. Flat landings still break at an end now and
// then, as real boards do.
inline Place place_of_break(float slope, bool across_rail, float roll) noexcept {
    if (across_rail) return Place::mid;
    if (slope > 0.35f) return roll < 0.75f ? Place::nose : Place::mid;
    if (slope < -0.35f) return roll < 0.75f ? Place::tail : Place::mid;
    return roll < 0.5f ? Place::mid : roll < 0.75f ? Place::nose : Place::tail;
}

// ---- a body's hit, for its sound -----------------------------------------------------------------
// The ragdoll's hits along their normal (Extension/HallOfMeat/hall_of_meat_model.h): a graze is 2 to
// 3 m/s, a fall on flat ground 3 to 9, a drop from height 13 and more. Below `body_hit` nothing is
// heard; the loudness rises to full at `body_full`.
inline constexpr float body_hit = 3.0f, body_full = 11.0f;
inline float body_loudness(float speed) noexcept {
    if (speed < body_hit) return 0.0f;
    return std::clamp(0.3f + 0.7f * (speed - body_hit) / (body_full - body_hit), 0.3f, 1.0f);
}
}
