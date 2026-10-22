#include "Engine/Core/Log/logging.h"
#include "overlay_internal.h"
#include "input_capture.h"
#include "held_input.h"
#include "Engine/Game/Input/playstation_report.h"
#include "Engine/Game/Input/bmx_input_sample.h"
#include "Engine/Game/Input/bmx_native_board_bridge.h"
#include "Extension/BMX/bmx_board_handoff_trace.h"
#include "cursor.h"
#include <cstdlib>
#include <hidsdi.h>
#include <atomic>
#include <format>
#include <array>
#include <cstring>
#include <string>
#include <mutex>
#include <set>
#include <span>
#include <tuple>

namespace dingosdk::overlay::detail {
// Window messages do not cover the game's independently polled input devices.
// Only the overlay's own Win32 backend and binding reader may bypass these gates.
thread_local HRAWINPUT noted_raw_input = nullptr;
namespace {
std::atomic<unsigned> prompt_xinput{}, prompt_direct{}, prompt_hid{}, prompt_raw{};
std::atomic<std::uint64_t> bmx_capture_until{};
std::atomic<std::uint64_t> bmx_interaction_until{};
std::atomic<std::uint64_t> bmx_quick_switch_until{};
// A read the game made while a card held its input: counted, by the way it read.
void note_prompt_read(std::atomic<unsigned>& count) {
    if (!overlay_input_access && state().prompt_input_active.load(std::memory_order_relaxed)) count.fetch_add(1, std::memory_order_relaxed);
}
}
PromptReads take_prompt_reads() noexcept {
    return {prompt_xinput.exchange(0), 0, prompt_direct.exchange(0), prompt_hid.exchange(0), prompt_raw.exchange(0)};
}
input::BmxNativeCapture polled_capture() {
    const auto error = GetLastError();
    const auto capture = input::bmx_native_capture(overlay_input_access != 0, owns_menu_cursor(state()),
        state().freecam_controller_active.load(std::memory_order_relaxed),
        state().prompt_input_active.load(std::memory_order_relaxed), bmx_input_capture_active(),
        bmx_interaction_capture_active());
    SetLastError(error);
    return capture;
}
// BEGIN PRIVATE NATIVE BOARD INPUT BRIDGE
namespace bridge_policy = input::native_board_bridge;
struct PrivateBoardInputReport {
    std::uint64_t wall{},ordinal{};unsigned phase{};DWORD user{},before_packet{},after_packet{};
    bridge_policy::Pad before_pad{},after_pad{};std::uint32_t physics{};
    bool pulse_offer{},packet_translated{},terminal{},physics_observed{};
};
struct PrivateBoardReadinessCount {std::uint64_t count{},deferred{},first_ms{},last_ms{};};
struct PrivateBoardInputBridge {
    std::atomic<bool> bound{},requested{};
    SRWLOCK lock=SRWLOCK_INIT;
    bridge_policy::Pulse pulse;
    std::array<PrivateBoardReadinessCount,static_cast<unsigned>(bridge_policy::ReadinessReason::count)> readiness_counts{};
    bridge_policy::ReadinessReason last_readiness=bridge_policy::ReadinessReason::not_evaluated;
    const char* last_readiness_stage="not_evaluated";
    bool last_readiness_identity{},last_readiness_focused{},last_readiness_ready{},last_readiness_armable{};
    std::atomic<std::uint64_t> queue_lock_refusals{},queue_last_attempt_ms{};
    std::atomic<const char*> queue_issue{"not_requested"};
    bridge_policy::PacketContinuity packets;
    std::array<std::uintptr_t,6> owner{};
    static constexpr std::size_t pulse_report_capacity=128;
    // One extra reserved row records the first actual terminal return, even
    // when the pulse ledger itself filled. Later physical returns are counted
    // and the latest is retained; no unbounded full-session ledger is claimed.
    std::array<PrivateBoardInputReport,pulse_report_capacity+1> reports{};std::size_t count{};
    std::uint64_t translated_returns{},terminal_returns{};
    bool terminal_emitted{};PrivateBoardInputReport last_terminal{};
};
PrivateBoardInputBridge& private_board_input_bridge() {static PrivateBoardInputBridge value;return value;}
bool bridge_environment_one(const wchar_t* name) noexcept {
    wchar_t value[2]{};return GetEnvironmentVariableW(name,value,2)==1&&value[0]==L'1';
}
bool bridge_environment_exact(const wchar_t* name,std::wstring_view expected) noexcept {
    wchar_t value[32]{};const auto n=GetEnvironmentVariableW(name,value,32);
    return n<32&&std::wstring_view(value,n)==expected;
}
void bind_private_bmx_native_input_handoff() noexcept;
void bind_human_native_handoff() noexcept;
void bind_private_board_input_bridge() noexcept {
    bind_private_bmx_native_input_handoff();
    bind_human_native_handoff();
    auto& bridge=private_board_input_bridge();
    if(!bridge.bound.exchange(true,std::memory_order_acq_rel))
        bridge.requested.store(bridge_environment_one(L"RESKATE_BMX_NATIVE_BOARD_INPUT_BRIDGE"),std::memory_order_release);
}
struct BridgeReadiness {
    bool identity{},focused{},ready{},armable{},physics_available{};std::uint32_t physics{};
    std::array<std::uintptr_t,6> owner{};
    bridge_policy::ReadinessReason reason=bridge_policy::ReadinessReason::not_evaluated;
};
BridgeReadiness private_board_input_readiness(const std::array<std::uintptr_t,6>* expected=nullptr) noexcept {
    using Reason=bridge_policy::ReadinessReason;
    BridgeReadiness result;
    const auto fail=[&](Reason reason){result.reason=reason;return result;};
    auto& bridge=private_board_input_bridge();
    if(!bridge.requested.load(std::memory_order_acquire))return fail(Reason::bridge_not_latched);
    if(private_bmx_native_input_handoff_requested()||!bridge_environment_one(L"RESKATE_BMX_NATIVE_BOARD_INPUT_BRIDGE"))return fail(Reason::bridge_flag_changed);
    if(!bridge_environment_one(L"RESKATE_BMX_NATIVE_CHASSIS"))return fail(Reason::native_chassis_flag_changed);
    if(!bridge_environment_exact(L"RESKATE_BMX_LAUNCH_MODE",L"human-ride"))return fail(Reason::launch_mode_changed);
    const auto& overlay_state=state();const auto window=overlay_state.window.load(std::memory_order_relaxed);
    if(!window)return fail(Reason::window_unavailable);
    if(!overlay_state.started.load(std::memory_order_relaxed)||!overlay_state.input_attached.load(std::memory_order_relaxed)||
       overlay_state.stop.load(std::memory_order_relaxed)||overlay_state.failed.load(std::memory_order_relaxed))return fail(Reason::overlay_not_running);
    if(!game_window_foreground(window))return fail(Reason::foreground_lost);
    bool escape{};{OverlayInputAccess access;escape=(GetAsyncKeyState(VK_ESCAPE)&0x8000)!=0;}
    if(escape)return fail(Reason::escape_held);
    result.focused=true;
    namespace trace=bmx::board_handoff_trace;
    if(!trace::store.enabled.load(std::memory_order_acquire))return fail(Reason::trace_not_initialized);
    if(trace::store.mode!=trace::Mode::native_only)return fail(Reason::trace_not_native_only);
    if(trace::store.automated_native_board)return fail(Reason::trace_automated_control);
    if(!trace::store.deferred_native_board_arm)return fail(Reason::trace_deferred_arm_disabled);
    if(!trace::selected())return fail(Reason::trace_scope_revalidation_failed);
    if(trace::store.sealed.load(std::memory_order_acquire))return fail(Reason::trace_sealed);
    NoBailSkater skater;std::uintptr_t client{};
    // This existing nonblocking owner API conflates publication contention and
    // invalid ownership. Do NOT guess that its failure is safe to defer.
    if(!try_no_bail_skater(0,skater,client))return fail(Reason::owner_unavailable);
    bool protected_now{};NoBailSkater rechecked;
    if(!inspect_no_bail_owner(client,skater.entity,rechecked,protected_now))return fail(Reason::owner_validation_failed);
    if(protected_now)return fail(Reason::owner_protected);
    if(rechecked.base!=skater.base||rechecked.entity!=skater.entity||rechecked.core!=skater.core||
       rechecked.context!=skater.context||rechecked.rig!=skater.rig)return fail(Reason::owner_chain_changed);
    result.owner={skater.base,client,skater.entity,skater.core,skater.context,skater.rig};
    if(expected&&result.owner!=*expected)return fail(Reason::owner_changed);
    trace::ClientState sdk;bool armed{},lease_active{};
    {
        trace::TryGuard guard;if(!guard.held)return fail(Reason::trace_busy);
        sdk=trace::store.sdk;armed=trace::store.armed.load(std::memory_order_acquire);
        result.armable=!trace::store.ever_armed&&!trace::store.control_arm_requested.load(std::memory_order_acquire);
        if(armed){
            bool found{};
            for(auto n=trace::store.count;n;--n)if(trace::store.rows[n-1].kind==trace::Kind::client){
                const auto& row=trace::store.rows[n-1];const auto now=GetTickCount64();
                if(now<row.wall_ms)return fail(Reason::client_row_clock_future);
                if(now-row.wall_ms>250)return fail(Reason::client_row_stale);
                lease_active=row.lease.active;found=true;break;
            }
            if(!found)return fail(Reason::client_row_missing);
        }
    }
    const auto now=GetTickCount64();
    if(!sdk.sampled_ms)return fail(Reason::client_snapshot_missing);
    if(now<sdk.sampled_ms)return fail(Reason::client_snapshot_clock_future);
    if(now-sdk.sampled_ms>250)return fail(Reason::client_snapshot_stale);
    if(sdk.active||sdk.pending||sdk.stepping_off||sdk.board_pending||sdk.capture||sdk.parts||sdk.motion_updates||
       sdk.pose_updates||sdk.render_updates||sdk.board_hide_updates||sdk.chassis_allocations||sdk.chassis_mounts||lease_active)
        return fail(Reason::bmx_activity_or_lease);
    if(!sdk.gameplay)return fail(Reason::client_gameplay_false);
    if(!sdk.solo)return fail(Reason::client_not_solo);
    // Preserve a current canonical owner/protection and physics check AFTER
    // the copied observer/client gates, not merely the earlier retry screen.
    NoBailSkater final_owner;bool final_protected{};
    if(!inspect_no_bail_owner(client,skater.entity,final_owner,final_protected))return fail(Reason::owner_revalidation_failed);
    if(final_protected)return fail(Reason::owner_revalidation_protected);
    if(final_owner.base!=skater.base||final_owner.entity!=skater.entity||final_owner.core!=skater.core||
       final_owner.context!=skater.context||final_owner.rig!=skater.rig)return fail(Reason::owner_revalidation_changed);
    if(!memory::peek(skater.context+0x1414,result.physics))return fail(Reason::physics_unavailable);
    result.physics_available=true;
    result.identity=true;
    // SDK focus includes source availability; console capture deliberately
    // leaves that sample unavailable even with the game window foreground.
    // Treat it as a preparation readiness wait AFTER owner/physics identity.
    // Pulse::poll still aborts any lost readiness after its first offer.
    if(!sdk.focused)return fail(Reason::client_focus_false);
    if(!armed)return fail(Reason::waiting_observer_arm);
    if(sdk.menu)return fail(Reason::waiting_menu);
    if(!sdk.native_control_input_allowed)return fail(Reason::waiting_native_input);
    if(polled_capture()!=input::BmxNativeCapture::none)return fail(Reason::waiting_capture);
    result.ready=true;result.reason=Reason::ready;return result;
}
void note_private_board_readiness(PrivateBoardInputBridge& bridge,const BridgeReadiness& readiness,const char* stage) noexcept {
    const auto now=GetTickCount64();const auto index=static_cast<unsigned>(readiness.reason);
    auto& count=bridge.readiness_counts[index];if(!count.count)count.first_ms=now;
    ++count.count;count.last_ms=now;
    if(!bridge.pulse.begun&&bridge_policy::preparation_retryable(readiness.reason))++count.deferred;
    bridge.last_readiness=readiness.reason;bridge.last_readiness_stage=stage;
    bridge.last_readiness_identity=readiness.identity;bridge.last_readiness_focused=readiness.focused;
    bridge.last_readiness_ready=readiness.ready;bridge.last_readiness_armable=readiness.armable;
}
bool queue_private_native_board_input() noexcept {
    struct Preserve {DWORD value=GetLastError();~Preserve(){SetLastError(value);}} preserve;
    auto& bridge=private_board_input_bridge();
    bridge.queue_last_attempt_ms=GetTickCount64();
    if(!TryAcquireSRWLockExclusive(&bridge.lock)){++bridge.queue_lock_refusals;bridge.queue_issue="queue_lock_busy";return false;}
    bool accepted{};
    try {
        const auto readiness=private_board_input_readiness();
        note_private_board_readiness(bridge,readiness,"queue");
        bridge.queue_issue=bridge_policy::name(readiness.reason);
        // The console is intentionally still open. The passive observer arms
        // only after it closes; no scenario report is emitted while waiting.
        if(readiness.identity&&readiness.focused&&readiness.physics==504&&readiness.armable) {
            accepted=bridge.pulse.queue(GetTickCount64(),0); // Explicit private slot0 only.
            if(accepted){bridge.owner=readiness.owner;bridge.queue_issue="queued";}
            else bridge.queue_issue="one_shot_already_used_or_invalid_queue_time";
        }else if(readiness.identity&&readiness.focused)bridge.queue_issue=readiness.physics!=504?
            "queue_native_state_not_504":"queue_observer_already_armed_or_requested";
    }catch(...){bridge.queue_issue="queue_exception";}
    ReleaseSRWLockExclusive(&bridge.lock);return accepted;
}
void cancel_private_native_board_input() noexcept {
    auto& bridge=private_board_input_bridge();AcquireSRWLockExclusive(&bridge.lock);
    if(bridge.pulse.live())bridge.pulse.abort("explicit_cancel");
    ReleaseSRWLockExclusive(&bridge.lock);
}
bridge_policy::Pad private_board_pad(const XINPUT_GAMEPAD& p) noexcept {
    return {p.wButtons,p.bLeftTrigger,p.bRightTrigger,p.sThumbLX,p.sThumbLY,p.sThumbRX,p.sThumbRY};
}
void apply_private_native_board_input(bool outer,DWORD user,const XINPUT_GAMEPAD& raw,XINPUT_STATE* output) noexcept {
    if(!outer||!output||user!=0)return;
    auto& bridge=private_board_input_bridge();if(!bridge.requested.load(std::memory_order_acquire))return;
    struct Preserve {DWORD value=GetLastError();~Preserve(){SetLastError(value);}} preserve;
    // Never fail open for one poll and then resume Y: that would introduce an
    // unrecorded release/re-press. All opted-in slot0 returns share one ordering
    // point. Lock holders do not call XInput or acquire the game's UI model.
    AcquireSRWLockExclusive(&bridge.lock);
    const auto before_packet=output->dwPacketNumber;const auto before_pad=private_board_pad(output->Gamepad);
    bridge_policy::Result result;std::uint32_t physics{};bool physics_observed{};
    if(bridge.pulse.live()&&bridge.count>=bridge.pulse_report_capacity)
        bridge.pulse.abort("report_capacity_exhausted");
    if(bridge.pulse.live()) {
        const auto readiness=private_board_input_readiness(&bridge.owner);
        const bool quiet=(raw.wButtons&~0x0fu)==0&&raw.bLeftTrigger<=12&&raw.bRightTrigger<=12&&
            std::abs(int(raw.sThumbLX))<=1638&&std::abs(int(raw.sThumbLY))<=1638&&
            std::abs(int(raw.sThumbRX))<=1638&&std::abs(int(raw.sThumbRY))<=1638;
        note_private_board_readiness(bridge,readiness,bridge.pulse.begun?"transaction":"preparation");
        physics=readiness.physics;physics_observed=readiness.physics_available;
        result=bridge.pulse.poll(GetTickCount64(),user,readiness.identity,
            readiness.focused,readiness.ready,quiet,readiness.physics,
            bridge_policy::preparation_retryable(readiness.reason),bridge_policy::name(readiness.reason));
        if(result.rewrite) {
            // Keep every existing filter result, including D-pad. Never
            // forge a connected slot or change ReSkate's own bypass reads.
            auto& pad=output->Gamepad;
            pad.wButtons=static_cast<WORD>((pad.wButtons&~0x8000u)|(result.north?0x8000u:0));
        }
    }
    if(bridge.packets.active||result.rewrite) {
        const auto after_pad=private_board_pad(output->Gamepad);
        output->dwPacketNumber=bridge.packets.translate(before_packet,after_pad);
        const bool terminal=!bridge.pulse.live();
        const PrivateBoardInputReport report{GetTickCount64(),++bridge.translated_returns,
            static_cast<unsigned>(bridge.pulse.phase),user,before_packet,output->dwPacketNumber,
            before_pad,after_pad,physics,result.rewrite,true,terminal,physics_observed};
        if(result.rewrite)bridge.reports[bridge.count++]=report;
        if(terminal) {
            ++bridge.terminal_returns;bridge.last_terminal=report;
            if(!bridge.terminal_emitted) {
                bridge.reports[bridge.count++]=report;bridge.terminal_emitted=true;
            }
        }
    }
    ReleaseSRWLockExclusive(&bridge.lock);
}
std::string private_native_board_input_status() {
    auto& bridge=private_board_input_bridge();AcquireSRWLockShared(&bridge.lock);
    auto reports=bridge.reports;const auto count=bridge.count;const auto owner=bridge.owner;
    const auto pulse=bridge.pulse;const auto packets=bridge.packets;
    const auto translated_returns=bridge.translated_returns,terminal_returns=bridge.terminal_returns;
    const auto terminal_emitted=bridge.terminal_emitted;const auto last_terminal=bridge.last_terminal;
    const auto requested=bridge.requested.load(std::memory_order_acquire);
    const auto readiness_counts=bridge.readiness_counts;const auto last_readiness=bridge.last_readiness;
    const auto last_stage=bridge.last_readiness_stage;
    const auto last_identity=bridge.last_readiness_identity,last_focused=bridge.last_readiness_focused;
    const auto last_ready=bridge.last_readiness_ready,last_armable=bridge.last_readiness_armable;
    const auto queue_issue=bridge.queue_issue.load();const auto queue_lock_refusals=bridge.queue_lock_refusals.load();
    const auto queue_last_attempt_ms=bridge.queue_last_attempt_ms.load();
    ReleaseSRWLockShared(&bridge.lock);
    const auto pad_json=[](const bridge_policy::Pad& p){return Json::array({p.buttons,p.left_trigger,p.right_trigger,
        p.left_x,p.left_y,p.right_x,p.right_y});};
    const auto report_json=[&](const PrivateBoardInputReport& r){return Json{
        {"wall_ms",r.wall},{"ordinal",r.ordinal},{"phase",r.phase},{"xinput_slot",r.user},{"before_packet",r.before_packet},
        {"after_packet",r.after_packet},{"before_pad",pad_json(r.before_pad)},{"after_pad",pad_json(r.after_pad)},
        {"observed_physics",r.physics_observed?Json(r.physics):Json(nullptr)},
        {"pulse_offer",r.pulse_offer},{"packet_translated",r.packet_translated},
        {"terminal_physical_passthrough",r.terminal}};};
    Json rows=Json::array();for(std::size_t i=0;i<count;++i)rows.push_back(report_json(reports[i]));
    Json readiness=Json::array();for(unsigned i=0;i<readiness_counts.size();++i){const auto& r=readiness_counts[i];if(r.count)readiness.push_back(Json{
        {"reason",bridge_policy::name(static_cast<bridge_policy::ReadinessReason>(i))},{"count",r.count},
        {"preparation_retryable_observations",r.deferred},{"first_ms",r.first_ms},{"last_ms",r.last_ms}});}
    return Json{{"schema",3},{"event","bmx_private_native_board_input"},{"enabled_at_install",requested},
        {"phase",static_cast<unsigned>(pulse.phase)},{"issue",pulse.issue},{"owner",owner},
        {"queue_issue",queue_issue},{"queue_lock_refusals",queue_lock_refusals},{"queue_last_attempt_ms",queue_last_attempt_ms},
        {"last_readiness_reason",bridge_policy::name(last_readiness)},{"last_readiness_stage",last_stage},
        {"last_readiness_identity",last_identity},{"last_readiness_focused",last_focused},
        {"last_readiness_ready",last_ready},{"last_readiness_armable",last_armable},{"readiness_counts",std::move(readiness)},
        {"preparation_retry_policy","classified observer unavailable reads only, before first offer; original preparation deadline unchanged"},
        {"xinput_slot",0},{"scope","private_XInput_only_not_native_PlayStation_HID"},
        {"queued_ms",pulse.queued},{"started_ms",pulse.started},{"preparation_limit_ms",bridge_policy::preparation_ms},
        {"transaction_limit_ms",bridge_policy::transaction_ms},{"press_reports",pulse.press_reports},
        {"release_reports",pulse.release_reports},{"rows",std::move(rows)},
        {"packet_domain_persistent",packets.active},{"latest_packet",packets.packet},
        {"translated_returns",translated_returns},{"terminal_physical_returns",terminal_returns},
        {"terminal_report_emitted",terminal_emitted},{"last_terminal_return",terminal_emitted?report_json(last_terminal):Json(nullptr)},
        {"terminal_history_scope","first and latest retained; intervening returns counted, not fully recorded"},
        {"input_consumed",nullptr},{"native_quality",nullptr}}.dump();
}
// END PRIVATE NATIVE BOARD INPUT BRIDGE
#include "private_bmx_native_input_handoff.inl"
#include "human_bmx_native_handoff.inl"

bool block_polled_input() {
    const auto capture=polled_capture();
    return capture==input::BmxNativeCapture::all||capture==input::BmxNativeCapture::controls;
}
bool bmx_input_capture_active() noexcept {
    const auto& s = state();
    return input::bmx_capture_live(bmx_capture_until.load(std::memory_order_acquire), GetTickCount64(),
        s.started.load(std::memory_order_relaxed) && !s.stop.load(std::memory_order_relaxed) &&
        !s.failed.load(std::memory_order_relaxed));
}
bool bmx_interaction_capture_active() noexcept {
    const auto& s=state();
    return input::bmx_north_capture_live(bmx_interaction_until.load(std::memory_order_acquire),
        bmx_quick_switch_until.load(std::memory_order_acquire),GetTickCount64(),
        s.started.load(std::memory_order_relaxed)&&!s.stop.load(std::memory_order_relaxed)&&
        !s.failed.load(std::memory_order_relaxed));
}

void note_input_record(const RAWINPUT& record) noexcept;
HeldInput& held_input() { static auto* value = new HeldInput; return *value; }

struct InputCaptureHooks {
    Hook hid_attributes, read_file, overlapped, overlapped_ex, completion, completion_ex, close_handle;
    struct HidRead { HANDLE file; void* buffer; DWORD capacity; PlayStationPad kind; };
    std::mutex hid_mutex;
    std::map<HANDLE, PlayStationPad> hid_devices;
    std::map<OVERLAPPED*, HidRead> hid_reads;
    Hook register_raw;
    Hook async_key, key, keyboard, raw_data, raw_buffer, capture, release_capture, cursor, show_cursor, physical_cursor, cursor_position;
    std::array<Hook, 5> xinput_state, xinput_extended, xinput_keystroke;
    // Hooks other programs put in front of those, hooked in turn (keep_xinput_capture_first).
    static constexpr std::size_t front_slots = 8;
    std::array<Hook, front_slots> xinput_front;
    std::mutex front_mutex;
    Hook direct_create;
    std::array<Hook, 2> device_create;
    // Mouse/keyboard and A/W devices may have distinct implementations. Slots
    // describe callable addresses, not the CreateDevice interface index.
    static constexpr std::size_t device_slots = 8;
    std::array<Hook, device_slots> device_state, device_data, device_format;
    struct Format {
        DWORD size{}; std::vector<DIOBJECTDATAFORMAT> objects;
        bool gamepad{};std::vector<DWORD> north_offsets;
    };
    std::mutex formats_mutex;
    std::map<void*, Format> formats;
};
InputCaptureHooks& input_hooks() { static auto* hooks = new InputCaptureHooks; return *hooks; }

BOOLEAN __stdcall captured_hid_attributes(HANDLE file, PHIDD_ATTRIBUTES attributes) {
    const auto result = original<decltype(&HidD_GetAttributes)>(input_hooks().hid_attributes)(file, attributes);
    const auto error = GetLastError();
    if (result && attributes) {
        const auto kind = playstation_pad(attributes->VendorID, attributes->ProductID);
        if (kind != PlayStationPad::none) {
            std::lock_guard lock(input_hooks().hid_mutex);
            input_hooks().hid_devices[file] = kind;
        }
    }
    SetLastError(error);
    return result;
}
std::atomic<dingosdk::overlay::GamepadFilter>& gamepad_filter() {
    static std::atomic<dingosdk::overlay::GamepadFilter> value{};
    return value;
}
// Full Bluetooth reports have a CRC over the 0xA1 input prefix and payload.
// A stale checksum makes the native reader discard a changed report.
void refresh_bluetooth_crc(std::uint8_t* bytes, DWORD size) noexcept {
    if (size != 78 || (bytes[0] != 0x31 && bytes[0] != 0x11)) return;
    std::uint32_t crc = 0xffffffffu;
    const auto feed = [&](std::uint8_t byte) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    };
    feed(0xa1);
    for (DWORD i = 0; i < size - 4; ++i) feed(bytes[i]);
    crc = ~crc;
    std::memcpy(bytes + size - 4, &crc, sizeof(crc));
}
void neutral_hid(const InputCaptureHooks::HidRead& read, DWORD size) {
    const auto capture=polled_capture();
    if (capture==input::BmxNativeCapture::none || !read.buffer || size > read.capacity) return;
    auto* bytes = static_cast<std::uint8_t*>(read.buffer);
    // USB or Bluetooth decides where the buttons are in a report with id 1. It is the device's
    // report length that tells them apart, and Windows pads every read to that length, so it
    // is how many bytes were read: not how large a buffer the game happens to read into. A
    // game reading a USB DualSense into a larger buffer had its d-pad byte written as "up"
    // here, and placed a session marker whenever a menu took the input. (Nothing is asked of
    // the game's handle for this: 2.0.4 did, as the pad was first read.)
    const bool bluetooth = playstation_bluetooth(size);
    if(!input::bmx_filter_playstation_report(read.kind,bluetooth,bytes,size,capture))return;
    static std::atomic<bool> reported{};
    if (!reported.exchange(true)) logging::printf(logging::Level::info, logging::Channel::input,
        "Native PlayStation HID report neutralized; overlay retains its independent input stream. (%s over %s, report 0x%02x of %lu bytes, "
        "the game's buffer %lu.)", read.kind == PlayStationPad::dualsense ? "DualSense" : "DualShock 4",
        bluetooth ? "Bluetooth" : "USB", bytes[0], size, read.capacity);
}
// Raw HID uses a device handle, not the ReadFile enumeration handle. Only the
// already supported Sony report layouts may pass; unknown devices stay blocked.
enum class RawHidFilter { unrecognized, filtered, invalid };
RawHidFilter filter_bmx_raw_hid(RAWINPUT& raw,UINT available,input::BmxNativeCapture capture) noexcept {
    struct Error {DWORD value=GetLastError();~Error(){SetLastError(value);}} error;
    constexpr auto payload=offsetof(RAWINPUT,data)+offsetof(RAWHID,bRawData);
    if(raw.header.dwType!=RIM_TYPEHID||available<payload||raw.header.dwSize< payload||
       raw.header.dwSize>available||!raw.data.hid.dwSizeHid||raw.data.hid.dwSizeHid>547||
       !raw.data.hid.dwCount||raw.data.hid.dwCount>64||
       raw.data.hid.dwCount>(raw.header.dwSize-payload)/raw.data.hid.dwSizeHid)return RawHidFilter::invalid;
    RID_DEVICE_INFO info{};info.cbSize=sizeof(info);UINT size=sizeof(info);
    if(GetRawInputDeviceInfoW(raw.header.hDevice,RIDI_DEVICEINFO,&info,&size)==UINT(-1)||
       info.dwType!=RIM_TYPEHID||info.hid.usUsagePage!=1||info.hid.usUsage!=5||
       info.hid.dwVendorId>0xffff||info.hid.dwProductId>0xffff)return RawHidFilter::unrecognized;
    const auto kind=playstation_pad(static_cast<std::uint16_t>(info.hid.dwVendorId),static_cast<std::uint16_t>(info.hid.dwProductId));
    if(kind==PlayStationPad::none)return RawHidFilter::unrecognized;
    for(DWORD i=0;i<raw.data.hid.dwCount;++i)
        if(!input::bmx_filter_playstation_report(kind,playstation_bluetooth(raw.data.hid.dwSizeHid),
            raw.data.hid.bRawData+i*raw.data.hid.dwSizeHid,raw.data.hid.dwSizeHid,capture))return RawHidFilter::invalid;
    return RawHidFilter::filtered;
}
bool bmx_dpad_raw_message(HRAWINPUT handle) noexcept {
    const auto error=GetLastError();
    const auto permitted=[&] {
        if(polled_capture()!=input::BmxNativeCapture::controls)return false;
        alignas(RAWINPUT) std::array<std::byte,4096> bytes{};UINT size=static_cast<UINT>(bytes.size());
        UINT got{};
        {OverlayInputAccess access;got=GetRawInputData(handle,RID_INPUT,bytes.data(),&size,sizeof(RAWINPUTHEADER));}
        if(got==UINT(-1)||got>bytes.size()||got<sizeof(RAWINPUTHEADER))return false;
        return filter_bmx_raw_hid(*reinterpret_cast<RAWINPUT*>(bytes.data()),got,input::BmxNativeCapture::controls)==RawHidFilter::filtered;
    }();
    SetLastError(error);return permitted;
}
// The gamepad filter (DingoSDKOverlaySetGamepadFilter) on the game's own native PlayStation
// reads, as filter_pad applies it to XInput: a DualShock 4 or DualSense read without Steam Input
// never goes through XInput. The report is parsed into XInput's layout, filtered, and what the
// filter changed (triggers, left stick X) is written back in the report's own layout.
inline constexpr unsigned hid_filter_slot = 3; // the filter's pad slot for native PlayStation pads
void filter_hid(const InputCaptureHooks::HidRead& read, DWORD size) noexcept {
    const auto filter = gamepad_filter().load(std::memory_order_acquire);
    if (!filter || !read.buffer || !size || size > read.capacity) return;
    auto* bytes = static_cast<std::uint8_t*>(read.buffer);
    const bool bluetooth = playstation_bluetooth(size);
    const auto parsed = parse_playstation_report(read.kind, bluetooth, bytes, size);
    if (!parsed) return;
    const dingosdk::overlay::GamepadValues before{parsed->buttons, parsed->left_trigger, parsed->right_trigger,
        parsed->left_x, parsed->left_y, parsed->right_x, parsed->right_y};
    auto pad = before;
    filter(hid_filter_slot, pad);
    if (pad.left_trigger == before.left_trigger && pad.right_trigger == before.right_trigger && pad.left_x == before.left_x) return;
    // Where parse_playstation_report found them: sticks (left X first), the shoulder byte (L2 0x04,
    // R2 0x08 digital bits) and the two analog triggers.
    std::uint8_t *sticks{}, *shoulder{}, *left{}, *right{};
    const auto id = bytes[0];
    if (read.kind == PlayStationPad::dualsense && ((id == 0x01 && !bluetooth) || id == 0x31)) {
        auto* data = bytes + (id == 0x31 ? 2 : 1);
        sticks = data; left = data + 4; right = data + 5; shoulder = data + 8;
    } else if (id == 0x01) {
        sticks = bytes + 1; shoulder = bytes + 6; left = bytes + 8; right = bytes + 9;
    } else if (read.kind == PlayStationPad::dualshock4 && id == 0x11) {
        sticks = bytes + 3; shoulder = bytes + 8; left = bytes + 10; right = bytes + 11;
    } else {
        return;
    }
    if (pad.left_trigger != before.left_trigger) {
        *left = pad.left_trigger;
        if (!pad.left_trigger) *shoulder = static_cast<std::uint8_t>(*shoulder & ~0x04u);
    }
    if (pad.right_trigger != before.right_trigger) {
        *right = pad.right_trigger;
        if (!pad.right_trigger) *shoulder = static_cast<std::uint8_t>(*shoulder & ~0x08u);
    }
    if (pad.left_x != before.left_x)
        sticks[0] = static_cast<std::uint8_t>(std::clamp(static_cast<int>(pad.left_x) / 258 + 128, 0, 255));
    refresh_bluetooth_crc(bytes, size);
}
// A finished native PlayStation read of the game's: neutral while a menu holds the input, else
// through the gamepad filter.
void handle_hid(const InputCaptureHooks::HidRead& read, DWORD size) noexcept {
    const auto capture=polled_capture();
    if(capture==input::BmxNativeCapture::none)filter_hid(read,size);
    else {
        if(capture==input::BmxNativeCapture::interaction)filter_hid(read,size);
        neutral_hid(read,size);
    }
}
void complete_hid(OVERLAPPED* request, DWORD bytes, bool success, bool finished) {
    auto& h = input_hooks();
    InputCaptureHooks::HidRead read{};
    {
        std::lock_guard lock(h.hid_mutex);
        const auto found = h.hid_reads.find(request);
        if (found == h.hid_reads.end()) return;
        read = found->second;
        if (finished) h.hid_reads.erase(found);
    }
    if (success) handle_hid(read, bytes);
}
BOOL WINAPI captured_read_file(HANDLE file, LPVOID buffer, DWORD size, LPDWORD bytes, LPOVERLAPPED request) {
    auto& h = input_hooks();
    InputCaptureHooks::HidRead read{file, buffer, size, PlayStationPad::none};
    if (!overlay_input_access) {
        // Some readers inspect a temporary enumeration handle, then open a
        // second handle for reports. Identify that handle as well.
        bool known{};
        {
            std::lock_guard lock(h.hid_mutex);
            known = h.hid_devices.contains(file);
        }
        if (!known && size >= 10 && size <= 547) {
            const auto error = GetLastError();
            HIDD_ATTRIBUTES attributes{sizeof(attributes)};
            const auto okay = original<decltype(&HidD_GetAttributes)>(h.hid_attributes)(file, &attributes);
            const auto kind = okay ? playstation_pad(attributes.VendorID, attributes.ProductID) : PlayStationPad::none;
            {
                std::lock_guard lock(h.hid_mutex);
                h.hid_devices[file] = kind;
            }
            SetLastError(error);
        }
        std::lock_guard lock(h.hid_mutex);
        const auto found = h.hid_devices.find(file);
        if (found != h.hid_devices.end() && found->second != PlayStationPad::none) {
            read.kind = found->second;
            if (request) h.hid_reads[request] = read;
        }
    }
    const auto result = original<decltype(&ReadFile)>(h.read_file)(file, buffer, size, bytes, request);
    const auto error = GetLastError();
    if (read.kind != PlayStationPad::none) {
        if (result) handle_hid(read, bytes ? *bytes : request ? static_cast<DWORD>(request->InternalHigh) : 0);
        if (!result && error != ERROR_IO_PENDING && request) complete_hid(request, 0, false, true);
    }
    SetLastError(error);
    return result;
}
BOOL WINAPI captured_overlapped(HANDLE file, LPOVERLAPPED request, LPDWORD bytes, BOOL wait) {
    const auto result = original<decltype(&GetOverlappedResult)>(input_hooks().overlapped)(file, request, bytes, wait);
    const auto error = GetLastError();
    complete_hid(request, result && bytes ? *bytes : 0, result != FALSE, result || error != ERROR_IO_INCOMPLETE);
    SetLastError(error); return result;
}
BOOL WINAPI captured_overlapped_ex(HANDLE file, LPOVERLAPPED request, LPDWORD bytes, DWORD timeout, BOOL alertable) {
    const auto result = original<decltype(&GetOverlappedResultEx)>(input_hooks().overlapped_ex)(file, request, bytes, timeout, alertable);
    const auto error = GetLastError();
    complete_hid(request, result && bytes ? *bytes : 0, result != FALSE,
        result || (error != ERROR_IO_INCOMPLETE && error != WAIT_TIMEOUT && error != WAIT_IO_COMPLETION));
    SetLastError(error); return result;
}
BOOL WINAPI captured_completion(HANDLE port, LPDWORD bytes, PULONG_PTR key, LPOVERLAPPED* request, DWORD timeout) {
    const auto result = original<decltype(&GetQueuedCompletionStatus)>(input_hooks().completion)(port, bytes, key, request, timeout);
    const auto error = GetLastError();
    if (request && *request) complete_hid(*request, result && bytes ? *bytes : 0, result != FALSE, true);
    SetLastError(error); return result;
}
BOOL WINAPI captured_completion_ex(HANDLE port, LPOVERLAPPED_ENTRY entries, ULONG count, PULONG removed, DWORD timeout, BOOL alertable) {
    const auto result = original<decltype(&GetQueuedCompletionStatusEx)>(input_hooks().completion_ex)(port, entries, count, removed, timeout, alertable);
    const auto error = GetLastError();
    if (result && entries && removed) for (ULONG i = 0; i < *removed; ++i)
        complete_hid(entries[i].lpOverlapped, entries[i].dwNumberOfBytesTransferred,
            static_cast<LONG>(entries[i].Internal) >= 0, true);
    SetLastError(error); return result;
}
BOOL WINAPI captured_close_handle(HANDLE file) {
    auto& h = input_hooks();
    {
        std::lock_guard lock(h.hid_mutex);
        if (h.hid_devices.erase(file))
            std::erase_if(h.hid_reads, [&](const auto& entry) { return entry.second.file == file; });
    }
    return original<decltype(&CloseHandle)>(h.close_handle)(file);
}

