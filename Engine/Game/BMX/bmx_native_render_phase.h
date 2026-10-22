#pragma once
#include "Engine/Game/BMX/bmx_spring_avatar_binding.h"

namespace dingosdk::bmx::native_render_phase {
namespace ab=avatar_binding;namespace np=native_coupled::policy;
struct Clock {bool available{};std::uint64_t serial{};double time{};};
struct Receipt {
    bool accepted{},one_step_latency{};ab::Epoch sampled_epoch;std::uint64_t source_lease_revision{};Clock render_clock;
};
// Presentation association only. The observed pair/frame and SDK source lease
// KEEP their sampled epoch. Native275 proves the render handoff at sample+1;
// no intermediate pair, body state or physics epoch is synthesized here.
inline bool clock_matches(const ab::Epoch& sampled,const Clock& actual,bool rendering) noexcept {
    if(!np::valid_epoch(sampled)||!actual.available||!std::isfinite(actual.time)||actual.time<0)return false;
    if(actual.serial==sampled.serial)return actual.time==sampled.time;
    if(!rendering||sampled.seconds!=static_cast<float>(np::native_seconds)||sampled.serial==std::numeric_limits<std::uint64_t>::max()||
       actual.serial!=sampled.serial+1)return false;
    const double expected=sampled.time+double(sampled.seconds);
    return std::isfinite(expected)&&expected>sampled.time&&actual.time==expected;
}
template<class Lease> inline Receipt qualify(const ab::Frame& frame,const Lease& source,const Clock& actual,bool rendering) noexcept {
    Receipt out;const auto& e=frame.actual.epoch;
    if(!frame.requested||!ab::valid(frame.actual)||!frame.native_motion_lease_revision||!clock_matches(e,actual,rendering)||
       !source.available||!source.active||!source.details_available||!source.epoch_valid||!source.timing_available||!source.owner_available||
       !source.offboard||!source.motion_supported||!source.full_rotation||source.lease_revision!=frame.native_motion_lease_revision||
       source.client!=e.owner_world[0]||source.entity!=e.owner_world[1]||source.core!=e.owner_world[2]||source.context!=e.owner_world[3]||source.rig!=e.owner_world[4]||
       source.serial!=e.serial||source.simulation_time!=e.time||source.seconds!=e.seconds)return out;
    out.accepted=true;out.one_step_latency=actual.serial!=e.serial;out.sampled_epoch=e;
    out.source_lease_revision=source.lease_revision;out.render_clock=actual;return out;
}
}
