#include "Engine/Game/BMX/bmx_automated_switch.h"
#include "Engine/Game/BMX/bmx_quick_switch_rehearsal.h"
#include "Engine/Game/BMX/equipment_switch.h"
#include "Extension/BMX/bmx_selftest.h"
#include "Extension/BMX/bmx_native_motion.h"
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
void check(bool good,const char* message){if(!good){std::cerr<<message<<'\n';std::exit(1);}}
namespace q=dingosdk::bmx::quick_switch_rehearsal;
using namespace dingosdk::bmx;
// A native-observation fixture, not runtime success evidence: real latches and
// routing consume the generated source, while asynchronous native modes are
// supplied explicitly. Live collision/support remains an in-game requirement.
struct QuickFixture {
    q::Scenario scenario;
    q::Frame frame;
    BikeQuickSwitchButton quick;
    BikeInteractionButton ordinary;
    std::uint64_t wall=1000;
    unsigned quick_events{},ordinary_events{},setups{};
    QuickFixture() {
        frame.owner={11,12,13,14,15};frame.available=frame.timing_available=frame.physics_available=true;
        frame.physics=100;frame.serial=10;frame.native_time=1;
        check(scenario.start(wall),"explicit scenario starts once");
    }
    void mount() {
        ++frame.mounts;frame.token=700+frame.mounts;frame.world=100;
        frame.allocations=1;frame.active=frame.pool_active=frame.capture=true;
        frame.parked=frame.carried=frame.pool_parked=false;
        frame.offboard=frame.standable=frame.motion_supported=true;frame.physics=504;frame.parts=8;
    }
    void park() {
        frame.active=frame.pool_active=frame.capture=false;frame.pool_parked=frame.parked=true;
        frame.offboard=frame.standable=frame.motion_supported=true;frame.physics=504;frame.parts=8;
    }
    q::Output tick(bool gates=true,bool quiet=true) {
        ++frame.serial;frame.native_time+=1.0/60;wall+=17;
        if(frame.active)++frame.applications;
        frame.board_transition.observed_wall_ms=wall;
        const auto out=scenario.step(frame,gates,quiet,wall);
        const bool enabled=!scenario.terminal();
        const bool need=enabled&&quick.owner_sample_needed(out.north,out.shoulder);
        const auto event=quick.update(out.north,out.shoulder,enabled,q::virtual_device,need,frame.owner,wall);
        const auto y=ordinary.update(out.north,enabled&&!event.suppress_north&&
            (frame.active||frame.parked||frame.crashed),wall);
        unsigned route{};
        if(event.pressed) {
            ++quick_events;
            switch(quick_bike_action(frame.active,frame.pending,frame.mounting,frame.parked,frame.crashed)) {
            case QuickBikeAction::skateboard:route=4;break;
            case QuickBikeAction::nearby_bike:route=7;break;
            case QuickBikeAction::equip_bike:route=1;break;
            default:break;
            }
        }
        if(y!=BikeInteraction::none)++ordinary_events;
        scenario.feedback(out,event,y,route);
        if(scenario.terminal())return out;
        if(out.setup_request) {
            ++setups;
            if(out.setup_request==1)mount();
            else if(out.setup_request==7)park();
            else if(out.setup_request==8)frame.carried=true;
            else check(false,"fixture rejects an unrecognized direct setup action");
        }
        if(route==4) {
            frame.active=frame.capture=frame.pool_active=frame.parked=frame.carried=false;
            frame.pool_parked=true;frame.offboard=frame.standable=false;frame.physics=100;frame.parts=0;frame.token=0;
            // Production equipment teardown resets ordinary/quick controls.
            // A held source must still be quarantined after that reset.
            quick.reset();ordinary.reset();
        } else if(route==1||route==7) {
            if(frame.carried)++frame.strict_setdowns;
            mount();quick.reset();ordinary.reset();
        }
        return out;
    }
};
QuickFixture native_board_fixture() {
    QuickFixture f;
    for(unsigned tick=0;tick<500&&f.scenario.phase()!=q::Phase::board&&!f.scenario.terminal();++tick)f.tick();
    check(f.scenario.awaiting_native_board(),"board wait follows a fully released single route4 pulse");return f;
}
void boarding_frame(QuickFixture& h,unsigned physics=200) {
    auto& f=h.frame;f.physics=physics;f.offboard=f.standable=f.motion_supported=false;
    auto& w=f.board_transition;w.request_owner=f.owner;
    w.pending_for_owner=w.request_clock_valid=w.parked_pool_proven=w.clock_unchanged=w.physics_unchanged=true;
    w.accepted_requests=1;w.observed_wall_ms=h.wall;w.pending_deadline_ms=h.wall+15000;
    w.request_wall_ms=h.wall;w.request_serial=f.serial;w.request_native_time=f.native_time;w.world=f.world;w.token=701;
}
void native_board_transition_tests() {
    const EquipmentOwner owner{11,12,13,14,15};SkateboardSwitch intent;
    check(!intent.pending_for(1000,owner)&&!intent.pending_deadline_ms(),"passive pending witness is false while idle");
    intent.request(1000,owner);
    check(intent.pending_for(1000,owner)&&intent.pending_for(15999,owner)&&intent.pending_deadline_ms()==16000,
        "passive witness binds existing full owner and original15-second deadline");
    auto replacement=owner;replacement.rig=99;
    check(!intent.pending_for(999,owner)&&!intent.pending_for(16000,owner)&&!intent.pending_for(1001,replacement),
        "pending witness rejects rollback, exact expiry and changed native owner");
    check(intent.update(1001,true,true,owner,true,true,false)==BoardSwitchResult::request_mount&&intent.pending_for(1002,owner),
        "passive sampling does not consume native request or alter normal retry state");
    check(intent.update(1350,true,true,owner,true,true,false)==BoardSwitchResult::waiting&&
        intent.update(1351,true,true,owner,true,true,false)==BoardSwitchResult::request_mount,
        "existing350ms retry behavior remains unchanged");
    check(intent.update(1352,true,true,owner,false,false,true)==BoardSwitchResult::complete&&!intent.pending_for(1353,owner),
        "grounded100 revokes pending; terminal proof may continue without pending intent");
    intent.request(UINT64_MAX-10,owner);check(!intent.pending_for(UINT64_MAX-9,owner),"overflowed legacy deadline never becomes valid witness");
    auto success=native_board_fixture();boarding_frame(success,200);
    const auto first=success.tick();check(!success.scenario.terminal()&&success.scenario.phase()==q::Phase::board&&
        !first.north&&!first.shoulder&&!first.setup_request,"proven200 only waits neutrally");
    const auto duplicate=success.scenario.step(success.frame,true,true,success.wall);
    check(success.scenario.board_transitions().samples200==1&&!duplicate.north&&!duplicate.shoulder&&!duplicate.setup_request,
        "duplicate transition clock never advances receipt counts or emits input");
    success.frame.physics=201;success.tick();
    check(success.scenario.board_transitions().samples201==1,"proven201 retained separately from200");
    success.frame.physics=100;success.frame.board_transition.pending_for_owner=false;
    success.tick();check(success.scenario.phase()==q::Phase::board,"one fresh grounded100 sample cannot complete board wait");
    success.tick();check(success.scenario.phase()==q::Phase::pulse&&success.scenario.pulse_count()==2,
        "two fresh100 observations complete even after production clears its pending intent");
    for(unsigned tick=0;tick<1000&&!success.scenario.terminal();++tick)success.tick();
    check(success.scenario.passed()&&success.quick_events==4&&!success.ordinary_events,
        "known boarding transition cannot add switch requests or ordinary Y leakage across all four routes");
    auto interrupted=native_board_fixture();boarding_frame(interrupted,200);interrupted.tick();
    interrupted.frame.physics=100;interrupted.tick();
    interrupted.frame.physics=201;interrupted.tick();
    interrupted.frame.physics=100;interrupted.tick();check(interrupted.scenario.phase()==q::Phase::board,
        "100 then201 then100 resets stability instead of counting nonconsecutive grounded samples");
    interrupted.tick();check(interrupted.scenario.phase()==q::Phase::pulse,"fresh second consecutive100 completes after interruption");
    auto duplicate_interruption=native_board_fixture();boarding_frame(duplicate_interruption,200);duplicate_interruption.tick();
    duplicate_interruption.frame.physics=100;duplicate_interruption.tick();
    duplicate_interruption.frame.physics=201;
    const auto repeated=duplicate_interruption.scenario.step(duplicate_interruption.frame,true,true,duplicate_interruption.wall);
    check(!repeated.north&&!duplicate_interruption.scenario.board_transitions().samples201,"duplicate201 does not count as new evidence");
    duplicate_interruption.frame.physics=100;duplicate_interruption.tick();
    check(duplicate_interruption.scenario.phase()==q::Phase::board,"even duplicate observed201 resets prior grounded stability");
    duplicate_interruption.tick();check(duplicate_interruption.scenario.phase()==q::Phase::pulse,"two subsequent fresh100 samples required after duplicate transition");
    for(unsigned fault=0;fault<34;++fault) {
        auto broken=native_board_fixture();boarding_frame(broken);auto bad=broken.frame;auto& w=bad.board_transition;
        ++bad.serial;bad.native_time+=1.0/60;const auto now=broken.wall+17;w.observed_wall_ms=now;
        if(fault==0)w.pending_for_owner=false;
        if(fault==1)w.request_clock_valid=false;
        if(fault==2)w.parked_pool_proven=false;
        if(fault==3)w.clock_unchanged=false;
        if(fault==4)w.physics_unchanged=false;
        if(fault==5)w.accepted_requests=0;
        if(fault==6)w.accepted_requests=65;
        if(fault==7)w.request_owner.context=99;
        if(fault==8)++w.world;
        if(fault==9)++w.token;
        if(fault==10)--w.observed_wall_ms;
        if(fault==11)w.pending_deadline_ms=now;
        if(fault==12)w.pending_deadline_ms=now+15001;
        if(fault==13)w.request_wall_ms=now+1;
        if(fault==14)w.request_serial=broken.scenario.pulses()[0].edge_serial-1;
        if(fault==15)w.request_native_time=std::numeric_limits<double>::quiet_NaN();
        if(fault==16)bad.active=true;
        if(fault==17)bad.pending=true;
        if(fault==18)bad.mounting=true;
        if(fault==19)bad.parked=true;
        if(fault==20)bad.carried=true;
        if(fault==21)bad.capture=true;
        if(fault==22)bad.parts=8;
        if(fault==23)bad.token=701;
        if(fault==24)bad.mounts=2;
        if(fault==25)bad.allocations=2;
        if(fault==26)bad.pool_active=true;
        if(fault==27)bad.pool_parked=false;
        if(fault==28)bad.offboard=true;
        if(fault==29)bad.standable=true;
        if(fault==30)bad.motion_supported=true;
        if(fault==31)bad.physics=101;
        if(fault==32)bad.physics=300;
        if(fault==33)bad.strict_setdowns=1;
        const auto out=broken.scenario.step(bad,true,true,now);
        check(broken.scenario.terminal()&&!broken.scenario.passed()&&!out.north&&!out.shoulder&&!out.setup_request,
            "boarding allowance rejects missing/revoked/foreign/stale witness, active BMX, air and ragdoll contexts");
    }
    QuickFixture first_held;
    for(unsigned tick=0;tick<500&&!first_held.scenario.pulses()[0].quick_events;++tick)first_held.tick();
    check(first_held.scenario.phase()==q::Phase::pulse&&!first_held.scenario.pulses()[0].released,"first shortcut is still held");
    boarding_frame(first_held,200);first_held.tick();
    check(first_held.scenario.terminal()&&!first_held.scenario.passed(),"boarding200 cannot bypass the initial held-pulse release requirement");
    auto held=native_board_fixture(); // Deliberate direct frame in a later fresh pulse is never board wait.
    held.tick();held.tick();check(held.scenario.phase()==q::Phase::pulse,"fixture entered next pulse");
    boarding_frame(held,200);held.tick();check(held.scenario.terminal()&&!held.scenario.passed(),"200 cannot be accepted in a pulse phase");
    auto revoked=native_board_fixture();boarding_frame(revoked,200);revoked.tick();
    revoked.frame.physics=201;revoked.frame.board_transition.pending_for_owner=false;revoked.tick();
    check(revoked.scenario.terminal()&&!revoked.scenario.passed(),"revoked pending witness during201 stops instead of waiting to recover it");
}
q::Frame untouched_frame() {
    q::Frame f;f.owner={11,12,13,14,15};f.available=f.physics_available=true;f.physics=100;return f;
}
void quick_preflight_tests() {
    q::Preflight cold;q::Scenario scenario;auto f=untouched_frame();
    check(cold.begin(1000),"explicit private request arms one preflight");
    check(cold.step(f,true,true,true,1000)==q::PreflightResult::waiting&&!scenario.started(),
        "first listener install with no TimeFeed cannot start scenario or emit setup/pulses");
    check(cold.samples()==1&&!cold.clock_seen(),"cold frame is retained as a preflight sample, not a native-clock proof");
    f.timing_available=true;f.serial=1;f.native_time=1.0/60;
    check(cold.step(f,true,true,true,1017)==q::PreflightResult::waiting&&!scenario.started(),"one fresh owned callback clock is insufficient");
    for(std::uint64_t now=1018;now<1050;++now)
        check(cold.step(f,true,true,true,now)==q::PreflightResult::waiting&&!scenario.started(),"duplicate native clocks do not qualify or start actions");
    f.serial=2;f.native_time=2.0/60;
    check(cold.step(f,true,true,true,1050)==q::PreflightResult::qualified&&cold.qualified(),"second distinct advancing same-owner callback clock qualifies");
    check(cold.first_clock().serial==1&&cold.qualified_clock().serial==2&&cold.wall_started()==1000&&cold.elapsed_ms()==50,
        "qualification retains original command wall origin and both actual clocks");
    check(scenario.start(cold.wall_started()),"scenario starts only after preflight qualification");
    const auto first=scenario.step(f,true,true,1050);
    check(first.setup_request==1&&!first.north&&!first.shoulder,"initial setup action is admitted only by qualified native observation");
    f.timing_available=false;
    const auto missing=scenario.step(f,true,true,1051);
    check(scenario.terminal()&&!scenario.passed()&&!missing.setup_request&&!missing.north,
        "active Scenario still immediately rejects timing loss; warmup exception never leaks into it");
    check(!cold.begin(1100),"qualified preflight cannot restart or renew the wall origin");
    q::Preflight expiry;q::Scenario original_deadline;f=untouched_frame();expiry.begin(1000);
    f.timing_available=true;f.serial=1;f.native_time=.016;expiry.step(f,true,true,true,1001);
    f.serial=2;f.native_time=.032;check(expiry.step(f,true,true,true,2999)==q::PreflightResult::qualified,"strict preflight may qualify before, not at, its deadline");
    original_deadline.start(expiry.wall_started());
    const auto expired=original_deadline.step(f,true,true,1000+q::wall_limit_ms);
    check(original_deadline.terminal()&&!original_deadline.passed()&&!expired.setup_request,
        "scenario wall deadline remains anchored to request even after delayed qualification");
    for(unsigned fault=0;fault<20;++fault) {
        q::Preflight broken;auto bad=untouched_frame();broken.begin(1000);broken.step(bad,true,true,true,1000);
        bool ready=true,gates=true,quiet=true;std::uint64_t now=1001;
        if(fault==0)ready=false;
        if(fault==1)gates=false;
        if(fault==2)quiet=false;
        if(fault==3)bad.available=false;
        if(fault==4)bad.owner.client=0;
        if(fault==5)bad.owner.context=999;
        if(fault==6)bad.physics_available=false;
        if(fault==7)bad.pool_failed=true;
        if(fault==8)bad.pool_suspended=true;
        if(fault==9)bad.crashed=true;
        if(fault==10)bad.physics=300;
        if(fault==11){bad.physics=504;bad.offboard=true;bad.motion_supported=false;}
        if(fault==12)bad.allocations=1;
        if(fault==13)bad.capture=true;
        if(fault==14)bad.pending=true;
        if(fault==15)bad.native_time=std::numeric_limits<double>::quiet_NaN();
        if(fault==16)now=999;
        if(fault==17)now=1000+q::preflight_limit_ms;
        if(fault==18){bad.timing_available=true;bad.serial=0;}
        if(fault==19){bad.physics=504;bad.offboard=bad.motion_supported=bad.standable=true;}
        check(broken.step(bad,ready,gates,quiet,now)==q::PreflightResult::rejected&&broken.terminal()&&!broken.qualified(),
            "preflight unknown, owner/fall/equipment/pool/gate/clock/deadline failure is terminal");
        bad=untouched_frame();bad.timing_available=true;bad.serial=10;bad.native_time=1;
        check(!broken.begin(3001)&&broken.step(bad,true,true,true,3001)==q::PreflightResult::rejected,
            "failed readiness cannot rearm into a later owner or emit delayed setup");
    }
    for(unsigned fault=0;fault<6;++fault) {
        q::Preflight broken;auto bad=untouched_frame();broken.begin(1000);
        bad.timing_available=true;bad.serial=10;bad.native_time=1;broken.step(bad,true,true,true,1000);
        if(fault==0)bad.timing_available=false;
        if(fault==1){bad.serial=9;bad.native_time=.9;}
        if(fault==2)bad.serial=11;
        if(fault==3)bad.native_time=1.1;
        if(fault==4)bad.owner.rig=999;
        if(fault==5)bad.native_time=.9;
        check(broken.step(bad,true,true,true,1017)==q::PreflightResult::rejected&&!broken.qualified(),
            "after first clock, loss/rollback/mismatched progression/owner change cannot restart readiness");
    }
    q::Preflight timeout;f=untouched_frame();timeout.begin(1000);
    for(std::uint64_t now=1000;now<3000;now+=50)
        check(timeout.step(f,true,true,true,now)==q::PreflightResult::waiting,"only initial missing clock may wait inside bounded preflight");
    check(timeout.step(f,true,true,true,3000)==q::PreflightResult::rejected&&timeout.terminal(),"missing callback feed times out at exact two-second boundary");
}
void quick_rehearsal_tests() {
    const auto selected=[](std::wstring_view profile,bool switch_test,bool human,bool unattended,bool exclusive=true) {
        return q::configuration_selected(true,profile,L"1",L"1",L"1",switch_test,human,unattended,exclusive);
    };
    check(selected(L"bmx-user-data-chassis-probe",true,false,false),"explicit private switch test selected");
    for(const auto profile:{L"",L"bmx-user-data-working-profile",L"bmx-user-data-chassis-probe-extra",L"BMX-USER-DATA-CHASSIS-PROBE"})
        check(!selected(profile,true,false,false),"quick source rejects ordinary and inexact profile names");
    check(!selected(L"bmx-user-data-chassis-probe",false,false,false),"native flag alone cannot substitute for SwitchTest");
    check(!selected(L"bmx-user-data-chassis-probe",true,true,false),"human riding cannot select the test source");
    check(!selected(L"bmx-user-data-chassis-probe",true,false,true),"unattended ride diagnostics are mutually exclusive");
    check(!selected(L"bmx-user-data-chassis-probe",true,false,false,false),"other diagnostics must be absent");
    check(!q::configuration_selected(false,L"bmx-user-data-chassis-probe",L"1",L"1",L"1",true,false,false,true),"development root required");
    for(const auto value:{L"",L"0",L"true",L"11",L"1 ",L" 1"}) {
        check(!q::configuration_selected(true,L"bmx-user-data-chassis-probe",value,L"1",L"1",true,false,false,true),"new selector is exact1");
        check(!q::configuration_selected(true,L"bmx-user-data-chassis-probe",L"1",value,L"1",true,false,false,true),"switch selector is exact1");
        check(!q::configuration_selected(true,L"bmx-user-data-chassis-probe",L"1",L"1",value,true,false,false,true),"native selector is exact1");
    }
    QuickFixture success;
    for(unsigned tick=0;tick<1000&&!success.scenario.terminal();++tick)success.tick();
    check(success.scenario.passed()&&success.quick_events==4&&success.ordinary_events==0&&success.setups==4,
        "real latches complete bike/board/bike/nearby/carry routes with no ordinary Y leakage");
    check(success.frame.mounts==4&&success.frame.allocations==1&&success.frame.token==704&&success.frame.strict_setdowns==1,
        "native fixture requires one pool, four fresh mount leases and one guarded carry setdown");
    check(success.scenario.observation_count()==9&&success.scenario.pulse_count()==4,
        "terminal evidence retains every native phase and every bounded pulse");
    for(unsigned i=0;i<4;++i) {
        const auto& p=success.scenario.pulses()[i];
        check(p.quick_events==1&&p.ordinary_events==0&&p.released&&p.north_capture_requested,
            "held chords, including latch resets during transitions, cannot repeat or leak Y");
        check(p.release_time-p.edge_time>=q::hold_seconds&&p.release_time-p.edge_time<.25&&
            p.edge_serial>p.first_serial&&p.release_serial>p.edge_serial&&p.last_serial>p.release_serial,
            "hold/release uses advancing native time and distinct neutral/release samples");
    }
    check(!success.scenario.start(success.wall+1),"completed source cannot be restarted");
    const auto stopped=success.tick();check(!stopped.north&&!stopped.shoulder&&!stopped.setup_request,"terminal source emits no future action");
    for(unsigned fault=0;fault<13;++fault) {
        QuickFixture broken;broken.tick();broken.tick();broken.tick();
        if(fault==0)broken.frame.owner.entity=999;
        if(fault==1)broken.frame.world=999;
        if(fault==2)broken.frame.crashed=true;
        if(fault==3)broken.frame.available=false;
        if(fault==4)broken.frame.physics_available=false;
        if(fault==5)broken.frame.pool_suspended=true;
        if(fault==6)broken.frame.allocations=2;
        if(fault==7)broken.frame.serial=1;
        if(fault==8)broken.frame.native_time=-1;
        if(fault==9)broken.wall+=q::wall_limit_ms;
        if(fault==10)broken.frame.motion_supported=false;
        if(fault==11)broken.frame.physics=300;
        if(fault==12)broken.frame.offboard=false;
        broken.tick();check(broken.scenario.terminal()&&!broken.scenario.passed(),"owner/world/fall/unknown/clock/deadline terminates the source");
        broken.frame={};const auto later=broken.tick();
        check(!later.north&&!later.shoulder&&!later.setup_request&&!broken.scenario.start(broken.wall),"terminal fault never retries or pulses into a later owner");
    }
    for(unsigned fault=0;fault<2;++fault) {
        QuickFixture broken;broken.tick(fault!=0,fault!=1);
        check(broken.scenario.terminal()&&!broken.scenario.passed(),"focus/UI failure or physical interference blocks the initial setup action");
    }
    QuickFixture duplicate;
    const auto first=duplicate.scenario.step(duplicate.frame,true,true,duplicate.wall);
    check(first.setup_request==1,"first native sample may request initial mount setup");
    for(unsigned tick=0;tick<100;++tick) {
        const auto same=duplicate.scenario.step(duplicate.frame,true,true,++duplicate.wall);
        check(!same.setup_request&&!same.north&&!same.shoulder,"duplicate native clocks cannot repeat setup or advance into a pulse");
    }
    duplicate.frame.serial++;duplicate.frame.native_time+=q::phase_limit_seconds+1;
    duplicate.scenario.step(duplicate.frame,true,true,++duplicate.wall);
    check(duplicate.scenario.terminal()&&!duplicate.scenario.passed(),"native admission timeout cannot become a delayed action");
    QuickFixture leak;leak.tick();leak.tick();leak.tick();
    auto sample=leak.tick();
    leak.scenario.feedback(sample,{},BikeInteraction::tap,0);
    check(leak.scenario.terminal()&&!leak.scenario.passed(),"ordinary interaction leakage is a terminal failure, not a passing counter");
}

}
void native_step_off_basis_tests() {
    namespace target=dingosdk::bmx::motion_target;
    // Use full pitch/roll/yaw bases rather than identity-only fixtures. The
    // exact legacy expression is the compatibility oracle for every yaw.
    for(float yaw:{-3.14159265f,-.7f,-0.f,0.f,.3f,1.7f,3.14159265f})
    for(const std::array<float,16> supplied:{
        std::array<float,16>{1,0,0,0,0,1,0,0,0,0,1,0,17,23,29,1},
        std::array<float,16>{0,0,-1,0,-1,0,0,0,0,1,0,0,-31,47,53,1},
        std::array<float,16>{.36f,.48f,-.8f,0,-.8f,.6f,0,0,.48f,.64f,.6f,0,100,-200,300,1}}) {
        const float c=std::cos(yaw),s=std::sin(yaw);
        const std::array<float,16> legacy{c,0,-s,0,0,1,0,0,s,0,c,0,0,0,0,1};
        const auto ordinary=target::yaw_basis(supplied,yaw,target::Basis::legacy_yaw);
        for(unsigned i=0;i<16;++i)check(std::bit_cast<std::uint32_t>(ordinary[i])==std::bit_cast<std::uint32_t>(legacy[i]),
            "ordinary yaw target keeps all16 exact legacy words");
        auto step=target::yaw_basis(supplied,yaw,target::Basis::native_step_off);
        check(target::same_basis_words(supplied,step),"physical step-off retains all9 exact supplied basis words independent of yaw");
        check(step[3]==0&&step[7]==0&&step[11]==0&&step[12]==0&&step[13]==0&&step[14]==0&&step[15]==1,
            "step-off helper does not import source translation or homogeneous lanes");
        for(unsigned axis=0;axis<3;++axis)step[12+axis]=float(100+axis)+float(axis+1)*.0166667f;
        check(target::same_basis_words(supplied,step),"existing final translation cannot alter the preserved source basis");
        for(unsigned column=0;column<3;++column)for(unsigned row=0;row<3;++row) {
            auto changed=step;const auto lane=column*4+row;
            changed[lane]=std::bit_cast<float>(std::bit_cast<std::uint32_t>(changed[lane])^1u);
            check(!target::same_basis_words(supplied,changed),"each changed source basis word is detected independently");
        }
    }
    // Exact passive comparison distinguishes signed zero and NaN payloads;
    // neither this helper nor these raw-word fixtures authorize native writes.
    std::array<float,16> words{};words[0]=std::bit_cast<float>(0x80000000u);words[5]=std::bit_cast<float>(0x7fc00011u);
    auto copy=target::yaw_basis(words,0,target::Basis::native_step_off);
    check(target::same_basis_words(words,copy),"source signedzero and raw payload bits are copied without normalization");
    copy[0]=0;check(!target::same_basis_words(words,copy),"signedzero changes cannot pass the exact word diagnostic");
    copy=target::yaw_basis(words,0,target::Basis::native_step_off);copy[5]=std::bit_cast<float>(0x7fc00012u);
    check(!target::same_basis_words(words,copy),"different nonfinite payloads cannot pass the exact word diagnostic");
    std::atomic<std::uint64_t> count{};
    target::count_bounded(count);check(count.load()==1,"callback diagnostic counts a selected target");
    count.store(target::counter_limit-1);target::count_bounded(count);target::count_bounded(count);
    check(count.load()==target::counter_limit,"callback diagnostics saturate at their explicit bound");
    count.store(UINT64_MAX);target::count_bounded(count);check(count.load()==UINT64_MAX,"already invalid high counter cannot wrap to apparent zero");
}
int main(){
    native_step_off_basis_tests();
    native_board_transition_tests();
    quick_preflight_tests();
    quick_rehearsal_tests();
    using namespace dingosdk::bmx;
    namespace a=automated_switch;
    check(a::configuration_selected(true,L"bmx-user-data-chassis-probe",L"1",L"1"),"exact explicit private native switch selected");
    check(!a::configuration_selected(false,L"bmx-user-data-chassis-probe",L"1",L"1"),"ordinary data root cannot select unattended switching");
    for(const auto profile:{L"",L"bmx-user-data",L"bmx-user-data-working-profile",L"bmx-user-data-ramp-test",L"bmx-user-data-chassis-probe-extra",L"BMX-USER-DATA-CHASSIS-PROBE"})
        check(!a::configuration_selected(true,profile,L"1",L"1"),"private profile name is exact");
    for(const auto value:{L"",L"0",L"true",L"11",L"1 ",L" 1"}){
        check(!a::configuration_selected(true,L"bmx-user-data-chassis-probe",value,L"1"),"switch flag must be exact1");
        check(!a::configuration_selected(true,L"bmx-user-data-chassis-probe",L"1",value),"native backend must be explicitly selected");
    }
    a::Lease lease;
    check(!lease.active(1000)&&!lease.expired(),"configuration alone does not start an input lease");
    lease.begin(1000);check(lease.active(1000),"first console lifecycle command starts a bounded lease");
    lease.begin(1500);check(lease.active(200999),"repeated commands do not alter the first deadline");
    check(!lease.active(201000)&&lease.expired(),"exact200second deadline expires");
    lease.begin(202000);check(!lease.active(202000)&&lease.expired(),"later commands cannot restart expiry or fall back to physical input");
    a::Lease reversal;reversal.begin(1000);
    check(!reversal.active(999)&&reversal.expired()&&!reversal.active(1001),"clock reversal permanently expires this process lease");
    selftest::Readiness gates{true,false,true,false,false,false,false,false};
    const auto allowed=[&](const selftest::Readiness& g){return a::lifecycle_allowed(true,true,true,
        selftest::input_allowed(selftest::Mode::unattended,true,g),true);};
    check(allowed(gates),"only physical focus and device availability may be replaced");
    check(!selftest::input_allowed(selftest::Mode::focused,false,gates),"ordinary focused play remains inhibited without physical focus/device");
    for(unsigned fault=0;fault<6;++fault){auto blocked=gates;
        if(fault==0)blocked.native_ui_available=false;
        if(fault==1)blocked.native_menu=true;
        if(fault==2)blocked.overlay_ready=false;
        if(fault==3)blocked.overlay_menu=true;
        if(fault==4)blocked.other_capture=true;
        if(fault==5)blocked.menu_requested=true;
        check(!allowed(blocked),"native menus, overlay readiness/menus and other capture owners remain authoritative");
    }
    for(unsigned fault=0;fault<5;++fault){bool lease_active=true,ready=true,solo=true,ui=true,backend=true;
        if(fault==0)lease_active=false;
        if(fault==1)ready=false;
        if(fault==2)solo=false;
        if(fault==3)ui=false;
        if(fault==4)backend=false;
        check(!a::lifecycle_allowed(lease_active,ready,solo,ui,backend),"expired/loading/network/UI/backend failures block every lifecycle phase");
    }
    const auto neutral=a::neutral_input();InputMapper mapper;
    for(unsigned tick=0;tick<240;++tick){const auto raw=a::neutral_input();
        check(raw.available&&raw.focused&&!raw.menu_open&&raw.device==a::synthetic_device&&raw.device==neutral.device,
            "stable diagnostic input never depends on physical focus/device identity");
        const auto actions=mapper.update(raw,{true},1.0/60);
        check(actions.pedal==0&&actions.brake==0&&actions.steer==0&&actions.air_pitch==0&&
            !actions.hop_pressed&&actions.handlebar==TrickDirection::none&&actions.tailwhip==TrickDirection::none&&actions.movement==catalog::TrickId::none,
            "neutral switch/carry input creates no propulsion, hop, trick, steering or pitch action");
    }
    std::cout<<"BMX automated switch gates, UI ownership, lease expiry, neutral input and native-clock preflight, proven board transitions and quick-switch scenario/adversarial tests passed.\n";
}
