#include "Engine/Game/BMX/equipment_switch.h"
#include <cstdlib>
#include <iostream>
using namespace dingosdk::bmx;
namespace {
void check(bool value, const char* reason) { if (!value) { std::cerr << reason << '\n'; std::exit(1); } }
constexpr EquipmentOwner rider{1, 2, 3, 4, 5};
CrashedSwitchInput recovered_rider() {
    CrashedSwitchInput input;
    input.expected = {rider, 6, 7, 8, 9, 10};
    input.current = input.expected;
    input.rider_available = input.body_available = input.coherent_snapshot = true;
    input.native_offboard = input.native_standable = true;
    return input;
}
void crashed_switch_admission() {
    auto input = recovered_rider();
    check(crashed_switch_preflight(input) == CrashedSwitchDecision::allowed,
        "fresh matching rider/world/body permits an explicit post-recovery switch");
    for (unsigned readiness = 0; readiness < 3; ++readiness) {
        input = recovered_rider();
        if (readiness != 1) input.native_standable = false;
        if (readiness != 0) input.native_offboard = false;
        check(crashed_switch_preflight(input) == CrashedSwitchDecision::preserve_wait,
            "falling, airborne offboard or native transition retains the bike without coercing recovery");
    }
    for (unsigned missing = 0; missing < 4; ++missing) {
        input = recovered_rider();
        if (missing == 0) input.rider_available = false;
        if (missing == 1) input.body_available = false;
        if (missing == 2) input.coherent_snapshot = false;
        if (missing == 3) input.current.owner = {};
        check(crashed_switch_preflight(input) == CrashedSwitchDecision::unavailable,
            "missing or incoherent current observations cannot authorize a switch or fake owner change");
    }
    for (unsigned owner = 0; owner < 5; ++owner) {
        input = recovered_rider();
        auto& current = input.current.owner;
        if (owner == 0) ++current.client;
        if (owner == 1) ++current.entity;
        if (owner == 2) ++current.core;
        if (owner == 3) ++current.context;
        if (owner == 4) ++current.rig;
        input.body_available = false;
        check(crashed_switch_preflight(input) == CrashedSwitchDecision::invalidated,
            "a known canonical rider replacement invalidates even while its old body is unavailable");
    }
    for (unsigned body = 0; body < 5; ++body) {
        input = recovered_rider();
        if (body == 0) ++input.current.world;
        if (body == 1) ++input.current.native_state;
        if (body == 2) ++input.current.body_index;
        if (body == 3) ++input.current.body_generation;
        if (body == 4) ++input.current.token;
        check(crashed_switch_preflight(input) == CrashedSwitchDecision::invalidated,
            "world, state, full body handle and mount token stay pinned across recovery");
        input.coherent_snapshot = false;
        check(crashed_switch_preflight(input) == CrashedSwitchDecision::unavailable,
            "an incoherent body cannot provide a trusted replacement identity");
    }
    input = recovered_rider(); input.expected.token = 0;
    check(crashed_switch_preflight(input) == CrashedSwitchDecision::invalidated,
        "missing expected retained lifetime cannot authorize native cleanup or switching");
    input = recovered_rider(); input.current.body_index = 1048576U;
    check(crashed_switch_preflight(input) == CrashedSwitchDecision::invalidated,
        "out of range coherent body identity is an invalid lifetime, not recoverable readiness");
    input = recovered_rider(); input.current.token = 0;
    check(crashed_switch_preflight(input) == CrashedSwitchDecision::invalidated,
        "a coherent snapshot with no owned body token cannot authorize a switch");
    SkateboardSwitch pending;
    input = recovered_rider(); input.native_standable = false;
    check(crashed_switch_preflight(input) == CrashedSwitchDecision::preserve_wait && !pending.active(),
        "refusing the first F6 starts no board timer or retained switch intent");
    input.native_standable = true;
    check(crashed_switch_preflight(input) == CrashedSwitchDecision::allowed && !pending.active(),
        "fresh get-up readiness alone never replays an old equipment request");
    pending.request(1000);
    check(pending.update(1001, true, true, rider, true, true, false) == BoardSwitchResult::request_mount,
        "a new explicit admitted request still uses the original bounded board switch contract");
}
}
void step_off_board_intent() {
    StepOffBoardSample sample;
    sample.allowed=sample.available=sample.timing_available=sample.parked_proven=true;
    sample.offboard=sample.standable=sample.motion_released=true;
    sample.owner=rider;sample.world=6;sample.token=10;sample.serial=100;
    sample.native_time=100.0/60;sample.root={1,2,3};
    StepOffBoardIntent intent;
    check(intent.update(1000,sample)==StepOffBoardResult::idle,"idle step-off intent cannot request a board");
    check(!intent.begin(1000,{},6,10,sample.root)&&!intent.active(),"missing owner cannot start a step-off intent");
    check(!intent.begin(1000,rider,0,10,sample.root)&&!intent.begin(1000,rider,6,0,sample.root),
        "world and token are required before retaining a step-off request");
    check(intent.begin(1000,rider,6,10,sample.root)&&!intent.begin(1100,rider,6,10,sample.root),
        "one successful step-off pins one intent without extending its deadline");
    sample.stepping_off=true;
    check(intent.update(1001,sample)==StepOffBoardResult::waiting,"active step-off never requests native board mounting");
    sample.stepping_off=false;
    check(intent.update(1002,sample)==StepOffBoardResult::waiting&&
        intent.update(1003,sample)==StepOffBoardResult::waiting,"step completion and its same-frame root cannot complete the switch");
    auto next=sample;++next.serial;next.native_time+=1.0/60;
    const auto pending=intent;
    for(unsigned missing=0;missing<6;++missing) {
        auto waiting=pending;auto blocked=next;
        if(missing==0)blocked.timing_available=false;
        if(missing==1)blocked.offboard=false;
        if(missing==2)blocked.standable=false;
        if(missing==3)blocked.motion_released=false;
        if(missing==4)blocked.root[0]+=.2f;
        if(missing==5)blocked.serial=sample.serial;
        check(waiting.update(1004,blocked)==StepOffBoardResult::waiting&&waiting.active(),
            "fresh timing, native standability, released motion and actual near endpoint remain mandatory");
    }
    check(intent.update(1004,next)==StepOffBoardResult::ready&&!intent.active()&&
        intent.update(1005,next)==StepOffBoardResult::idle,"one fresh owned endpoint emits exactly one ready result");
    for(unsigned changed=0;changed<8;++changed) {
        auto invalid=pending;auto replacement=next;
        if(changed==0)++replacement.owner.client;
        if(changed==1)++replacement.owner.entity;
        if(changed==2)++replacement.owner.core;
        if(changed==3)++replacement.owner.context;
        if(changed==4)++replacement.owner.rig;
        if(changed==5)++replacement.world;
        if(changed==6)++replacement.token;
        if(changed==7)replacement.parked_proven=false;
        check(invalid.update(1004,replacement)==StepOffBoardResult::cancelled&&!invalid.active(),
            "every owner/world/token or parked proof change cancels without adopting a replacement");
    }
    auto suppressed=pending;auto unfocused=next;unfocused.allowed=false;
    check(suppressed.update(1004,unfocused)==StepOffBoardResult::cancelled&&
        suppressed.update(1005,next)==StepOffBoardResult::idle,"focus/menu loss cannot resume an old equipment action");
    auto missing=pending;auto unavailable=next;unavailable.available=false;++unavailable.owner.entity;
    check(missing.update(1004,unavailable)==StepOffBoardResult::waiting&&
        missing.update(1005,next)==StepOffBoardResult::ready,"missing observations do not replace the pinned owner");
    auto expired=pending;
    check(expired.update(16000,next)==StepOffBoardResult::timed_out&&!expired.active(),
        "step completion cannot extend the original fixed request deadline");
    auto backward=pending;
    check(backward.update(999,next)==StepOffBoardResult::timed_out,"backward wall clock cancels retained intent");
    backward=pending;auto old=next;old.serial=99;
    check(backward.update(1004,old)==StepOffBoardResult::cancelled,"native serial regression cannot complete an old step");
    backward=pending;old=next;old.native_time=sample.native_time-.01;
    check(backward.update(1004,old)==StepOffBoardResult::cancelled,"native time regression cancels the request");
    auto cancelled=pending;cancelled.cancel();
    check(cancelled.update(1004,next)==StepOffBoardResult::idle,
        "explicit second F6 cancellation cannot also become a board request");
}
int main() {
    crashed_switch_admission();
    step_off_board_intent();
    check(natural_dismount_state(100) && natural_dismount_state(103),
        "neutral skating and native ollie crouch permit the ordinary offboard request");
    for (const auto state : {0u, 99u, 101u, 102u, 104u, 105u, 200u, 201u, 300u, 401u, 504u})
        check(!natural_dismount_state(state), "other native states cannot acquire a BMX dismount request");
    check(natural_dismount_selection(100, 100) && natural_dismount_selection(103, 103),
        "matching committed neutral or crouched state permits native request consumption");
    check(!natural_dismount_selection(100, 103) && !natural_dismount_selection(103, 100) &&
        !natural_dismount_selection(103, 200) && !natural_dismount_selection(200, 200),
        "a stale selector state or an actual pop cannot consume the queued dismount");
    EquipmentToggleKey key;
    check(!key.update(true, true), "preheld startup hotkey does not toggle");
    check(!key.update(false, true) && key.update(true, true), "release then press toggles");
    check(!key.update(true, true), "held hotkey never repeats");
    check(!key.update(false, false) && !key.update(true, true), "focus/menu suppression requires fresh release");
    check(!key.update(false, true) && key.update(true, true), "neutral rearms after suppression");
    SkateboardSwitch state;
    check(state.update(0, true, true, rider, true, true, false) == BoardSwitchResult::idle, "idle never requests native action");
    state.request(1000);
    check(state.update(1001, false, true, rider, true, true, false) == BoardSwitchResult::blocked, "menu/focus/cleanup blocks native mount");
    check(state.update(1002, true, false, {}, true, true, false) == BoardSwitchResult::waiting, "missing owner waits without request");
    check(state.update(1003, true, true, rider, true, false, false) == BoardSwitchResult::waiting, "airborne offboard does not request mount");
    check(state.update(1004, true, true, rider, true, true, false) == BoardSwitchResult::request_mount, "grounded owned rider requests native mount");
    check(state.update(1005, true, true, rider, true, true, false) == BoardSwitchResult::waiting, "request does not repeat each frame");
    check(state.update(1353, true, true, rider, true, true, false) == BoardSwitchResult::waiting, "retry waits beyond old 250ms lease");
    check(state.update(1354, true, true, rider, true, true, false) == BoardSwitchResult::request_mount, "native request can retry at350ms");
    check(state.update(1355, true, true, rider, false, false, false) == BoardSwitchResult::waiting, "mount animation200 is not a completed board100");
    check(state.update(1400, true, true, rider, false, false, true) == BoardSwitchResult::complete && !state.active(), "actual grounded board observation completes once");
    state.request(2000);
    check(state.update(2001, true, true, rider, true, true, false) == BoardSwitchResult::request_mount, "fresh switch request");
    auto replacement = rider; replacement.core = 9;
    check(state.update(2002, true, true, replacement, true, true, false) == BoardSwitchResult::owner_changed && !state.active(), "same entity with replaced native core cancels");
    state.request(3000);
    check(state.update(18000, false, false, {}, false, false, false) == BoardSwitchResult::timed_out && !state.active(), "menus cannot extend 15-second switch timeout");
    state.request(19000); state.cancel();
    check(state.update(19001, true, true, rider, true, true, false) == BoardSwitchResult::idle, "explicit cancel suppresses further native requests");
    state.request(20000, rider);
    check(state.update(20001, true, true, replacement, true, true, false) == BoardSwitchResult::owner_changed && !state.active(),
        "a pinned admission owner rejects replacement before the first native mount request");
    state.request(21000, rider);
    check(state.update(21001, false, true, replacement, true, true, false) == BoardSwitchResult::blocked,
        "blocked observation cannot replace the pinned admission owner");
    check(state.update(21002, true, false, {}, true, true, false) == BoardSwitchResult::waiting,
        "missing observation cannot erase the pinned admission owner");
    check(state.update(21003, true, true, replacement, true, true, false) == BoardSwitchResult::owner_changed && !state.active(),
        "replacement after blocked or unavailable observations still cannot adopt an old switch intent");
    state.request(22000, rider);
    check(state.update(22001, true, true, rider, true, true, false) == BoardSwitchResult::request_mount,
        "the same pinned admission owner can make the original native request");
    std::cout << "BMX equipment switch tests passed\n";
}
