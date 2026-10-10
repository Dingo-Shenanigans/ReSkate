#pragma once
#include <array>
#include <cstdint>

namespace car_grab { struct TargetInfo; }

namespace dingosdk::car_grab_hud {
// Copied values only. Publishing never transfers native pointers or calls the
// renderer; presentation never queries the game or changes controls.
// Reaching, palm contact and full grip are separate producer-reported states.
// Kinematic towing has its own car state and never represents physical grip.
// The renderer never estimates progress from time or distance.
enum class Phase {
    hidden, idle, available, seeking, attached, released, blocked,
    unavailable, approaching, palm_contact, towing
};
enum class Cause {
    none, let_go, braked, jumped, out_of_reach, car_disappeared,
    vehicle_stale, reset, not_on_board, controls_busy,
    car_not_detected, surface_unavailable, vehicle_too_slow
};
struct Feedback {
    Phase phase = Phase::hidden;
    Cause cause = Cause::none;
    bool controller{};
    float speed_mps{}, distance{};
    std::uint64_t world{};
    // Optional rear-bumper marker, using the camera transform/FOV already
    // validated and copied on the local game thread. Invalid projection data
    // hides the marker while leaving the state card intact.
    bool marker{};
    std::array<float, 3> anchor{};
    bool camera_valid{};
    std::array<float, 16> camera{};
    float vertical_fov = 55.f;
};
struct SearchFeedback { Phase phase; Cause cause; };
// Advisory copied-value presentation; it cannot authorize towing.
SearchFeedback search_feedback(const car_grab::TargetInfo&) noexcept;
void publish(Feedback feedback) noexcept;
void clear() noexcept;
}

namespace dingosdk::overlay::detail {
// Poll before the hidden-overlay early return; draw after ImGui::NewFrame.
bool car_grab_hud_pending();
void draw_car_grab_hud();
}
