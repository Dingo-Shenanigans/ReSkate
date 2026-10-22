#pragma once
#include "Engine/Game/BMX/bmx_native_coupled_policy.h"
#include "Engine/Game/BMX/bmx_mount_pose.h"
#include "Engine/Game/BMX/bmx_mount_transition.h"
#include <array>
#include <cstdint>

// Pure calibrated presentation mapping. It never changes a physical basis,
// integrates a second body, authorizes a native write, or guesses a stand offset.
namespace dingosdk::bmx::avatar_binding {
namespace np=native_coupled::policy;
using Vec=np::Vec;using Quat=np::Quat;using Epoch=np::Epoch;
enum class Phase { inactive,entry,awaiting_neutral,calibrated,riding,refused };
struct Observation {
    bool available{};Epoch epoch;
    Vec spring_com{},spring_velocity{},spring_angular_velocity{},bike_root{};
    Quat spring_orientation{0,0,0,1},bike_orientation{0,0,0,1};
};
struct NativeRoot {
    bool available{},coherent_epoch{};Epoch epoch;
    Vec position{};std::array<float,16> transform{};
};
struct Capture {
    bool requested{},root_height_verified{},mounted_geometry_verified{},neutral{};
    float root_height{},mount_progress{},rider_blend{};
    NativeRoot native;mount_pose::Seed seed;
};
struct Calibration {
    bool ready{};Phase phase{Phase::inactive};Epoch bind_epoch;
    Vec local_root_offset{};Quat local_root_basis{0,0,0,1};
};
struct Target {
    bool ready{};Phase phase{Phase::inactive};Epoch epoch;
    Vec root{},point_velocity{};Quat rotation{0,0,0,1};
};
struct Frame {
    // requested distinguishes a missing coupled witness from the unchanged
    // legacy path. A requested but invalid witness refuses the bound pose.
    bool requested{};Observation actual;Capture capture;
    std::uint64_t native_motion_lease_revision{};
};
inline Vec add(Vec a,Vec b) noexcept{for(unsigned i=0;i<3;++i)a[i]+=b[i];return a;}
inline Vec sub(Vec a,Vec b) noexcept{for(unsigned i=0;i<3;++i)a[i]-=b[i];return a;}
inline Vec cross(Vec a,Vec b) noexcept{return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
inline bool same_identity(const Epoch& a,const Epoch& b) noexcept {
    return np::valid_epoch(a)&&np::valid_epoch(b)&&a.owner_world==b.owner_world&&a.bodies==b.bodies&&a.token==b.token;
}
inline bool valid(const Observation& value) noexcept {
    Quat spring,bike;
    return value.available&&np::valid_epoch(value.epoch)&&np::finite(value.spring_com)&&np::finite(value.bike_root)&&
        np::finite(value.spring_velocity,50)&&np::finite(value.spring_angular_velocity,40)&&
        np::unit(value.spring_orientation,spring)&&np::unit(value.bike_orientation,bike);
}
inline bool close(Vec a,Vec b,float tolerance=.00025f) noexcept{return np::length(sub(a,b))<=tolerance;}
inline bool close_rotation(Quat a,Quat b) noexcept{return np::pose_matches({},a,{},b);}
inline Calibration capture(const Observation& actual,const Capture& witness) noexcept {
    Calibration out;out.phase=Phase::awaiting_neutral;
    if(!witness.requested)return out;
    if(witness.mount_progress!=1||witness.rider_blend!=1){out.phase=Phase::entry;return out;}
    if(!valid(actual)||!witness.native.available||!witness.native.coherent_epoch||
       witness.native.epoch!=actual.epoch||!witness.root_height_verified||!witness.mounted_geometry_verified||!witness.neutral||
       !std::isfinite(witness.root_height)||witness.root_height<0||witness.root_height>2||
       !np::finite(witness.native.position))return out;
    const auto& epoch=actual.epoch;
    if(!mount_pose::matches(witness.seed,epoch.owner_world[5],epoch.owner_world[7],epoch.serial,epoch.time)||
       !close(actual.bike_root,witness.seed.root,.003f)||!close_rotation(actual.bike_orientation,witness.seed.orientation))return out;
    // This is a completed-mount witness, not the first standing sample beside
    // the bike. root_height must itself come from the caller's verified mount.
    const auto mounted_root=add(witness.seed.root,np::rotate(witness.seed.orientation,{0,witness.root_height,0}));
    if(!close(witness.native.position,mounted_root,.003f))return out;
    Quat native_basis,spring;
    if(!mount_transition::native_rotation(witness.native.transform,native_basis)||!np::unit(actual.spring_orientation,spring))return out;
    out.local_root_offset=np::rotate(np::conjugate(spring),sub(witness.native.position,actual.spring_com));
    out.local_root_basis=np::multiply(np::conjugate(spring),native_basis);
    if(!np::finite(out.local_root_offset,2)||!np::unit(out.local_root_basis,out.local_root_basis))return Calibration{};
    out.bind_epoch=epoch;out.ready=true;out.phase=Phase::calibrated;return out;
}
inline Target sample(const Calibration& calibration,const Observation& actual) noexcept {
    Target out;out.phase=Phase::refused;
    if(!calibration.ready||!valid(actual)||!same_identity(calibration.bind_epoch,actual.epoch)||
       actual.epoch.serial<calibration.bind_epoch.serial||actual.epoch.time<calibration.bind_epoch.time||
       !np::finite(calibration.local_root_offset,2))return out;
    // Duplicate callbacks can evaluate the same accepted epoch, but a later
    // serial must have later native time; this function has no render clock.
    if((actual.epoch.serial==calibration.bind_epoch.serial)!=(actual.epoch.time==calibration.bind_epoch.time))return out;
    Quat spring,basis;
    if(!np::unit(actual.spring_orientation,spring)||!np::unit(calibration.local_root_basis,basis))return out;
    const auto offset=np::rotate(spring,calibration.local_root_offset);
    out.root=add(actual.spring_com,offset);
    out.rotation=np::multiply(spring,basis);
    out.point_velocity=add(actual.spring_velocity,cross(actual.spring_angular_velocity,offset));
    if(!np::finite(out.root)||!np::finite(out.point_velocity,28)||!np::unit(out.rotation,out.rotation))return Target{};
    out.epoch=actual.epoch;out.ready=true;out.phase=Phase::riding;return out;
}
// SDK lease association only; current native clock, owner/layout, placement,
// unchanged ancestors and completed pose-write checks remain separate.
template<class Lease> inline bool motion_lease_matches(const Frame& frame,const Target& target,const Lease& lease) noexcept {
    if(!frame.requested||!target.ready||frame.actual.epoch!=target.epoch||!frame.native_motion_lease_revision||
       !lease.available||!lease.active||!lease.details_available||!lease.epoch_valid||!lease.timing_available||!lease.owner_available||
       !lease.offboard||!lease.motion_supported||!lease.full_rotation||lease.lease_revision!=frame.native_motion_lease_revision)return false;
    const auto& e=target.epoch;
    return lease.client==e.owner_world[0]&&lease.entity==e.owner_world[1]&&lease.core==e.owner_world[2]&&
        lease.context==e.owner_world[3]&&lease.rig==e.owner_world[4]&&lease.serial==e.serial&&lease.simulation_time==e.time&&
        lease.seconds==e.seconds&&close(lease.target,target.root)&&close(lease.velocity,target.point_velocity)&&close_rotation(lease.rotation,target.rotation);
}
}
