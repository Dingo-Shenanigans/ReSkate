#pragma once
#include <cstdint>

namespace dingosdk {
bool start_no_bail(std::uintptr_t image_base) noexcept;
bool no_bail_available() noexcept;
// Publish from the validated local client tick. Returns owner availability even
// when both controls are off. Manual protection expires if ticks stop arriving.
bool update_no_bail(std::uintptr_t client, std::uintptr_t entity, bool manual,
    bool flying, std::uint64_t flight_expires) noexcept;
void clear_no_bail() noexcept;
// S.K.A.T.E.: while `locked`, the local skater cannot get back on the board once it is off
// (its mount request is dropped). Publish from the client tick; it expires if ticks stop.
// Releasing it also leaves the skater's teleport option on the board again, since a turn's
// teleport may have set it off (skater component +0xc0) and the SDK's own teleports keep it.
void update_board_lock(std::uintptr_t client, std::uintptr_t entity, bool locked) noexcept;
// The physics state the local skater's selector last chose, for the trainer (air time, bail
// markers). Publish the skater to watch from the client tick; it expires if ticks stop.
struct PhysicsStateWatch {
    bool valid{};
    std::uint32_t state{};
    std::uint32_t previous{}; // the state before this one
    float previous_seconds{}; // how long that one lasted
    std::uint64_t changes{}, wipeouts{}; // counted since the process started
};
void watch_physics_state(std::uintptr_t client, std::uintptr_t entity) noexcept;
PhysicsStateWatch watched_physics_state() noexcept;
// Stopping flight must not discard the independent manual preference.
void clear_no_bail_flight() noexcept;
// One bail cause the game recorded for the watched local skater, as it was recorded: the
// native cause code and magnitude, and which of the three native collision sites raised it
// when it is an impact bail (impact_bail_calls, reasons 13, 9 and 14), otherwise -1.
struct ImpactEvent {
    std::uint64_t sequence{};
    std::uint64_t tick{};   // GetTickCount64() when the game recorded it
    std::int32_t reason{};
    float magnitude{};
    std::int32_t site{-1};
};
// Publish the skater to watch from the client tick; it expires if ticks stop. Events are
// recorded on the game's physics thread, so they are queued here and read on the client
// tick. Recording works whether or not No Bail is protecting the skater.
void watch_impacts(std::uintptr_t client, std::uintptr_t entity) noexcept;
// Makes the local skater bail now, the way the game's own impact sites do: raises the impact
// request bit in the physics context and records an impact cause. Works only while the skater
// resolves as the live local one. False if it could not be done. Call again on the next ticks
// until the physics state becomes a wipeout: the native per-step reset can clear one attempt.
bool force_bail(std::uintptr_t client, std::uintptr_t entity) noexcept;
// The oldest event not yet taken. At most the last eight are kept. False when none is waiting.
bool take_impact(ImpactEvent& event) noexcept;
}