// Where the jump at an entry point goes, through any stubs on the way: the first address inside
// a module, and that module. Null when the entry point is not hooked (or cannot be read).
struct Landing { void* at{}; HMODULE module{}; };
Landing hook_landing(void* entry) {
    const auto self = GetCurrentProcess();
    const auto read = [&](const void* at, void* out, std::size_t size) {
        SIZE_T got{};
        return ReadProcessMemory(self, at, out, size, &got) && got == size;
    };
    auto* at = static_cast<std::uint8_t*>(entry);
    for (int hop = 0; hop < 8; ++hop) {
        std::uint8_t code[16]{};
        if (!read(at, code, sizeof(code))) return {};
        std::uint8_t* next{};
        if (code[0] == 0xe9) {
            std::int32_t offset; std::memcpy(&offset, code + 1, 4);
            next = at + 5 + offset;
        } else if (code[0] == 0xff && code[1] == 0x25) {
            std::int32_t offset; std::memcpy(&offset, code + 2, 4);
            if (!read(at + 6 + offset, &next, sizeof(next))) return {};
        } else if (code[0] == 0x48 && code[1] == 0xb8 && code[10] == 0xff && code[11] == 0xe0) {
            std::memcpy(&next, code + 2, 8);
        } else return {};
        at = next;
        HMODULE module{};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(at), &module) && module) return {at, module};
    }
    return {};
}
HMODULE module_of(const void* address) {
    HMODULE module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(address), &module);
    return module;
}
std::string module_name(HMODULE module) {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* name = path;
    for (const wchar_t* c = path; *c; ++c) if (*c == L'\\') name = c + 1;
    std::string text;
    for (; *name; ++name) text += static_cast<char>(*name);
    return text;
}
std::string xinput_entry_owners() {
    std::string text;
    const auto& h = input_hooks();
    const auto owner = [](void* entry) {
        const auto landing = hook_landing(entry);
        return landing.module ? module_name(landing.module) : std::string("not hooked");
    };
    constexpr std::array names{"xinput1_4", "xinput1_3", "xinput9_1_0", "xinput1_2", "xinput1_1"};
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (h.xinput_state[i].target) text += (text.empty() ? "" : ", ") + std::string(names[i]) + " " + owner(h.xinput_state[i].target);
        if (h.xinput_extended[i].target) text += (text.empty() ? "" : ", ") + std::string(names[i]) + " (extended) " + owner(h.xinput_extended[i].target);
    }
    return text.empty() ? "none" : text;
}

