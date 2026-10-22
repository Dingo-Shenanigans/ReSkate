#pragma once
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <string>

namespace dingosdk::bmx {
namespace motion_target {
// This policy selects the physical on-foot rig basis only. Translation,
// velocity, ownership, clock and the short movement lease remain separate.
enum class Basis { legacy_yaw, native_step_off };
inline std::array<float,16> yaw_basis(const std::array<float,16>& supplied,float yaw,Basis policy) noexcept {
    std::array<float,16> result{};
    if(policy==Basis::native_step_off) {
        result[15]=1;
        for(unsigned column=0;column<3;++column)for(unsigned row=0;row<3;++row)
            result[column*4+row]=supplied[column*4+row];
    } else {
        const float c=std::cos(yaw),s=std::sin(yaw);
        result={c,0,-s,0,0,1,0,0,s,0,c,0,0,0,0,1};
    }
    return result;
}
inline bool same_basis_words(const std::array<float,16>& supplied,const std::array<float,16>& target) noexcept {
    for(unsigned column=0;column<3;++column)for(unsigned row=0;row<3;++row) {
        const auto index=column*4+row;
        if(std::bit_cast<std::uint32_t>(supplied[index])!=std::bit_cast<std::uint32_t>(target[index]))return false;
    }
    return true;
}
inline constexpr std::uint64_t counter_limit=65535;
inline void count_bounded(std::atomic<std::uint64_t>& value) noexcept {
    auto old=value.load(std::memory_order_relaxed);
    while(old<counter_limit&&!value.compare_exchange_weak(old,old+1,std::memory_order_relaxed)){}
}
} // namespace motion_target
struct NativeFrame {
    std::uintptr_t client{}, entity{}, core{}, context{}, rig{};
    std::array<float, 16> transform{};
    std::array<float, 3> position{};
    float seconds{};
    std::uint64_t serial{};
    double simulation_time{};
    bool available{}, offboard{}, standable{}, motion_supported{}, timing_available{};
    std::string issue;
};
// All acquisition/submission happens on the verified client update thread. The existing
// guarded motion/physics hooks consume a short lease; no per-frame teleports are used.
bool enable_native_motion(std::uintptr_t base) noexcept;
NativeFrame read_native_frame(std::uintptr_t base, std::uintptr_t client) noexcept;
// Passive diagnostics only. Optional fields never change NativeFrame readiness
// or authorize a motion lease, animation request or native write.
struct NativeRiderBodyObservation {
    std::uint32_t index{};
    bool available{};
    bool position_raw_available{}, position_xyz_available{}, flags_available{};
    std::array<float,4> position{}; // raw PhysicalRig record +0x50 float4
    std::array<std::uint32_t,4> position_words{}; // exact bits; lane4 meaning unknown
    std::uint32_t flags{}; // raw record +0x60 pending flags; not contact classification
};
struct NativeRiderObservation {
    bool available{}, state_available{}, transition_lock_available{}, ragdoll_available{}, velocity_available{};
    bool protected_owner{}, pending_offset_available{};
    std::uint32_t physics_state{}, offboard_flags{}, impact_flags{}, animation_request_mask{}, state_kind{};
    std::uintptr_t active_offset{}, pending_offset{}, active_vtable_rva{};
    std::uint8_t transition_lock{}, ragdoll_byte{};
    std::array<float,3> motion_velocity{};
    // Explicitly requested handoff diagnostics only. context+0x830 is the raw
    // candidate target consumed by FollowRagdoll; its upstream source is unknown.
    // context+0x7c0 is its alternative under raw offboard_flags mask0x4.
    // active+0x160 is read only for an exact recognized ragdoll substate.
    bool context_830_available{}, context_7c0_available{}, active_160_available{}, rig_4720_available{}, rig_4760_available{};
    // Read success and finite XYZ/translation are separate from legacy whole
    // float4/matrix validity. Raw words retain NaN/padding bits without assigning
    // fourth-lane semantics. XYZ uses lanes0..2; matrix translation lanes12..14.
    bool context_830_raw_available{}, context_830_xyz_available{};
    bool context_7c0_raw_available{}, context_7c0_xyz_available{};
    bool active_160_raw_available{}, active_160_xyz_available{};
    bool rig_4720_raw_available{}, rig_4720_translation_available{};
    bool rig_4760_raw_available{}, rig_4760_translation_available{};
    bool physical_rig_available{};
    std::array<float,4> context_830{}, context_7c0{}, active_160{};
    std::array<float,16> rig_4720{}, rig_4760{};
    std::array<std::uint32_t,4> context_830_words{}, context_7c0_words{}, active_160_words{};
    std::array<std::uint32_t,16> rig_4720_words{}, rig_4760_words{};
    std::uintptr_t physical_rig{}, physical_rig_parts{};
    std::array<NativeRiderBodyObservation,4> physical_rig_bodies{}; // indices 0,1,12,23
};
NativeRiderObservation observe_native_rider(std::uintptr_t base,const NativeFrame&,
    bool nonblocking_clock=false,bool handoff_details=false) noexcept;
// Optional copied SDK lease association only. Never resolves native ownership or
// proves that a native callback consumed this submission. Existing status users
// retain the same try-lock and active semantics; details are opt-in.
struct NativeMotionLeaseStatus {
    bool available{},active{},details_available{},epoch_valid{};
    std::uint64_t lease_revision{},epoch_revision{},serial{};double simulation_time{};
    std::uintptr_t client{},entity{},core{},context{},rig{};
    std::array<float,16> transform{};std::array<float,3> position{},velocity{},target{};
    std::array<float,4> rotation{};float seconds{},yaw{};
    bool timing_available{},owner_available{},offboard{},standable{},motion_supported{},full_rotation{};
};
static_assert(sizeof(NativeMotionLeaseStatus)<=256);
NativeMotionLeaseStatus native_motion_lease_status(bool details=false) noexcept;
// Passive matching-skater clock only. Does not resolve ownership, acquire a
// movement lease, or establish a physics-world synchronization boundary.
bool read_native_clock(std::uintptr_t entity, std::uintptr_t core,
                       std::uint64_t& serial, double& simulation_time) noexcept;
// Native callbacks must not wait for the time-feed writer.
bool read_native_clock_try(std::uintptr_t entity, std::uintptr_t core,
                           std::uint64_t& serial,double& simulation_time) noexcept;
bool submit_native_motion(const NativeFrame&, const std::array<float, 3>& velocity,
                          const std::array<float, 3>& target, float yaw) noexcept;
// Only the physical step-off uses the native supplied on-foot basis. The
// ordinary yaw motion API above retains its existing legacy basis contract.
bool submit_native_step_off_motion(const NativeFrame&, const std::array<float,3>& velocity,
                                  const std::array<float,3>& target,float yaw) noexcept;
// Rider follows a measured physical chassis. Rotation is Hamilton XYZW;
// target remains the native rider/bike-root position, not chassis COM.
bool submit_native_chassis_motion(const NativeFrame&, const std::array<float, 3>& velocity,
                                 const std::array<float, 3>& target,
                                 const std::array<float, 4>& rotation) noexcept;
void release_native_motion() noexcept;
bool native_motion_healthy() noexcept;
std::string native_motion_issue();
std::uint64_t native_motion_updates() noexcept;
struct NativeMotionBasisStatus {
    const char* mode="Default";bool qualified_scope{},refused{};
    std::uint64_t selected_full_rotation_targets{},preserved_supplied_bases{};
    // Independently sampled, saturating SDK callback counters. Not native
    // completion or an atomic multi-counter receipt; no additional native read.
    std::uint64_t step_off_target_calls{},step_off_basis_word_matches{},step_off_basis_word_mismatches{};
};
// SDK selection counters sampled independently, not native completion proof.
NativeMotionBasisStatus native_motion_basis_status() noexcept;
void apply_native_velocity(std::uintptr_t core) noexcept;
bool native_motion_target(std::uintptr_t rig, std::uintptr_t context,
    const std::array<float, 16>* supplied, std::array<float, 16>& target) noexcept;
}
