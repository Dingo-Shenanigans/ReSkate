#pragma once
#include "bmx_geometry.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>

// Authored presentation only. Raw cadence, wheel rotors, forces, contact,
// controls and authored trick angles are not modified by this policy.
namespace dingosdk::bmx::crank_presentation {
inline constexpr double pi=std::numbers::pi,tau=2*pi;
inline constexpr double leveling_rate=4.5; // rad/s, an adaptation, not a recovered Streets value
struct Identity {
    std::array<std::uintptr_t,6> owner_world{}; // client, entity, core, context, rig, physics world
    std::uint64_t body_token{};
    bool operator==(const Identity&) const=default;
};
inline bool complete(const Identity& key) noexcept {
    if(!key.body_token)return false;
    for(auto p:key.owner_world)if(!p)return false;
    return true;
}
struct State {
    Identity identity{};
    double phase{},raw_phase{},target{},time{};
    std::uint64_t serial{};
    bool initialized{},observed{},coasting{};
};
struct Observation {
    Identity identity{};
    double raw_phase{},time{},seconds{};
    std::uint64_t serial{};
    bool enabled{},pedaling{},reserved{};
};
struct Result {State state;bool valid{},advanced{},rebased{},leveling{};};
inline double wrap(double a) noexcept {return std::remainder(a,tau);}
inline double horizontal_angle(std::array<double,3> arm) noexcept {
    // X-axis rotation gives y'=y*cos(a)-z*sin(a). Thus atan2(y,z)
    // puts this measured crank arm horizontally, preserving the source axis.
    return std::atan2(arm[1],arm[2]);
}
inline double source_horizontal_angle() noexcept {
    std::array<double,3> arm{};
    for(unsigned i=0;i<3;++i)arm[i]=double(geometry::left_pedal.spindle[i])-geometry::bottom_bracket[i];
    return horizontal_angle(arm);
}
inline State suspend(State previous) noexcept {
    // Same owned bike can resume at its displayed phase. A new identity or
    // fresh start is independently seeded from its own source cadence.
    previous.observed=false;previous.coasting=false;return previous;
}
inline Result step(State previous,const Observation& sample) noexcept {
    Result out{previous};
    if(!complete(sample.identity)||!sample.serial||!std::isfinite(sample.raw_phase)||
       std::abs(sample.raw_phase)>1e6||!std::isfinite(sample.time)||sample.time<0||
       !std::isfinite(sample.seconds)||sample.seconds<0||sample.seconds>.1||
       (previous.initialized&&(!std::isfinite(previous.phase)||!std::isfinite(previous.raw_phase)||
         !std::isfinite(previous.target)||!std::isfinite(previous.time))))return out;
    out.valid=true;
    if(sample.seconds==0)return out; // no paused integration or clock consumption
    if(!previous.initialized||previous.identity!=sample.identity) {
        out.state={sample.identity,wrap(sample.raw_phase),wrap(sample.raw_phase),0,sample.time,sample.serial,true,true,false};
        out.rebased=true;return out;
    }
    if(previous.observed&&sample.serial==previous.serial) {
        if(sample.time!=previous.time){out.valid=false;out.state=suspend(previous);}
        return out;
    }
    const bool successor=previous.observed&&previous.serial!=UINT64_MAX&&sample.serial==previous.serial+1&&
        std::abs(sample.time-previous.time-sample.seconds)<=1e-7;
    if(!successor) {
        // Do not catch up through an unobserved interval or replay raw jumps.
        out.state.raw_phase=wrap(sample.raw_phase);out.state.time=sample.time;out.state.serial=sample.serial;
        out.state.observed=true;out.state.coasting=false;out.rebased=true;return out;
    }
    auto& next=out.state;
    next.raw_phase=wrap(sample.raw_phase);next.time=sample.time;next.serial=sample.serial;
    out.advanced=true;
    if(!sample.enabled){next.coasting=false;return out;}
    // Pedaling resumes from the displayed phase by applying only the new raw
    // cadence increment, rather than snapping back to its frozen source angle.
    next.phase=wrap(previous.phase+wrap(next.raw_phase-previous.raw_phase));
    if(sample.pedaling||sample.reserved){next.coasting=false;return out;}
    if(!previous.coasting)next.target=wrap(next.phase+std::remainder(source_horizontal_angle()-next.phase,pi));
    next.coasting=true;
    const double remaining=wrap(next.target-next.phase);
    const double movement=std::clamp(remaining,-leveling_rate*sample.seconds,leveling_rate*sample.seconds);
    next.phase=wrap(next.phase+movement);out.leveling=movement!=0;
    return out;
}
}