// The game's own read only: not ReSkate's (overlay_input_access) and not an inner layer of a
// read already being answered (Steam's hook going on to XInput runs inside OverlayInputAccess).
void filter_pad(bool outer, DWORD user, XINPUT_STATE* output) noexcept {
    if (!outer || !output) return;
    const auto filter = gamepad_filter().load(std::memory_order_acquire);
    if (!filter) return;
    auto& g = output->Gamepad;
    dingosdk::overlay::GamepadValues pad{g.wButtons, g.bLeftTrigger, g.bRightTrigger, g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY};
    filter(static_cast<unsigned>(user), pad);
    g.wButtons = pad.buttons;
    g.bLeftTrigger = pad.left_trigger;
    g.bRightTrigger = pad.right_trigger;
    g.sThumbLX = pad.left_x;
    g.sThumbLY = pad.left_y;
    g.sThumbRX = pad.right_x;
    g.sThumbRY = pad.right_y;
}
// Another program's hook on XInput that was put in front of the capture's own after it was
// installed (Steam's overlay does, for Steam Input, and answers the game with its own pad
// without going on to XInput): the capture then never sees the game's reads, and a menu that
// holds the game's input does not hold its controller. That hook's own function is hooked
// then, so the capture is in front of it again.
template<std::size_t I>
DWORD WINAPI captured_front(DWORD user, XINPUT_STATE* output) {
    const auto capture = polled_capture();
    const bool outer = overlay_input_access == 0;
    note_prompt_read(prompt_xinput);
    DWORD result;
    { // It may go on to XInput's own entry point, which is hooked as well.
        OverlayInputAccess access;
        result = original<DWORD(WINAPI*)(DWORD, XINPUT_STATE*)>(input_hooks().xinput_front[I])(user, output);
    }
    const auto raw_pad=result==ERROR_SUCCESS&&output?output->Gamepad:XINPUT_GAMEPAD{};
    const auto raw_packet=result==ERROR_SUCCESS&&output?output->dwPacketNumber:0;
    note_private_bmx_original_xinput(user,result,output);
    note_human_native_original(outer,user,result,output);
    if (result == ERROR_SUCCESS && output && capture!=input::BmxNativeCapture::none) {
        if(capture==input::BmxNativeCapture::interaction)filter_pad(outer,user,output);
        input::bmx_filter_native_pad(output->Gamepad,capture);
        output->dwPacketNumber ^= input::bmx_native_packet_tag(capture);
    } else if (result == ERROR_SUCCESS) {
        filter_pad(outer, user, output);
    }
    if(result==ERROR_SUCCESS&&output) {
        apply_private_native_board_input(outer,user,raw_pad,output);
        apply_private_bmx_native_input_handoff(outer,user,raw_packet,raw_pad,output);
        apply_human_native_handoff(outer,user,raw_packet,raw_pad,output);
    }
    return result;
}
void keep_xinput_capture_first() {
    auto& h = input_hooks();
    static const HMODULE self = module_of(&keep_xinput_capture_first);
    const auto detours = []<std::size_t... I>(std::index_sequence<I...>) {
        return std::array{reinterpret_cast<void*>(captured_front<I>)...};
    }(std::make_index_sequence<InputCaptureHooks::front_slots>{});
    std::lock_guard lock(h.front_mutex);
    const auto guard = [&](void* entry) {
        if (!entry) return;
        const auto landing = hook_landing(entry);
        // Not hooked over, the capture's own, or a jump inside XInput itself.
        if (!landing.at || landing.module == self || landing.module == module_of(entry)) return;
        for (const auto& hook : h.xinput_front) if (hook.target == landing.at) return;
        for (std::size_t slot = 0; slot < h.xinput_front.size(); ++slot) {
            if (h.xinput_front[slot].target) continue;
            const bool okay = install(h.xinput_front[slot], landing.at, detours[slot]);
            logging::printf(okay ? logging::Level::info : logging::Level::warning, logging::Channel::input,
                okay ? "XInput is answered by %s ahead of ReSkate: its reads are now held with the rest of the game's input."
                     : "XInput is answered by %s ahead of ReSkate, and that could not be hooked: a controller may reach the game behind a menu.",
                module_name(landing.module).c_str());
            return;
        }
    };
    for (std::size_t i = 0; i < h.xinput_state.size(); ++i) guard(h.xinput_state[i].target), guard(h.xinput_extended[i].target);
}
SHORT WINAPI captured_async_key(int key) {
    const auto value = original<SHORT(WINAPI*)(int)>(input_hooks().async_key)(key);
    return block_polled_input() ? 0 : value;
}
SHORT WINAPI captured_key(int key) {
    const auto value = original<SHORT(WINAPI*)(int)>(input_hooks().key)(key);
    return block_polled_input() ? 0 : value;
}
BOOL WINAPI captured_keyboard(PBYTE keys) {
    const auto result = original<BOOL(WINAPI*)(PBYTE)>(input_hooks().keyboard)(keys);
    if (result && keys && block_polled_input()) std::memset(keys, 0, 256);
    return result;
}
UINT WINAPI captured_raw_data(HRAWINPUT input, UINT command, LPVOID data, PUINT size, UINT header_size) {
    const auto result = original<UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT)>(
        input_hooks().raw_data)(input, command, data, size, header_size);
    if (result != UINT(-1) && data && command == RID_INPUT && !overlay_input_access && input != noted_raw_input &&
        result >= offsetof(RAWINPUT, data) + sizeof(RAWMOUSE) &&
        static_cast<RAWINPUT*>(data)->header.dwType == RIM_TYPEMOUSE)
        note_raw_mouse_packet(static_cast<RAWINPUT*>(data)->data.mouse);
    if (result != UINT(-1) && data && command == RID_INPUT && !overlay_input_access &&
        result >= offsetof(RAWINPUT, data) + sizeof(RAWKEYBOARD))
        note_input_record(*static_cast<RAWINPUT*>(data));
    if (result != UINT(-1) && data && command == RID_INPUT && result >= sizeof(RAWINPUTHEADER)) {
        auto* raw = static_cast<RAWINPUT*>(data);
        const bool mouse = raw->header.dwType == RIM_TYPEMOUSE && result >= offsetof(RAWINPUT, data) + sizeof(RAWMOUSE);
        const bool keyboard =
            raw->header.dwType == RIM_TYPEKEYBOARD && result >= offsetof(RAWINPUT, data) + sizeof(RAWKEYBOARD);
        const auto capture=polled_capture();
        if (capture==input::BmxNativeCapture::none||
            (capture==input::BmxNativeCapture::interaction&&(mouse||keyboard))) {
            if ((mouse || keyboard) && !overlay_input_access) held_input().seen(*raw);
        } else if(capture==input::BmxNativeCapture::interaction) {
            if(raw->header.dwType==RIM_TYPEHID&&result>=offsetof(RAWINPUT,data)+8&&
                filter_bmx_raw_hid(*raw,result,capture)==RawHidFilter::invalid)raw->data.hid.dwCount=0;
        } else if ((mouse || keyboard) && held_input().release_only(*raw)) {
            // A key or button the game holds still comes up; presses stay blocked.
        } else if (mouse)
            raw->data.mouse = {};
        else if (keyboard) {
            raw->data.keyboard = {};
            raw->data.keyboard.VKey = 255; // documented discard packet; never synthesize a key press
            raw->data.keyboard.Flags = RI_KEY_BREAK;
            raw->data.keyboard.Message = WM_KEYUP;
        } else if (raw->header.dwType == RIM_TYPEHID && result >= offsetof(RAWINPUT, data) + 8) {
            if(capture!=input::BmxNativeCapture::controls||
                filter_bmx_raw_hid(*raw,result,capture)!=RawHidFilter::filtered)raw->data.hid.dwCount=0;
        }
    }
    return result;
}
// The latest keyboard and mouse input the game read, with the device it came from, for the
// log line when the game switches to mouse and keyboard: which input made it switch. Raw
// input injected by software (SendInput: Steam Input's desktop layout, macro tools) carries
// no device handle.
struct LastInput {
    std::atomic<ULONGLONG> key_at{}, mouse_at{}, message_at{};
    std::atomic<std::uint32_t> key{}; // virtual key | make code << 16
    std::atomic<std::int32_t> mouse_x{}, mouse_y{};
    std::atomic<std::uint32_t> mouse_buttons{};
    std::atomic<HANDLE> key_device{}, mouse_device{};
    std::atomic<std::uint32_t> message{}, message_key{}; // the game window's last input message
};
LastInput& last_input() { static auto* value = new LastInput; return *value; }
void note_input_record(const RAWINPUT& record) noexcept {
    auto& last = last_input();
    if (record.header.dwType == RIM_TYPEKEYBOARD) {
        last.key.store(record.data.keyboard.VKey | (std::uint32_t{record.data.keyboard.MakeCode} << 16),
                       std::memory_order_relaxed);
        last.key_device.store(record.header.hDevice, std::memory_order_relaxed);
        last.key_at.store(GetTickCount64(), std::memory_order_relaxed);
    } else if (record.header.dwType == RIM_TYPEMOUSE &&
               (record.data.mouse.lLastX || record.data.mouse.lLastY || record.data.mouse.usButtonFlags)) {
        last.mouse_x.store(record.data.mouse.lLastX, std::memory_order_relaxed);
        last.mouse_y.store(record.data.mouse.lLastY, std::memory_order_relaxed);
        last.mouse_buttons.store(record.data.mouse.usButtonFlags, std::memory_order_relaxed);
        last.mouse_device.store(record.header.hDevice, std::memory_order_relaxed);
        last.mouse_at.store(GetTickCount64(), std::memory_order_relaxed);
    }
}
std::string input_device_name(HANDLE device) {
    if (!device) return "no device (injected by software)";
    std::array<wchar_t, 256> name{};
    UINT size = static_cast<UINT>(name.size());
    if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, name.data(), &size) == UINT(-1)) return "an unknown device";
    std::string text;
    for (const wchar_t* c = name.data(); *c && text.size() < 120; ++c) text.push_back(*c < 128 ? static_cast<char>(*c) : '?');
    return text;
}
void note_input_message(UINT message, WPARAM wp) noexcept {
    auto& last = last_input();
    last.message.store(message, std::memory_order_relaxed);
    last.message_key.store(static_cast<std::uint32_t>(wp), std::memory_order_relaxed);
    last.message_at.store(GetTickCount64(), std::memory_order_relaxed);
}
// What the game last read from the keyboard and mouse, within the 3 s before now.
std::string recent_input() {
    auto& last = last_input();
    const auto now = GetTickCount64();
    std::string text;
    const auto age = [&](ULONGLONG at) { return std::to_string(now - at) + " ms before"; };
    if (const auto at = last.key_at.load(); at && now - at <= 3000) {
        const auto key = last.key.load();
        text += std::format("key 0x{:02x} (scan 0x{:02x}) from {}, {}", key & 0xffff, key >> 16,
                            input_device_name(last.key_device.load()), age(at));
    }
    if (const auto at = last.mouse_at.load(); at && now - at <= 3000) {
        if (!text.empty()) text += "; ";
        text += std::format("mouse move ({}, {}) buttons 0x{:x} from {}, {}", last.mouse_x.load(), last.mouse_y.load(),
                            last.mouse_buttons.load(), input_device_name(last.mouse_device.load()), age(at));
    }
    if (const auto at = last.message_at.load(); at && now - at <= 3000) {
        if (!text.empty()) text += "; ";
        text += std::format("window message 0x{:x} ({:#x}), {}", last.message.load(), last.message_key.load(), age(at));
    }
    return text.empty() ? "no keyboard or mouse input in the 3 s before" : text;
}
UINT WINAPI captured_raw_buffer(PRAWINPUT data, PUINT size, UINT header_size) {
    // Calling the API drains queued input. While the overlay owns input the game gets only the
    // releases of keys and buttons it holds (HeldInput), so nothing pressed in the menu or chat
    // replays when it closes and nothing pressed before it stays stuck.
    const auto result = original<UINT(WINAPI*)(PRAWINPUT, PUINT, UINT)>(
        input_hooks().raw_buffer)(data, size, header_size);
    const auto capture = result != UINT(-1) && data ? polled_capture() : input::BmxNativeCapture::none;
    note_prompt_read(prompt_raw);
    if (result != UINT(-1) && data && header_size == sizeof(RAWINPUTHEADER)) {
        // Skate drains its mouse here; read the packets on the way past.
        // NEXTRAWINPUTBLOCK: records are 8-byte aligned on x64.
        const auto align = [](std::uintptr_t at) { return (at + 7) & ~std::uintptr_t{7}; };
        auto* record = data;
        auto write = reinterpret_cast<std::uintptr_t>(data);
        UINT kept{};
        for (UINT i = 0; i < result && record; ++i) {
            const auto bytes = record->header.dwSize;
            const auto next = align(reinterpret_cast<std::uintptr_t>(record) + bytes);
            if (record->header.dwType == RIM_TYPEMOUSE) note_raw_mouse_packet(record->data.mouse);
            note_input_record(*record);
            const bool mouse = record->header.dwType == RIM_TYPEMOUSE && bytes >= offsetof(RAWINPUT, data) + sizeof(RAWMOUSE);
            const bool keyboard =
                record->header.dwType == RIM_TYPEKEYBOARD && bytes >= offsetof(RAWINPUT, data) + sizeof(RAWKEYBOARD);
            if(capture==input::BmxNativeCapture::interaction) {
                if((mouse||keyboard)&&!overlay_input_access)held_input().seen(*record);
                if(record->header.dwType!=RIM_TYPEHID||
                    filter_bmx_raw_hid(*record,bytes,capture)!=RawHidFilter::invalid) {
                    if(write!=reinterpret_cast<std::uintptr_t>(record))std::memmove(reinterpret_cast<void*>(write),record,bytes);
                    write=align(write+bytes);++kept;
                }
            } else if (capture==input::BmxNativeCapture::none) {
                if ((mouse || keyboard) && !overlay_input_access) held_input().seen(*record);
            } else if ((mouse || keyboard) && held_input().release_only(*record)) {
                // Kept records move forward over the dropped ones, in the same layout.
                if (write != reinterpret_cast<std::uintptr_t>(record))
                    std::memmove(reinterpret_cast<void*>(write), record, bytes);
                write = align(write + bytes);
                ++kept;
            } else if(capture==input::BmxNativeCapture::controls&&
                filter_bmx_raw_hid(*record,bytes,capture)==RawHidFilter::filtered) {
                if(write!=reinterpret_cast<std::uintptr_t>(record))std::memmove(reinterpret_cast<void*>(write),record,bytes);
                write=align(write+bytes);++kept;
            }
            record = reinterpret_cast<RAWINPUT*>(next);
        }
        if (capture!=input::BmxNativeCapture::none) return kept;
    }
    return capture!=input::BmxNativeCapture::none ? 0 : result;
}
// The game's latest mouse registration, and whether the overlay has replaced
// it with a cursor-moving one.
struct RawMouse {
    std::mutex mutex;
    RAWINPUTDEVICE game{};
    bool known{}, overridden{};
};
RawMouse& raw_mouse() { static auto* value = new RawMouse; return *value; }
constexpr DWORD cursor_blocking_flags = RIDEV_NOLEGACY | RIDEV_CAPTUREMOUSE | RIDEV_NOHOTKEYS;
bool is_mouse(const RAWINPUTDEVICE& device) { return device.usUsagePage == 1 && device.usUsage == 2; }

