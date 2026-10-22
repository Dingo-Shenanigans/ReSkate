#pragma once
#include "Engine/Game/BMX/bmx_human_native_handoff.h"

// Opt-in human reverse leg. Uses the same genuine original source/edge and
// process packet domain as the forward leg; never writes a native selector.
namespace dingosdk::bmx::human_native_handoff {
enum class ReturnPhase:unsigned {idle,admission_wait,release_wait,pulsing,observing,complete,aborted};
class Return {
    ReturnPhase phase_=ReturnPhase::idle;
    AdmissionWait waiting_;bridge::Pulse pulse_;
    native::Sample baseline_{},latest_{},first504_{};
    std::uint64_t deadline_{},last_wall_{},last_serial_{},ready_serial_{};double last_time_{};
    const char* issue_="not_requested";
    static bool clean(const native::Sample& s) noexcept {
        return s.configuration&&s.gates&&s.canonical&&!s.protected_owner&&s.clock_coherent&&
            s.sdk_idle&&s.pool_released&&s.motion_released&&s.publication_inactive&&s.parts_zero;
    }
    static native::Sample source_snapshot(native::Sample s,const Intent& source) noexcept {
        const auto selected=single_slot(s.sdk_device);unsigned fresh_mask{};
        for(unsigned i=0;i<4;++i){const auto& original=source.originals()[i];
            if(original.success&&original.wall&&s.now>=original.wall&&s.now-original.wall<=native::freshness_ms)fresh_mask|=1u<<i;
        }
        s.original_slot0_fresh=selected<4&&(fresh_mask&(1u<<selected));
        s.only_slot0=selected<4&&fresh_mask==(1u<<selected);
        s.original_quiet=selected<4&&quiet(source.originals()[selected].raw);return s;
    }
    bool coherent(const native::Sample& s) noexcept {
        return s.now>=last_wall_&&s.now<deadline_&&s.sampled_ms&&s.sampled_ms<=s.now&&s.now-s.sampled_ms<=native::freshness_ms&&
            s.serial&&std::isfinite(s.native_time)&&s.native_time>=0&&s.serial>=last_serial_&&s.native_time>=last_time_&&
            (s.serial==last_serial_)==(s.native_time==last_time_)&&s.owner==baseline_.owner&&s.body==baseline_.body&&s.counters==baseline_.counters&&clean(s);
    }
public:
    bool active() const noexcept{return phase_==ReturnPhase::admission_wait||phase_==ReturnPhase::release_wait||phase_==ReturnPhase::pulsing||phase_==ReturnPhase::observing;}
    bool complete() const noexcept{return phase_==ReturnPhase::complete;}
    ReturnPhase phase() const noexcept{return phase_;}const char* issue() const noexcept{return issue_;}
    const auto& pulse() const noexcept{return pulse_;}const auto& baseline() const noexcept{return baseline_;}
    const auto& latest() const noexcept{return latest_;}const auto& first504() const noexcept{return first504_;}
    const auto& waiting() const noexcept{return waiting_;}std::uint64_t deadline() const noexcept{return deadline_;}
    void cancel(Intent& source,const char* why) noexcept {
        if(!active())return;
        phase_=ReturnPhase::aborted;issue_=why;waiting_.cancel(why);if(pulse_.live())pulse_.abort(why);source.cancel(why);
    }
    bool begin(const native::Sample& s,const AdmissionContext& c,Intent& source) noexcept {
        if(active()||source.active()||!c.allowed||!c.sdk_chord_held||c.now!=s.now||c.sampled_ms!=s.sampled_ms||
            c.serial!=s.serial||c.native_time!=s.native_time||c.owner!=s.owner||c.device!=s.sdk_device||
            !native::valid(s.owner)||!s.body.valid()||!clean(s)||!s.serial||!std::isfinite(s.native_time)||s.native_time<0||
            !s.sampled_ms||s.sampled_ms>s.now||s.now-s.sampled_ms>native::freshness_ms||
            (s.physics!=100&&s.physics!=103)||s.offboard||s.now>std::numeric_limits<std::uint64_t>::max()-15000)return false;
        // A new deliberate gesture may start a new generation; never reset source
        // witnesses or packet counters. Failure cannot arm an automatic retry.
        waiting_={};pulse_={};baseline_=latest_=source_snapshot(s,source);first504_={};ready_serial_=0;
        deadline_=s.now+15000;last_wall_=s.now;last_serial_=s.serial;last_time_=s.native_time;
        if(source.admit(c.now,c.device,c.owner)){phase_=ReturnPhase::release_wait;issue_="release_actual_LB_and_Y_before_reverse_pulse";return true;}
        if(waiting_.begin(c,source)){phase_=ReturnPhase::admission_wait;issue_="waiting_for_actual_original_reverse_chord";return true;}
        phase_=ReturnPhase::aborted;issue_="reverse_original_source_refused";return false;
    }
    void note(unsigned user,bool success,std::uint64_t now,Intent& source) noexcept {
        if(phase_==ReturnPhase::admission_wait)waiting_.note(user,success,now,source);
        if(active()&&phase_!=ReturnPhase::admission_wait&&!source.active())cancel(source,"reverse_actual_source_lost");
    }
    void observe(const native::Sample& s,const Guard& g,bool sdk_chord_held,Intent& source) noexcept {
        if(!active())return;
        if(s.now!=g.now||s.sampled_ms!=g.sampled_ms||s.owner!=g.owner||s.sdk_device!=g.device||!g.allowed||!coherent(s)){
            cancel(source,"reverse_context_cleanup_owner_counter_or_clock_changed");return;
        }
        const bool advanced=s.serial>last_serial_;
        last_wall_=s.now;last_serial_=s.serial;last_time_=s.native_time;latest_=source_snapshot(s,source);
        if(phase_==ReturnPhase::admission_wait){
            AdmissionContext c{s.now,s.sampled_ms,s.serial,s.native_time,s.owner,s.sdk_device,g.allowed,sdk_chord_held};
            if(!waiting_.consume(c,source)){
                if(!waiting_.active())cancel(source,waiting_.issue());return;
            }
            if(!source.admit(c.now,c.device,c.owner)){cancel(source,"reverse_final_original_admission_refused");return;}
            phase_=ReturnPhase::release_wait;issue_="release_actual_LB_and_Y_before_reverse_pulse";
        }
        if(!source.guard(g)){cancel(source,source.issue());return;}
        if((phase_==ReturnPhase::pulsing||phase_==ReturnPhase::observing)&&
            (!source.released()||!s.sdk_quiet||!s.capture_released)){
            cancel(source,"reverse_released_input_or_capture_proof_lost");return;
        }
        const bool board=s.physics==100||s.physics==103;
        if(!board&&!(s.physics==504&&pulse_.press_reports)){cancel(source,"reverse_unexpected_native_state");return;}
        if(phase_==ReturnPhase::release_wait){
            if(!board){cancel(source,"reverse_initial_board_state_lost");return;}
            if(!source.released()||!s.sdk_quiet||!s.capture_released){ready_serial_=0;return;}
            if(!advanced)return;
            if(!ready_serial_){ready_serial_=s.serial;issue_="waiting_for_second_released_board_sample";return;}
            if(!pulse_.queue(s.now,source.slot(),bridge::Direction::board_dismount)){cancel(source,"reverse_pulse_queue_refused");return;}
            phase_=ReturnPhase::pulsing;issue_="one_actual_original_reverse_pulse_queued";
        }
        if(s.physics==504&&!first504_.serial)first504_=latest_;
        if(phase_==ReturnPhase::observing&&s.physics==504&&s.offboard&&s.standable&&s.motion_supported){
            phase_=ReturnPhase::complete;issue_="actual_native504_after_one_reverse_input_sequence";source.cancel(issue_);
        }
    }
    bridge::Result poll(std::uint64_t now,unsigned user,bool identity,bool focused,bool ready,std::uint32_t physics,const bridge::Pad& raw,Intent& source) noexcept {
        bridge::Result out;
        if(phase_!=ReturnPhase::pulsing||user!=source.slot())return out;
        if(!source.active()||!identity||!focused||!ready||now<latest_.now||now-latest_.now>native::freshness_ms||
            now>=deadline_||!latest_.sdk_quiet||!latest_.capture_released||!quiet(raw)){
            cancel(source,"reverse_callback_readiness_lost");return out;
        }
        out=pulse_.poll(now,user,identity,focused,ready,true,physics,false,"reverse_callback_readiness_lost");
        if(pulse_.phase==bridge::Phase::aborted)cancel(source,pulse_.issue);
        else if(pulse_.phase==bridge::Phase::complete){phase_=ReturnPhase::observing;issue_="reverse_input_complete_waiting_for_actual_native504";}
        return out;
    }
};
} // namespace dingosdk::bmx::human_native_handoff
