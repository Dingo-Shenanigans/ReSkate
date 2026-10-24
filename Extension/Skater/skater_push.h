#pragma once
#include <array>
#include <optional>

// A push for the local skater, for plugins (reskate.skater.push / reskate.skater.hold): a
// horizontal velocity the next on-board physics step writes (client_noclip.cpp), within two
// seconds of the request, and a hold that keeps the board still (a countdown, a checkpoint
// return). Any thread.
namespace dingosdk::skater_push {
using Vec3 = std::array<float, 3>;
inline constexpr float max_speed = 120.f;
// Finite, horizontal (y == 0) and at most max_speed.
bool valid(const Vec3& velocity) noexcept;
// Replaces any pending push; an invalid velocity only cancels it.
void request(const Vec3& velocity) noexcept;
// The skater is still getting back on the board: the pending push waits two seconds more.
void wait() noexcept;
void reset() noexcept;
void hold(bool on) noexcept;
bool holding() noexcept;
// A push is waiting for its physics step.
bool pending() noexcept;
// The physics step: the velocity to write now (the pending push, else zero while held), or nothing.
std::optional<Vec3> peek() noexcept;
// The physics step wrote it.
void accepted(const Vec3& velocity) noexcept;
} // namespace dingosdk::skater_push
