#pragma once
#include "Engine/Game/BMX/bmx_interaction.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include "Engine/Game/BMX/bmx_board_handoff_policy.h"
#ifdef _WIN32
#include "Engine/Core/Platform/user_data_paths.h"
#include <Windows.h>
#endif

namespace dingosdk::bmx::quick_switch_rehearsal {
inline constexpr std::uint32_t virtual_device=0x424d5803;
inline constexpr std::uint64_t wall_limit_ms=190000;
inline constexpr double phase_limit_seconds=25,hold_seconds=.2;
inline bool configuration_selected(bool development,std::wstring_view profile,
    std::wstring_view quick,std::wstring_view automated,std::wstring_view native,
    bool switch_test,bool human_ride,bool unattended,bool exclusive) noexcept {
    return development&&profile==L"bmx-user-data-chassis-probe"&&quick==L"1"&&
        automated==L"1"&&native==L"1"&&switch_test&&!human_ride&&!unattended&&exclusive;
}
inline bool enabled() noexcept {
#ifdef _WIN32
    static const bool selected=[]() noexcept {
        try {
            if(!user_data::development_requested())return false;
            const auto root=user_data::override_root();if(!root)return false;
            const auto exact=[](const wchar_t* name) noexcept {
                wchar_t value[2]{};return GetEnvironmentVariableW(name,value,2)==1&&value[0]==L'1';
            };
            if(!exact(L"RESKATE_BMX_QUICK_SWITCH_TEST")||!exact(L"RESKATE_BMX_AUTOMATED_SWITCH")||
               !exact(L"RESKATE_BMX_NATIVE_CHASSIS"))return false;
            wchar_t mode[32]{};
            const auto mode_count=GetEnvironmentVariableW(L"RESKATE_BMX_LAUNCH_MODE",mode,32);
            if(mode_count!=11||std::wstring_view(mode)!=L"switch-test")return false;
            for(const auto* name:{L"RESKATE_BMX_AUTOMATED_RIDE",L"RESKATE_BMX_RAMP_TEST",L"RESKATE_BMX_RAMP_RIDE",
                L"RESKATE_BMX_CHASSIS_PROBE",L"RESKATE_BMX_CHASSIS_PROBE_PULSE",L"RESKATE_BMX_PROFILE_PERFORMANCE",
                L"RESKATE_BMX_GETUP_PULSE",L"RESKATE_BMX_GRIP_PROBE",L"RESKATE_BMX_IMPACT_RECOVERY",L"RESKATE_BMX_NATIVE_STARTUP_DELAY",L"RESKATE_BMX_PLAYER_STARTUP_REQUEST",L"RESKATE_BMX_POSTFALL_REMOUNT_TEST",L"RESKATE_BMX_INTENT_OBSERVER",
                L"RESKATE_BMX_INTENT_ACTIVATION",L"RESKATE_BMX_BINDING_OBSERVER",L"RESKATE_BMX_NATIVE_FALL_OBSERVER",
                L"RESKATE_BMX_TRICK_REHEARSAL",L"RESKATE_BMX_HANDOFF_PHASE_PROBE",L"RESKATE_BMX_FIRST_MOUNT_PRODUCER_PROBE"}) {
                wchar_t value[4]{};const auto count=GetEnvironmentVariableW(name,value,4);
                if(count&&!(count==1&&value[0]==L'0'))return false;
            }
            for(const auto* name:{L"RESKATE_BMX_CATALOG_TEST",L"RESKATE_BMX_IMPACT_TEST"}) {
                wchar_t value[2]{};if(GetEnvironmentVariableW(name,value,2))return false;
            }
            for(const auto* name:{L"RESKATE_BMX_POSE_COMPARISON",L"RESKATE_BMX_MOTION_BASIS_COMPARISON"}) {
                wchar_t value[32]{};const auto count=GetEnvironmentVariableW(name,value,32);
                if(count&&(count>=32||std::wstring_view(value)!=L"Default"))return false;
            }
            return configuration_selected(true,root->filename().wstring(),L"1",L"1",L"1",true,false,false,true);
        }catch(...){return false;}
    }();
    return selected;
#else
    return false;
#endif
}
enum class Phase : unsigned {
    idle,initial_mount,pulse,board,returned_bike,parked_near,nearby_bike,parked_carry,carried,final_bike,complete,failed
};
inline const char* phase_name(Phase phase) noexcept {
    switch(phase) {
    case Phase::idle:return "idle";case Phase::initial_mount:return "initial_mount";case Phase::pulse:return "pulse";
    case Phase::board:return "native_board";case Phase::returned_bike:return "returned_bike";
    case Phase::parked_near:return "parked_near";case Phase::nearby_bike:return "nearby_bike";
    case Phase::parked_carry:return "parked_carry";case Phase::carried:return "carried";
    case Phase::final_bike:return "final_bike";case Phase::complete:return "complete";case Phase::failed:return "failed";
    }return "invalid";
}
struct BoardTransitionEvidence {
    BikeQuickSwitchOwner request_owner;
    bool pending_for_owner{},request_clock_valid{},parked_pool_proven{},clock_unchanged{},physics_unchanged{};
    unsigned accepted_requests{};
    std::uint64_t observed_wall_ms{},pending_deadline_ms{},request_wall_ms{},request_serial{},token{};
    double request_native_time{};
    std::uintptr_t world{};
};
struct Frame {
    BikeQuickSwitchOwner owner;
    bool available{},timing_available{},offboard{},standable{},motion_supported{},physics_available{};
    bool active{},pending{},mounting{},parked{},carried{},crashed{},pool_active{},pool_parked{},capture{};
    bool pool_failed{},pool_suspended{};
    std::uint32_t physics{};
    std::uint64_t serial{},token{},applications{};
    double native_time{};
    std::uintptr_t world{};
    unsigned allocations{},mounts{},parts{},strict_setdowns{};
    BoardTransitionEvidence board_transition;
};
// The listener can be installed on the first request tick while TimeFeed is
// still empty. Only that pre-scenario clock absence may be waited out. Owner,
// physics, UI/focus and an untouched pool must already be proven on every tick.
// This class emits no action and cannot restart after any terminal result.
inline constexpr std::uint64_t preflight_limit_ms=2000;
enum class PreflightResult { waiting,qualified,rejected };
class Preflight {
    bool started_{},terminal_{},qualified_{},clock_seen_{},state_seen_{},initial_offboard_{};
    std::uint64_t wall_start_{},last_wall_{};
    unsigned samples_{};
    std::uint32_t initial_physics_{};
    BikeQuickSwitchOwner owner_{};
    Frame first_clock_{},qualified_clock_{};
    const char* issue_="not_started";
public:
    bool begin(std::uint64_t now) noexcept {
        if(started_||terminal_)return false;
        started_=true;wall_start_=last_wall_=now;issue_="waiting_for_native_clock";return true;
    }
    void fail(const char* issue) noexcept {
        if(terminal_)return;terminal_=true;qualified_=false;issue_=issue;
    }
    bool started() const noexcept{return started_;}
    bool terminal() const noexcept{return terminal_;}
    bool qualified() const noexcept{return qualified_;}
    bool clock_seen() const noexcept{return clock_seen_;}
    unsigned samples() const noexcept{return samples_;}
    std::uint64_t wall_started() const noexcept{return wall_start_;}
    std::uint64_t elapsed_ms() const noexcept{return last_wall_>=wall_start_?last_wall_-wall_start_:0;}
    const char* issue() const noexcept{return issue_;}
    const Frame& first_clock() const noexcept{return first_clock_;}
    const Frame& qualified_clock() const noexcept{return qualified_clock_;}
    PreflightResult step(const Frame& frame,bool motion_ready,bool gates,bool physical_quiet,std::uint64_t now) noexcept {
        if(!started_||terminal_)return qualified_?PreflightResult::qualified:PreflightResult::rejected;
        ++samples_;
        if(now<last_wall_||now<wall_start_){fail("preflight_wall_clock_rollback");return PreflightResult::rejected;}
        last_wall_=now;
        if(now-wall_start_>=preflight_limit_ms){fail("preflight_native_clock_timeout");return PreflightResult::rejected;}
        if(!gates){fail("preflight_focus_menu_or_lifecycle_gate");return PreflightResult::rejected;}
        if(!physical_quiet){fail("preflight_physical_input_interference");return PreflightResult::rejected;}
        if(!motion_ready){fail("preflight_native_motion_unavailable");return PreflightResult::rejected;}
        if(!frame.available){fail("preflight_native_frame_unavailable");return PreflightResult::rejected;}
        if(!frame.owner.valid()){fail("preflight_canonical_owner_unavailable");return PreflightResult::rejected;}
        if(owner_.valid()&&owner_!=frame.owner){fail("preflight_canonical_owner_changed");return PreflightResult::rejected;}
        owner_=frame.owner;
        if(!frame.physics_available){fail("preflight_native_physics_unavailable");return PreflightResult::rejected;}
        if(frame.pool_failed||frame.pool_suspended||frame.crashed){fail("preflight_pool_failure_suspension_or_fall");return PreflightResult::rejected;}
        if((frame.physics!=100&&frame.physics!=504)||(frame.offboard&&(!frame.motion_supported||frame.physics!=504))||
           (!frame.offboard&&frame.physics!=100)){fail("preflight_unsupported_native_physics_or_ragdoll");return PreflightResult::rejected;}
        if(frame.active||frame.pending||frame.mounting||frame.parked||frame.carried||frame.pool_active||frame.pool_parked||
           frame.capture||frame.allocations||frame.mounts||frame.token||frame.applications||frame.parts||frame.world||frame.strict_setdowns) {
            fail("preflight_not_an_untouched_idle_pool");return PreflightResult::rejected;
        }
        if(!state_seen_) {state_seen_=true;initial_physics_=frame.physics;initial_offboard_=frame.offboard;}
        else if(frame.physics!=initial_physics_||frame.offboard!=initial_offboard_) {
            fail("preflight_native_equipment_state_changed");return PreflightResult::rejected;
        }
        if(!std::isfinite(frame.native_time)||frame.native_time<0) {
            fail("preflight_invalid_native_clock");return PreflightResult::rejected;
        }
        if(!frame.timing_available) {
            if(clock_seen_){fail("preflight_native_clock_lost");return PreflightResult::rejected;}
            issue_="waiting_for_first_native_clock";return PreflightResult::waiting;
        }
        if(!frame.serial) {
            fail("preflight_invalid_native_clock");return PreflightResult::rejected;
        }
        if(!clock_seen_) {
            first_clock_=frame;clock_seen_=true;issue_="waiting_for_advancing_native_clock";
            return PreflightResult::waiting;
        }
        if(frame.serial<first_clock_.serial||frame.native_time<first_clock_.native_time||
           (frame.serial==first_clock_.serial)!=(frame.native_time==first_clock_.native_time)) {
            fail("preflight_native_clock_changed");return PreflightResult::rejected;
        }
        if(frame.serial==first_clock_.serial)return PreflightResult::waiting;
        qualified_clock_=frame;terminal_=qualified_=true;issue_="qualified_two_native_clock_samples";
        return PreflightResult::qualified;
    }
};
struct Output {
    bool replace{},north{},shoulder{},pulse_sample{};
    unsigned pulse_index{};
    int setup_request{};
};
struct Observation {
    Phase phase=Phase::idle;
    std::uint64_t serial{},token{},applications{};
    double native_time{};
    std::uintptr_t world{};
    unsigned mounts{},allocations{},parts{};
    bool offboard{},standable{},active{},parked{},carried{},pool_active{},pool_parked{},capture{},motion_supported{};
    std::uint32_t physics{};
    BikeQuickSwitchOwner owner;
    unsigned strict_setdowns{};
    // Preserve the actual accepted Frame flags in the compact receipt.
    bool pending{},mounting{},crashed{};
};
struct Pulse {
    unsigned index{},samples{},quick_events{},ordinary_events{},route{};
    std::uint64_t first_serial{},edge_serial{},release_serial{},last_serial{};
    double first_time{},edge_time{},release_time{},last_time{};
    bool north_capture_requested{},released{};
};
struct BoardTransitionHistory {
    unsigned samples200{},samples201{};
    Frame first200,last200,first201,last201,grounded_first,grounded_confirmed;
    bool grounded_proven{};
};
// One explicit process-local scenario, advanced only by current native clocks
// and later native state. It generates no OS/controller packet and cannot be
// selected in the ordinary riding profile. Direct setup actions are separate
// from the four LB+North pulses whose latch/routing are under test.
class Scenario {
    Phase phase_=Phase::idle,after_pulse_=Phase::idle;
    BikeQuickSwitchOwner owner_{};
    std::uintptr_t world_{};
    std::uint64_t wall_start_{},last_wall_{},last_serial_{},initial_token_{},last_mount_applications_{};
    double last_time_{},phase_start_{},hold_start_{};
    board_handoff_policy::Dwell private_board_dwell_;
    unsigned stable_{},neutral_ticks_{},release_ticks_{},pulse_count_{},observation_count_{},setup_count_{};
    bool started_{},terminal_{},passed_{},holding_{},releasing_{},reported_{};
    const char* issue_="not_started";
    std::array<Observation,16> observations_{};
    std::array<Pulse,4> pulses_{};
    std::array<int,4> setup_requests_{};
    BoardTransitionHistory board_transitions_;
    void record(const Frame& frame,Phase phase) noexcept {
        if(observation_count_>=observations_.size()){fail("observation_capacity");return;}
        observations_[observation_count_++]={phase,frame.serial,frame.token,frame.applications,frame.native_time,
            frame.world,frame.mounts,frame.allocations,frame.parts,frame.offboard,frame.standable,frame.active,
            frame.parked,frame.carried,frame.pool_active,frame.pool_parked,frame.capture,frame.motion_supported,frame.physics,frame.owner,frame.strict_setdowns,frame.pending,frame.mounting,frame.crashed};
        if(phase==Phase::initial_mount||phase==Phase::returned_bike||phase==Phase::nearby_bike||phase==Phase::final_bike)
            last_mount_applications_=frame.applications;
    }
    void enter(Phase phase,const Frame& frame) noexcept {phase_=phase;phase_start_=frame.native_time;stable_=0;}
    int setup(int request) noexcept {
        if(setup_count_>=setup_requests_.size()){fail("setup_capacity");return 0;}
        setup_requests_[setup_count_++]=request;return request;
    }
    bool mounted(const Frame& frame,unsigned expected_mounts) const noexcept {
        return frame.active&&!frame.pending&&!frame.mounting&&!frame.parked&&!frame.carried&&!frame.crashed&&
            frame.pool_active&&!frame.pool_parked&&frame.capture&&frame.physics==504&&frame.parts==8&&
            frame.offboard&&frame.mounts==expected_mounts&&frame.token==initial_token_+expected_mounts-1&&
            frame.applications>last_mount_applications_;
    }
    bool parked(const Frame& frame) const noexcept {
        return !frame.active&&!frame.pending&&!frame.mounting&&frame.parked&&!frame.carried&&!frame.crashed&&
            !frame.pool_active&&frame.pool_parked&&!frame.capture&&frame.offboard&&frame.standable&&
            frame.physics==504&&frame.parts==8;
    }
    bool board_transition_allowed(const Frame& f,std::uint64_t now) const noexcept {
        const auto& w=f.board_transition;
        return awaiting_native_board()&&(f.physics==200||f.physics==201)&&
            !f.offboard&&!f.standable&&!f.motion_supported&&!f.active&&!f.pending&&!f.mounting&&!f.parked&&!f.carried&&
            !f.crashed&&!f.capture&&f.parts==0&&f.token==0&&f.mounts==1&&f.allocations==1&&f.strict_setdowns==0&&
            !f.pool_active&&f.pool_parked&&f.world==world_&&w.world==world_&&w.token==initial_token_&&
            w.pending_for_owner&&w.request_owner==owner_&&w.request_owner==f.owner&&
            w.accepted_requests>=1&&w.accepted_requests<=64&&w.request_clock_valid&&w.parked_pool_proven&&
            w.clock_unchanged&&w.physics_unchanged&&w.observed_wall_ms==now&&w.pending_deadline_ms>=15000&&
            now>=w.pending_deadline_ms-15000&&now<w.pending_deadline_ms&&
            w.request_wall_ms>=w.pending_deadline_ms-15000&&w.request_wall_ms<=now&&
            w.request_serial>=pulses_[0].edge_serial&&w.request_serial<=f.serial&&
            std::isfinite(w.request_native_time)&&w.request_native_time>=pulses_[0].edge_time&&w.request_native_time<=f.native_time;
    }
    bool record_board_transition(const Frame& frame) noexcept {
        if(board_transitions_.samples200+board_transitions_.samples201>=1800){fail("board_transition_sample_limit");return false;}
        auto& count=frame.physics==200?board_transitions_.samples200:board_transitions_.samples201;
        auto& first=frame.physics==200?board_transitions_.first200:board_transitions_.first201;
        auto& last=frame.physics==200?board_transitions_.last200:board_transitions_.last201;
        if(!count)first=frame;
        ++count;last=frame;return true;
    }
    void pulse(const Frame& frame,Phase after) noexcept {
        if(pulse_count_>=pulses_.size()){fail("pulse_capacity");return;}
        after_pulse_=after;neutral_ticks_=release_ticks_=0;holding_=releasing_=false;
        auto& value=pulses_[pulse_count_];value.index=pulse_count_;value.first_serial=frame.serial;
        value.first_time=frame.native_time;++pulse_count_;enter(Phase::pulse,frame);
    }
public:
    void fail(const char* issue) noexcept {
        if(terminal_)return;terminal_=true;passed_=false;phase_=Phase::failed;issue_=issue;
    }
    // Diagnostic-only client opt-in. Default0 preserves the existing scenario.
    void observe_board_dwell_for_private_trace(double seconds) noexcept {
        if(!started_)private_board_dwell_.configure(seconds);
    }
    bool start(std::uint64_t now) noexcept {
        if(started_||terminal_)return false;
        started_=true;wall_start_=last_wall_=now;issue_="running";return true;
    }
    bool started() const noexcept{return started_;}
    bool terminal() const noexcept{return terminal_;}
    bool passed() const noexcept{return passed_;}
    const char* issue() const noexcept{return issue_;}
    Phase phase() const noexcept{return phase_;}
    bool report_pending() const noexcept {return terminal_&&!reported_;}
    void mark_reported() noexcept {reported_=true;}
    // Explicit private prefix result; does not claim the existing four-route pass.
    void finish_private_native_input_prefix(bool passed,const char* issue) noexcept {
        if(!awaiting_native_board()||terminal_)return;
        terminal_=true;passed_=passed;phase_=passed?Phase::complete:Phase::failed;issue_=issue;
    }
    const auto& observations() const noexcept{return observations_;}
    unsigned observation_count() const noexcept{return observation_count_;}
    const auto& pulses() const noexcept{return pulses_;}
    unsigned pulse_count() const noexcept{return pulse_count_;}
    bool awaiting_native_board() const noexcept {
        return phase_==Phase::board&&pulse_count_==1&&pulses_[0].released&&pulses_[0].quick_events==1&&
            pulses_[0].ordinary_events==0&&pulses_[0].route==4;
    }
    const BoardTransitionHistory& board_transitions() const noexcept{return board_transitions_;}
    const auto& setup_requests() const noexcept{return setup_requests_;}
    unsigned setup_count() const noexcept{return setup_count_;}
    Output step(const Frame& frame,bool gates,bool physical_quiet,std::uint64_t wall_now) noexcept {
        Output out;out.replace=started_;
        if(!started_||terminal_)return out;
        if(!gates||!physical_quiet){fail(gates?"physical_input_interference":"focus_menu_or_lifecycle_gate");return out;}
        if(wall_now<last_wall_||wall_now<wall_start_||wall_now-wall_start_>=wall_limit_ms){fail("wall_deadline_or_rollback");return out;}
        last_wall_=wall_now;
        if(!frame.available||!frame.timing_available||!frame.physics_available||!frame.owner.valid()||
           !frame.serial||!std::isfinite(frame.native_time)||frame.native_time<0||frame.pool_failed||
           frame.pool_suspended||frame.crashed){fail("unavailable_native_observation_or_fall");return out;}
        const bool board_transition=(frame.physics==200||frame.physics==201)&&board_transition_allowed(frame,wall_now);
        if(!board_transition&&((frame.physics!=100&&frame.physics!=504)||(frame.offboard&&(!frame.motion_supported||frame.physics!=504))||
           (!frame.offboard&&frame.physics!=100))){fail("unsupported_native_physics_or_ragdoll");return out;}
        if(owner_.valid()&&owner_!=frame.owner){fail("native_owner_changed");return out;}
        owner_=frame.owner;
        if(last_serial_&&(frame.serial<last_serial_||frame.native_time<last_time_||
            (frame.serial==last_serial_)!=(frame.native_time==last_time_))){fail("native_clock_changed");return out;}
        const bool duplicate=frame.serial==last_serial_;
        if(world_&&(frame.world!=world_||frame.allocations!=1)){fail("native_pool_or_world_changed");return out;}
        if(board_transition) {
            private_board_dwell_.interrupt();
            stable_=0;board_transitions_.grounded_first={};board_transitions_.grounded_confirmed={};board_transitions_.grounded_proven=false;
        } // Even a duplicate observed boarding state interrupts grounded stability.
        if(duplicate) {
            out.north=holding_&&!releasing_;out.shoulder=out.north;
            out.pulse_sample=phase_==Phase::pulse;out.pulse_index=pulse_count_?pulse_count_-1:0;return out;
        }
        last_serial_=frame.serial;last_time_=frame.native_time;
        if(phase_!=Phase::idle&&frame.native_time-phase_start_>phase_limit_seconds){fail("native_phase_timeout");return out;}
        if(board_transition) {
            stable_=0; // 100 -> 201 -> 100 is not two consecutive grounded samples.
            (void)record_board_transition(frame);return out; // neutral; no new setup or pulse
        }
        if(phase_==Phase::idle){record(frame,Phase::idle);enter(Phase::initial_mount,frame);out.setup_request=setup(1);return out;}
        if(phase_==Phase::pulse) {
            out.pulse_sample=true;out.pulse_index=pulse_count_-1;
            auto& value=pulses_[out.pulse_index];++value.samples;value.last_serial=frame.serial;value.last_time=frame.native_time;
            if(neutral_ticks_<2){++neutral_ticks_;return out;}
            if(!holding_&&!releasing_){holding_=true;hold_start_=frame.native_time;value.edge_serial=frame.serial;value.edge_time=frame.native_time;}
            if(holding_&&frame.native_time-hold_start_>.35){fail("native_hold_overrun");return out;}
            if(holding_&&frame.native_time-hold_start_>=hold_seconds){holding_=false;releasing_=true;value.release_serial=frame.serial;value.release_time=frame.native_time;}
            if(holding_){out.north=out.shoulder=true;return out;}
            if(releasing_&&++release_ticks_>=2) {
                value.released=true;
                if(value.quick_events!=1||value.ordinary_events){fail("pulse_not_one_shot_or_ordinary_leak");return out;}
                enter(after_pulse_,frame);
            }
            return out;
        }
        bool accept=false;
        switch(phase_) {
        case Phase::initial_mount:
            if(!initial_token_&&frame.active&&frame.world&&frame.token&&frame.token<=UINT64_MAX-3&&frame.allocations==1&&frame.mounts==1) {
                initial_token_=frame.token;world_=frame.world;
            }
            accept=initial_token_&&mounted(frame,1);break;
        case Phase::board:
            accept=!frame.active&&!frame.pending&&!frame.parked&&!frame.carried&&!frame.crashed&&frame.physics==100&&
                !frame.offboard&&frame.parts==0&&!frame.capture&&!frame.pool_active&&frame.pool_parked&&frame.mounts==1;break;
        case Phase::returned_bike:accept=mounted(frame,2);break;
        case Phase::parked_near:accept=parked(frame)&&frame.mounts==2&&frame.token==initial_token_+1;break;
        case Phase::nearby_bike:accept=mounted(frame,3);break;
        case Phase::parked_carry:accept=parked(frame)&&frame.mounts==3&&frame.token==initial_token_+2;break;
        case Phase::carried:
            accept=!frame.active&&!frame.pending&&frame.parked&&frame.carried&&!frame.crashed&&!frame.pool_active&&
                frame.pool_parked&&!frame.capture&&frame.physics==504&&frame.offboard&&frame.standable&&frame.parts==8&&
                frame.mounts==3&&frame.token==initial_token_+2;break;
        case Phase::final_bike:accept=mounted(frame,4)&&frame.strict_setdowns==1;break;
        default:fail("invalid_scenario_phase");return out;
        }
        if(phase_==Phase::board) {
            if(!accept)private_board_dwell_.interrupt();
            if(!accept) {board_transitions_.grounded_first={};board_transitions_.grounded_confirmed={};board_transitions_.grounded_proven=false;}
            else if(!stable_) {board_transitions_.grounded_first=frame;
                (void)private_board_dwell_.observe(true,{frame.serial,frame.native_time},wall_now);}
            else {board_transitions_.grounded_confirmed=frame;board_transitions_.grounded_proven=true;}
        }
        stable_=accept?stable_+1:0;
        if(stable_<2)return out;
        // Keep all rev4 transition/unsupported-state guards unchanged. This only
        // delays the next existing synthetic pulse after accepted board100.
        if(phase_==Phase::board&&!private_board_dwell_.observe(true,{frame.serial,frame.native_time},wall_now))return out;
        record(frame,phase_);
        switch(phase_) {
        case Phase::initial_mount:pulse(frame,Phase::board);break;
        case Phase::board:pulse(frame,Phase::returned_bike);break;
        case Phase::returned_bike:enter(Phase::parked_near,frame);out.setup_request=setup(7);break;
        case Phase::parked_near:pulse(frame,Phase::nearby_bike);break;
        case Phase::nearby_bike:enter(Phase::parked_carry,frame);out.setup_request=setup(7);break;
        case Phase::parked_carry:enter(Phase::carried,frame);out.setup_request=setup(8);break;
        case Phase::carried:pulse(frame,Phase::final_bike);break;
        case Phase::final_bike:
            terminal_=passed_=true;phase_=Phase::complete;issue_="four_native_quick_switch_routes_completed";break;
        default:fail("invalid_transition");break;
        }
        return out;
    }
    void feedback(const Output& output,const BikeQuickSwitchEvent& quick,BikeInteraction ordinary,unsigned routed_request) noexcept {
        if(!started_||terminal_)return;
        if(ordinary!=BikeInteraction::none){
            if(output.pulse_sample&&output.pulse_index<pulse_count_)++pulses_[output.pulse_index].ordinary_events;
            fail("ordinary_y_leaked_from_virtual_chord");return;
        }
        if(!output.pulse_sample) {
            if(quick.pressed)fail("unexpected_quick_event_outside_pulse");return;
        }
        if(output.pulse_index>=pulse_count_){fail("invalid_pulse_feedback");return;}
        auto& value=pulses_[output.pulse_index];value.north_capture_requested=value.north_capture_requested||quick.capture_north;
        if(quick.pressed) {
            ++value.quick_events;value.route=routed_request;
            const unsigned expected=output.pulse_index==0?4:output.pulse_index==1?1:7;
            if(value.quick_events!=1||routed_request!=expected||!quick.suppress_north||!quick.capture_north)
                fail("unexpected_or_repeated_quick_route");
        }
    }
};
}
