// Included inside overlay::detail, after the unchanged NativeOnly input bridge.
namespace handoff=bmx::native_input_handoff;
struct BmxOriginalSlot {std::uint64_t wall{};bool success{};bridge_policy::Pad raw{};DWORD packet{};};
struct BmxHandoffInputReport {PrivateBoardInputReport result;bridge_policy::Pad original_raw;DWORD original_packet{};};
struct PrivateBmxInputHandoff {
    std::atomic<bool> bound{},requested{};SRWLOCK lock=SRWLOCK_INIT;
    handoff::Session session;bridge_policy::Pulse pulse;bridge_policy::PacketContinuity packets;
    std::array<BmxOriginalSlot,4> original{};
    std::array<BmxHandoffInputReport,129> reports{};unsigned count{};
    std::uint64_t translated{},terminal_returns{};BmxHandoffInputReport latest_terminal{};bool terminal_recorded{};
};
PrivateBmxInputHandoff& private_bmx_handoff() {static PrivateBmxInputHandoff s;return s;}
void bind_private_bmx_native_input_handoff() noexcept {
    auto& s=private_bmx_handoff();if(!s.bound.exchange(true))s.requested=bridge_environment_one(L"RESKATE_BMX_NATIVE_INPUT_HANDOFF");
}
bool private_bmx_native_input_handoff_requested() noexcept {return private_bmx_handoff().requested.load();}
bool handoff_absent_or_zero(const wchar_t* name) noexcept {
    return bridge_environment_exact(name,L"")||bridge_environment_exact(name,L"0");
}
bool private_bmx_handoff_configuration(bool completed_observer_allowed=false) noexcept {
    namespace trace=bmx::board_handoff_trace;
    return private_bmx_native_input_handoff_requested()&&bridge_environment_one(L"RESKATE_BMX_NATIVE_INPUT_HANDOFF")&&
        bridge_environment_one(L"RESKATE_BMX_NATIVE_CHASSIS")&&bridge_environment_one(L"RESKATE_BMX_QUICK_SWITCH_TEST")&&
        bridge_environment_one(L"RESKATE_BMX_AUTOMATED_SWITCH")&&bridge_environment_exact(L"RESKATE_BMX_LAUNCH_MODE",L"switch-test")&&
        handoff_absent_or_zero(L"RESKATE_BMX_NATIVE_BOARD_INPUT_BRIDGE")&&
        handoff_absent_or_zero(L"RESKATE_BMX_NATIVE_BOARD_CONTROL")&&handoff_absent_or_zero(L"RESKATE_BMX_NATIVE_BOARD_DEFERRED_ARM")&&
        trace::selected()&&trace::store.mode==trace::Mode::quick_switch&&!trace::store.automated_native_board&&
        !trace::store.deferred_native_board_arm&&(!trace::store.sealed.load()||
            (completed_observer_allowed&&std::string_view(trace::store.stop.load())=="observed_board100_dwell_complete_not_a_pose_pass"));
}
bool private_bmx_raw_quiet(const bridge_policy::Pad& p) noexcept {
    return !p.buttons&&p.left_trigger<=12&&p.right_trigger<=12&&std::abs(int(p.left_x))<=1638&&
        std::abs(int(p.left_y))<=1638&&std::abs(int(p.right_x))<=1638&&std::abs(int(p.right_y))<=1638;
}
// Original result only, before any filter, including SDK bypass observations.
// No extra XInput call; an absent/failed original result never becomes a pad.
void note_private_bmx_original_xinput(DWORD user,DWORD result,const XINPUT_STATE* output) noexcept {
    auto& s=private_bmx_handoff();if(!s.requested.load()||user>=s.original.size())return;
    const auto error=GetLastError();AcquireSRWLockExclusive(&s.lock);
    auto& o=s.original[user];o.wall=GetTickCount64();o.success=result==ERROR_SUCCESS&&output;
    o.raw=o.success?private_board_pad(output->Gamepad):bridge_policy::Pad{};
    o.packet=o.success?output->dwPacketNumber:0;
    if(s.session.live()&&((user==0&&!o.success)||(user!=0&&o.success))) {
        s.session.abort("original_xinput_source_disconnected_or_extra_slot");
        if(s.pulse.live())s.pulse.abort("original_xinput_source_disconnected_or_extra_slot");
    }
    ReleaseSRWLockExclusive(&s.lock);SetLastError(error);
}
bool begin_private_bmx_native_input_handoff(const handoff::Ticket& ticket,std::uint64_t now) noexcept {
    auto& s=private_bmx_handoff();if(!s.requested.load())return false;
    const auto error=GetLastError();AcquireSRWLockExclusive(&s.lock);
    const bool accepted=private_bmx_handoff_configuration()&&s.session.begin(ticket,now);
    ReleaseSRWLockExclusive(&s.lock);SetLastError(error);return accepted;
}
void cancel_private_bmx_native_input_handoff(const char* reason) noexcept {
    auto& s=private_bmx_handoff();AcquireSRWLockExclusive(&s.lock);
    s.session.abort(reason);if(s.pulse.live())s.pulse.abort(reason);
    ReleaseSRWLockExclusive(&s.lock);
}
void update_private_bmx_native_input_handoff(handoff::Sample sample) noexcept {
    auto& s=private_bmx_handoff();if(!s.requested.load())return;
    const auto error=GetLastError();AcquireSRWLockExclusive(&s.lock);
    if(!s.session.live()){ReleaseSRWLockExclusive(&s.lock);SetLastError(error);return;}
    sample.configuration=sample.configuration&&private_bmx_handoff_configuration(
        s.session.phase()==handoff::Phase::observing&&s.session.saw103()&&s.session.saw100());
    sample.capture_released=sample.capture_released&&polled_capture()==input::BmxNativeCapture::none&&
        !input::bmx_capture_live(bmx_quick_switch_until.load(),sample.now,true);
    const auto& slot=s.original[0];sample.original_slot0_fresh=slot.success&&slot.wall&&sample.now>=slot.wall&&sample.now-slot.wall<=handoff::freshness_ms;
    sample.original_quiet=slot.success&&private_bmx_raw_quiet(slot.raw);
    sample.only_slot0=true;
    for(unsigned i=1;i<s.original.size();++i)if(s.original[i].success&&sample.now>=s.original[i].wall&&
        sample.now-s.original[i].wall<=handoff::freshness_ms)sample.only_slot0=false;
    const bool ready=s.session.observe(sample);
    if(ready) {
        // Existing quick observer starts only after the initiating SDK chord,
        // its capture and all cleanup have been independently released.
        bmx::board_handoff_trace::arm_quick_switch();
        if(!bmx::board_handoff_trace::store.armed.load())s.session.abort("passive_quick_observer_failed_to_arm");
        else if(!s.pulse.queue(sample.now,0)||!s.session.pulse_queued())s.session.abort("one_shot_pulse_queue_refused");
    }
    if(s.session.phase()==handoff::Phase::aborted&&s.pulse.live())s.pulse.abort(s.session.issue());
    ReleaseSRWLockExclusive(&s.lock);SetLastError(error);
}
handoff::Phase private_bmx_native_input_handoff_phase() noexcept {
    auto& s=private_bmx_handoff();AcquireSRWLockShared(&s.lock);const auto phase=s.session.phase();ReleaseSRWLockShared(&s.lock);return phase;
}
void apply_private_bmx_native_input_handoff(bool outer,DWORD user,DWORD original_packet,const XINPUT_GAMEPAD& raw,XINPUT_STATE* output) noexcept {
    auto& s=private_bmx_handoff();if(!s.requested.load()||!outer||!output||user!=0)return;
    const auto error=GetLastError();AcquireSRWLockExclusive(&s.lock);
    const auto now=GetTickCount64();const auto before_packet=output->dwPacketNumber;const auto before_pad=private_board_pad(output->Gamepad);
    bridge_policy::Result result;std::uint32_t physics{};bool physics_valid{};
    if(s.pulse.live()) {
        if(s.count>=128)s.session.abort("input_report_capacity_exhausted");
        const auto& owner=s.session.ticket().owner;
        NoBailSkater a,b;std::uintptr_t client{};bool protected_a{},protected_b{};
        const auto current_owner=[&](const NoBailSkater& o){return handoff::Owner{o.base,client,o.entity,o.core,o.context,o.rig};};
        bool identity=s.session.callback_snapshot_ready(now)&&private_bmx_handoff_configuration()&&
            try_no_bail_skater(0,a,client)&&current_owner(a)==owner&&
            inspect_no_bail_owner(client,a.entity,b,protected_a)&&current_owner(b)==owner&&!protected_a;
        const auto& os=state();const auto window=os.window.load();
        bool escape{};{OverlayInputAccess access;escape=(GetAsyncKeyState(VK_ESCAPE)&0x8000)!=0;}
        const bool focused=window&&game_window_foreground(window)&&os.started.load()&&os.input_attached.load()&&!os.stop.load()&&!os.failed.load()&&!escape;
        const bool no_capture=polled_capture()==input::BmxNativeCapture::none&&
            !input::bmx_capture_live(bmx_quick_switch_until.load(),now,true);
        physics_valid=identity&&memory::peek(a.context+0x1414,physics);
        identity=identity&&physics_valid&&inspect_no_bail_owner(client,a.entity,b,protected_b)&&current_owner(b)==owner&&!protected_b;
        const bool quiet=private_bmx_raw_quiet(private_board_pad(raw));
        const bool ready=identity&&focused&&no_capture&&bmx::board_handoff_trace::store.armed.load()&&s.session.latest().sdk_quiet;
        result=s.pulse.poll(now,user,identity,focused,ready,quiet,physics,false,"post_bmx_callback_readiness_lost");
        if(result.rewrite)output->Gamepad.wButtons=static_cast<WORD>((output->Gamepad.wButtons&~0x8000u)|(result.north?0x8000u:0));
        if(s.pulse.phase==bridge_policy::Phase::aborted)s.session.abort(s.pulse.issue);
        else if(s.pulse.phase==bridge_policy::Phase::complete)s.session.pulse_complete(true);
    }
    if(s.packets.active||result.rewrite) {
        const auto final_pad=private_board_pad(output->Gamepad);output->dwPacketNumber=s.packets.translate(before_packet,final_pad);
        const bool terminal=!s.pulse.live();const BmxHandoffInputReport row{{now,++s.translated,static_cast<unsigned>(s.pulse.phase),
            user,before_packet,output->dwPacketNumber,before_pad,final_pad,physics,result.rewrite,true,terminal,physics_valid},private_board_pad(raw),original_packet};
        if(result.rewrite&&s.count<128)s.reports[s.count++]=row;
        if(terminal){++s.terminal_returns;s.latest_terminal=row;if(!s.terminal_recorded&&s.count<s.reports.size()){
            s.reports[s.count++]=row;s.terminal_recorded=true;}}
    }
    ReleaseSRWLockExclusive(&s.lock);SetLastError(error);
}
std::string private_bmx_native_input_handoff_status() {
    auto& s=private_bmx_handoff();AcquireSRWLockShared(&s.lock);
    const auto session=s.session;const auto pulse=s.pulse;const auto packets=s.packets;const auto reports=s.reports;
    const auto original=s.original;const auto count=s.count;const auto translated=s.translated,terminal=s.terminal_returns;
    const auto last=s.latest_terminal;const bool terminal_recorded=s.terminal_recorded;ReleaseSRWLockShared(&s.lock);
    const auto pad=[](const bridge_policy::Pad& p){return Json::array({p.buttons,p.left_trigger,p.right_trigger,p.left_x,p.left_y,p.right_x,p.right_y});};
    const auto report=[&](const BmxHandoffInputReport& offered){const auto& r=offered.result;return Json{{"wall_ms",r.wall},{"ordinal",r.ordinal},{"phase",r.phase},
        {"slot",r.user},{"before_packet",r.before_packet},{"after_packet",r.after_packet},{"before_pad",pad(r.before_pad)},
        {"after_pad",pad(r.after_pad)},{"original_packet",offered.original_packet},{"original_raw_pad",pad(offered.original_raw)},
        {"original_return_success",true},{"physics",r.physics_observed?Json(r.physics):Json(nullptr)},{"offer",r.pulse_offer},{"terminal",r.terminal}};};
    Json rows=Json::array();for(unsigned i=0;i<count;++i)rows.push_back(report(reports[i]));
    Json sources=Json::array();for(unsigned i=0;i<original.size();++i)sources.push_back(Json{{"slot",i},{"wall_ms",original[i].wall},
        {"original_success",original[i].success},{"raw_pad",pad(original[i].raw)},{"original_packet",original[i].packet}});
    const auto& ticket=session.ticket();const auto& latest=session.latest();
    return Json{{"schema",1},{"scope","PRIVATE_one_shot_post_BMX_original_XInput_slot0"},{"sdk_trigger","synthetic_quick_switch_prefix"},
        {"phase",static_cast<unsigned>(session.phase())},{"issue",session.issue()},{"owner",ticket.owner},
        {"body",Json::array({std::uint64_t(ticket.body.world),std::uint64_t(ticket.body.state),std::uint64_t(ticket.body.index),std::uint64_t(ticket.body.generation),ticket.body.token})},
        {"cleanup_counters",ticket.counters},{"latest_counters",latest.counters},{"deadline_ms",ticket.deadline_ms},
        {"latest_native_serial",latest.serial},{"latest_native_time",latest.native_time},{"latest_physics",latest.physics},
        {"native103_observed",session.saw103()},{"native100_observed",session.saw100()},
        {"pulse_phase",static_cast<unsigned>(pulse.phase)},{"pulse_issue",pulse.issue},{"pulse_started_ms",pulse.started},
        {"press_reports",pulse.press_reports},{"release_reports",pulse.release_reports},{"transaction_limit_ms",bridge_policy::transaction_ms},
        {"translated_returns",translated},{"packet_domain_persistent",packets.active},{"terminal_returns",terminal},
        {"rows",std::move(rows)},{"latest_terminal",terminal_recorded?report(last):Json(nullptr)},
        {"original_source_observations",std::move(sources)},{"input_consumed",nullptr},{"visual_quality",nullptr},
        {"physical_LB_Y_acceptance_verified",false},{"software_board_fallback_enabled",false},{"software_board_requests_observed",nullptr}}.dump();
}
