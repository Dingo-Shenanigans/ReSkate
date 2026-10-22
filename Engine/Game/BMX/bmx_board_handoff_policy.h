#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace dingosdk::bmx::board_handoff_policy {
inline constexpr std::size_t capacity=512;
inline constexpr std::uint64_t wall_limit_ms=8000,sample_interval_ms=50,prepare_limit_ms=3000;
inline constexpr double trace_dwell_seconds=2.25,driver_dwell_seconds=2.5;
// Result of a one-character Win32 read, with LastError cleared before it.
// Missing/defined-empty options are neutral; every nonempty option conflicts.
inline bool absent_or_empty_option(std::uint32_t copied,std::uint32_t error,std::uint32_t missing_error) noexcept {
    return copied==0&&(error==0||error==missing_error);
}
enum class Mode:unsigned {disabled,quick_switch,native_only};
// Private deferred passive console arming only. Async preparation and every
// observation/gameplay acceptance window retain their original budgets.
inline constexpr std::uint64_t deferred_passive_prearm_limit_ms=30000;
inline bool deferred_passive_arm_allowed(Mode mode,bool automated_native_board,int flag) noexcept {
    return flag==0||(flag==1&&mode==Mode::native_only&&!automated_native_board);
}
inline std::uint64_t native_only_prearm_limit_ms(Mode mode,bool automated_native_board,bool deferred) noexcept {
    return deferred&&mode==Mode::native_only&&!automated_native_board?deferred_passive_prearm_limit_ms:prepare_limit_ms;
}
inline bool native_only_prearm_expired(std::uint64_t requested,std::uint64_t now,Mode mode,bool automated_native_board,bool deferred) noexcept {
    return now<requested||now-requested>=native_only_prearm_limit_ms(mode,automated_native_board,deferred);
}
enum class Kind:unsigned {client,skater_published,board_published};
struct Selection {
    bool explicit_flag{},exact_root{},supported_image{},native_chassis{},conflicts_clear{};
    bool quick_switch{},automated_switch{};
    std::wstring_view requested_mode,launch_mode;
    bool automated_native_board{};
};
inline Mode select(const Selection& s) noexcept {
    if(!s.explicit_flag||!s.exact_root||!s.supported_image||!s.native_chassis||!s.conflicts_clear)return Mode::disabled;
    if(s.automated_native_board&&s.requested_mode!=L"native-only")return Mode::disabled;
    if(s.requested_mode==L"quick-switch"&&s.launch_mode==L"switch-test"&&s.quick_switch&&s.automated_switch)return Mode::quick_switch;
    if(s.requested_mode==L"native-only"&&s.launch_mode==L"human-ride"&&!s.quick_switch&&!s.automated_switch)return Mode::native_only;
    return Mode::disabled;
}
inline bool native_control_standing(std::uint32_t physics,bool offboard,bool standable,bool motion_supported) noexcept {
    return physics==504&&offboard&&standable&&motion_supported;
}
inline bool native_control_untouched(std::uint64_t motion,std::uint64_t pose,std::uint64_t render) noexcept {
    return !motion&&!pose&&!render;
}
inline bool native_board_input_neutral(const std::array<float,8>& axes,unsigned buttons,bool hop,bool dismount,bool shoulder_left,bool shoulder_right) noexcept {
    if(buttons||hop||dismount||shoulder_left||shoulder_right)return false;
    for(const auto axis:axes)if(!std::isfinite(axis)||std::abs(axis)>.05f)return false;
    return true;
}
using Owner=std::array<std::uintptr_t,6>;
struct Clock {std::uint64_t serial{};double time{};};
// Only the explicitly selected private post-BMX terminal observation may
// request one exact client row between normal50ms retained samples.
struct TerminalClientTarget {
    bool requested{};Owner owner{};Clock clock{};std::uint32_t physics{};
    std::uint64_t sampled_ms{},deadline_ms{};
};
enum class TerminalClientAction {ordinary,force,existing,refused};
struct TerminalClientRetention {
    bool attempted{},retained{},cadence_pending{};const char* issue="not_requested";
    TerminalClientAction observe(const TerminalClientTarget& t,Mode mode,bool armed,bool sealed,bool gates,
        const Owner& owner,Clock clock,std::uint32_t physics,std::uint64_t wall,bool already_retained=false) noexcept {
        if(!t.requested||attempted)return TerminalClientAction::ordinary;
        attempted=true;issue="private_terminal_client_refused";
        if(mode!=Mode::quick_switch||!armed||sealed||!gates)return TerminalClientAction::refused;
        for(auto part:t.owner)if(!part)return TerminalClientAction::refused;
        if(owner!=t.owner||!clock.serial||clock.serial!=t.clock.serial||!std::isfinite(clock.time)||clock.time<0||
           clock.time!=t.clock.time||physics!=100||t.physics!=100||!t.sampled_ms||wall<t.sampled_ms||
           wall-t.sampled_ms>250||wall>=t.deadline_ms||t.deadline_ms<=t.sampled_ms||t.deadline_ms-t.sampled_ms>15000)
            return TerminalClientAction::refused;
        if(already_retained){retained=true;issue="exact_terminal_client_already_retained";return TerminalClientAction::existing;}
        cadence_pending=true;issue="exact_terminal_client_cadence_request";return TerminalClientAction::force;
    }
    void complete() noexcept {if(!cadence_pending)return;cadence_pending=false;retained=true;issue="exact_terminal_client_retained";}
};
enum class Result {ignored,throttled,admitted,configuration_changed,read_failure,owner_changed,clock_changed,wall_exhausted,capacity_exhausted,invalid_kind};
inline bool failed(Result r) noexcept {return r!=Result::ignored&&r!=Result::throttled&&r!=Result::admitted;}
inline const char* name(Result r) noexcept {
    switch(r){case Result::configuration_changed:return "private_configuration_changed";case Result::read_failure:return "required_read_unavailable";
    case Result::owner_changed:return "owner_changed";case Result::clock_changed:return "native_clock_changed";
    case Result::wall_exhausted:return "wall_window_exhausted";case Result::capacity_exhausted:return "row_capacity_exhausted";
    case Result::invalid_kind:return "invalid_row_kind";default:return "not_failed";}
}
struct Window {
    bool armed{},ever_armed{},sealed{};Result stop=Result::ignored;
    std::uint64_t started{},last_observed_wall{};Owner pinned{};Clock last{};std::size_t count{};
    std::array<std::uint64_t,3> last_wall{},last_serial{};
    bool arm(std::uint64_t now) noexcept {
        if(ever_armed)return false;ever_armed=armed=true;started=now;return true;
    }
    Result fail(Result reason) noexcept {sealed=true;stop=reason;return reason;}
    Result before_read(std::uint64_t now,bool configured=true) noexcept {
        if(!armed||sealed)return Result::ignored;
        if(!configured)return fail(Result::configuration_changed);
        if(now<started||now<last_observed_wall||now-started>=wall_limit_ms)return fail(Result::wall_exhausted);
        if(count>=capacity)return fail(Result::capacity_exhausted);
        return Result::admitted;
    }
    Result observe(Kind kind,const Owner& owner,Clock clock,std::uint64_t now,bool configured=true,bool reads=true,bool terminal_client=false) noexcept {
        if(!armed||sealed)return Result::ignored;
        if(!configured)return fail(Result::configuration_changed);
        if(!reads)return fail(Result::read_failure);
        if(now<started||now<last_observed_wall||now-started>=wall_limit_ms)return fail(Result::wall_exhausted);
        for(auto part:owner)if(!part)return fail(Result::read_failure);
        if(pinned[2]&&pinned!=owner)return fail(Result::owner_changed);
        if(!clock.serial||!std::isfinite(clock.time)||clock.time<0)return fail(Result::read_failure);
        if(last.serial&&(clock.serial<last.serial||clock.time<last.time||
            ((clock.serial==last.serial)!=(clock.time==last.time))))return fail(Result::clock_changed);
        const auto k=static_cast<unsigned>(kind);if(k>=3)return fail(Result::invalid_kind);
        pinned=owner;last=clock;last_observed_wall=now;
        if(clock.serial==last_serial[k]||(!(terminal_client&&kind==Kind::client)&&now<last_wall[k]+sample_interval_ms))return Result::throttled;
        if(count>=capacity)return fail(Result::capacity_exhausted);
        return Result::admitted;
    }
    void retain(Kind kind,Clock clock,std::uint64_t now) noexcept {
        ++count;const auto k=static_cast<unsigned>(kind);last_serial[k]=clock.serial;last_wall[k]=now;
    }
};
// Zero duration preserves the original two-observation driver schedule. The
// caller still owns its native guards and invokes interrupt on transitional200/201.
struct Dwell {
    double seconds{},first_native{},last_native{};std::uint64_t first_wall{},last_wall{},last_serial{};bool started{};
    void configure(double value) noexcept {if(std::isfinite(value)&&value>=2&&value<=3)seconds=value;}
    void interrupt() noexcept {started=false;first_native=last_native=0;first_wall=last_wall=last_serial=0;}
    bool observe(bool accepted,Clock clock,std::uint64_t wall) noexcept {
        if(seconds==0)return accepted;
        if(!accepted||!clock.serial||!std::isfinite(clock.time)||clock.time<0){interrupt();return false;}
        if(started&&(clock.serial<last_serial||clock.time<last_native||wall<last_wall||
           ((clock.serial==last_serial)!=(clock.time==last_native)))){interrupt();return false;}
        if(!started){started=true;first_native=clock.time;first_wall=wall;}
        last_serial=clock.serial;last_native=clock.time;last_wall=wall;
        return clock.serial!=0&&clock.time-first_native>=seconds&&wall-first_wall>=static_cast<std::uint64_t>(seconds*1000);
    }
};
// Close the observer only after EACH independently sampled CPU publication
// kind meets the unchanged native and wall duration, not just the client kind.
struct PublicationDwell {
    std::array<Dwell,3> streams{};std::array<bool,3> ready{};
    void configure(double seconds) noexcept {for(auto& stream:streams)stream.configure(seconds);}
    void interrupt() noexcept {for(auto& stream:streams)stream.interrupt();ready={};}
    bool observe(Kind kind,bool accepted,Clock clock,std::uint64_t wall) noexcept {
        const auto index=static_cast<std::size_t>(kind);
        if(index>=streams.size()||!accepted){interrupt();return false;}
        ready[index]=streams[index].observe(true,clock,wall);
        return ready[0]&&ready[1]&&ready[2];
    }
};
// An explicit ACTIVE software native-board request, separate from the passive
// observer and from ordinary Y/controller input. No retries or motion/pose work.
enum class NativeBoardAction {wait,request,observe,failed};
struct NativeBoardControl {
    bool baseline_ready{},attempted{},accepted{},failed{},saw_board{};
    Owner owner{};Clock baseline{},requested{};std::uint64_t baseline_wall{},requested_wall{};
    const char* issue="not_requested";
    NativeBoardAction fail(const char* why) noexcept {failed=true;issue=why;return NativeBoardAction::failed;}
    NativeBoardAction observe(const Owner& current,Clock clock,std::uint64_t wall,std::uint32_t physics,
        bool offboard,bool standable,bool motion_supported,bool untouched_and_guarded) noexcept {
        if(failed)return NativeBoardAction::failed;
        if(!untouched_and_guarded)return fail("native_board_control_not_untouched_or_guarded");
        for(auto part:current)if(!part)return fail("native_board_control_owner_unavailable");
        if(!clock.serial||!std::isfinite(clock.time)||clock.time<0)return fail("native_board_control_clock_unavailable");
        if(!baseline_ready) {
            if(!native_control_standing(physics,offboard,standable,motion_supported))return fail("native_board_control_baseline_not_standing");
            baseline_ready=true;owner=current;baseline=clock;baseline_wall=wall;issue="baseline_retained_waiting_fresh_clock";
            return NativeBoardAction::wait;
        }
        if(current!=owner)return fail("native_board_control_owner_changed");
        if(wall<baseline_wall||clock.serial<baseline.serial||clock.time<baseline.time||
           ((clock.serial==baseline.serial)!=(clock.time==baseline.time)))return fail("native_board_control_clock_changed");
        if(!attempted) {
            if(!native_control_standing(physics,offboard,standable,motion_supported))return fail("native_board_control_request_not_standing");
            if(clock.serial==baseline.serial)return NativeBoardAction::wait;
            attempted=true;requested=clock;requested_wall=wall;issue="native_board_request_attempted";
            return NativeBoardAction::request;
        }
        if(!accepted)return fail("native_board_control_request_rejected");
        if(clock.serial<requested.serial||clock.time<requested.time||wall<requested_wall)
            return fail("native_board_control_request_clock_changed");
        if(physics==100&&!offboard){saw_board=true;issue="native_board100_observed_not_pose_pass";return NativeBoardAction::observe;}
        if(!saw_board&&(physics==200||physics==201)&&!offboard){issue="native_board_transition_observed";return NativeBoardAction::observe;}
        // The existing selector request expires after250ms. This does not renew
        // it or infer consumption: a still-standing unconsumed request fails.
        if(!saw_board&&native_control_standing(physics,offboard,standable,motion_supported)&&
           wall-requested_wall<250&&clock.time-requested.time<.5)return NativeBoardAction::observe;
        return fail("native_board_control_unexpected_native_state_or_request_expired");
    }
    bool complete_request(bool value) noexcept {
        if(!attempted||accepted||failed)return false;
        accepted=value;if(!value)fail("native_board_control_request_rejected");return value;
    }
};

enum class Preparation {pending,ready,timed_out};
inline Preparation preparation(bool ready,std::uint64_t started,std::uint64_t now) noexcept {
    if(now<started||now-started>=prepare_limit_ms)return Preparation::timed_out;
    return ready?Preparation::ready:Preparation::pending;
}
template<class Observe,class Failure>struct ObserverExit {
    Observe observe;Failure failure;
    ~ObserverExit() noexcept {try{observe();}catch(...){failure();}}
};
template<class Observe,class Failure>ObserverExit(Observe,Failure)->ObserverExit<Observe,Failure>;
} // namespace dingosdk::bmx::board_handoff_policy