// Logs how the game registers raw input (flags change with its input mode),
// once per distinct registration.
BOOL WINAPI captured_register_raw(PCRAWINPUTDEVICE devices, UINT count, UINT size) {
    const auto register_raw = original<BOOL(WINAPI*)(PCRAWINPUTDEVICE, UINT, UINT)>(input_hooks().register_raw);
    BOOL result{};
    if (devices && count && size == sizeof(RAWINPUTDEVICE) && count <= 64) {
        std::vector<RAWINPUTDEVICE> requested(devices, devices + count);
        auto& mouse = raw_mouse();
        std::lock_guard lock(mouse.mutex);
        for (auto& device : requested) {
            if (!is_mouse(device)) continue;
            if (device.dwFlags & RIDEV_REMOVE) mouse.known = false;
            else { mouse.game = device; mouse.known = true; }
            // While the overlay owns the pointer the cursor must keep moving.
            if (mouse.overridden && !(device.dwFlags & RIDEV_REMOVE)) device.dwFlags &= ~cursor_blocking_flags;
        }
        result = register_raw(requested.data(), count, size);
        if (result)
            for (const auto& device : requested)
                if (is_mouse(device))
                    state().mouse_legacy_blocked.store(!(device.dwFlags & RIDEV_REMOVE) &&
                        (device.dwFlags & RIDEV_NOLEGACY) == RIDEV_NOLEGACY);
        // Skate re-registers the mouse whenever it switches between controller and
        // mouse/keyboard: the one trace of which input it thinks the player uses.
        static DWORD last_mode = ~0ul;
        for (const auto& device : devices ? std::span(devices, count) : std::span<const RAWINPUTDEVICE>{})
            if (is_mouse(device) && device.dwFlags != last_mode) {
                last_mode = device.dwFlags;
                // Leaving controller mode: say what input came just before.
                const auto cause = device.dwFlags & RIDEV_NOLEGACY ? std::string{} : "; last input: " + recent_input();
                dingosdk::logging::printf(dingosdk::logging::Level::info, dingosdk::logging::Channel::input,
                    "Game input mode: mouse registered with flags 0x%lx (%s)%s%s.", device.dwFlags,
                    device.dwFlags & RIDEV_REMOVE ? "removed"
                    : (device.dwFlags & RIDEV_NOLEGACY) ? "controller: no Windows mouse messages" : "mouse and keyboard",
                    mouse.overridden ? "; the overlay owns the pointer" : "", cause.c_str());
            }
    } else {
        result = register_raw(devices, count, size);
    }
    try {
        static std::mutex mutex;
        static std::set<std::tuple<USHORT, USHORT, DWORD>> seen;
        for (UINT i = 0; devices && size == sizeof(RAWINPUTDEVICE) && i < count; ++i) {
            const auto& device = devices[i];
            {
                std::lock_guard lock(mutex);
                if (!seen.emplace(device.usUsagePage, device.usUsage, device.dwFlags).second) continue;
            }
            dingosdk::logging::printf(dingosdk::logging::Level::debug, dingosdk::logging::Channel::input,
                "Game raw input registration: page=%u usage=%u flags=0x%lx window=%p result=%d",
                device.usUsagePage, device.usUsage, device.dwFlags, device.hwndTarget, result ? 1 : 0);
        }
    } catch (...) {}
    return result;
}
void apply_raw_mouse(RawMouse& mouse, bool overlay) {
    auto device = mouse.game;
    if (overlay) device.dwFlags &= ~cursor_blocking_flags;
    const auto register_raw = original<BOOL(WINAPI*)(PCRAWINPUTDEVICE, UINT, UINT)>(input_hooks().register_raw);
    OverlayInputAccess access;
    const bool applied = register_raw ? register_raw(&device, 1, sizeof(device)) != FALSE
                                      : RegisterRawInputDevices(&device, 1, sizeof(device)) != FALSE;
    if (applied) state().mouse_legacy_blocked.store((device.dwFlags & RIDEV_NOLEGACY) == RIDEV_NOLEGACY);
    dingosdk::logging::printf(dingosdk::logging::Level::info, dingosdk::logging::Channel::input,
        "Raw mouse registration %s (flags=0x%lx, %s, Windows error %lu).",
        overlay ? "released for the overlay" : "returned to the game", device.dwFlags, applied ? "ok" : "failed",
        applied ? 0ul : GetLastError());
}
HWND WINAPI captured_capture(HWND window) {
    if (block_polled_input() || (overlay_input_access &&
        GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId())) return GetCapture();
    return original<HWND(WINAPI*)(HWND)>(input_hooks().capture)(window);
}
BOOL WINAPI captured_release_capture() {
    if (block_polled_input()) return TRUE;
    return original<BOOL(WINAPI*)()>(input_hooks().release_capture)();
}
HCURSOR WINAPI captured_cursor(HCURSOR cursor) {
    if (block_polled_input()) return GetCursor();
    return original<HCURSOR(WINAPI*)(HCURSOR)>(input_hooks().cursor)(cursor);
}
int WINAPI captured_show_cursor(BOOL show) {
    // ImGui draws the menu cursor. Do not let the game change the OS display
    // counter while it is open; the counter resumes unchanged on close.
    if (block_polled_input()) return show ? 0 : -1;
    return original<int(WINAPI*)(BOOL)>(input_hooks().show_cursor)(show);
}
BOOL WINAPI captured_physical_cursor(int x, int y) {
    if (block_polled_input()) return TRUE;
    return original<BOOL(WINAPI*)(int, int)>(input_hooks().physical_cursor)(x, y);
}
BOOL WINAPI captured_cursor_position(LPPOINT output) {
    thread_local POINT last{};
    thread_local bool sampled = false;
    if (output && block_polled_input() && sampled) { *output = last; return TRUE; }
    const auto result = original<BOOL(WINAPI*)(LPPOINT)>(input_hooks().cursor_position)(output);
    if (result && output && !overlay_input_access) { last = *output; sampled = true; }
    return result;
}
template<std::size_t I, bool Extended = false>
DWORD WINAPI captured_xinput(DWORD user, XINPUT_STATE* output) {
    const auto& hook = Extended ? input_hooks().xinput_extended[I] : input_hooks().xinput_state[I];
    const auto capture = polled_capture();
    const bool outer = overlay_input_access == 0;
    note_prompt_read(prompt_xinput);
    DWORD result;
    { // XInput exports can forward through another hooked XInput export.
        OverlayInputAccess access;
        result = original<DWORD(WINAPI*)(DWORD, XINPUT_STATE*)>(hook)(user, output);
    }
    const auto raw_pad=result==ERROR_SUCCESS&&output?output->Gamepad:XINPUT_GAMEPAD{};
    const auto raw_packet=result==ERROR_SUCCESS&&output?output->dwPacketNumber:0;
    note_private_bmx_original_xinput(user,result,output);
    note_human_native_original(outer,user,result,output);
    if (result == ERROR_SUCCESS && output && capture!=input::BmxNativeCapture::none) {
        if(capture==input::BmxNativeCapture::interaction)filter_pad(outer,user,output);
        input::bmx_filter_native_pad(output->Gamepad,capture);
        // The game must notice a neutral state even if the physical packet did
        // not change between opening the menu and its next controller poll.
        output->dwPacketNumber ^= input::bmx_native_packet_tag(capture);
    } else if (result == ERROR_SUCCESS) {
        filter_pad(outer, user, output);
    }
    if(result==ERROR_SUCCESS&&output) {
        apply_private_native_board_input(outer,user,raw_pad,output);
        apply_private_bmx_native_input_handoff(outer,user,raw_packet,raw_pad,output);
        apply_human_native_handoff(outer,user,raw_packet,raw_pad,output);
    }
    return result;
}
template<std::size_t I>
DWORD WINAPI captured_keystroke(DWORD user, DWORD reserved, PXINPUT_KEYSTROKE output) {
    const auto result = original<DWORD(WINAPI*)(DWORD, DWORD, PXINPUT_KEYSTROKE)>(
        input_hooks().xinput_keystroke[I])(user, reserved, output);
    if (result == ERROR_SUCCESS && output && !input::bmx_native_dpad_keystroke(output->VirtualKey,polled_capture())) {
        *output = {}; return ERROR_EMPTY;
    }
    return result;
}

