#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace dingosdk::bmx::grip_probe_policy {
inline constexpr std::size_t capacity=128;
// This window follows SDK Frame.airborne, not a native contact or GPU fence.
// Four consecutive observed native serials avoid the ramp's two-step lip gap.
struct Window {
    std::uintptr_t owner{};std::uint64_t token{},last_serial{};double last_time{};
    unsigned air_steps{},count{},duplicates{},missing_clock{},serial_gaps{},busy{};
    bool armed{},sealed{},owner_changed{},clock_rewound{};
    bool observe(std::uintptr_t entity,std::uint64_t mount_token,bool clock_available,
                 std::uint64_t serial,double time,bool airborne) noexcept {
        if(sealed)return false;
        if(!clock_available||!serial||!std::isfinite(time)){++missing_clock;return false;}
        if(owner&&(owner!=entity||token!=mount_token)){owner_changed=sealed=true;return false;}
        if(!owner){owner=entity;token=mount_token;}
        if(last_serial&&serial==last_serial){++duplicates;return false;}
        if(last_serial&&(serial<last_serial||time<=last_time)){clock_rewound=sealed=true;return false;}
        if(last_serial&&serial!=last_serial+1){++serial_gaps;air_steps=0;}
        last_serial=serial;last_time=time;
        air_steps=airborne?air_steps+1:0;
        if(!armed&&air_steps>=4)armed=true;
        if(!armed)return false;
        if(++count==capacity)sealed=true;
        return true;
    }
};
struct Owner {
    std::uintptr_t client{},entity{},core{},context{},rig{};
    bool operator==(const Owner&) const=default;
};
inline bool complete(const Owner& x) noexcept {return x.client&&x.entity&&x.core&&x.context&&x.rig;}
// Readback is an observation only after fresh existing ownership/layout guards.
// A lease epoch is SDK historical association, never native write completion.
struct Edge {
    std::uintptr_t entity{},core{},context{},rig{},client{};
    std::uint64_t epoch{},lease_revision{},serial{};double time{};
    bool clock{},lease{},active{},epoch_valid{};
    Owner actual_owner{};bool actual_available{};
};
inline Owner lease_owner(const Edge& x) noexcept {return {x.client,x.entity,x.core,x.context,x.rig};}
inline bool owner_joined(const Edge& x) noexcept {
    return x.actual_available&&complete(x.actual_owner)&&x.actual_owner==lease_owner(x);
}
inline bool same_admission(const Edge& source,const Edge& before,const Edge& after) noexcept {
    const auto valid=[](const Edge& x){return x.clock&&x.lease&&x.active&&x.epoch_valid&&x.entity&&x.core&&x.context&&x.rig&&x.client&&
        x.serial&&std::isfinite(x.time)&&x.epoch&&x.epoch==x.lease_revision&&owner_joined(x);};
    const auto same=[&](const Edge& x){return valid(x)&&x.entity==source.entity&&x.core==source.core&&
        x.context==source.context&&x.rig==source.rig&&x.client==source.client&&x.epoch==source.epoch&&
        x.lease_revision==source.lease_revision&&x.serial==source.serial&&x.time==source.time&&x.actual_owner==source.actual_owner;};
    return valid(source)&&same(before)&&same(after);
}
}
