#pragma once
#include "Engine/Game/Skater/skater_body.h"
#include "Extension/Skater/skater_physics.h"
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

// What the local skater's physics does, as news for plugins (reskate's board_landed, body_hit and
// bail_end events, board.motion, the bail's force): read in each physics step No Bail hands on
// (no_bail.h), where the values are exact, and kept for the client tick to take. Nothing is read
// until want() is first called.
namespace dingosdk::skater_events {
// The board came down from the air: how fast it was coming down, how fast along, onto a rail.
struct Landing {
    float down_speed{}, speed{};
    bool rail{};
};
// A body hit something in a bail, hard enough to be heard (at most one a bone every 220 ms).
struct BodyHit {
    std::uint8_t bone{}; // skater_body::Bone
    float impact{};      // metres per second along the contact's normal
    bool vehicle{}, board{};
};
// A bail's fall, once the body is up or 3 s have passed: what each body hit and slid, summed.
struct Fall {
    float seconds{};
    std::array<float, skater_body::count> impact{}, slide{};
    float left{}, right{}; // the limbs of each side, summed
};
// The force and cause of the last wipeout or slam (the game's cause collector), and when (ms).
struct Slam {
    float force{};
    std::int32_t cause{-1};
    std::uint64_t at{};
};
// Every physics step as it was read (the physics_step event): when, on the steady clock in
// milliseconds (as reskate.time.now), and what it read.
struct StepNews {
    double at{};
    skater_physics::Step step;
};
// Client thread: starts reading the steps (once).
void want() noexcept;
// Client thread: from now on every step is kept for take_steps too (at most the last 512).
void want_steps() noexcept;
std::vector<StepNews> take_steps();
std::vector<Landing> take_landings();
std::vector<BodyHit> take_hits();
std::vector<Fall> take_falls();
// The board's velocity in the last step (m/s), while it is known.
std::optional<std::array<float, 3>> board_velocity() noexcept;
// Its spin then (angular velocity in the world, radians a second), while it is known.
std::optional<std::array<float, 3>> board_spin() noexcept;
Slam last_slam() noexcept;
// From No Bail's hooks (no_bail.cpp), beside Board Break's: a cause recorded, a wipeout chosen.
void note_cause(std::int32_t reason, float magnitude) noexcept;
void note_wipeout(std::uintptr_t causes) noexcept;
}