template<std::size_t I>
HRESULT STDMETHODCALLTYPE captured_device_format(void* device, const DIDATAFORMAT* format) {
    const auto result = original<HRESULT(STDMETHODCALLTYPE*)(void*, const DIDATAFORMAT*)>(
        input_hooks().device_format[I])(device, format);
    if (SUCCEEDED(result) && format && format->dwSize == sizeof(DIDATAFORMAT) &&
        format->dwObjSize == sizeof(DIOBJECTDATAFORMAT) && format->dwNumObjs <= 1024 &&
        (!format->dwNumObjs || format->rgodf)) {
        InputCaptureHooks::Format copy;
        copy.size = format->dwDataSize;
        bool absolute_axis=false,pov=false;
        for (DWORD i = 0; i < format->dwNumObjs; ++i) {
            const auto& object=format->rgodf[i];copy.objects.push_back(object);
            absolute_axis=absolute_axis||(object.dwType&DIDFT_ABSAXIS)!=0;
            pov=pov||(object.dwType&DIDFT_POV)!=0;
            if(object.pguid&&IsEqualGUID(*object.pguid,GUID_Button)&&
                input::bmx_north_button((object.dwType&DIDFT_BUTTON)!=0,DIDFT_GETINSTANCE(object.dwType)))
                copy.north_offsets.push_back(object.dwOfs);
        }
        // Typed joystick axes+POV distinguish a pad from keyboard scan-code
        // buttons and relative mouse buttons. No state-size-only inference.
        copy.gamepad=absolute_axis&&pov;
        std::lock_guard lock(input_hooks().formats_mutex);
        input_hooks().formats[device] = std::move(copy);
    }
    return result;
}
template<std::size_t I>
HRESULT STDMETHODCALLTYPE captured_device_state(void* device, DWORD size, LPVOID data) {
    const auto result = original<HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPVOID)>(
        input_hooks().device_state[I])(device, size, data);
    note_prompt_read(prompt_direct);
    const auto capture=polled_capture();
    if (SUCCEEDED(result) && data && capture!=input::BmxNativeCapture::none) {
        std::lock_guard lock(input_hooks().formats_mutex);
        const auto found = input_hooks().formats.find(device);
        if(capture==input::BmxNativeCapture::interaction) {
            if(found!=input_hooks().formats.end()&&found->second.gamepad&&found->second.size==size)
                for(const auto offset:found->second.north_offsets)(void)input::bmx_clear_north_byte(data,size,offset);
            return result;
        }
        // Four POVs are the standard DIJOYSTATE/2 layout. Preserve only typed
        // POV values; buttons/axes/padding always receive the normal neutral.
        std::array<std::pair<DWORD,DWORD>,4> povs{};std::size_t pov_count{};
        if(capture==input::BmxNativeCapture::controls) {
            const auto remember=[&](DWORD offset) {
                if(offset>size||size-offset<sizeof(DWORD)||pov_count==povs.size())return;
                DWORD value{};std::memcpy(&value,static_cast<std::byte*>(data)+offset,sizeof(value));
                povs[pov_count++]={offset,input::bmx_native_pov(value,capture)};
            };
            if(found!=input_hooks().formats.end()&&found->second.size==size) {
                for(const auto& object:found->second.objects)if(object.dwType&DIDFT_POV)remember(object.dwOfs);
            } else if(size==sizeof(DIJOYSTATE)||size==sizeof(DIJOYSTATE2)) {
                for(DWORD i=0;i<4;++i)remember(static_cast<DWORD>(offsetof(DIJOYSTATE,rgdwPOV))+i*sizeof(DWORD));
            }
        }
        std::memset(data, 0, size);
        if (found != input_hooks().formats.end() && found->second.size == size) {
            for (const auto& object : found->second.objects) {
                if (object.dwOfs > size || size - object.dwOfs < 4) continue;
                DWORD neutral = 0;
                if (object.dwType & DIDFT_POV) neutral = 0xffffffffu;
                else if (object.dwType & DIDFT_ABSAXIS) {
                    neutral = 32767; // Standard DirectInput axis midpoint if range lookup fails.
                    DIPROPRANGE range{};
                    range.diph = {sizeof(range), sizeof(range.diph), object.dwOfs, DIPH_BYOFFSET};
                    auto** vtable = *reinterpret_cast<void***>(device);
                    using GetProperty = HRESULT(STDMETHODCALLTYPE*)(void*, REFGUID, LPDIPROPHEADER);
                    if (SUCCEEDED(reinterpret_cast<GetProperty>(vtable[5])(device, DIPROP_RANGE, &range.diph)))
                        neutral = static_cast<DWORD>((static_cast<std::int64_t>(range.lMin) + range.lMax) / 2);
                } else continue;
                std::memcpy(static_cast<std::byte*>(data) + object.dwOfs, &neutral, 4);
            }
        } else if (size == sizeof(DIJOYSTATE) || size == sizeof(DIJOYSTATE2)) {
            // A zeroed DirectInput axis is full travel, not the center. Some
            // controllers use the standard joystick layout without a format
            // record we can inspect; keep those pads neutral while captured.
            auto* bytes = static_cast<std::byte*>(data);
            const DWORD center = 32767;
            const DWORD released_pov = 0xffffffffu;
            for (std::size_t offset = 0; offset < 8 * sizeof(DWORD); offset += sizeof(DWORD))
                std::memcpy(bytes + offset, &center, sizeof(center));
            for (std::size_t offset = 8 * sizeof(DWORD); offset < 12 * sizeof(DWORD); offset += sizeof(DWORD))
                std::memcpy(bytes + offset, &released_pov, sizeof(released_pov));
        }
        for(std::size_t i=0;i<pov_count;++i)std::memcpy(static_cast<std::byte*>(data)+povs[i].first,&povs[i].second,sizeof(DWORD));
    }
    return result;
}
template<std::size_t I>
HRESULT STDMETHODCALLTYPE captured_device_data(void* device, DWORD size, LPDIDEVICEOBJECTDATA data, LPDWORD count, DWORD flags) {
    const auto capture = polled_capture();
    const auto capacity=count?*count:0;
    const auto result = original<HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD)>(
        input_hooks().device_data[I])(device, size, data, count,
            capture!=input::BmxNativeCapture::none&&capture!=input::BmxNativeCapture::interaction?flags&~DIGDD_PEEK:flags);
    if (SUCCEEDED(result) && capture!=input::BmxNativeCapture::none && count) {
        if(capture==input::BmxNativeCapture::interaction) {
            if(data&&*count<=capacity&&*count<=1024&&(size==sizeof(DIDEVICEOBJECTDATA)||size==4*sizeof(DWORD))) {
                std::lock_guard lock(input_hooks().formats_mutex);
                const auto found=input_hooks().formats.find(device);
                if(found!=input_hooks().formats.end()&&found->second.gamepad)for(DWORD i=0;i<*count;++i) {
                    auto* record=reinterpret_cast<std::byte*>(data)+i*size;DWORD offset{};
                    std::memcpy(&offset,record,sizeof(offset));
                    const bool north=offset<found->second.size&&
                        std::find(found->second.north_offsets.begin(),found->second.north_offsets.end(),offset)!=found->second.north_offsets.end();
                    if(north){const DWORD released=0;std::memcpy(record+sizeof(DWORD),&released,sizeof(released));}
                }
            }
            return result;
        }
        DWORD kept{};
        if(capture==input::BmxNativeCapture::controls&&data&&*count<=capacity&&*count<=1024&&
           (size==sizeof(DIDEVICEOBJECTDATA)||size==4*sizeof(DWORD))) {
            std::lock_guard lock(input_hooks().formats_mutex);
            const auto found=input_hooks().formats.find(device);
            if(found!=input_hooks().formats.end())for(DWORD i=0;i<*count;++i) {
                auto* record=reinterpret_cast<std::byte*>(data)+i*size;DWORD offset{},value{};
                std::memcpy(&offset,record,sizeof(offset));
                const bool pov=std::any_of(found->second.objects.begin(),found->second.objects.end(),[&](const auto& object) {
                    return (object.dwType&DIDFT_POV)&&object.dwOfs==offset&&offset<=found->second.size&&found->second.size-offset>=sizeof(DWORD);
                });
                if(!pov)continue;
                std::memcpy(&value,record+sizeof(DWORD),sizeof(value));value=input::bmx_native_pov(value,capture);
                auto* target=reinterpret_cast<std::byte*>(data)+kept*size;
                if(target!=record)std::memmove(target,record,size);
                std::memcpy(target+sizeof(DWORD),&value,sizeof(value));++kept;
            }
        }
        *count=kept;
    }
    return result;
}
bool install_device_capture(void* device) {
    auto** table = *reinterpret_cast<void***>(device);
    auto& hooks = input_hooks();
    const auto states = []<std::size_t... I>(std::index_sequence<I...>) {
        return std::array{reinterpret_cast<void*>(captured_device_state<I>)...};
    }(std::make_index_sequence<InputCaptureHooks::device_slots>{});
    const auto data = []<std::size_t... I>(std::index_sequence<I...>) {
        return std::array{reinterpret_cast<void*>(captured_device_data<I>)...};
    }(std::make_index_sequence<InputCaptureHooks::device_slots>{});
    const auto formats = []<std::size_t... I>(std::index_sequence<I...>) {
        return std::array{reinterpret_cast<void*>(captured_device_format<I>)...};
    }(std::make_index_sequence<InputCaptureHooks::device_slots>{});
    const auto add = [&](auto& family, void* target, const auto& detours) {
        for (const auto& hook : family) if (hook.target == target) return true;
        for (std::size_t slot = 0; slot < family.size(); ++slot)
            if (!family[slot].target) return install(family[slot], target, detours[slot]);
        return false;
    };
    // Keep installing independent methods if one fails. Report the exact gap.
    const bool state_ready = add(hooks.device_state, table[9], states);
    const bool data_ready = add(hooks.device_data, table[10], data);
    const bool format_ready = add(hooks.device_format, table[11], formats);
    if (!state_ready || !data_ready || !format_ready)
        logging::log(logging::Level::warning, logging::Channel::input,
            "DirectInput capture incomplete: state={}, buffered={}, format={}; mouse input may reach the game while the menu is open.",
            state_ready, data_ready, format_ready);
    std::lock_guard formats_lock(hooks.formats_mutex);
    hooks.formats.erase(device); // COM addresses can be reused after Release.
    return state_ready && data_ready && format_ready;
}

