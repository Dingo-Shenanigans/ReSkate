#pragma once
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
namespace dingosdk::trick_logger {
// Diagnostic policy: state 100/101/103 supports riding, not proof of a successful trick.
struct AttemptResult {
    std::uint64_t id{}, start{}, end{};
    double air_degrees{}, correction_degrees{};
    int provisional_spin{};
    std::string outcome;
};
class AttemptTracker {
    bool previous_{}, previous_air_{}, active_{};
    double yaw_{}, time_{}, ground_{}, quiet_{}, air_{}, correction_{};
    std::uint64_t next_{}, id_{}, start_{};
    std::optional<AttemptResult> finish(double now, const char* reason) {
        if(!active_) return {};
        active_=false;
        const double total=air_+correction_, magnitude=std::abs(total);
        // At most 60 degrees short of the next 180 increment; explicitly provisional.
        const int spin=static_cast<int>(std::floor((magnitude+60)/180))*180;
        return AttemptResult{id_,start_,static_cast<std::uint64_t>(now),air_,correction_,spin,reason};
    }
public:
    std::uint64_t active_id() const { return active_?id_:0; }
    std::optional<AttemptResult> stop(double now) { previous_=false;return finish(now,"recording_stopped"); }
    std::optional<AttemptResult> update(double now, bool valid, bool air, bool ground, double yaw) {
        if(valid && previous_ && now==time_) return {};
        if(!valid || !std::isfinite(yaw) || !std::isfinite(now) || (previous_ && (now<=time_ || now-time_>250))) {
            previous_=false;return finish(now,"tracking_interrupted");
        }
        std::optional<AttemptResult> result;
        if(previous_) {
            double delta=std::remainder(yaw-yaw_,360.0), dt=now-time_;
            if(air && !previous_air_) {
                result=finish(now,"next_launch_before_settle");
                active_=true;id_=++next_;start_=static_cast<std::uint64_t>(time_);
                air_=correction_=0;ground_=quiet_=0;
            }
            if(active_) {
                if(air) air_+=delta;
                else if(!ground) result=finish(now,"offboard_or_unsupported_state");
                else {
                    if(previous_air_) { ground_=now;quiet_=0; }
                    correction_+=delta;
                    quiet_=std::abs(delta)*1000/dt<35 ? quiet_+dt:0;
                    if(now-ground_>=100 && quiet_>=120) result=finish(now,"riding_settled_provisional");
                    else if(now-ground_>=350) result=finish(now,"landing_window_expired");
                }
            }
        }
        previous_=true;previous_air_=air;yaw_=yaw;time_=now;return result;
    }
};
}
