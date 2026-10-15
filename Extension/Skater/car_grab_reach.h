#pragma once
#include "car_grab/controller.h"
#include <cstdint>

namespace dingosdk::car_grab_native {
enum class ReachStatus { idle, blending, applied, unavailable, approaching, contact };
struct ReachTarget {
    std::uintptr_t base{}, client{}, context{}, entity{};
    std::uint64_t world{}, vehicle{};
    car_grab::Vec3 point{};
    double sampled_at{};
    bool active{};
    car_grab::Vec3 normal{}, tangent{}, velocity{};
    std::uint64_t surface_id{};
    bool wrap_valid{};
    car_grab::Vec3 top_point{}, bottom_point{}, top_normal{}, bottom_normal{};
};
struct ReachFeedback {
    car_grab::SkaterGripReach grip_reach{};
    bool unclamped{}, palm_contact{}, finger_grip{};
    float palm_error{};
    std::uint64_t surface_id{};
};
// A snapshot only for the exact local ownership/world/car tuple. Feedback is
// timestamped; the caller must reject samples older than 150 ms.
bool read_reach_feedback(const ReachTarget& identity, ReachFeedback& out) noexcept;
// Only publish a target after the native bridge has validated local ownership
// and the selected car. active=false blends a normal release out; clear_reach
// immediately disarms on focus, world, identity or eligibility failure.
void publish_reach(const ReachTarget&) noexcept;
void clear_reach() noexcept;
ReachStatus reach_status() noexcept;
// Existing shared animation hook calls this after the native evaluation, once.
// It changes evaluated arm, wrist and digit rotations; never physics bodies.
void reach_on_animation(std::uintptr_t component) noexcept;
} // namespace dingosdk::car_grab_native