template<std::size_t I>
HRESULT STDMETHODCALLTYPE captured_device_create(void* owner, REFGUID guid, void** output, LPUNKNOWN outer) {
    const auto result = original<HRESULT(STDMETHODCALLTYPE*)(void*, REFGUID, void**, LPUNKNOWN)>(
        input_hooks().device_create[I])(owner, guid, output, outer);
    if (SUCCEEDED(result) && output && *output) {
        std::lock_guard lock(state().hook_mutex);
        install_device_capture(*output);
    }
    return result;
}

bool prepare_standard_devices(HMODULE direct) {
    using Create = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, void**, IUnknown*);
    const auto create = reinterpret_cast<Create>(GetProcAddress(direct, "DirectInput8Create"));
    if (!create) return false;
    bool ready = true;
    std::array<void*, 2> create_targets{};
    std::size_t create_count{};
    for (const auto& iid : {IID_IDirectInput8A, IID_IDirectInput8W}) {
        ComPtr<IUnknown> owner;
        if (FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, iid,
            reinterpret_cast<void**>(owner.GetAddressOf()), nullptr))) { ready = false; continue; }
        auto** table = *reinterpret_cast<void***>(owner.Get());
        create_targets[create_count++] = table[3];
        using CreateDevice = HRESULT(STDMETHODCALLTYPE*)(void*, REFGUID, void**, IUnknown*);
        for (const auto& guid : {GUID_SysMouse, GUID_SysKeyboard}) {
            ComPtr<IUnknown> device;
            if (FAILED(reinterpret_cast<CreateDevice>(table[3])(owner.Get(), guid,
                reinterpret_cast<void**>(device.GetAddressOf()), nullptr))) { ready = false; continue; }
            // No Acquire or cooperative-level change: these temporary objects
            // identify shared implementations before the game starts workers.
            if (!install_device_capture(device.Get())) ready = false;
        }
    }
    auto& family = input_hooks().device_create;
    const std::array detours{reinterpret_cast<void*>(captured_device_create<0>),
        reinterpret_cast<void*>(captured_device_create<1>)};
    for (std::size_t i = 0; i < create_count; ++i) {
        if (std::any_of(family.begin(), family.end(), [&](const auto& hook) { return hook.target == create_targets[i]; })) continue;
        const auto slot = std::find_if(family.begin(), family.end(), [](const auto& hook) { return !hook.target; });
        if (slot == family.end() || !install(*slot, create_targets[i], detours[slot - family.begin()])) ready = false;
    }
    return ready;
}

