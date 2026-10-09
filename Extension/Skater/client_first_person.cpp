#include "client_source_spawn.h"
#include "client_source_spawn_internal.h"
#include "Extension/Multiplayer/Remote/native_pose_layout.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "free_flight.h"
#include <cmath>

namespace dingosdk::client_source::detail {
bool first_person_write_fov(std::uintptr_t camera, float fov) noexcept {
    if (!std::isfinite(fov) || fov <= 1.0f || fov >= 175.0f || camera < 0x10000 || camera > source_highest - 0x100) return false;
    __try { *reinterpret_cast<volatile float*>(camera + camera_fov_offset) = fov; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Puts back the FreeCamera's own FOV so a later Freecam session is unaffected.
void first_person_restore_fov(InteractiveDebug& debug) noexcept {
    if (debug.first_person_saved_fov > 0 && debug.camera_identity.camera)
        (void)first_person_write_fov(debug.camera_identity.camera, debug.first_person_saved_fov);
    debug.first_person_saved_fov = 0;
}
// Puts back the FOV the camera had before Freecam's own FOV was applied.
void free_camera_restore_fov(InteractiveDebug& debug) noexcept {
    if (debug.free_camera_saved_fov > 0 && debug.camera_owned && debug.camera_identity.camera) {
        if (debug.first_person) {
            // First person put its own value on top; hand it the original instead.
            if (debug.first_person_saved_fov > 0) debug.first_person_saved_fov = debug.free_camera_saved_fov;
        } else {
            (void)first_person_write_fov(debug.camera_identity.camera, debug.free_camera_saved_fov);
        }
    }
    debug.free_camera_saved_fov = 0;
}
FirstPersonArm& first_person_arm() {
    static auto* arm = new FirstPersonArm;
    return *arm;
}
void first_person_disarm() noexcept {
    auto& arm = first_person_arm();
    arm.thread.store(0, std::memory_order_release);
    arm.watched.store(0, std::memory_order_release);
    multiplayer::set_local_hidden_joint(0, 0, {1, 1, 1});
    arm.spring.reset();
    std::lock_guard lock(arm.snapshot_mutex);
    arm.captures = 0;
}
namespace {
// First person. The animation output pose is parent-local {scale, Hamilton
// quaternion, translation} per joint, and joint 1 (AITrajectory) carries the
// character's world placement, so composing from the root joint lands directly
// in world space. Indices are the 395-joint Animation/Dingo/AnimBase_Default_
// Skeleton, which drives every body type (the 137-joint per-body skeletons are
// render rigs). Its Head joint has local +X up, +Y out of the face and -Z toward
// the skater's right; both measured on a live skater on 2026-09-21.
constexpr std::uint32_t first_person_skeleton_joints = 395;
constexpr std::array<std::uint16_t, 10> first_person_head_chain{0, 1, 7, 42, 43, 44, 45, 101, 102, 103};
constexpr std::uint16_t first_person_face = 107, first_person_right_eye = 108, first_person_left_eye = 109;
// Metres ahead of the eyes along the face, so the skater's own face mesh sits
// behind the camera rather than across the near plane.
constexpr float first_person_forward_offset = 0.05f;
}
bool first_person_read(std::uintptr_t address, void* out, std::size_t size) noexcept {
    if (address < 0x10000 || size > 0x10000 || address > source_highest - size) return false;
    __try { std::memcpy(out, reinterpret_cast<const void*>(address), size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
namespace {
bool first_person_write(std::uintptr_t address, const void* in, std::size_t size) noexcept {
    if (address < 0x10000 || size > 0x10000 || address > source_highest - size) return false;
    __try { std::memcpy(reinterpret_cast<void*>(address), in, size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Shrinks the head joint of the pose just evaluated, on the thread that
// evaluated it, so the skater's head and everything parented to it (face,
// eyes, hair, headwear) collapse to a point instead of filling the view. The
// next update rewrites the whole pose, so nothing needs restoring. Poses sent
// to other players keep the real scale (set_local_hidden_joint).
constexpr std::uint16_t first_person_head_joint = 103;
// The head joint's real scale, packed as three floats; read back when the
// camera composes a pose whose head is already shrunk.
std::atomic<std::uint64_t> first_person_head_scale_xy{}, first_person_head_scale_z{};
void first_person_hide_head(std::uintptr_t base, std::uintptr_t component) noexcept {
    std::uintptr_t holder{};
    if (!first_person_read(component + 0xa0, &holder, 8) || !holder) return;
    const auto pose = multiplayer::read_native_pose_layout(first_person_read, base, holder, 512);
    if (!pose.buffer || pose.count != first_person_skeleton_joints) return;
    const auto scale_address = pose.buffer + first_person_head_joint * 0x30ULL;
    std::array<float, 3> scale{};
    if (!first_person_read(scale_address, scale.data(), sizeof(scale))) return;
    for (const auto value : scale) if (!std::isfinite(value) || value < 0.05f || value > 20.0f) return; // Already hidden.
    multiplayer::set_local_hidden_joint(component, first_person_head_joint, scale);
    std::uint64_t xy{}; std::memcpy(&xy, scale.data(), 8);
    std::uint32_t z{}; std::memcpy(&z, &scale[2], 4);
    first_person_head_scale_xy.store(xy, std::memory_order_relaxed);
    first_person_head_scale_z.store(z, std::memory_order_release);
    constexpr std::array<float, 3> hidden{0.001f, 0.001f, 0.001f};
    (void)first_person_write(scale_address, hidden.data(), sizeof(hidden));
}
struct FirstPersonJoint {
    std::array<float, 4> rotation{0, 0, 0, 1};
    std::array<float, 3> position{};
    float scale = 1;
};
std::array<float, 3> first_person_rotate(const std::array<float, 4>& q, const std::array<float, 3>& v) {
    const float tx = 2 * (q[1] * v[2] - q[2] * v[1]), ty = 2 * (q[2] * v[0] - q[0] * v[2]),
        tz = 2 * (q[0] * v[1] - q[1] * v[0]);
    return {v[0] + q[3] * tx + (q[1] * tz - q[2] * ty), v[1] + q[3] * ty + (q[2] * tx - q[0] * tz),
        v[2] + q[3] * tz + (q[0] * ty - q[1] * tx)};
}
// Appends one parent-local joint to an accumulated world transform.
FirstPersonJoint first_person_child(const FirstPersonJoint& parent, std::uintptr_t buffer, std::uint16_t index) {
    std::array<float, 12> bone{}; // scale.xyzw | rotation.xyzw | position.xyzw; .w of scale/position is metadata
    source_require(first_person_read(buffer + index * 0x30ULL, bone.data(), sizeof(bone)), "Head pose is unreadable.");
    // First person shrinks the head it draws; compose with the real scale.
    if (index == first_person_head_joint && bone[0] < 0.05f && bone[1] < 0.05f && bone[2] < 0.05f) {
        const auto z = first_person_head_scale_z.load(std::memory_order_acquire);
        const auto xy = first_person_head_scale_xy.load(std::memory_order_relaxed);
        if (z) { std::memcpy(bone.data(), &xy, 8); std::memcpy(&bone[2], &z, 4); }
    }
    for (const std::size_t i : {0u, 1u, 2u, 4u, 5u, 6u, 7u, 8u, 9u, 10u})
        source_require(std::isfinite(bone[i]) && std::abs(bone[i]) < 1000000.0f, "Head pose is not ready.");
    const auto& q = parent.rotation;
    const auto offset = first_person_rotate(q, {bone[8] * parent.scale, bone[9] * parent.scale, bone[10] * parent.scale});
    FirstPersonJoint child;
    for (std::size_t i = 0; i < 3; ++i) child.position[i] = parent.position[i] + offset[i];
    child.rotation = {q[3] * bone[4] + q[0] * bone[7] + q[1] * bone[6] - q[2] * bone[5],
        q[3] * bone[5] - q[0] * bone[6] + q[1] * bone[7] + q[2] * bone[4],
        q[3] * bone[6] + q[0] * bone[5] - q[1] * bone[4] + q[2] * bone[7],
        q[3] * bone[7] - q[0] * bone[4] - q[1] * bone[5] - q[2] * bone[6]};
    child.scale = parent.scale * (bone[0] + bone[1] + bone[2]) / 3.0f;
    return child;
}
}
// The local skater's animation component, from the verified player-bound skater.
std::uintptr_t first_person_component(std::uintptr_t base, std::uintptr_t client) {
    overlay::DebugModel skater;
    (void)debug_skater(base, client, skater);
    std::uintptr_t component{}, component_type{};
    source_require(first_person_read(skater.skater_identity + 0x628, &component, 8) && component &&
        first_person_read(component, &component_type, 8) && component_type == base + addr::engine::skater_component_vtable,
        "Skater animation is unavailable.");
    return component;
}
// The physics state selector's current state: skater component +0x70 is the
// physics core, core +0x3c0 its context, context +0x1414 the state (no_bail.cpp).
std::optional<bool> first_person_on_foot(std::uintptr_t base, std::uintptr_t client) noexcept {
    try {
        const auto component = first_person_component(base, client);
        std::uintptr_t core{}, core_type{}, context{};
        std::uint32_t state{};
        if (!first_person_read(component + 0x70, &core, 8) || !first_person_read(core, &core_type, 8) ||
            core_type != base + addr::no_bail::bail_core_vtable ||
            !first_person_read(core + 0x3c0, &context, 8) || !first_person_read(context + 0x1414, &state, 4))
            return std::nullopt;
        return state == addr::no_bail::offboard_physics_state;
    } catch (...) { return std::nullopt; }
}
// Writes the head camera into the rows (right, up, backward, position) of a
// native camera matrix, leaving each row's fourth lane as the camera had it.
first_person::Vec3 first_person_head_matrix(std::uintptr_t base, std::uintptr_t component, std::array<float, 16>& matrix) {
    std::uintptr_t holder{};
    source_require(first_person_read(component + 0xa0, &holder, 8) && holder, "Skater animation is unavailable.");
    const auto pose = multiplayer::read_native_pose_layout(first_person_read, base, holder, 512);
    source_require(pose.buffer && pose.count == first_person_skeleton_joints,
        "First person needs the standard skater skeleton.");
    FirstPersonJoint head;
    first_person::Vec3 origin{};
    for (const auto joint : first_person_head_chain) {
        head = first_person_child(head, pose.buffer, joint);
        if (joint == 1) origin = head.position;
    }
    const auto face = first_person_child(head, pose.buffer, first_person_face);
    const auto right_eye = first_person_child(face, pose.buffer, first_person_right_eye).position;
    const auto left_eye = first_person_child(face, pose.buffer, first_person_left_eye).position;
    float separation = 0;
    for (std::size_t i = 0; i < 3; ++i) separation += (right_eye[i] - left_eye[i]) * (right_eye[i] - left_eye[i]);
    separation = std::sqrt(separation) / std::max(head.scale, 0.01f);
    source_require(separation > 0.03f && separation < 0.15f, "Head pose failed its shape check.");
    const auto right = first_person_rotate(head.rotation, {0, 0, -1});
    const auto up = first_person_rotate(head.rotation, {1, 0, 0});
    const auto forward = first_person_rotate(head.rotation, {0, 1, 0});
    for (std::size_t i = 0; i < 3; ++i) {
        matrix[i] = right[i];
        matrix[4 + i] = up[i];
        matrix[8 + i] = -forward[i];
        matrix[12 + i] = (right_eye[i] + left_eye[i]) * 0.5f + forward[i] * first_person_forward_offset * head.scale;
    }
    source_require(valid_flight_transform(matrix), "Head camera pose is invalid.");
    return origin;
}
// Every animation update of the local skater, on whichever thread runs it
// (usually a worker), captures its head pose; reads only, never the camera.
// Each frame runs: client tick (camera) -> the local skater's animation, often
// on a worker thread -> that pose handed to the renderer, on the engine thread
// (measured 2026-09-22). The handoff is the one point that is after this
// frame's pose and still on the camera's thread, so the head is published
// there, from the capture its animation update just took: the camera then
// matches the body being drawn on every frame, with no lag.
void first_person_on_render(std::uintptr_t animation_interface) noexcept {
    auto& arm = first_person_arm();
    const auto component = arm.watched.load(std::memory_order_acquire);
    if (!component) return;
    std::uintptr_t holder{};
    if (!first_person_read(component + 0xa0, &holder, 8) || !holder || animation_interface != holder + 0xc0) return;
    const auto thread = arm.thread.load(std::memory_order_acquire);
    if (!thread || GetCurrentThreadId() != thread) return; // The camera is only written on the engine thread.
    const auto error = GetLastError();
    try {
        FirstPersonArm::Snapshot latest;
        bool have{};
        {
            std::lock_guard lock(arm.snapshot_mutex);
            if (arm.captures) { latest = arm.latest; have = true; }
        }
        // Only this frame's capture (taken moments ago); otherwise keep the tick's.
        if (have && source_flight_now() - latest.time < 0.02) {
            auto matrix = arm.matrix;
            for (const std::size_t i : {0u, 1u, 2u, 4u, 5u, 6u, 8u, 9u, 10u, 12u, 13u, 14u}) matrix[i] = latest.head[i];
            matrix = arm.spring.sample(matrix, arm.settings, latest.origin);
            source_require(valid_flight_transform(matrix), "First-person spring pose is invalid.");
            arm.transform(arm.camera, &matrix);
            arm.matrix = matrix;
            if (arm.fov > 0) (void)first_person_write_fov(arm.camera, arm.fov);
        }
    } catch (...) {}
    SetLastError(error);
}
void first_person_on_animation(std::uintptr_t component) noexcept {
    auto& arm = first_person_arm();
    if (!component || component != arm.watched.load(std::memory_order_acquire)) return;
    const auto error = GetLastError();
    const auto now = source_flight_now();
    try {
        std::array<float, 16> head{};
        head[15] = 1;
        const auto origin = first_person_head_matrix(arm.watched_base.load(std::memory_order_acquire), component, head);
        {
            std::lock_guard lock(arm.snapshot_mutex);
            arm.latest = {head, origin, now};
            ++arm.captures;
        }
        first_person_hide_head(arm.watched_base.load(std::memory_order_acquire), component);
    } catch (...) {} // The tick keeps publishing and reports why.
    SetLastError(error);
}
}
