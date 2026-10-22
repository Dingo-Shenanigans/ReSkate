#pragma once
#include "Engine/Game/BMX/bmx_native_input_handoff.h"

// Explicit Ride prototype. The frozen private Session is reused as a logical
// single-source cleanup/native-observation gate, not as proof of physical slot0.
// Original selected-slot identity is proved here before any normalization.
namespace dingosdk::bmx::human_native_handoff {
namespace native=native_input_handoff;
namespace bridge=input::native_board_bridge;
inline bool quiet(const bridge::Pad& p) noexcept {
    return !p.buttons&&p.left_trigger<=12&&p.right_trigger<=12&&std::abs(int(p.left_x))<=1638&&
        std::abs(int(p.left_y))<=1638&&std::abs(int(p.right_x))<=1638&&std::abs(int(p.right_y))<=1638;
}
inline bool gesture_only(bridge::Pad p) noexcept {p.buttons&=~std::uint16_t(0x8100);return quiet(p);}
inline unsigned single_slot(unsigned mask) noexcept {
    for(unsigned i=0;i<4;++i)if(mask==(1u<<i))return i;return 4;
}
struct Original {std::uint64_t wall{};bool success{};std::uint32_t packet{};bridge::Pad raw{};};
struct Guard {std::uint64_t now{},sampled_ms{};native::Owner owner{};unsigned device{};bool allowed{},sdk_gesture_only{},sdk_released{};};
class Intent {
    std::array<Original,4> originals_{};
    std::array<bridge::PacketContinuity,4> packets_{}; // Never reset with a request.
    std::array<bool,4> north_armed_{};
    std::array<std::uint64_t,4> north_rise_{},spent_rise_{};
    native::Session session_;bridge::Pulse pulse_;
    std::array<Original,4> admission_{};
    native::Sample first103_{},first100_{};
    native::Owner owner_{};std::uint64_t generation_{},accepted_{},last_{};
    std::uint64_t observed_serial_{};double observed_time_{};
    unsigned slot_=4;bool active_{},transferred_{},released_{},original_released_{};
    const char* issue_="not_requested";
    bool fresh(unsigned slot,std::uint64_t now) const noexcept {
        const auto& o=originals_[slot];return o.success&&o.wall&&now>=o.wall&&now-o.wall<=native::freshness_ms;
    }
    bool unique(std::uint64_t now,unsigned selected) const noexcept {
        if(selected>3||!fresh(selected,now))return false;
        for(unsigned i=0;i<4;++i)if(i!=selected&&fresh(i,now))return false;return true;
    }
public:
    void cancel(const char* why) noexcept {
        if(!active_)return;active_=false;issue_=why;session_.abort(why);if(pulse_.live())pulse_.abort(why);
    }
    void note(bool outer,unsigned user,bool success,std::uint64_t now,std::uint32_t packet,const bridge::Pad& raw) noexcept {
        if(!outer||user>3)return;
        if(originals_[user].wall&&now<originals_[user].wall){originals_[user]={};north_armed_[user]=false;north_rise_[user]=0;cancel("original_clock_reversed");return;}
        const bool previous_north=originals_[user].success&&(originals_[user].raw.buttons&0x8000)!=0;
        if(!success){north_armed_[user]=false;north_rise_[user]=0;}
        else if(!(raw.buttons&0x8000))north_armed_[user]=true;
        else if(!previous_north&&north_armed_[user]){north_armed_[user]=false;north_rise_[user]=(raw.buttons&0x0100)?now:0;}
        originals_[user]={now,success,packet,success?raw:bridge::Pad{}};
        if(active_&&((user==slot_&&!success)||(user!=slot_&&success)))cancel("original_source_disconnected_or_extra_slot");
        if(active_&&user==slot_&&success){
            if(!(original_released_?quiet(raw):gesture_only(raw)))cancel("new_physical_activity");
            if(quiet(raw))original_released_=true;
        }
    }
    bool admit(std::uint64_t now,unsigned device,const native::Owner& owner) noexcept {
        if(active_||generation_==std::numeric_limits<std::uint64_t>::max()||!native::valid(owner)||
            now>std::numeric_limits<std::uint64_t>::max()-15000)return false;
        const auto selected=single_slot(device);if(selected>3)return false;
        if(!unique(now,selected)||!north_rise_[selected]||north_rise_[selected]==spent_rise_[selected]||
            now<north_rise_[selected]||now-north_rise_[selected]>native::freshness_ms||
            !gesture_only(originals_[selected].raw)||(originals_[selected].raw.buttons&0x8100)!=0x8100){
            issue_="fresh_single_original_XInput_chord_required";return false;
        }
        slot_=selected;++generation_;spent_rise_[slot_]=north_rise_[slot_];owner_=owner;accepted_=last_=now;active_=true;
        transferred_=released_=original_released_=false;
        admission_=originals_;first103_=first100_={};
        session_={};pulse_={};issue_="stepping_off_release_LB_and_Y";return true;
    }
    bool guard(const Guard& g) noexcept {
        if(!active_)return false;
        const auto deadline=transferred_?session_.ticket().deadline_ms:accepted_+15000;
        if(g.now<last_||g.now>=deadline){cancel("original_switch_deadline_or_wall_clock");return false;}last_=g.now;
        if(!g.allowed||g.owner!=owner_||g.device!=(1u<<slot_)||!g.sampled_ms||g.sampled_ms>g.now||g.now-g.sampled_ms>native::freshness_ms||!unique(g.now,slot_)){
            cancel("human_context_owner_device_or_source_changed");return false;
        }
        const auto& raw=originals_[slot_].raw;
        if(!g.sdk_gesture_only||!(released_?quiet(raw):gesture_only(raw))||
            (released_&&!g.sdk_released)){cancel("new_physical_activity");return false;}
        // Both views must witness release. It cannot be invented after a failed read.
        if(g.sdk_released&&quiet(raw))released_=true;
        if(!released_)issue_="waiting_for_actual_chord_release";
        return true;
    }
    bool transfer(const native::Ticket& ticket,std::uint64_t now) noexcept {
        if(!active_||transferred_||ticket.owner!=owner_||now<last_||now>=accepted_+15000||ticket.deadline_ms>accepted_+15000||
            !session_.begin(ticket,now)){cancel("human_cleanup_ticket_refused");return false;}
        transferred_=true;last_=now;observed_serial_=ticket.serial;observed_time_=ticket.native_time;
        issue_="waiting_for_release_cleanup_and_native504";return true;
    }
    void observe(native::Sample sample,const Guard& g) noexcept {
        if(!guard(g)||!transferred_)return;
        if(sample.now!=g.now||sample.sampled_ms!=g.sampled_ms||sample.owner!=g.owner||!sample.clock_coherent||!sample.serial||
            !std::isfinite(sample.native_time)||sample.serial<observed_serial_||sample.native_time<observed_time_||
            (sample.serial==observed_serial_)!=(sample.native_time==observed_time_)){
            cancel("human_transferred_native_clock_changed");return;
        }
        observed_serial_=sample.serial;observed_time_=sample.native_time;
        // Until physical release, still check the immutable cleanup ticket,
        // owner/protection, wall/native clocks and all ordinary read gates.
        if(!released_){
            const auto& t=session_.ticket();
            if(!sample.configuration||!sample.gates||!sample.canonical||sample.protected_owner||sample.owner!=t.owner||
                sample.body!=t.body||!sample.pool_released||sample.counters!=t.counters||!sample.clock_coherent||
                sample.physics!=504||!sample.offboard||!sample.standable||!sample.motion_supported)cancel("human_cleanup_changed_before_release");
            return;
        }
        // Explicit selected-slot -> logical single-source normalization. No
        // hardcoded physical slot0; unique() has checked the actual slot above.
        sample.original_slot0_fresh=true;sample.original_quiet=quiet(originals_[slot_].raw);
        sample.only_slot0=true;sample.sdk_device=1;
        if(session_.observe(sample)) {
            if(!pulse_.queue(sample.now,slot_)||!session_.pulse_queued())cancel("human_pulse_queue_refused");
        }
        if(sample.physics==103&&session_.saw103()&&!first103_.serial)first103_=sample;
        if(sample.physics==100&&session_.saw100()&&!first100_.serial)first100_=sample;
        if(session_.terminal()){active_=false;issue_=session_.issue();}
        else issue_=session_.issue();
    }
    bridge::Result poll(std::uint64_t now,unsigned user,bool identity,bool focused,bool ready,std::uint32_t physics,const bridge::Pad& raw) noexcept {
        bridge::Result r;
        if(!active_||!transferred_||!pulse_.live())return r;
        if(!identity||!focused||!ready||!session_.callback_snapshot_ready(now)){
            cancel("human_callback_readiness_lost");return r;
        }
        r=pulse_.poll(now,user,identity&&session_.callback_snapshot_ready(now),focused,ready,quiet(raw),physics,false,"human_callback_readiness_lost");
        if(pulse_.phase==bridge::Phase::aborted)cancel(pulse_.issue);
        else if(pulse_.phase==bridge::Phase::complete)session_.pulse_complete(true);
        return r;
    }
    std::uint32_t packet(unsigned user,std::uint32_t before,const bridge::Pad& final,bool offer) noexcept {
        if(user>3)return before;auto& p=packets_[user];return p.active||offer?p.translate(before,final):before;
    }
    bool active() const noexcept{return active_;}bool transferred() const noexcept{return transferred_;}
    unsigned slot() const noexcept{return slot_;}std::uint64_t generation() const noexcept{return generation_;}
    bool released() const noexcept{return released_;}const char* issue() const noexcept{return issue_;}
    const native::Owner& owner() const noexcept{return owner_;}
    const native::Session& session() const noexcept{return session_;}
    const bridge::Pulse& pulse() const noexcept{return pulse_;}
    const auto& originals() const noexcept{return originals_;}const auto& packets() const noexcept{return packets_;}
    const auto& admission() const noexcept{return admission_;}std::uint64_t accepted_ms() const noexcept{return accepted_;}
    const auto& north_armed() const noexcept{return north_armed_;}
    const auto& north_rises() const noexcept{return north_rise_;}
    const auto& spent_rises() const noexcept{return spent_rise_;}
    const auto& first103() const noexcept{return first103_;}const auto& first100() const noexcept{return first100_;}
};
// SDK sampling can precede the next genuine game-facing original poll. This
// bounded intent waits for that poll; it cannot admit, park, pulse or forge input.
enum class AdmissionSource:unsigned {blocked,waiting,ready};
inline AdmissionSource admission_source(const Intent& intent,std::uint64_t now,unsigned device) noexcept {
    const auto slot=single_slot(device);
    if(intent.active()||slot>3||intent.generation()==std::numeric_limits<std::uint64_t>::max()||
        now>std::numeric_limits<std::uint64_t>::max()-15000)return AdmissionSource::blocked;
    const auto& all=intent.originals();
    const auto fresh=[&](unsigned i){const auto& o=all[i];return o.success&&o.wall&&now>=o.wall&&now-o.wall<=native::freshness_ms;};
    if(!fresh(slot))return AdmissionSource::blocked;
    for(unsigned i=0;i<4;++i)if(i!=slot&&fresh(i))return AdmissionSource::blocked;
    const auto& raw=all[slot].raw;if(!gesture_only(raw))return AdmissionSource::blocked;
    if(!(raw.buttons&0x8000))return intent.north_armed()[slot]?AdmissionSource::waiting:AdmissionSource::blocked;
    const auto rise=intent.north_rises()[slot];
    return raw.buttons==0x8100&&rise&&rise!=intent.spent_rises()[slot]&&now>=rise&&now-rise<=native::freshness_ms?
        AdmissionSource::ready:AdmissionSource::blocked;
}
struct AdmissionContext {
    std::uint64_t now{},sampled_ms{},serial{};double native_time{};
    native::Owner owner{};unsigned device{};bool allowed{},sdk_chord_held{};
};
enum class AdmissionWaitResult:unsigned {idle,waiting,ready,canceled};
class AdmissionWait {
    native::Owner owner_{};unsigned device_{};
    std::uint64_t started_{},last_{},sampled_{},serial_{};double native_time_{};
    bool active_{};std::uint16_t seen_buttons_{};const char* issue_="not_pending";
    bool context(const AdmissionContext& c) const noexcept {
        return c.allowed&&c.sdk_chord_held&&native::valid(c.owner)&&single_slot(c.device)<4&&
            c.sampled_ms&&c.sampled_ms<=c.now&&c.now-c.sampled_ms<=native::freshness_ms&&
            c.serial&&std::isfinite(c.native_time)&&c.native_time>=0;
    }
public:
    void cancel(const char* why) noexcept {if(active_){active_=false;issue_=why;}}
    bool begin(const AdmissionContext& c,const Intent& intent) noexcept {
        if(active_||!context(c)||admission_source(intent,c.now,c.device)!=AdmissionSource::waiting)return false;
        owner_=c.owner;device_=c.device;started_=last_=c.now;sampled_=c.sampled_ms;serial_=c.serial;native_time_=c.native_time;
        active_=true;seen_buttons_=intent.originals()[single_slot(device_)].raw.buttons;issue_="waiting_for_fresh_original_North";return true;
    }
    // Called for every genuine outer original, so transient activity/release
    // cannot disappear between SDK observations. SDK/nested reads never call it.
    void note(unsigned user,bool success,std::uint64_t now,const Intent& intent) noexcept {
        if(!active_)return;
        if(now<last_||now-started_>native::freshness_ms){cancel("pending_original_clock_or_deadline");return;}
        if((user==single_slot(device_)&&!success)||(user!=single_slot(device_)&&success)){
            cancel("pending_original_source_changed");return;
        }
        const auto source=admission_source(intent,now,device_);
        const auto buttons=intent.originals()[single_slot(device_)].raw.buttons;
        if(source==AdmissionSource::blocked||(buttons&seen_buttons_)!=seen_buttons_){
            cancel("pending_original_activity_or_release");return;
        }
        seen_buttons_|=buttons;
    }
    AdmissionWaitResult update(const AdmissionContext& c,const Intent& intent) noexcept {
        if(!active_)return AdmissionWaitResult::idle;
        if(!context(c)||c.owner!=owner_||c.device!=device_||c.now<last_||c.now-started_>native::freshness_ms||
            c.sampled_ms<sampled_||c.serial<serial_||c.native_time<native_time_||
            (c.serial==serial_)!=(c.native_time==native_time_)){
            cancel("pending_context_clock_release_or_deadline");return AdmissionWaitResult::canceled;
        }
        last_=c.now;sampled_=c.sampled_ms;serial_=c.serial;native_time_=c.native_time;
        const auto source=admission_source(intent,c.now,device_);
        const auto buttons=intent.originals()[single_slot(device_)].raw.buttons;
        if(source==AdmissionSource::blocked||(buttons&seen_buttons_)!=seen_buttons_){
            cancel("pending_original_source_or_release");return AdmissionWaitResult::canceled;
        }
        seen_buttons_|=buttons;
        if(source==AdmissionSource::ready){issue_="fresh_original_ready";return AdmissionWaitResult::ready;}
        return AdmissionWaitResult::waiting;
    }
    bool consume(const AdmissionContext& c,const Intent& intent) noexcept {
        if(update(c,intent)!=AdmissionWaitResult::ready)return false;
        active_=false;issue_="fresh_original_ready_consumed";return true;
    }
    bool active() const noexcept{return active_;}const char* issue() const noexcept{return issue_;}
    const auto& owner() const noexcept{return owner_;}unsigned device() const noexcept{return device_;}
    std::uint64_t started_ms() const noexcept{return started_;}std::uint64_t sampled_ms() const noexcept{return sampled_;}
};
} // namespace dingosdk::bmx::human_native_handoff
