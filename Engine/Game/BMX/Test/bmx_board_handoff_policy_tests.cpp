#include "Engine/Game/BMX/bmx_board_handoff_policy.h"
#include "Engine/Game/BMX/bmx_quick_switch_rehearsal.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif
namespace p=dingosdk::bmx::board_handoff_policy;
namespace q=dingosdk::bmx::quick_switch_rehearsal;
namespace {
unsigned checks{};
void check(bool value,const char* why){++checks;if(!value){std::cerr<<why<<'\n';std::exit(1);}}
constexpr p::Owner owner{1,2,3,4,5,6};
p::Window seeded(){p::Window w;check(w.arm(1000),"first arm");check(w.observe(p::Kind::client,owner,{10,1},1000)==p::Result::admitted,"seed admitted");w.retain(p::Kind::client,{10,1},1000);return w;}
void option_environment(){
    check(p::absent_or_empty_option(0,0,203),"successful empty option is neutral");
    check(p::absent_or_empty_option(0,203,203),"missing option is neutral");
    check(!p::absent_or_empty_option(0,5,203),"unknown Win32 read failure is not empty");
    check(!p::absent_or_empty_option(1,0,203),"nonempty option remains conflicting");
#if defined(_WIN32)
    const wchar_t* name=L"RESKATE_BMX_HANDOFF_EMPTY_FIXTURE";
    for(const auto value:{static_cast<const wchar_t*>(nullptr),L"",L"0",L"1",L"side",L"superman",L" ",L"0 "}) {
        check(SetEnvironmentVariableW(name,value)!=0,"isolated child-process environment fixture set");
        SetLastError(ERROR_SUCCESS);const auto required=GetEnvironmentVariableW(name,nullptr,0);
        wchar_t copied_value{};SetLastError(ERROR_SUCCESS);const auto copied=GetEnvironmentVariableW(name,&copied_value,1);const auto error=GetLastError();
        const bool expected=!value||!*value;
        check(p::absent_or_empty_option(copied,error,ERROR_ENVVAR_NOT_FOUND)==expected,
            "actual Windows one-character read accepts only missing/empty; zero, one, named and whitespace values still conflict");
        if(value&&!*value)check(required==1&&copied==0&&error==ERROR_SUCCESS,
            "actual defined-empty Win32 size query returns terminator1 while buffered read returns0; old activation check rejected it");
        if(!value)check(required==0&&copied==0&&error==ERROR_ENVVAR_NOT_FOUND,"actual missing Win32 result remains distinct");
    }
    check(SetEnvironmentVariableW(name,nullptr)!=0,"isolated environment fixture removed");
#endif
}
void selection(){
    p::Selection s{true,true,true,true,true,true,true,L"quick-switch",L"switch-test"};
    check(p::select(s)==p::Mode::quick_switch,"exact quick case");
    for(unsigned i=0;i<7;++i){auto x=s;
        if(i==0)x.explicit_flag=false;if(i==1)x.exact_root=false;if(i==2)x.supported_image=false;
        if(i==3)x.native_chassis=false;if(i==4)x.conflicts_clear=false;if(i==5)x.quick_switch=false;if(i==6)x.automated_switch=false;
        check(p::select(x)==p::Mode::disabled,"every quick gate required");}
    s.quick_switch=s.automated_switch=false;s.requested_mode=L"native-only";s.launch_mode=L"human-ride";
    check(p::select(s)==p::Mode::native_only,"actual launcher human-ride control");
    for(auto mode:{L"ride",L"switch-test",L"unattended-test",L"Human-Ride",L""}){auto x=s;x.launch_mode=mode;check(p::select(x)==p::Mode::disabled,"wrong launch modes refused");}
    auto x=s;x.quick_switch=true;check(p::select(x)==p::Mode::disabled,"native control cannot run quick scenario");
    x=s;x.automated_switch=true;check(p::select(x)==p::Mode::disabled,"native control cannot run automated switch");
    x=s;x.requested_mode=L"NativeOnly";check(p::select(x)==p::Mode::disabled,"mode exact");
}
void admission(){
    check(p::native_control_untouched(0,0,0),"native-only untouched custom counters");
    check(!p::native_control_untouched(1,0,0),"native-only rejects previous motion writes");
    check(!p::native_control_untouched(0,1,0),"native-only rejects previous pose writes");
    check(!p::native_control_untouched(0,0,1),"native-only rejects previous render writes");
    check(p::native_control_standing(504,true,true,true),"native-only starts standing offboard");
    check(!p::native_control_standing(504,true,false,false),"211 unsupported504 cannot arm native control");
    check(!p::native_control_standing(100,false,true,true),"already mounted board cannot arm native control");
    check(!p::native_control_standing(504,false,true,true),"native-only offboard required");
    auto gate=seeded();check(gate.before_read(8999)==p::Result::admitted,"cheap gate permits final in-window reads");
    check(gate.before_read(9000)==p::Result::wall_exhausted,"cheap gate stops reads at8seconds");
    gate=seeded();check(gate.before_read(1001,false)==p::Result::configuration_changed,"cheap gate configuration revoke");
    gate=seeded();gate.count=p::capacity;check(gate.before_read(1001)==p::Result::capacity_exhausted,"cheap gate stops capacity reads");
    p::Window dormant;check(dormant.observe(p::Kind::client,owner,{1,0},1000)==p::Result::ignored,"no reads admitted before arm");
    auto w=seeded();check(!w.arm(2000),"one-shot arm");
    for(auto kind:{p::Kind::skater_published,p::Kind::board_published}){check(w.observe(kind,owner,{10,1},1000)==p::Result::admitted,"same clock other kind retained independently");w.retain(kind,{10,1},1000);}
    check(w.count==3,"three kinds bounded separately");
    check(w.observe(p::Kind::client,owner,{10,1},1020)==p::Result::throttled,"duplicate serial not a new sample");
    check(w.observe(p::Kind::client,owner,{11,1.01},1025)==p::Result::throttled,"50ms is retention minimum");
    check(w.observe(p::Kind::client,owner,{11,1.01},1050)==p::Result::admitted,"later same observed clock can retain after spacing");
    for(unsigned i=0;i<owner.size();++i){auto a=seeded();auto changed=owner;++changed[i];
        check(a.observe(p::Kind::client,changed,{11,1.01},1050)==p::Result::owner_changed,"all owner fields pinned");
        check(a.observe(p::Kind::client,owner,{12,1.02},1100)==p::Result::ignored,"no rebound after owner failure");}
    for(unsigned fault=0;fault<5;++fault){auto a=seeded();p::Clock c{11,1.01};
        if(fault==0)c={9,.9};if(fault==1)c={11,.9};if(fault==2)c={10,1.01};if(fault==3)c={11,1};
        if(fault==4)c={0,std::numeric_limits<double>::quiet_NaN()};
        check(p::failed(a.observe(p::Kind::client,owner,c,1050)),"invalid or inconsistent clock seals");check(a.sealed,"clock failure sticky");}
    w=seeded();check(w.observe(p::Kind::client,owner,{11,1.01},1050,false)==p::Result::configuration_changed,"configuration revoke seals");
    w=seeded();check(w.observe(p::Kind::client,owner,{11,1.01},1050,true,false)==p::Result::read_failure,"required early read failure seals");
    w=seeded();check(w.observe(p::Kind::client,owner,{11,1.01},9000)==p::Result::wall_exhausted,"8 second exclusive boundary");
    w=seeded();check(w.observe(p::Kind::client,owner,{11,1.01},999)==p::Result::wall_exhausted,"wall rollback seals");
    w=seeded();w.count=p::capacity;check(w.observe(p::Kind::client,owner,{11,1.01},1050)==p::Result::capacity_exhausted,"defensive array capacity fence");
}
void dwell(){
    p::Dwell off;
    for(unsigned i=0;i<100;++i){const bool accepted=i%3!=0;check(off.observe(accepted,{i,double(i)},i*7)==accepted,"default0 adds no schedule gate");}
    p::Dwell d;d.configure(p::driver_dwell_seconds);
    check(!d.observe(true,{100,10},1000),"configured dwell starts");
    check(!d.observe(true,{100,10},1100),"duplicate native clock cannot satisfy dwell");
    check(!d.observe(true,{101,12.5},1200),"native time alone not enough");
    check(d.observe(true,{102,12.6},3500),"both clocks meet driver dwell");
    d.interrupt();check(!d.observe(true,{103,13},3600),"200/201 interrupt starts fresh");
    check(!d.observe(false,{104,14},4600),"non100 breaks dwell");
    check(!d.observe(true,{105,15},5600),"return to100 starts again");
    check(!d.observe(true,{106,17.5},5700),"old wall origin not reused");
    check(d.observe(true,{107,17.6},8100),"full second dwell accepted");
    check(!d.observe(true,{106,17.5},8200)&&!d.started,"backward clock resets");
    d.configure(std::numeric_limits<double>::quiet_NaN());check(d.seconds==2.5,"invalid duration cannot alter selected dwell");
    q::Scenario scenario;scenario.observe_board_dwell_for_private_trace(2.5);check(scenario.start(1000),"scenario starts");
    q::Frame f;f.available=f.timing_available=f.physics_available=true;f.owner={1,2,3,4,5};f.serial=1;f.native_time=.01;
    f.physics=504;f.offboard=true;f.motion_supported=f.standable=false;
    (void)scenario.step(f,true,true,1010);
    check(scenario.terminal()&&!scenario.passed()&&std::string_view(scenario.issue())=="unsupported_native_physics_or_ragdoll","211-style unsupported504 remains a failure with dwell enabled");
}
void native_board_control(){
    p::Selection s{true,true,true,true,true,false,false,L"native-only",L"human-ride",true};
    check(p::select(s)==p::Mode::native_only,"explicit active software control remains separate native-only");
    auto wrong=s;wrong.requested_mode=L"quick-switch";wrong.launch_mode=L"switch-test";wrong.quick_switch=wrong.automated_switch=true;
    check(p::select(wrong)==p::Mode::disabled,"active software board control cannot leak into quick scenario");
    for(unsigned gate=0;gate<5;++gate){auto x=s;if(gate==0)x.explicit_flag=false;if(gate==1)x.exact_root=false;
        if(gate==2)x.supported_image=false;if(gate==3)x.native_chassis=false;if(gate==4)x.conflicts_clear=false;
        check(p::select(x)==p::Mode::disabled,"every existing active native-only selector gate required");}
    std::array<float,8> axes{};check(p::native_board_input_neutral(axes,0,false,false,false,false),"untouched input sample neutral");
    for(unsigned axis=0;axis<8;++axis){auto moved=axes;moved[axis]=.06f;check(!p::native_board_input_neutral(moved,0,false,false,false,false),"every existing input axis can reject interference");}
    axes[0]=std::numeric_limits<float>::quiet_NaN();check(!p::native_board_input_neutral(axes,0,false,false,false,false),"nonfinite input not neutral");axes={};
    for(unsigned action=0;action<5;++action)check(!p::native_board_input_neutral(axes,action==0?1U:0,action==1,action==2,action==3,action==4),"buttons and mapped actions not neutral");
    const auto baseline=[](){p::NativeBoardControl c;check(c.observe(owner,{10,1},1000,504,true,true,true,true)==p::NativeBoardAction::wait,"armed retained standing baseline precedes action");return c;};
    auto c=baseline();check(c.observe(owner,{10,1},1050,504,true,true,true,true)==p::NativeBoardAction::wait&&!c.attempted,"duplicate native clock cannot request");
    check(c.observe(owner,{13,1.05},1050,504,true,true,true,true)==p::NativeBoardAction::request&&c.attempted,"later fresh clock requests once");
    check(c.complete_request(true),"record actual accepted request result");
    check(c.observe(owner,{16,1.1},1100,200,false,false,false,true)==p::NativeBoardAction::observe,"real native200 is evidence only");
    check(c.observe(owner,{19,1.15},1150,201,false,false,false,true)==p::NativeBoardAction::observe,"real native201 is evidence only");
    check(c.observe(owner,{22,1.2},1200,100,false,false,false,true)==p::NativeBoardAction::observe&&c.saw_board,"native100 observed without granting pose acceptance");
    check(c.observe(owner,{25,1.25},1250,100,false,false,false,true)==p::NativeBoardAction::observe,"native100 remains observation not repeat request");
    check(!c.complete_request(true),"successful one-shot cannot complete a second native request");
    for(unsigned fault=0;fault<12;++fault){auto x=baseline();auto current=owner;p::Clock clock{13,1.05};auto wall=1050ULL;
        auto physics=504U;bool off=true,standing=true,motion=true,guard=true;
        if(fault<6)++current[fault];if(fault==6)clock={9,.9};if(fault==7)clock={10,1.05};if(fault==8)wall=999;
        if(fault==9)guard=false;if(fault==10)standing=false;if(fault==11)physics=100;
        check(x.observe(current,clock,wall,physics,off,standing,motion,guard)==p::NativeBoardAction::failed&&!x.attempted,"changed owner, clock, gates, fall or premature board prevents all native calls");
        check(x.observe(owner,{30,2},2000,504,true,true,true,true)==p::NativeBoardAction::failed,"failure sticky no retries");}
    c=baseline();(void)c.observe(owner,{13,1.05},1050,504,true,true,true,true);check(!c.complete_request(false)&&c.failed,"rejected native call is terminal");
    check(c.observe(owner,{16,1.1},1100,504,true,true,true,true)==p::NativeBoardAction::failed,"native request rejection cannot requeue");
    for(unsigned fault=0;fault<5;++fault){auto x=baseline();(void)x.observe(owner,{13,1.05},1050,504,true,true,true,true);check(x.complete_request(true),"request accepted fixture");
        if(fault==4)(void)x.observe(owner,{16,1.1},1100,100,false,false,false,true);
        check(x.observe(owner,{19,1.15},fault==0?1300:1150,fault==1?999U:fault==2?200U:504U,true,fault!=3,true,true)==p::NativeBoardAction::failed,
            "expired unconsumed request, unknown state, bad transition owner mode, ragdoll504 or regression after100 fail closed");}
    c=baseline();(void)c.observe(owner,{13,1.05},1050,504,true,true,true,true);check(c.complete_request(true),"direct100 fixture accepted");
    check(c.observe(owner,{16,1.1},1100,100,false,false,false,true)==p::NativeBoardAction::observe,"fast direct100 needs no fabricated200/201");
}
void async_and_exit(){
    check(p::preparation(false,1000,1001)==p::Preparation::pending,"cold async layout pending");
    check(p::preparation(true,1000,3999)==p::Preparation::ready,"bounded async completion");
    check(p::preparation(false,1000,4000)==p::Preparation::timed_out,"async timeout");
    check(p::preparation(true,1000,4000)==p::Preparation::timed_out,"late completion cannot bypass timeout");
    check(p::preparation(false,1000,999)==p::Preparation::timed_out,"prepare rollback");
    for(unsigned path=0;path<3;++path){unsigned observed{},failed{};bool locked=false;int error=7;
        struct ErrorRestore{int& value;int saved;~ErrorRestore(){value=saved;}};
        const auto operation=[&]{ErrorRestore restore{error,error};
            const p::ObserverExit exit{[&]{++observed;check(!locked,"observer after inner rider mutex release");error=99;if(path==2)throw 42;},[&]()noexcept{++failed;}};
            {struct Lock{bool& b;Lock(bool& v):b(v){b=true;}~Lock(){b=false;}} lock{locked};if(path==0)return;error=9;}
        };operation();check(observed==1,"exactly one observer on ordinary/early/exception paths");check(failed==(path==2?1U:0U),"diagnostic exception contained");check(error==7,"outer LastError restoration runs last");}
}
}
void publication_dwell() {
        p::PublicationDwell all;all.configure(2.25);
        for(const auto kind:{p::Kind::client,p::Kind::skater_published,p::Kind::board_published})
            check(!all.observe(kind,true,{3176,52.9333301782608},1000),"three-stream dwell first samples cannot close observer");
        check(!all.observe(p::Kind::client,true,{3313,55.216663375496864},3344),"client completion alone cannot close CPU observer");
        check(!all.observe(p::Kind::skater_published,true,{3311,55.18333004415035},3297),"actual218 publication135ticks remains strictly below2.25; no epsilon acceptance");
        check(!all.observe(p::Kind::board_published,true,{3311,55.18333004415035},3297),"both short publication streams still cannot close observer");
        check(!all.observe(p::Kind::skater_published,true,{3315,55.249996706843376},3360),"one completed publication stream cannot stand for the other");
        check(all.observe(p::Kind::board_published,true,{3315,55.249996706843376},3360),"all three strict dual-clock windows close observer");
        check(!all.observe(p::Kind::client,false,{3316,55.26666337251663},3377),"leaving grounded100 invalidates every stream");
        check(!all.observe(p::Kind::board_published,true,{3317,55.28333003818989},3394),"old completed windows cannot survive an interruption");
        check(!all.observe(static_cast<p::Kind>(3),true,{3318,55.299996703863144},3410),"invalid publication kind cannot complete dwell");
}
void deferred_passive_prearm() {
    check(p::prepare_limit_ms==3000&&p::wall_limit_ms==8000&&p::trace_dwell_seconds==2.25,"original async/observation/dwell budgets unchanged");
    for(const auto mode:{p::Mode::disabled,p::Mode::quick_switch,p::Mode::native_only})for(const auto active:{false,true}) {
        const bool allowed=mode==p::Mode::native_only&&!active;
        check(p::deferred_passive_arm_allowed(mode,active,0),"default-zero deferred flag is neutral");
        check(p::deferred_passive_arm_allowed(mode,active,1)==allowed,"only passive native-only can opt in");
        for(const auto invalid:{-1,2,3})check(!p::deferred_passive_arm_allowed(mode,active,invalid),"malformed flag refuses every scope");
        for(const auto deferred:{false,true}) {
            const auto budget=deferred&&allowed?30000ULL:3000ULL;
            check(p::native_only_prearm_limit_ms(mode,active,deferred)==budget,"separate bounded prearm budget");
            check(!p::native_only_prearm_expired(1000,1000+budget-1,mode,active,deferred),"last prearm millisecond admitted");
            check(p::native_only_prearm_expired(1000,1000+budget,mode,active,deferred),"exact prearm deadline refused");
            check(p::native_only_prearm_expired(1000,999,mode,active,deferred),"prearm wall rollback refused");
        }
    }
    check(p::preparation(false,1000,4000)==p::Preparation::timed_out,"deferred allowance never extends async preparation");
}
int main(){deferred_passive_prearm();option_environment();selection();admission();dwell();native_board_control();async_and_exit();publication_dwell();std::cout<<"handoff policy checks="<<checks<<'\n';}