HRESULT WINAPI captured_direct_create(HINSTANCE instance, DWORD version, REFIID iid, LPVOID* output, LPUNKNOWN outer) {
    const auto result = original<HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN)>(
        input_hooks().direct_create)(instance, version, iid, output, outer);
    if (SUCCEEDED(result) && output && *output) {
        auto** table = *reinterpret_cast<void***>(*output);
        std::lock_guard lock(state().hook_mutex);
        auto& hooks = input_hooks();
        // A/W implementations sometimes share their callable address.
        for (const auto& hook : hooks.device_create) if (hook.target == table[3]) return result;
        if (!hooks.device_create[0].target)
            install(hooks.device_create[0], table[3], reinterpret_cast<void*>(captured_device_create<0>));
        else install(hooks.device_create[1], table[3], reinterpret_cast<void*>(captured_device_create<1>));
    }
    return result;
}

namespace {
// `owns_pointer` is asked only when the packet moves a cursor Windows is not moving.
template<class OwnsPointer> void note_packet(const RAWMOUSE& mouse, OwnsPointer&& owns_pointer) {
    auto& s = state();
    const bool relative = !(mouse.usFlags & MOUSE_MOVE_ABSOLUTE);
    if (relative) {
        s.raw_mouse_x.fetch_add(mouse.lLastX);
        s.raw_mouse_y.fetch_add(mouse.lLastY);
    }
    auto buttons = s.raw_mouse_buttons.load();
    const auto flags = mouse.usButtonFlags;
    if (flags & RI_MOUSE_LEFT_BUTTON_DOWN) buttons |= 1u;
    if (flags & RI_MOUSE_LEFT_BUTTON_UP) buttons &= ~1u;
    if (flags & RI_MOUSE_RIGHT_BUTTON_DOWN) buttons |= 2u;
    if (flags & RI_MOUSE_RIGHT_BUTTON_UP) buttons &= ~2u;
    if (flags & RI_MOUSE_MIDDLE_BUTTON_DOWN) buttons |= 4u;
    if (flags & RI_MOUSE_MIDDLE_BUTTON_UP) buttons &= ~4u;
    s.raw_mouse_buttons.store(buttons);
    s.raw_mouse_time.store(GetTickCount64());
    // Windows is not moving the cursor (RIDEV_NOLEGACY): move it for the
    // overlay so its pointer and the editor's hover keep working.
    if (relative && (mouse.lLastX || mouse.lLastY) && s.mouse_legacy_blocked.load() && owns_pointer()) {
        OverlayInputAccess access;
        POINT cursor{};
        if (GetCursorPos(&cursor)) SetCursorPos(cursor.x + mouse.lLastX, cursor.y + mouse.lLastY);
    }
}
} // namespace

