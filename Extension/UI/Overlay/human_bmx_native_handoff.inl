// Included after the frozen private adapter. Human prototype has its own state,
// actual-slot witnesses and per-slot persistent packet domains.
namespace human=bmx::human_native_handoff;
struct HumanNativeHandoff {
    std::atomic<bool> bound{},requested{};SRWLOCK lock=SRWLOCK_INIT;
    human::Intent intent;human::AdmissionWait admission_wait;human::Return reverse;bool reverse_ledger{};
    struct AdmissionAttempt {
        bool present{},inspection_attempted{},inspection_available{},owner_matches{},policy_called{},accepted{};
        std::uint64_t now{};unsigned device{},config_evaluated{},config_passed{};
        handoff::Owner owner{};BmxSwitchAdmissionProtection protection;
        std::array<human::Original,4> originals{};std::array<bool,4> north_armed{};
        std::array<std::uint64_t,4> north_rises{},spent_rises{};
        const char* reason="not_attempted";
    } admission;
    struct Row {std::uint64_t generation{},wall{};unsigned slot{},phase{};DWORD original_packet{},before_packet{},after_packet{};
        bridge_policy::Pad original_raw{},before{},after{};bool offer{},physics_observed{};std::uint32_t physics{};};
    std::array<Row,129> rows{};unsigned count{};std::uint64_t total{},terminal_returns{};Row latest_terminal{};
};
HumanNativeHandoff& human_native_handoff() {static HumanNativeHandoff s;return s;}
void bind_human_native_handoff() noexcept {
    auto& s=human_native_handoff();if(!s.bound.exchange(true))s.requested=bridge_environment_one(L"RESKATE_BMX_HUMAN_NATIVE_SWITCH");
}
bool human_native_handoff_requested() noexcept {return human_native_handoff().requested.load();}
bool human_native_handoff_configuration() noexcept {
    return human_native_handoff_requested()&&bridge_environment_one(L"RESKATE_BMX_HUMAN_NATIVE_SWITCH")&&
        bridge_environment_one(L"RESKATE_BMX_NATIVE_CHASSIS")&&bridge_environment_exact(L"RESKATE_BMX_LAUNCH_MODE",L"human-ride")&&
        handoff_absent_or_zero(L"RESKATE_BMX_NATIVE_INPUT_HANDOFF")&&handoff_absent_or_zero(L"RESKATE_BMX_NATIVE_BOARD_INPUT_BRIDGE")&&
        handoff_absent_or_zero(L"RESKATE_BMX_QUICK_SWITCH_TEST")&&handoff_absent_or_zero(L"RESKATE_BMX_AUTOMATED_SWITCH")&&
        handoff_absent_or_zero(L"RESKATE_BMX_AUTOMATED_RIDE")&&handoff_absent_or_zero(L"RESKATE_BMX_NATIVE_BOARD_CONTROL")&&
        handoff_absent_or_zero(L"RESKATE_BMX_NATIVE_BOARD_DEFERRED_ARM")&&handoff_absent_or_zero(L"RESKATE_BMX_BOARD_HANDOFF_TRACE")&&
        bridge_environment_exact(L"RESKATE_BMX_BOARD_HANDOFF_MODE",L"")&&handoff_absent_or_zero(L"RESKATE_BMX_RAMP_TEST")&&
        handoff_absent_or_zero(L"RESKATE_BMX_RAMP_RIDE");
}
void note_human_native_original(bool outer,DWORD user,DWORD result,const XINPUT_STATE* output) noexcept {
    auto& s=human_native_handoff();if(!s.requested.load()||!outer||user>3)return;
    const auto error=GetLastError();AcquireSRWLockExclusive(&s.lock);
    const auto now=GetTickCount64();
    s.intent.note(outer,user,result==ERROR_SUCCESS&&output,now,
        result==ERROR_SUCCESS&&output?output->dwPacketNumber:0,
        result==ERROR_SUCCESS&&output?private_board_pad(output->Gamepad):bridge_policy::Pad{});
    s.admission_wait.note(user,result==ERROR_SUCCESS&&output,now,s.intent);
    s.reverse.note(user,result==ERROR_SUCCESS&&output,now,s.intent);
    ReleaseSRWLockExclusive(&s.lock);SetLastError(error);
}
bool admit_human_native_handoff_impl(std::uint64_t now,unsigned device,const handoff::Owner& owner,const human::AdmissionContext* pending,bool resume) noexcept {
    auto& s=human_native_handoff();if(!s.requested.load())return false;
    const auto error=GetLastError();AcquireSRWLockExclusive(&s.lock);
    auto& attempt=s.admission;attempt={};attempt.present=true;attempt.now=now;attempt.device=device;attempt.owner=owner;
    attempt.originals=s.intent.originals();attempt.north_armed=s.intent.north_armed();attempt.north_rises=s.intent.north_rises();attempt.spent_rises=s.intent.spent_rises();
    const auto config_check=[&](unsigned bit,bool passed){attempt.config_evaluated|=1u<<bit;if(passed)attempt.config_passed|=1u<<bit;return passed;};
    const bool configuration=config_check(0,human_native_handoff_requested())&&config_check(1,bridge_environment_one(L"RESKATE_BMX_HUMAN_NATIVE_SWITCH"))&&
        config_check(2,bridge_environment_one(L"RESKATE_BMX_NATIVE_CHASSIS"))&&config_check(3,bridge_environment_exact(L"RESKATE_BMX_LAUNCH_MODE",L"human-ride"))&&
        config_check(4,handoff_absent_or_zero(L"RESKATE_BMX_NATIVE_INPUT_HANDOFF"))&&config_check(5,handoff_absent_or_zero(L"RESKATE_BMX_NATIVE_BOARD_INPUT_BRIDGE"))&&
        config_check(6,handoff_absent_or_zero(L"RESKATE_BMX_QUICK_SWITCH_TEST"))&&config_check(7,handoff_absent_or_zero(L"RESKATE_BMX_AUTOMATED_SWITCH"))&&
        config_check(8,handoff_absent_or_zero(L"RESKATE_BMX_AUTOMATED_RIDE"))&&config_check(9,handoff_absent_or_zero(L"RESKATE_BMX_NATIVE_BOARD_CONTROL"))&&
        config_check(10,handoff_absent_or_zero(L"RESKATE_BMX_NATIVE_BOARD_DEFERRED_ARM"))&&config_check(11,handoff_absent_or_zero(L"RESKATE_BMX_BOARD_HANDOFF_TRACE"))&&
        config_check(12,bridge_environment_exact(L"RESKATE_BMX_BOARD_HANDOFF_MODE",L""))&&config_check(13,handoff_absent_or_zero(L"RESKATE_BMX_RAMP_TEST"))&&
        config_check(14,handoff_absent_or_zero(L"RESKATE_BMX_RAMP_RIDE"));
    NoBailSkater canonical;bool accepted{};
    if(!configuration)attempt.reason="configuration_refused";
    else {
        attempt.inspection_attempted=true;
        attempt.inspection_available=inspect_bmx_switch_admission_owner(owner[1],owner[2],canonical,attempt.protection);
        if(!attempt.inspection_available)attempt.reason="canonical_or_lease_snapshot_unavailable";
        else if(!(attempt.owner_matches=handoff::Owner{canonical.base,owner[1],canonical.entity,canonical.core,canonical.context,canonical.rig}==owner))attempt.reason="canonical_owner_mismatch";
        else if(attempt.protection.manual)attempt.reason="independent_manual_protection";
        else if(attempt.protection.flight)attempt.reason="independent_flight_protection";
        else if(attempt.protection.board_lock)attempt.reason="independent_board_lock";
        else if(!attempt.protection.own_bmx)attempt.reason="matching_live_BMX_lease_required";
        else if(resume&&(!pending||!s.admission_wait.consume(*pending,s.intent)))attempt.reason="pending_gesture_no_longer_ready";
        else {attempt.policy_called=true;accepted=s.intent.admit(now,device,owner);attempt.reason=accepted?"accepted":"original_source_or_intent_policy_refused";}
    }
    attempt.accepted=accepted;
    // Arm under the same original-source lock as the rejected admission. This
    // cannot miss an intervening original release/activity or renew the window.
    if(!resume&&!accepted&&attempt.policy_called&&pending&&pending->now==now&&pending->owner==owner&&pending->device==device)
        (void)s.admission_wait.begin(*pending,s.intent);
    if(resume&&!accepted)s.admission_wait.cancel("pending_final_admission_refused");
    if(accepted){s.reverse_ledger=false;s.count=0;s.total=s.terminal_returns=0;s.latest_terminal={};}
    ReleaseSRWLockExclusive(&s.lock);SetLastError(error);return accepted;
}
bool admit_human_native_handoff(std::uint64_t now,unsigned device,const handoff::Owner& owner) noexcept {
    return admit_human_native_handoff_impl(now,device,owner,nullptr,false);
}
bool admit_human_native_handoff(human::AdmissionContext c) noexcept {
    return admit_human_native_handoff_impl(c.now,c.device,c.owner,&c,false);
}
bool resume_human_native_handoff(human::AdmissionContext c) noexcept {
    return admit_human_native_handoff_impl(c.now,c.device,c.owner,&c,true);
}
human::AdmissionWaitResult poll_human_native_admission(human::AdmissionContext c) noexcept {
    auto& s=human_native_handoff();const auto error=GetLastError();AcquireSRWLockExclusive(&s.lock);
    c.allowed=c.allowed&&human_native_handoff_configuration();
    const auto result=s.admission_wait.update(c,s.intent);
    ReleaseSRWLockExclusive(&s.lock);SetLastError(error);return result;
}
bool human_native_admission_waiting() noexcept {
    auto& s=human_native_handoff();AcquireSRWLockShared(&s.lock);const bool active=s.admission_wait.active();ReleaseSRWLockShared(&s.lock);return active;
}
void cancel_human_native_admission(const char* why) noexcept {
    auto& s=human_native_handoff();AcquireSRWLockExclusive(&s.lock);s.admission_wait.cancel(why);ReleaseSRWLockExclusive(&s.lock);
}
bool transfer_human_native_handoff(const handoff::Ticket& ticket,std::uint64_t now) noexcept {
    auto& s=human_native_handoff();const auto error=GetLastError();AcquireSRWLockExclusive(&s.lock);
    const bool accepted=human_native_handoff_configuration()&&s.intent.transfer(ticket,now);
    if(!accepted)s.intent.cancel("human_cleanup_transfer_refused");
    ReleaseSRWLockExclusive(&s.lock);SetLastError(error);return accepted;
}
void cancel_human_native_handoff(const char* reason) noexcept {
    auto& s=human_native_handoff();AcquireSRWLockExclusive(&s.lock);s.reverse.cancel(s.intent,reason);s.intent.cancel(reason);s.admission_wait.cancel(reason);ReleaseSRWLockExclusive(&s.lock);
}
bool begin_human_native_return(handoff::Sample sample,human::AdmissionContext context) noexcept {
    auto& s=human_native_handoff();if(!s.requested.load())return false;
    const auto error=GetLastError();AcquireSRWLockExclusive(&s.lock);
    NoBailSkater canonical;bool protected_owner{};
    const bool canonical_now=human_native_handoff_configuration()&&
        inspect_no_bail_owner(context.owner[1],context.owner[2],canonical,protected_owner)&&!protected_owner&&
        handoff::Owner{canonical.base,context.owner[1],canonical.entity,canonical.core,canonical.context,canonical.rig}==context.owner;
    context.allowed=context.allowed&&canonical_now&&!s.admission_wait.active();sample.configuration=sample.configuration&&canonical_now;
    const bool accepted=s.reverse.begin(sample,context,s.intent);
    if(accepted){s.reverse_ledger=true;s.count=0;s.total=s.terminal_returns=0;s.latest_terminal={};}
    ReleaseSRWLockExclusive(&s.lock);SetLastError(error);return accepted;
}
void update_human_native_return(handoff::Sample sample,human::Guard guard,bool sdk_chord_held) noexcept {
    auto& s=human_native_handoff();const auto error=GetLastError();AcquireSRWLockExclusive(&s.lock);
    guard.allowed=guard.allowed&&human_native_handoff_configuration();sample.configuration=sample.configuration&&human_native_handoff_configuration();
    sample.capture_released=sample.capture_released&&polled_capture()==input::BmxNativeCapture::none&&
        !input::bmx_capture_live(bmx_quick_switch_until.load(),sample.now,true);
    s.reverse.observe(sample,guard,sdk_chord_held,s.intent);
    ReleaseSRWLockExclusive(&s.lock);SetLastError(error);
}
void cancel_human_native_return(const char* why) noexcept {
    auto& s=human_native_handoff();AcquireSRWLockExclusive(&s.lock);s.reverse.cancel(s.intent,why);ReleaseSRWLockExclusive(&s.lock);
}
bool human_native_return_active() noexcept {
    auto& s=human_native_handoff();AcquireSRWLockShared(&s.lock);const bool active=s.reverse.active();ReleaseSRWLockShared(&s.lock);return active;
}
bool human_native_return_complete() noexcept {
    auto& s=human_native_handoff();AcquireSRWLockShared(&s.lock);const bool done=s.reverse.complete();ReleaseSRWLockShared(&s.lock);return done;
}
std::uint64_t human_native_return_deadline() noexcept {
    auto& s=human_native_handoff();AcquireSRWLockShared(&s.lock);const auto deadline=s.reverse.deadline();ReleaseSRWLockShared(&s.lock);return deadline;
}
bool human_native_handoff_active() noexcept {
    auto& s=human_native_handoff();AcquireSRWLockShared(&s.lock);const auto active=s.intent.active();ReleaseSRWLockShared(&s.lock);return active;
}
bool human_native_handoff_complete() noexcept {
    auto& s=human_native_handoff();AcquireSRWLockShared(&s.lock);const auto done=s.intent.session().phase()==handoff::Phase::complete;ReleaseSRWLockShared(&s.lock);return done;
}
std::uint64_t human_native_handoff_deadline() noexcept {
    auto& s=human_native_handoff();AcquireSRWLockShared(&s.lock);
    const auto deadline=s.intent.active()?s.intent.accepted_ms()+15000:0;ReleaseSRWLockShared(&s.lock);return deadline;
}
void guard_human_native_handoff(human::Guard guard) noexcept {
    auto& s=human_native_handoff();const auto error=GetLastError();AcquireSRWLockExclusive(&s.lock);
    guard.allowed=guard.allowed&&human_native_handoff_configuration();(void)s.intent.guard(guard);
    ReleaseSRWLockExclusive(&s.lock);SetLastError(error);
}
void update_human_native_handoff(handoff::Sample sample,human::Guard guard) noexcept {
    auto& s=human_native_handoff();const auto error=GetLastError();AcquireSRWLockExclusive(&s.lock);
    guard.allowed=guard.allowed&&human_native_handoff_configuration();sample.configuration=sample.configuration&&human_native_handoff_configuration();
    sample.capture_released=sample.capture_released&&polled_capture()==input::BmxNativeCapture::none&&
        !input::bmx_capture_live(bmx_quick_switch_until.load(),sample.now,true);
    s.intent.observe(sample,guard);ReleaseSRWLockExclusive(&s.lock);SetLastError(error);
}
void apply_human_native_handoff(bool outer,DWORD user,DWORD original_packet,const XINPUT_GAMEPAD& raw,XINPUT_STATE* output) noexcept {
    auto& s=human_native_handoff();if(!s.requested.load()||!outer||!output||user>3)return;
    const auto error=GetLastError();AcquireSRWLockExclusive(&s.lock);
    const auto now=GetTickCount64();const auto before=private_board_pad(output->Gamepad);const auto before_packet=output->dwPacketNumber;
    bridge_policy::Result result;std::uint32_t observed_physics{};bool physics_observed{};
    const bool reverse=s.reverse_ledger;
    if(s.intent.active()&&((reverse&&s.reverse.active()&&s.reverse.pulse().live())||(!reverse&&s.intent.transferred()&&s.intent.pulse().live()))&&user==s.intent.slot()) {
        if(s.count>=128){s.reverse.cancel(s.intent,"human_input_ledger_capacity");s.intent.cancel("human_input_ledger_capacity");}
        const auto owner=s.intent.owner();NoBailSkater a,b;std::uintptr_t client{};bool protected_a{},protected_b{};
        const auto current=[&](const NoBailSkater& o){return handoff::Owner{o.base,client,o.entity,o.core,o.context,o.rig};};
        bool identity=human_native_handoff_configuration()&&try_no_bail_skater(0,a,client)&&current(a)==owner&&
            inspect_no_bail_owner(client,a.entity,b,protected_a)&&current(b)==owner&&!protected_a;
        const auto& os=state();const auto window=os.window.load();bool escape{};
        {OverlayInputAccess access;escape=(GetAsyncKeyState(VK_ESCAPE)&0x8000)!=0;}
        const bool focused=window&&game_window_foreground(window)&&os.started.load()&&os.input_attached.load()&&!os.stop.load()&&!os.failed.load()&&!escape;
        const bool no_capture=polled_capture()==input::BmxNativeCapture::none&&
            !input::bmx_capture_live(bmx_quick_switch_until.load(),now,true);
        std::uint32_t physics{};identity=identity&&memory::peek(a.context+0x1414,physics)&&
            inspect_no_bail_owner(client,a.entity,b,protected_b)&&current(b)==owner&&!protected_b;
        observed_physics=physics;physics_observed=identity;
        if(reverse)result=s.reverse.poll(now,user,identity,focused,no_capture&&before==private_board_pad(raw),physics,private_board_pad(raw),s.intent);
        else result=s.intent.poll(now,user,identity,focused,no_capture&&before==private_board_pad(raw),physics,private_board_pad(raw));
        if(result.rewrite)output->Gamepad.wButtons=static_cast<WORD>((output->Gamepad.wButtons&~0x8000u)|(result.north?0x8000u:0));
    }
    const auto after=private_board_pad(output->Gamepad);output->dwPacketNumber=s.intent.packet(user,before_packet,after,result.rewrite);
    if(output->dwPacketNumber!=before_packet||result.rewrite) {
        const auto pulse_phase=reverse?s.reverse.pulse().phase:s.intent.pulse().phase;
        HumanNativeHandoff::Row row{s.intent.generation(),now,unsigned(user),unsigned(pulse_phase),original_packet,before_packet,
            output->dwPacketNumber,private_board_pad(raw),before,after,result.rewrite,physics_observed,observed_physics};++s.total;
        if(result.rewrite&&s.count<128)s.rows[s.count++]=row;
        if(reverse?!s.reverse.pulse().live():!s.intent.pulse().live()){++s.terminal_returns;s.latest_terminal=row;}
    }
    ReleaseSRWLockExclusive(&s.lock);SetLastError(error);
}
std::string human_native_handoff_status() {
    auto& s=human_native_handoff();AcquireSRWLockShared(&s.lock);const auto intent=s.intent;const auto rows=s.rows;
    const auto attempt_snapshot=s.admission;const auto pending=s.admission_wait;const auto reverse=s.reverse;
    const auto count=s.count;const auto terminal=s.latest_terminal;const auto total=s.total,terminals=s.terminal_returns;ReleaseSRWLockShared(&s.lock);
    const auto pad=[](const bridge_policy::Pad& p){return Json::array({p.buttons,p.left_trigger,p.right_trigger,p.left_x,p.left_y,p.right_x,p.right_y});};
    const auto row=[&](const HumanNativeHandoff::Row& r){return Json{{"generation",r.generation},{"wall_ms",r.wall},{"slot",r.slot},{"phase",r.phase},
        {"original_packet",r.original_packet},{"before_packet",r.before_packet},{"after_packet",r.after_packet},{"original_raw_pad",pad(r.original_raw)},
        {"before_pad",pad(r.before)},{"after_pad",pad(r.after)},{"offer",r.offer},{"physics",r.physics_observed?Json(r.physics):Json(nullptr)}};};
    Json offers=Json::array();for(unsigned i=0;i<count;++i)offers.push_back(row(rows[i]));
    Json sources=Json::array();for(unsigned i=0;i<4;++i){const auto& o=intent.originals()[i];sources.push_back(Json{{"slot",i},{"wall_ms",o.wall},{"success",o.success},
        {"packet",o.packet},{"raw_pad",pad(o.raw)},{"packet_domain_active",intent.packets()[i].active},{"translated_packet",intent.packets()[i].packet}});}
    Json admission=Json::array();for(unsigned i=0;i<4;++i){const auto& o=intent.admission()[i];admission.push_back(Json{{"slot",i},{"wall_ms",o.wall},
        {"success",o.success},{"original_packet",o.packet},{"original_raw_pad",pad(o.raw)}});}
    const auto sample=[](const handoff::Sample& v){return Json{{"wall_ms",v.now},{"sampled_ms",v.sampled_ms},{"serial",v.serial},{"native_time",v.native_time},
        {"owner",v.owner},{"body",Json::array({std::uint64_t(v.body.world),std::uint64_t(v.body.state),std::uint64_t(v.body.index),std::uint64_t(v.body.generation),v.body.token})},
        {"cleanup_counters",v.counters},{"physics",v.physics},{"configuration",v.configuration},{"gates",v.gates},
        {"canonical",v.canonical},{"protected_owner",v.protected_owner},{"clock_coherent",v.clock_coherent},{"pool_released",v.pool_released},
        {"sdk_idle",v.sdk_idle},{"motion_released",v.motion_released},{"publication_inactive",v.publication_inactive},{"parts_zero",v.parts_zero},
        {"capture_released",v.capture_released},{"selected_original_fresh",v.original_slot0_fresh},{"selected_original_quiet",v.original_quiet},
        {"only_selected_original",v.only_slot0},{"sdk_quiet",v.sdk_quiet},{"gesture_released",v.gesture_released}};};
    const auto& ticket=intent.session().ticket();
    static constexpr std::array<const char*,15> config_names{"latched_requested","human_flag","native_chassis","human_ride","private_handoff_off","private_bridge_off",
        "quick_test_off","automated_switch_off","automated_ride_off","native_board_control_off","deferred_arm_off","passive_trace_off","passive_mode_empty","ramp_test_off","ramp_ride_off"};
    Json config=Json::object();for(unsigned i=0;i<config_names.size();++i)config[config_names[i]]=(attempt_snapshot.config_evaluated&(1u<<i))?Json((attempt_snapshot.config_passed&(1u<<i))!=0):Json(nullptr);
    Json attempt_sources=Json::array();for(unsigned i=0;i<4;++i){const auto& o=attempt_snapshot.originals[i];attempt_sources.push_back(Json{{"slot",i},{"wall_ms",o.wall},
        {"success",o.success},{"original_packet",o.packet},{"original_raw_pad",pad(o.raw)},{"north_armed",attempt_snapshot.north_armed[i]},
        {"north_rise_ms",attempt_snapshot.north_rises[i]},{"spent_north_rise_ms",attempt_snapshot.spent_rises[i]}});}
    const auto& protection=attempt_snapshot.protection;
    const Json attempt=attempt_snapshot.present?Json{{"wall_ms",attempt_snapshot.now},{"sdk_device_mask",attempt_snapshot.device},{"requested_owner",attempt_snapshot.owner},
        {"reason",attempt_snapshot.reason},{"accepted",attempt_snapshot.accepted},{"configuration_checks",std::move(config)},
        {"inspection_attempted",attempt_snapshot.inspection_attempted},{"inspection_available",attempt_snapshot.inspection_attempted?Json(attempt_snapshot.inspection_available):Json(nullptr)},
        {"owner_matches",attempt_snapshot.inspection_available?Json(attempt_snapshot.owner_matches):Json(nullptr)},
        {"own_live_BMX",attempt_snapshot.inspection_available?Json(protection.own_bmx):Json(nullptr)},
        {"manual_protected",attempt_snapshot.inspection_available?Json(protection.manual):Json(nullptr)},
        {"flight_protected",attempt_snapshot.inspection_available?Json(protection.flight):Json(nullptr)},
        {"board_locked",attempt_snapshot.inspection_available?Json(protection.board_lock):Json(nullptr)},
        {"protection_sampled_ms",attempt_snapshot.inspection_available?Json(protection.sampled_ms):Json(nullptr)},
        {"protection_verified_ms",attempt_snapshot.inspection_available?Json(protection.verified_ms):Json(nullptr)},
        {"bmx_until_ms",attempt_snapshot.inspection_available?Json(protection.bmx_until):Json(nullptr)},
        {"manual_until_ms",attempt_snapshot.inspection_available?Json(protection.manual_until):Json(nullptr)},
        {"flight_until_ms",attempt_snapshot.inspection_available?Json(protection.flight_until):Json(nullptr)},
        {"board_until_ms",attempt_snapshot.inspection_available?Json(protection.board_until):Json(nullptr)},
        {"policy_called",attempt_snapshot.policy_called},{"original_source_snapshot",std::move(attempt_sources)}}:Json(nullptr);
    return Json{{"schema",1},{"scope","OPT_IN_Ride_actual_original_XInput_human_LB_Y"},{"generation",intent.generation()},
        {"admission_wait",Json{{"active",pending.active()},{"issue",pending.issue()},{"owner",pending.owner()},
            {"device",pending.device()},{"started_ms",pending.started_ms()},{"sampled_ms",pending.sampled_ms()},{"max_wait_ms",human::native::freshness_ms}}},
        {"last_admission_attempt",attempt},{"active",intent.active()},{"slot",intent.slot()},{"issue",intent.issue()},{"phase",unsigned(intent.session().phase())},
        {"released",intent.released()},{"owner",intent.owner()},{"deadline_ms",intent.session().ticket().deadline_ms},
        {"native103_observed",intent.session().saw103()},{"native100_observed",intent.session().saw100()},
        {"accepted_ms",intent.accepted_ms()},{"admission_original_slots",std::move(admission)},
        {"cleanup_ticket_body",Json::array({std::uint64_t(ticket.body.world),std::uint64_t(ticket.body.state),std::uint64_t(ticket.body.index),std::uint64_t(ticket.body.generation),ticket.body.token})},
        {"cleanup_ticket_counters",ticket.counters},{"cleanup_ticket_serial",ticket.serial},{"cleanup_ticket_native_time",ticket.native_time},
        {"first_native103",intent.first103().serial?sample(intent.first103()):Json(nullptr)},
        {"first_native100",intent.first100().serial?sample(intent.first100()):Json(nullptr)},{"latest_native",sample(intent.session().latest())},
        {"pulse_phase",unsigned(intent.pulse().phase)},{"pulse_issue",intent.pulse().issue},{"sources",std::move(sources)},
        {"offers",std::move(offers)},{"translated_returns",total},{"terminal_returns",terminals},
        {"latest_terminal",terminals?row(terminal):Json(nullptr)},{"report_scope","128 offers and latest terminal; terminal history is counted, not fully retained"},
        {"reverse",Json{{"phase",unsigned(reverse.phase())},{"active",reverse.active()},{"complete",reverse.complete()},
            {"issue",reverse.issue()},{"deadline_ms",reverse.deadline()},{"pulse_phase",unsigned(reverse.pulse().phase)},
            {"pulse_issue",reverse.pulse().issue},{"press_reports",reverse.pulse().press_reports},{"release_reports",reverse.pulse().release_reports},
            {"baseline_native",sample(reverse.baseline())},{"first_native504",reverse.first504().serial?sample(reverse.first504()):Json(nullptr)},
            {"latest_native",sample(reverse.latest())},{"pending_issue",reverse.waiting().issue()},{"software_dismount_fallback_enabled",false}}},
        {"software_board_fallback_enabled",false},
        {"physical_controller_acceptance_verified",false},{"visual_quality",nullptr}}.dump();
}
