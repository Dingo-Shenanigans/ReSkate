#pragma once
// What the trainer changes outside Gameplay/SkatePhysicsTuning, as a session's host shares it
// with its guests: the tuning asset travels as the session's physics tuning already
// (Extension/Skater/physics_tuning.h); this is the rest, so that "the host's physics for
// everyone" means all of it. The bytes are the session's physics extras
// (Engine/Game/Multiplayer/session_physics.h). No game access: plain data in, plain data out.
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace dingosdk::trainer {
// What the trick multipliers may be, here and in the trainer's own options.
inline constexpr float height_low = 0.001f, height_high = 1.0e6f; // heights; the sliders stop far short, a typed number need not
inline constexpr float catch_low = 1.0f, catch_high = 100.0f;    // where in a jump a flip is caught, percent of the air time
inline constexpr float flip_low = 0.01f, flip_high = 1000.0f;     // board flip speed; the slider stops far short, a typed number need not
inline constexpr float pump_high = 1.0e6f;                             // pump power; 1 is the game's own
inline constexpr float revert_boost_high = 1.0e6f;                     // revert boost strength; 0 is off
inline constexpr float cruise_high = 100.0f;                     // auto push, m/s

struct SessionExtras {
    // x of the game's own: board flip speed, and the heights of the hippy jump, no comply,
    // boneless and on-foot jump.
    float flip_speed{1}, hippy_height{1}, nocomply_height{1}, boneless_height{1}, offboard_height{1};
    // Auto push: the speed a rolling skater is carried up to, m/s; 0: off.
    float cruise{};
    // x of the game's own while an underflip turns, and whether only its under part does (its flip then at the flip
    // speed). Sent only when not the game's own (version 2), so a guest on an older trainer still reads a host who
    // leaves them alone.
    float underflip_speed{1};
    bool underflip_split{};
    // Values of the game's tuning classes that are not the game's own: an index into
    // class_fields (trainer_classes.h) and the value, by ascending index.
    std::vector<std::pair<std::uint16_t, float>> classes;
    bool operator==(const SessionExtras &) const = default;
    // Nothing differs from the game's own.
    bool stock() const { return *this == SessionExtras{}; }
};
// Identifies the table of class fields the indices refer to (the game build's): a host and a
// guest whose tables differ share the multipliers only.
std::uint32_t class_table_id() noexcept;
// Nothing at all for extras that are the game's own.
std::vector<std::uint8_t> encode_session_extras(const SessionExtras &extras);
// A host's bytes: empty is the game's own; nullopt is not an encoding (also the game's own to
// whoever asks, and worth a log line). Multipliers are held to the ranges above, and class
// values written for another table are left out.
std::optional<SessionExtras> decode_session_extras(std::span<const std::uint8_t> bytes);
} // namespace dingosdk::trainer
