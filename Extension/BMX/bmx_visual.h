#pragma once
#include <array>
#include <cstdint>
#include "Engine/Game/BMX/bmx_trick_catalog.h"
#include "Engine/Game/BMX/bmx_spring_avatar_binding.h"

namespace dingosdk::bmx::visual {
enum class RiderMode { riding, none, carrying };
// Bike root is ground/contact height, +Z forward, +Y up; angles are radians.
struct Frame {
    bool active{};
    std::array<float, 3> position{};
    float yaw{}, pitch{}, roll{}, steer{}, front_wheel_phase{}, rear_wheel_phase{}, crank_phase{}, barspin{}, tailwhip{};
    std::uintptr_t entity{}; // optional live local rider; never a cached native core
    float preload{}, front_weight{}, rear_weight{};
    bool airborne{};
    float barspin_release{}, tailwhip_release{}; // [0,1]: release limbs until an actual trick catch
    // Optional measured chassis rotation, Hamilton XYZW. Position remains the
    // assembled bike root; convert COM with chassis::root_from_com before publishing.
    std::array<float, 4> chassis_quaternion{0, 0, 0, 1};
    bool chassis_quaternion_available{}; // false retains the exact legacy Euler path
    catalog::TrickId movement_id{catalog::TrickId::none};
    float movement_phase{}, movement_progress{};
    float release_left_hand{}, release_right_hand{}, release_left_foot{}, release_right_foot{};
    float landing_compression{}, pedal_effort{};
    // Optional native-time presentation of the existing4cm air extension.
    // Legacy frames retain their airborne posture until the bridge supplies it.
    float air_extension_blend{}; // [0,1]; never a physical support/hop flag
    bool air_extension_available{};
    // Optional measured two-wheel incline presentation; defaults preserve legacy
    // posing exactly. Native-step bridge owns advance/reset, never render time.
    float rider_uphill_blend{},rider_gaze_counterpitch{}; // [0,1], [0,25deg] radians
    bool rider_posture_available{};
    // Presentation-only relative bike articulation; never a native body target.
    float movement_bike_pitch{}, movement_bike_roll{};
    RiderMode rider_mode{RiderMode::riding};
    float rider_blend{1}; // transition from current native pose to the bike pose
    // Ordinary mounts retain native limb lengths and first fresh pose contacts.
    // This token belongs to the acquired local body, never to an old rider.
    std::uint64_t mount_token{};
    float mount_progress{1};
    bool mount_from_left{};
    // Accepted controller presentation phase, exported only by native ride.
    // This is not a second animation clock or a physical support/trick flag.
    bool movement_timing_available{},movement_holding{},movement_recovering{},movement_caught{};
    float movement_recovery_progress{};
    std::uint64_t native_body_token{}; // admitted physical owner; mount_token is entry-only
    // Actual same-epoch Spring and bike observations. Default unrequested keeps
    // the existing one-body presentation byte-for-byte on its existing path.
    avatar_binding::Frame spring_avatar;
};
struct Publication {Frame frame;std::uint64_t revision{};bool available{};};
// Client tick stages a frame, places its exact copy, then admits only the
// matching successful all-part receipt. This is native setter ordering, not a
// guarantee about final GPU consumption or a native pose-buffer lifetime.
void begin_client_tick() noexcept;
Publication pending_placement() noexcept;
bool complete_placement(std::uint64_t revision,std::uintptr_t entity,bool all_parts_placed) noexcept;
void publish(const Frame& frame) noexcept; // stages only; inactive withdrawal is immediate
void draw() noexcept; // overlay thread; shaded 3D triangles, no world depth buffer
bool uses_reference_mesh() noexcept;
std::uint64_t draws() noexcept; // nonempty triangle submissions, never frame publications
bool pending() noexcept; // keeps the overlay drawing while a fresh bike frame is active
}
