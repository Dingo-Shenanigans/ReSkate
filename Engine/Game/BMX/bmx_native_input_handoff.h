#pragma once
#include "Engine/Game/Input/bmx_native_board_bridge.h"
#include "Engine/Game/BMX/bmx_board_handoff_policy.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

// PRIVATE one-shot post-BMX experiment; no game calls, state writes or fallback.
namespace dingosdk::bmx::native_input_handoff {
using Owner=std::array<std::uintptr_t,6>; // base,client,entity,core,context,rig
using Counters=std::array<std::uint64_t,7>; // motion,pose,render,hide,forces,allocations,mounts
inline constexpr std::uint64_t freshness_ms=250,board_dwell_ms=2250;
struct Body {
    std::uintptr_t world{},state{};std::uint32_t index=0xffffffffU,generation{};std::uint64_t token{};
    bool operator==(const Body&) const = default;
    bool valid() const noexcept{return world&&state&&index<1048576U&&token;}
};
inline bool valid(const Owner& o) noexcept {for(auto p:o)if(!p)return false;return true;}
struct Ticket {Owner owner{};Body body;Counters counters{};std::uint64_t deadline_ms{},serial{};double native_time{};};
struct Sample {
    std::uint64_t now{},sampled_ms{},serial{};double native_time{};
    Owner owner{};Body body;Counters counters{};unsigned physics{},sdk_device{};
    bool configuration{},gates{},canonical{},protected_owner{},clock_coherent{},pool_released{};
    bool sdk_idle{},motion_released{},publication_inactive{},parts_zero{},capture_released{};
    bool original_slot0_fresh{},original_quiet{},only_slot0{},sdk_quiet{},gesture_released{};
    bool offboard{},standable{},motion_supported{};
};
enum class Phase:unsigned {idle,cleanup_wait,pulsing,observing,complete,aborted};
class Session {
    Phase phase_=Phase::idle;Ticket ticket_{};Sample latest_{};
    std::uint64_t last_wall_{},last_serial_{},ready_serial_{};
    board_handoff_policy::Dwell dwell_;
    double last_time_{};bool used_{},saw103_{},saw100_{},ready_{};
    const char* issue_="not_requested";
public:
    bool begin(const Ticket& t,std::uint64_t now) noexcept {
        if(used_||!valid(t.owner)||!t.body.valid()||!t.serial||!std::isfinite(t.native_time)||t.native_time<0||
           t.deadline_ms<=now||t.deadline_ms-now>15000)return false;
        used_=true;ticket_=t;last_wall_=now;last_serial_=t.serial;last_time_=t.native_time;
        dwell_.configure(double(board_dwell_ms)/1000);
        phase_=Phase::cleanup_wait;issue_="waiting_for_released_cleanup_and_two_native_samples";return true;
    }
    bool live() const noexcept{return phase_==Phase::cleanup_wait||phase_==Phase::pulsing||phase_==Phase::observing;}
    bool terminal() const noexcept{return phase_==Phase::complete||phase_==Phase::aborted;}
    Phase phase() const noexcept{return phase_;}bool ready() const noexcept{return ready_;}
    bool saw103() const noexcept{return saw103_;}bool saw100() const noexcept{return saw100_;}
    const char* issue() const noexcept{return issue_;}const Ticket& ticket() const noexcept{return ticket_;}
    const Sample& latest() const noexcept{return latest_;}
    void abort(const char* why) noexcept {if(live()){phase_=Phase::aborted;ready_=false;issue_=why;}}
    // Returns true exactly while a fresh ready504 snapshot can queue the shared Pulse.
    bool observe(const Sample& s) noexcept {
        if(!live())return false;
        latest_=s;ready_=false;
        if(s.now<last_wall_||s.now>=ticket_.deadline_ms){abort("handoff_deadline_or_wall_clock");return false;}last_wall_=s.now;
        if(!s.configuration||!s.gates){abort("handoff_configuration_focus_menu_or_lifecycle");return false;}
        if(!s.canonical||s.protected_owner||s.owner!=ticket_.owner||s.body!=ticket_.body||!s.pool_released){abort("handoff_owner_body_or_release_proof");return false;}
        if(s.counters!=ticket_.counters){abort("handoff_bmx_counter_delta_after_cleanup");return false;}
        if(!s.clock_coherent||!s.serial||!std::isfinite(s.native_time)||s.native_time<last_time_||s.serial<last_serial_||
           (s.serial==last_serial_)!=(s.native_time==last_time_)){abort("handoff_native_clock");return false;}
        const bool advanced=s.serial>last_serial_;last_serial_=s.serial;last_time_=s.native_time;
        if(!s.sampled_ms||s.sampled_ms>s.now||s.now-s.sampled_ms>freshness_ms){abort("handoff_source_snapshot_stale");return false;}
        if(!s.original_slot0_fresh||!s.only_slot0||s.sdk_device!=1||!s.original_quiet||!s.sdk_quiet){abort("handoff_actual_xinput_source_or_activity");return false;}
        const bool released=s.sdk_idle&&s.motion_released&&s.publication_inactive&&s.parts_zero&&s.capture_released&&s.gesture_released;
        if(!released){ready_serial_=0;if(phase_!=Phase::cleanup_wait)abort("handoff_cleanup_readiness_lost");else issue_="waiting_for_chord_release_and_cleanup";return false;}
        if(phase_==Phase::cleanup_wait) {
            if(s.physics!=504||!s.offboard||!s.standable||!s.motion_supported){abort("handoff_initial_native_state_not_standing504");return false;}
            if(!advanced)return false;
            if(!ready_serial_){ready_serial_=s.serial;issue_="waiting_for_second_released_native_sample";return false;}
            ready_=s.serial>ready_serial_;return ready_;
        }
        if(s.physics==103){saw103_=true;dwell_.interrupt();issue_="native103_observed";}
        else if(s.physics==100) {
            if(!saw103_){abort("native100_without_observed103");return false;}
            saw100_=true;
            const bool dwelled=dwell_.observe(true,{s.serial,s.native_time},s.now);
            if(phase_==Phase::observing&&dwelled) {
                phase_=Phase::complete;issue_="native103_then100_dwell_not_visual_quality";
            }
        }else if(s.physics==504) {
            if(saw103_||saw100_){abort("native_state_regressed_after_boarding");return false;}
        }else {abort("unexpected_native_state_no_software_fallback");return false;}
        return false;
    }
    bool pulse_queued() noexcept {
        if(phase_!=Phase::cleanup_wait||!ready_)return false;
        phase_=Phase::pulsing;ready_=false;issue_="one_native_input_transaction_queued";return true;
    }
    void pulse_complete(bool complete) noexcept {
        if(phase_!=Phase::pulsing)return;
        if(!complete){abort("native_input_offer_failed_no_retry");return;}
        phase_=Phase::observing;issue_="input_offer_complete_waiting_for_native103_then100";
    }
    // Callback consumes only the immutable fresh client proof; canonical owner,
    // protection, current physics/focus and original raw pad are checked again.
    bool callback_snapshot_ready(std::uint64_t now) const noexcept {
        return live()&&phase_!=Phase::cleanup_wait&&now>=latest_.now&&now-latest_.now<=freshness_ms&&
            now<ticket_.deadline_ms&&latest_.sdk_idle&&latest_.pool_released&&latest_.motion_released&&
            latest_.publication_inactive&&latest_.parts_zero&&latest_.capture_released&&latest_.gesture_released;
    }
};
} // namespace dingosdk::bmx::native_input_handoff
