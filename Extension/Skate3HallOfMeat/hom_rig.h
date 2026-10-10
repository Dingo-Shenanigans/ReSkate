#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

// Hall Of Meat's view of the local skater's skeleton, for the X-ray. The game thread reads the
// animation pose every client tick (hom_rig.cpp) and publishes one world-space segment per bone of
// the original's X-ray skeleton (the 19 `dem_bones_hom` meshes); the HUD projects them.
namespace dingosdk::skate3_hom {
// Same order as bones() in hom_core.cpp.
inline constexpr std::size_t rig_bones = 19;
struct RigSegment {
    std::array<float, 3> a{}, b{};
    std::array<float, 3> va{}, vb{}; // m/s, from the last two reads
    float radius{}; // metres
    // Which way the bone's front faces, from the joint's own rotation (so hands, feet and ribs keep
    // their roll while the body tumbles). Only when has_front; otherwise the body forward is used.
    std::array<float, 3> front{};
    bool has_front{};
};
struct RigView {
    bool valid{};
    std::array<RigSegment, rig_bones> bones{};
    std::array<float, 3> forward{0, 0, 1}; // the skater's body forward (pelvis local +Y)
    std::uintptr_t base{};
    std::chrono::steady_clock::time_point at{};
    float period{0.016f}; // seconds between the last two reads
};
namespace rig_detail {
inline std::mutex mutex;
inline RigView latest;
} // namespace rig_detail
inline void publish_rig(const RigView &view) noexcept {
    std::lock_guard lock(rig_detail::mutex);
    RigView next = view;
    const auto &previous = rig_detail::latest;
    const double dt = std::chrono::duration<double>(view.at - previous.at).count();
    if (previous.valid && dt > 0.003 && dt < 0.12) next.period = static_cast<float>(dt);
    if (previous.valid && dt > 0.003 && dt < 0.12)
        for (std::size_t i = 0; i < rig_bones; ++i)
            for (std::size_t k = 0; k < 3; ++k) {
                next.bones[i].va[k] = static_cast<float>((view.bones[i].a[k] - previous.bones[i].a[k]) / dt);
                next.bones[i].vb[k] = static_cast<float>((view.bones[i].b[k] - previous.bones[i].b[k]) / dt);
            }
    rig_detail::latest = next;
}
// Nothing when the skeleton was not read recently.
inline RigView latest_rig(std::chrono::milliseconds max_age = std::chrono::milliseconds(250)) noexcept {
    std::lock_guard lock(rig_detail::mutex);
    if (!rig_detail::latest.valid || std::chrono::steady_clock::now() - rig_detail::latest.at > max_age) return {};
    return rig_detail::latest;
}
// Set by the HUD while a bail is on screen: the game's own interface is hidden meanwhile.
inline std::atomic<bool> &hide_game_ui() {
    static std::atomic<bool> value{};
    return value;
}
// Whether the skater stands upright (head well above the hips), from the last skeleton read.
// A body of the local skater touched a vehicle in the last physics step (skate.'s contact records).
// skate.'s own word on the body off the board: 1 a ragdoll (a crash), 0 under control (on foot, a
// glide, a dive), -1 not readable.
inline std::atomic<int> &rig_ragdoll() {
    static std::atomic<int> value{-1};
    return value;
}
inline std::atomic<bool> &vehicle_contact() {
    static std::atomic<bool> value{};
    return value;
}
inline std::atomic<bool> &rig_upright() {
    static std::atomic<bool> value{};
    return value;
}
// `hom roll 0|1`: take each bone's roll from its joint (on) or from the body forward (off).
// Whether the body is lying down (head no more than ~0.3 m above the hips).
inline std::atomic<bool> &rig_lying() {
    static std::atomic<bool> value{};
    return value;
}
// Whether the last skeleton came from the ragdoll's physics bodies (else the animation pose).
inline std::atomic<bool> &rig_from_bodies() {
    static std::atomic<bool> value{};
    return value;
}
// Extra milliseconds the X-ray is carried ahead of the last skeleton read (`hom lead`), saved with
// the profile: a machine whose picture shows the body later than the read can catch the bones up.
inline std::atomic<float> &bone_lead_ms() {
    static std::atomic<float> value{0.0f};
    return value;
}
inline std::atomic<bool> &roll_from_joints() {
    static std::atomic<bool> value{true};
    return value;
}
inline std::atomic<bool> &phys_probe_requested() {
    static std::atomic<bool> value{};
    return value;
}
inline std::atomic<bool> &rig_probe_requested() {
    static std::atomic<bool> value{};
    return value;
}
// Game thread, once per client tick, before the bail tracker.
void rig_tick(std::uintptr_t base, std::uintptr_t client) noexcept;
} // namespace dingosdk::skate3_hom
