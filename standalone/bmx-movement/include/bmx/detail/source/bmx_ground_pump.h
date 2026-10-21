#pragma once
#include <algorithm>
#include <cmath>

// Independent single-chassis adaptation of the matched Streets down-pump
// window, not a native Unity implementation or its complex PumpSystem.
// Authored .11 VelocityChange / .01 seconds -> nominal11m/s^2. Apply at COM
// in WORLD down and solve current tire contacts against it in the same step.
namespace bmx::detail::source::ground_pump {
inline constexpr double nominal_down_acceleration=11.0;
inline constexpr double down_threshold=.3,maximum_direct_seconds=.45;
inline constexpr double minimum_air_seconds=.15,automatic_seconds=.3,automatic_input=.4;
enum class Status {disabled,invalid_sample,waiting_release,neutral,unsupported,inhibited,manual,
    upward,exhausted,direct,automatic,combined};
inline const char* name(Status value) noexcept {
    switch(value) {
    case Status::disabled:return "disabled";case Status::invalid_sample:return "invalid_sample";
    case Status::waiting_release:return "waiting_release";case Status::neutral:return "neutral";
    case Status::unsupported:return "unsupported";case Status::inhibited:return "inhibited";
    case Status::manual:return "manual";case Status::upward:return "upward";
    case Status::exhausted:return "exhausted";case Status::direct:return "direct";
    case Status::automatic:return "automatic";case Status::combined:return "combined";
    }
    return "invalid_sample";
}
struct Sample {
    bool enabled{},operator_available{},supported{},inhibited{};
    double input{},manual{},nose_manual{},world_upward_speed{},pedal{};
};
struct Result {
    bool valid{},applied{},automatic_armed{};
    Status status{Status::disabled};
    double down_acceleration{},direct_input{},automatic_input{},direct_seconds{},automatic_remaining{},air_seconds{};
};
class Controller {
    bool direct_armed_{},supported_seen_{},previous_supported_{},flight_{};
    double direct_elapsed_{},automatic_remaining_{},air_seconds_{};
public:
    void reset() noexcept {*this={};}
    Result step(double h,Sample s) noexcept {
        Result out;
        if(!std::isfinite(h)||h<=0||h>.1||
           !std::isfinite(s.input)||!std::isfinite(s.manual)||!std::isfinite(s.nose_manual)||
           !std::isfinite(s.world_upward_speed)||!std::isfinite(s.pedal)) {
            reset();out.status=Status::invalid_sample;return out;
        }
        out.valid=true;
        if(!s.enabled||!s.operator_available) {reset();return out;}
        s.input=std::clamp(s.input,-1.0,1.0);
        const bool released=s.input>=-down_threshold;
        if(released) {direct_armed_=true;direct_elapsed_=0;}
        if(!s.supported) {
            // No held request/window survives takeoff. Air duration begins at
            // zero on first unsupported observation: that interval may contain
            // takeoff. Only subsequent observed unsupported intervals count.
            direct_armed_=false;direct_elapsed_=automatic_remaining_=0;
            if(previous_supported_&&supported_seen_) {flight_=true;air_seconds_=0;}
            else if(flight_)air_seconds_=std::min(60.0,air_seconds_+h);
            previous_supported_=false;out.air_seconds=air_seconds_;
            out.status=Status::unsupported;return out;
        }
        const bool landing=flight_&&air_seconds_>minimum_air_seconds;
        out.air_seconds=air_seconds_;
        supported_seen_=previous_supported_=true;flight_=false;air_seconds_=0;
        if(s.inhibited) {
            direct_armed_=false;direct_elapsed_=automatic_remaining_=0;
            out.status=Status::inhibited;return out;
        }
        if(s.manual>.05||s.nose_manual>.05) {
            direct_armed_=false;direct_elapsed_=automatic_remaining_=0;
            out.status=Status::manual;return out;
        }
        if(landing) {automatic_remaining_=automatic_seconds;out.automatic_armed=true;}
        // Treat direct+automatic as one command: strongest wins, never sum two
        // force branches. Partial final steps preserve each time-integral bound.
        double direct=0,automatic=0;
        if(!released&&direct_armed_) {
            const double remaining=std::max(0.0,maximum_direct_seconds-direct_elapsed_);
            direct=-s.input*std::min(h,remaining)/h;
            direct_elapsed_=std::min(maximum_direct_seconds,direct_elapsed_+h);
            if(direct_elapsed_>=maximum_direct_seconds)direct_armed_=false;
        }
        if(automatic_remaining_>0) {
            automatic=automatic_input*std::min(h,automatic_remaining_)/h;
            automatic_remaining_=std::max(0.0,automatic_remaining_-h);
        }
        out.direct_input=direct;out.automatic_input=automatic;
        out.direct_seconds=direct_elapsed_;out.automatic_remaining=automatic_remaining_;
        // Rejected upward samples still consume window time; no deferred burst
        // or flight/hop force can be leased to a later supported observation.
        if(s.world_upward_speed>.5) {out.status=Status::upward;return out;}
        const double amount=std::max(direct,automatic);
        out.down_acceleration=nominal_down_acceleration*amount*(std::abs(s.pedal)>.0001?.75:1.0);
        out.applied=out.down_acceleration>0;
        if(out.applied)out.status=direct>0?(automatic>0?Status::combined:Status::direct):Status::automatic;
        else if(!released)out.status=direct_elapsed_>=maximum_direct_seconds?Status::exhausted:Status::waiting_release;
        else out.status=Status::neutral;
        return out;
    }
};
} // namespace bmx::detail::source::ground_pump
