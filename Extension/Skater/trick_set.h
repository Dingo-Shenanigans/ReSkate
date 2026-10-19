#pragma once
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <array>
namespace dingosdk::trick_logger {
inline constexpr double grind_spin_minimum=115.0;
inline std::string starting_stance(std::string_view stance,std::string_view nollie,std::string_view base={}) {
    if(stance.find("SWITCH")!=std::string_view::npos)return "switch";
    if(stance.find("FAKIE")!=std::string_view::npos)return "fakie";
    if(stance.find("NOLLIE")!=std::string_view::npos || !nollie.empty() || base=="Nollie" || base=="Quick Nollie" || base=="ID_TRICK_NOLLIE" || base=="ID_TRICK_QUICKNOLLIE" || base=="ID_TRICK_BONEDNOLLIE")return "nollie";
    return "normal";
}
struct MovementPhase {
    std::uint64_t id{},begin{},end{};
    std::size_t segment{};
    std::string kind;
};
struct CaptureSpan {
    std::uint64_t first{},last{};
    bool closed{};
    bool contains(std::uint64_t id) const {return first && id>=first && id<=last;}
};
// A rail transfer remains within the route. Ground contact (including a
// manual) closes it; without grinds, only the first eligible movement phase is
// retained. Native sequence completion remains a separate landing decision.
inline CaptureSpan capture_span(const std::vector<MovementPhase>& phases,std::uint64_t first,bool grinds) {
    CaptureSpan span{first,first,false};bool found{},ground_start{};
    for(const auto& phase:phases) {
        if(phase.id<first)continue;
        if(!found){if(phase.id!=first)return {};found=true;ground_start=phase.kind=="ground";span.last=phase.id;continue;}
        if(!grinds || ground_start || phase.kind=="ground") {span.closed=true;break;}
        span.last=phase.id;
    }
    return found?span:CaptureSpan{};
}
struct SpinSegment {
    std::uint64_t begin{},end{},stance_observation{};
    bool grind_adjacent{};
    double air{}, landing{};
    double accepted_net{}, excluded_ground{};
    int degrees{};
    std::optional<bool> goofy;
    std::string stance, direction;
    double recovery_travel{};
    bool recovery_closed{};
    bool rotation_complete{true};
};
struct TrickSetResult {
    std::uint64_t id{},start{},end{};
    std::string outcome;
    std::optional<bool> landed;
    std::vector<SpinSegment> segments;
    std::vector<MovementPhase> phases;
    bool rotation_complete{true};
    double grounded_travel{};
};
struct SetSample {
    std::uint64_t now{},steps{},wipeouts{},sequence_ends{},entity{};
    std::uint32_t state{};
    bool valid{},ragdoll{};
    double yaw{};
    std::optional<bool> goofy;
    std::array<float,3> position{};
    bool position_known{};
};
class TrickSetTracker {
    bool previous_{},active_{},air_{},landing_{},sequence_seen_{},suspended_{};
    bool require_sequence_{true};
    bool bounded_{},grinds_{};
    std::uint64_t capture_first_{};
    SetSample prev_{};
    std::uint64_t next_{},start_sequence_{},start_wipeouts_{},touchdown_steps_{};
    TrickSetResult set_;
    double grounded_travel_{};
    std::optional<TrickSetResult> finish(std::uint64_t now,const char* reason,std::optional<bool> landed) {
        if(!active_) return {};
        active_=false;suspended_=false;set_.end=now;set_.outcome=reason;set_.landed=landed;
        if(bounded_ && capture_first_) {
            set_.rotation_complete=true;
            const auto span=capture();
            for(const auto& phase:set_.phases) {
                if(phase.id>span.last)break;
                if(phase.kind=="air" && span.contains(phase.id) && phase.segment && phase.segment<=set_.segments.size())
                    set_.rotation_complete&=set_.segments[phase.segment-1].rotation_complete;
            }
        }
        for(auto& spin:set_.segments) {
            const double measured=spin.air+spin.landing;
            const double net=measured;
            spin.accepted_net=net;spin.excluded_ground=0;
            // Summing heading deltas can leave an exact 115-degree turn a few
            // floating-point ulps below the inclusive boundary. This tolerance
            // only absorbs numerical cancellation, not another gameplay margin.
            spin.degrees=spin.grind_adjacent && std::abs(net)+1e-6<grind_spin_minimum?0:static_cast<int>(std::floor((std::abs(net)+90)/180))*180;
            if(spin.degrees && spin.goofy && !spin.stance.empty()) {
                bool fs=net>0; // Y-up signed atan2(x,z): positive is CCW viewed from above.
                if(*spin.goofy) fs=!fs;
                if(spin.stance=="switch") fs=!fs;
                spin.direction=fs?"FS":"BS";
            }
        }
        return set_;
    }
public:
    TrickSetTracker() = default;
    explicit TrickSetTracker(bool require_sequence):require_sequence_(require_sequence) {}
    TrickSetTracker(bool require_sequence,bool bounded,bool grinds):require_sequence_(require_sequence),bounded_(bounded),grinds_(grinds) {}
    CaptureSpan capture() const {return capture_span(set_.phases,capture_first_,grinds_);}
    void retain_from(std::uint64_t phase) {if(phase && (!capture_first_ || phase<capture_first_))capture_first_=phase;}
    bool excessive_recovery() const {
        if(!bounded_)return grounded_travel_>=450;
        const auto span=capture();
        for(const auto& phase:set_.phases) {
            if(phase.id>span.last)break;
            if(phase.kind=="air" && span.contains(phase.id) && phase.segment && phase.segment<=set_.segments.size() &&
                set_.segments[phase.segment-1].recovery_travel>=450)return true;
        }
        return false;
    }
    std::uint64_t active_id() const {return active_?set_.id:0;}
    std::uint64_t capture_id() const {return set_.id;}
    std::uint64_t started_at() const {return active_?set_.start:0;}
    double grounded_travel() const {return grounded_travel_;}
    std::size_t segment_id() const {return active_?set_.segments.size():0;}
    bool recovering() const {return active_ && prev_.state==101;}
    const std::vector<MovementPhase>& phases() const {return set_.phases;}
    const MovementPhase* phase_at(std::uint64_t now) const {
        for(auto it=set_.phases.rbegin();it!=set_.phases.rend();++it)
            if(now>=it->begin && (!it->end || now<it->end))return &*it;
        return nullptr;
    }
    void stance(std::size_t segment,std::string value,std::uint64_t observation=0) {
        if(active_ && segment && segment<=set_.segments.size()) {
            auto& target=set_.segments[segment-1];
            if(target.stance.empty() || (observation && target.stance_observation==observation)) {
                target.stance=std::move(value);target.stance_observation=observation;
            }
        }
    }
    void begin_continuation() {
        if(active_ || !previous_)return;
        set_={};capture_first_=0;set_.id=++next_;set_.start=prev_.now;active_=true;
        start_sequence_=prev_.sequence_ends;start_wipeouts_=prev_.wipeouts;
        sequence_seen_=landing_=false;grounded_travel_=0;touchdown_steps_=prev_.steps;
        set_.phases.push_back({1,prev_.now,0,0,prev_.state>=400 && prev_.state<500?"grind":"ground"});
    }
    std::optional<TrickSetResult> stop(std::uint64_t now) {previous_=false;return finish(now,"recording_stopped",{});}
    std::optional<TrickSetResult> update(const SetSample& s) {
        if(active_ && (s.wipeouts>start_wipeouts_ || s.state==300 || s.ragdoll)) {
            previous_=false;return finish(s.now,"bailed",false);
        }
        if(active_ && s.state==504) {previous_=false;return finish(s.now,"offboard",{});}
        if(previous_ && (s.entity!=prev_.entity || s.now<prev_.now || s.now-prev_.now>250)) {
            previous_=false;return finish(s.now,"tracking_lost",{});
        }
        if(previous_ && s.position_known && prev_.position_known) {
            double distance2{};for(unsigned i=0;i<3;++i){const double d=s.position[i]-prev_.position[i];distance2+=d*d;}
            if(distance2>25) {previous_=false;return finish(s.now,"position_reset",{});}
        }
        if(s.state==700) {previous_=false;return finish(s.now,"position_reset",{});}
        if(!s.valid || !std::isfinite(s.yaw)) {
            suspended_=true;return {};
        }
        if(previous_ && s.now==prev_.now)return {};
        const bool air=s.state==200||s.state==201||s.state==203||s.state==701;
        const bool grind=s.state>=400&&s.state<500;
        const bool ground=s.state==100||s.state==101||s.state==103;
        if(!air&&!grind&&!ground && s.state!=602) {previous_=false;return finish(s.now,"unsupported_state",{});}
        if(previous_) {
            const bool missing_rotation=suspended_ && (air || air_ || landing_ || (active_ && !grind));
            const double delta=suspended_?0:std::remainder(s.yaw-prev_.yaw,360.0);
            if(active_ && missing_rotation) {
                set_.rotation_complete=false;
                if(!set_.segments.empty() && !set_.segments.back().recovery_closed)set_.segments.back().rotation_complete=false;
            }
            suspended_=false;
            if(!active_ && air && !air_) {
                set_={};capture_first_=0;set_.rotation_complete=!missing_rotation;set_.id=++next_;set_.start=prev_.now;active_=true;
                start_sequence_=s.sequence_ends;start_wipeouts_=s.wipeouts;sequence_seen_=false;landing_=false;
            }
            if(active_) {
                if(air)grounded_travel_=0;
                // Count travel, not net angle, throughout a grounded recovery
                // chain. Opposite turns cannot erase the allowance used.
                else if(!air_ && !grind && (s.state==101 || (prev_.state==101 && ground))) {
                    grounded_travel_+=std::abs(delta);set_.grounded_travel=grounded_travel_;
                    if(!bounded_ && grounded_travel_>=450)return finish(s.now,"excessive_grounded_rotation",false);
                }
                if(air&&!air_) {
                    if(bounded_ && !set_.segments.empty())set_.segments.back().recovery_closed=true;
                    SpinSegment segment;segment.goofy=s.goofy;segment.begin=prev_.now;segment.grind_adjacent=prev_.state>=400 && prev_.state<500;segment.rotation_complete=!missing_rotation;set_.segments.push_back(segment);
                    landing_=false;
                }
                const std::string phase_kind=air?"air":grind?"grind":"ground";
                if(set_.phases.empty() || set_.phases.back().kind!=phase_kind) {
                    if(!set_.phases.empty())set_.phases.back().end=s.now;
                    set_.phases.push_back({set_.phases.size()+1,set_.phases.empty()?set_.start:s.now,0,set_.segments.size(),phase_kind});
                }
                if(s.sequence_ends>start_sequence_)sequence_seen_=true;
                if(!set_.segments.empty()) {
                    auto& segment=set_.segments.back();
                    if(!bounded_ || !segment.recovery_closed) {
                        if(air || air_)segment.air+=delta;
                        else if((s.state==101 || (prev_.state==101 && ground)) && !grind) {
                            segment.landing+=delta;segment.recovery_travel+=std::abs(delta);
                        }
                    }
                    if(air_ && !air)segment.end=s.now;
                    if(grind && air_)segment.grind_adjacent=true;
                    if(bounded_ && grind)segment.recovery_closed=true;
                }
                if(excessive_recovery())return finish(s.now,"excessive_grounded_rotation",false);
                // A landing revert may begin after ordinary riding (including a
                // brief manual transition). Reopen measured landing rotation only
                // for the native 101 action, while the same sequence is pending.
                if(s.state==101 && !air_ && !set_.segments.empty()) {
                    // Rotation was measured once above, using native revert transitions.
                    landing_=true;
                }
                if(ground && (air_ || (prev_.state>=400&&prev_.state<500))) {
                    landing_=s.state==101;touchdown_steps_=s.steps;
                } else if(ground && s.state!=101) landing_=false;
                if(grind) {landing_=false;sequence_seen_=false;start_sequence_=s.sequence_ends;}
                // Ordinary riding after a subsequent physics step confirms touchdown.
                // Gameplay and diagnostics require native HUD sequence completion; this
                // is the all-wheel sequence event, not the separate line timer.
                // No line timer, low-yaw timeout, or auto-revert preference is involved.
                if(ground && s.state!=101 && (!require_sequence_ || sequence_seen_) && s.steps>touchdown_steps_) {
                    prev_=s;air_=false;previous_=true;return finish(s.now,"landed",true);
                }
            }
        }
        suspended_=false;prev_=s;air_=air;previous_=true;return {};
    }
};
}
