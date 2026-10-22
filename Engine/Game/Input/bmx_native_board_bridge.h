#pragma once
#include <cstdint>
#include <limits>

// Private XInput-only experiment. This policy never writes native game state.
namespace dingosdk::input::native_board_bridge {
inline constexpr std::uint64_t preparation_ms=30000,transaction_ms=250;
inline constexpr std::uint64_t baseline_ms=20,press_end_ms=180,minimum_release_ms=20,max_poll_gap_ms=120;
enum class ReadinessReason:unsigned { not_evaluated,ready,bridge_not_latched,bridge_flag_changed,native_chassis_flag_changed,launch_mode_changed,overlay_not_running,window_unavailable,foreground_lost,escape_held,trace_not_initialized,trace_scope_revalidation_failed,trace_not_native_only,trace_automated_control,trace_deferred_arm_disabled,trace_sealed,owner_unavailable,owner_validation_failed,owner_protected,owner_chain_changed,owner_changed,owner_revalidation_failed,owner_revalidation_protected,owner_revalidation_changed,physics_unavailable,trace_busy,client_row_missing,client_row_stale,client_row_clock_future,client_snapshot_missing,client_snapshot_stale,client_snapshot_clock_future,bmx_activity_or_lease,client_gameplay_false,client_not_solo,client_focus_false,waiting_observer_arm,waiting_menu,waiting_native_input,waiting_capture,count };
inline const char* name(ReadinessReason reason) noexcept {
    switch(reason){
    case ReadinessReason::not_evaluated:return "not_evaluated";
    case ReadinessReason::ready:return "ready";
    case ReadinessReason::bridge_not_latched:return "bridge_not_latched";
    case ReadinessReason::bridge_flag_changed:return "bridge_flag_changed";
    case ReadinessReason::native_chassis_flag_changed:return "native_chassis_flag_changed";
    case ReadinessReason::launch_mode_changed:return "launch_mode_changed";
    case ReadinessReason::overlay_not_running:return "overlay_not_running";
    case ReadinessReason::window_unavailable:return "window_unavailable";
    case ReadinessReason::foreground_lost:return "foreground_lost";
    case ReadinessReason::escape_held:return "escape_held";
    case ReadinessReason::trace_not_initialized:return "trace_not_initialized";
    case ReadinessReason::trace_scope_revalidation_failed:return "trace_scope_revalidation_failed";
    case ReadinessReason::trace_not_native_only:return "trace_not_native_only";
    case ReadinessReason::trace_automated_control:return "trace_automated_control";
    case ReadinessReason::trace_deferred_arm_disabled:return "trace_deferred_arm_disabled";
    case ReadinessReason::trace_sealed:return "trace_sealed";
    case ReadinessReason::owner_unavailable:return "owner_unavailable";
    case ReadinessReason::owner_validation_failed:return "owner_validation_failed";
    case ReadinessReason::owner_protected:return "owner_protected";
    case ReadinessReason::owner_chain_changed:return "owner_chain_changed";
    case ReadinessReason::owner_changed:return "owner_changed";
    case ReadinessReason::owner_revalidation_failed:return "owner_revalidation_failed";
    case ReadinessReason::owner_revalidation_protected:return "owner_revalidation_protected";
    case ReadinessReason::owner_revalidation_changed:return "owner_revalidation_changed";
    case ReadinessReason::physics_unavailable:return "physics_unavailable";
    case ReadinessReason::trace_busy:return "trace_busy";
    case ReadinessReason::client_row_missing:return "client_row_missing";
    case ReadinessReason::client_row_stale:return "client_row_stale";
    case ReadinessReason::client_row_clock_future:return "client_row_clock_future";
    case ReadinessReason::client_snapshot_missing:return "client_snapshot_missing";
    case ReadinessReason::client_snapshot_stale:return "client_snapshot_stale";
    case ReadinessReason::client_snapshot_clock_future:return "client_snapshot_clock_future";
    case ReadinessReason::bmx_activity_or_lease:return "bmx_activity_or_lease";
    case ReadinessReason::client_gameplay_false:return "client_gameplay_false";
    case ReadinessReason::client_not_solo:return "client_not_solo";
    case ReadinessReason::client_focus_false:return "client_focus_false";
    case ReadinessReason::waiting_observer_arm:return "waiting_observer_arm";
    case ReadinessReason::waiting_menu:return "waiting_menu";
    case ReadinessReason::waiting_native_input:return "waiting_native_input";
    case ReadinessReason::waiting_capture:return "waiting_capture";
    case ReadinessReason::count:break;
    }return "invalid_readiness_reason";
}
inline bool preparation_retryable(ReadinessReason reason) noexcept {
    return reason==ReadinessReason::trace_busy||reason==ReadinessReason::client_row_missing||
        reason==ReadinessReason::client_row_stale||reason==ReadinessReason::client_snapshot_missing||
        reason==ReadinessReason::client_snapshot_stale;
}

enum class Phase:unsigned {idle,waiting,baseline,press,release,complete,aborted};
enum class Direction:unsigned {board_mount,board_dismount};
struct Result {bool rewrite{},north{},phase_changed{};Phase phase{};};
struct Pad {
    std::uint16_t buttons{};std::uint8_t left_trigger{},right_trigger{};
    std::int16_t left_x{},left_y{},right_x{},right_y{};
    bool operator==(const Pad&) const = default;
};
// The counter domain cannot safely be returned to the raw counter: it might
// equal the last synthetic press packet and hide release from a caching reader.
// Once activated, preserve this private domain through the process lifetime.
// Full final-pad equality also covers filter changes without a raw packet bump.
struct PacketContinuity {
    bool active{};std::uint32_t packet{},last_original{};Pad last_pad{};
    std::uint32_t translate(std::uint32_t original,const Pad& final_pad) noexcept {
        if(!active){active=true;packet=original+1;}
        else if(original!=last_original||final_pad!=last_pad)++packet;
        last_original=original;last_pad=final_pad;return packet;
    }
};
struct Pulse {
    Phase phase=Phase::idle;
    Direction direction=Direction::board_mount;
    std::uint64_t queued{},started{},last{},last_poll{},release_started{};
    std::uint32_t slot{};
    unsigned rewrites{},press_reports{},release_reports{};
    bool used{},begun{},baseline_seen{},release_seen{};
    const char* issue="not_requested";
    bool queue(std::uint64_t now,unsigned user,Direction requested=Direction::board_mount) noexcept {
        if(used||user>3||now>std::numeric_limits<std::uint64_t>::max()-preparation_ms)return false;
        used=true;direction=requested;queued=last=now;slot=user;phase=Phase::waiting;issue="waiting_for_armed_untouched_baseline";return true;
    }
    void abort(const char* reason) noexcept {phase=Phase::aborted;issue=reason;}
    bool live() const noexcept {return phase==Phase::waiting||phase==Phase::baseline||phase==Phase::press||phase==Phase::release;}
    Result poll(std::uint64_t now,unsigned user,bool identity_ok,
                bool focused,bool ready,bool quiet,std::uint32_t physics,
                bool transient_before_start=false,const char* readiness_issue="owner_configuration_or_focus_changed") noexcept {
        Result out;out.phase=phase;
        if(!live())return out;
        if(now<last){abort("clock_reversed");out.phase=phase;return out;}last=now;
        if(!begun&&now-queued>=preparation_ms){abort("preparation_expired");out.phase=phase;return out;}
        if(!identity_ok||!focused){
            // Only explicitly classified unavailable observer reads may wait,
            // and only before the first offer. Deadlines are never restarted.
            if(!begun&&focused&&transient_before_start){issue=readiness_issue;return out;}
            abort(readiness_issue);out.phase=phase;return out;
        }
        if(user!=slot)return out;
        if(!ready){if(begun)abort(readiness_issue);else issue=readiness_issue;out.phase=phase;return out;}
        if(!quiet){abort("physical_controls_not_quiet");out.phase=phase;return out;}
        if(!begun){
            if(direction==Direction::board_mount?physics!=504:physics!=100&&physics!=103){
                abort(direction==Direction::board_mount?"initial_state_not_offboard504":"initial_state_not_native_board100_or103");out.phase=phase;return out;
            }
            begun=true;started=last_poll=now;
        }else if(now-last_poll>max_poll_gap_ms){abort("poll_stalled");out.phase=phase;return out;}
        last_poll=now;
        const auto elapsed=now-started;
        if(elapsed>=transaction_ms){abort("transaction_expired");out.phase=phase;return out;}
        const bool initial_state=direction==Direction::board_mount?physics==504:physics==100||physics==103;
        const bool target_state=direction==Direction::board_mount?physics==103||physics==100:physics==504;
        if(!initial_state&&!(press_reports&&target_state)){abort("unexpected_native_state");out.phase=phase;return out;}
        // Private experimental timing: reserve enough time before the hard
        //250ms cap for a release actually observed by a successful game poll.
        Phase next=elapsed<baseline_ms?Phase::baseline:elapsed<press_end_ms?Phase::press:Phase::release;
        if((!initial_state||phase==Phase::release)&&next==Phase::press)next=Phase::release;
        if(next==Phase::press&&!baseline_seen){abort("fresh_release_not_observed");out.phase=phase;return out;}
        if(next==Phase::release&&release_seen&&now-release_started>=minimum_release_ms){
            if(!press_reports||!release_seen){abort("press_or_release_not_observed");out.phase=phase;return out;}
            phase=Phase::complete;issue="report_sequence_complete_not_native_consumption";out.phase=phase;return out;
        }
        out.phase_changed=next!=phase;
        phase=next;
        out.rewrite=true;out.north=next==Phase::press;out.phase=phase;
        ++rewrites;
        if(next==Phase::baseline)baseline_seen=true;
        if(next==Phase::press)++press_reports;
        if(next==Phase::release){
            if(!release_seen)release_started=now;
            release_seen=true;++release_reports;
        }
        issue="game_facing_report_rewritten_not_consumption";return out;
    }
};
} // namespace dingosdk::input::native_board_bridge