void note_raw_mouse_packet(const RAWMOUSE& mouse) {
    note_packet(mouse, [] { return owns_menu_cursor(state()); });
}

void note_raw_mouse_packet(const RAWMOUSE& mouse, bool overlay_owns_pointer) {
    note_packet(mouse, [overlay_owns_pointer] { return overlay_owns_pointer; });
}

void sync_raw_mouse_registration() {
    sync_raw_mouse_registration(owns_menu_cursor(state()));
}

void sync_raw_mouse_registration(bool overlay_owns_pointer) {
    try {
        auto& mouse = raw_mouse();
        std::lock_guard lock(mouse.mutex);
        const bool overlay = overlay_owns_pointer;
        if (overlay == mouse.overridden) return;
        mouse.overridden = overlay;
        // Only a registration that actually blocks the cursor needs replacing.
        if (!mouse.known || !(mouse.game.dwFlags & cursor_blocking_flags)) return;
        apply_raw_mouse(mouse, overlay);
    } catch (...) {}
}

bool install_input_capture() {
    bind_private_board_input_bridge();
    auto& h = input_hooks();
    const auto hid = LoadLibraryExW(L"hid.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto kernel = GetModuleHandleW(L"kernel32.dll");
    if (!hid || !install(h.hid_attributes, reinterpret_cast<void*>(GetProcAddress(hid, "HidD_GetAttributes")),
            reinterpret_cast<void*>(captured_hid_attributes))) return false;
    const auto kernel_hook = [&](Hook& hook, const char* name, void* detour) {
        return install(hook, reinterpret_cast<void*>(GetProcAddress(kernel, name)), detour);
    };
    if (!kernel_hook(h.read_file, "ReadFile", reinterpret_cast<void*>(captured_read_file)) ||
        !kernel_hook(h.overlapped, "GetOverlappedResult", reinterpret_cast<void*>(captured_overlapped)) ||
        !kernel_hook(h.overlapped_ex, "GetOverlappedResultEx", reinterpret_cast<void*>(captured_overlapped_ex)) ||
        !kernel_hook(h.completion, "GetQueuedCompletionStatus", reinterpret_cast<void*>(captured_completion)) ||
        !kernel_hook(h.completion_ex, "GetQueuedCompletionStatusEx", reinterpret_cast<void*>(captured_completion_ex)) ||
        !kernel_hook(h.close_handle, "CloseHandle", reinterpret_cast<void*>(captured_close_handle))) return false;
    const auto user32 = GetModuleHandleW(L"user32.dll");
    const auto add = [&](Hook& hook, const char* name, void* detour) {
        const bool okay = install(hook, reinterpret_cast<void*>(GetProcAddress(user32, name)), detour);
        if (!okay) dingosdk::logging::write(dingosdk::logging::Level::warning, dingosdk::logging::Channel::input, name);
        return okay;
    };
    add(h.register_raw, "RegisterRawInputDevices", reinterpret_cast<void*>(captured_register_raw));
    if (!add(h.async_key, "GetAsyncKeyState", reinterpret_cast<void*>(captured_async_key)) ||
        !add(h.key, "GetKeyState", reinterpret_cast<void*>(captured_key)) ||
        !add(h.keyboard, "GetKeyboardState", reinterpret_cast<void*>(captured_keyboard)) ||
        !add(h.raw_data, "GetRawInputData", reinterpret_cast<void*>(captured_raw_data)) ||
        !add(h.raw_buffer, "GetRawInputBuffer", reinterpret_cast<void*>(captured_raw_buffer)) ||
        !add(h.capture, "SetCapture", reinterpret_cast<void*>(captured_capture)) ||
        !add(h.release_capture, "ReleaseCapture", reinterpret_cast<void*>(captured_release_capture)) ||
        !add(h.cursor, "SetCursor", reinterpret_cast<void*>(captured_cursor)) ||
        !add(h.cursor_position, "GetCursorPos", reinterpret_cast<void*>(captured_cursor_position)) ||
        !add(h.show_cursor, "ShowCursor", reinterpret_cast<void*>(captured_show_cursor))) return false;
    // Some Windows builds alias both cursor-position exports to one function,
    // which is already intercepted by the overlay's SetCursorPos hook.
    const auto physical = GetProcAddress(user32, "SetPhysicalCursorPos");
    if (physical && physical != GetProcAddress(user32, "SetCursorPos") &&
        !add(h.physical_cursor, "SetPhysicalCursorPos", reinterpret_cast<void*>(captured_physical_cursor))) return false;
    const std::array<void*, 5> states{reinterpret_cast<void*>(captured_xinput<0>), reinterpret_cast<void*>(captured_xinput<1>),
        reinterpret_cast<void*>(captured_xinput<2>), reinterpret_cast<void*>(captured_xinput<3>), reinterpret_cast<void*>(captured_xinput<4>)};
    const std::array<void*, 5> extended{reinterpret_cast<void*>(captured_xinput<0, true>), reinterpret_cast<void*>(captured_xinput<1, true>),
        reinterpret_cast<void*>(captured_xinput<2, true>), reinterpret_cast<void*>(captured_xinput<3, true>), reinterpret_cast<void*>(captured_xinput<4, true>)};
    const std::array<void*, 5> strokes{reinterpret_cast<void*>(captured_keystroke<0>), reinterpret_cast<void*>(captured_keystroke<1>),
        reinterpret_cast<void*>(captured_keystroke<2>), reinterpret_cast<void*>(captured_keystroke<3>), reinterpret_cast<void*>(captured_keystroke<4>)};
    const std::array names{L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll", L"xinput1_2.dll", L"xinput1_1.dll"};
    std::vector<void*> seen;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto module = LoadLibraryExW(names[i], nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) continue;
        const auto hook_export = [&](Hook& hook, const char* name, void* detour) {
            const auto target = reinterpret_cast<void*>(GetProcAddress(module, name));
            if (!target || std::find(seen.begin(), seen.end(), target) != seen.end()) return true;
            if (!install(hook, target, detour)) { dingosdk::logging::printf(dingosdk::logging::Level::error, dingosdk::logging::Channel::input, "XInput input hook failed: module=%zu target=%p", i, target); return false; }
            seen.push_back(target); return true;
        };
        if (!hook_export(h.xinput_state[i], "XInputGetState", states[i]) ||
            !hook_export(h.xinput_extended[i], MAKEINTRESOURCEA(100), extended[i]) ||
            !hook_export(h.xinput_keystroke[i], "XInputGetKeystroke", strokes[i])) return false;
    }
    const auto direct = LoadLibraryExW(L"dinput8.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (direct) {
        if (prepare_standard_devices(direct))
            logging::write(logging::Level::info, logging::Channel::input, "DirectInput mouse/keyboard capture prepared before game startup.");
        else logging::write(logging::Level::warning, logging::Channel::input,
            "DirectInput startup preparation incomplete; device creation will retry the missing hooks.");
        if (!install(h.direct_create, reinterpret_cast<void*>(GetProcAddress(direct, "DirectInput8Create")),
            reinterpret_cast<void*>(captured_direct_create))) return false;
    }
    dingosdk::logging::write(dingosdk::logging::Level::info, dingosdk::logging::Channel::input, "Menu input capture installed: keyboard, raw input, XInput, DirectInput, PlayStation HID and cursor ownership.");
    return true;
}

}

extern "C" void DingoSDKOverlaySetGamepadFilter(dingosdk::overlay::GamepadFilter filter) {
    dingosdk::overlay::detail::gamepad_filter().store(filter, std::memory_order_release);
}

extern "C" void DingoSDKOverlaySetBmxInputCapture(bool active) {
    using namespace dingosdk::overlay::detail;
    const auto& s = state();
    const bool alive = s.started.load(std::memory_order_relaxed) && !s.stop.load(std::memory_order_relaxed) &&
        !s.failed.load(std::memory_order_relaxed);
    bmx_capture_until.store(active && alive ? GetTickCount64() + dingosdk::input::bmx_capture_lease_ms : 0,
        std::memory_order_release);
}

extern "C" void DingoSDKOverlaySetBmxInteractionCapture(bool active) {
    using namespace dingosdk::overlay::detail;
    const auto& s=state();
    const bool alive=s.started.load(std::memory_order_relaxed)&&!s.stop.load(std::memory_order_relaxed)&&
        !s.failed.load(std::memory_order_relaxed);
    bmx_interaction_until.store(active&&alive?GetTickCount64()+dingosdk::input::bmx_capture_lease_ms:0,
        std::memory_order_release);
}

extern "C" void DingoSDKOverlaySetBmxQuickSwitchCapture(bool active) {
    using namespace dingosdk::overlay::detail;
    const auto& s=state();
    const bool alive=s.started.load(std::memory_order_relaxed)&&!s.stop.load(std::memory_order_relaxed)&&
        !s.failed.load(std::memory_order_relaxed);
    bmx_quick_switch_until.store(active&&alive?GetTickCount64()+dingosdk::input::bmx_capture_lease_ms:0,
        std::memory_order_release);
}
