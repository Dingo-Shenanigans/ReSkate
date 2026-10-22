#pragma once
#include "bmx_chassis_ride.h"
#include "bmx_native_chassis.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace dingosdk::bmx::ride_trace {
enum class InputSource : unsigned { adapter_sample, flat_rehearsal, ramp_rehearsal, catalog_rehearsal, trick_rehearsal, private_switch_neutral };
struct Sample {
    chassis_ride::Step step;
    RawInput input;
    native_chassis::Status producer;
    std::array<float,3> rider_position{};
    bool offboard{},standable{},motion_supported{};
    RawInput sampled_input; // before any private rehearsal or mount/impact gate
    InputSource input_source{};
    bool input_neutralized{};
    bool private_switch_gate{};
};
// Fixed storage: no allocation, serialization or file IO on accepted ride steps.
// A mount is a separate history; duplicate/stale steps cannot rewrite evidence.
template<std::size_t Capacity=240> class History {
    static_assert(Capacity>0);
    std::array<Sample,Capacity> samples_{};
    std::size_t next_{},size_{};
    std::uint64_t token_{},serial_{};
    bool sealed_{};
public:
    bool record(const Sample& sample) noexcept {
        const auto& step=sample.step;
        if(sealed_||!step.ready||!step.body_token||!step.serial||step.serial<=serial_||
           (token_&&step.body_token!=token_))return false;
        token_=step.body_token;serial_=step.serial;
        samples_[next_]=sample;next_=(next_+1)%Capacity;
        if(size_<Capacity)++size_;
        return true;
    }
    void reset() noexcept {next_=size_=0;token_=serial_=0;sealed_=false;}
    bool seal() noexcept {if(sealed_||!size_)return false;sealed_=true;return true;}
    std::size_t size() const noexcept{return size_;}
    std::uint64_t token() const noexcept{return token_;}
    template<class Visit> void each(Visit&& visit) const {
        const auto first=(next_+Capacity-size_)%Capacity;
        for(std::size_t i=0;i<size_;++i)visit(samples_[(first+i)%Capacity]);
    }
};
}
