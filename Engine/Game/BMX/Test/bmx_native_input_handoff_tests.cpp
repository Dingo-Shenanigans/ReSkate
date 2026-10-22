#include "Engine/Game/BMX/bmx_native_input_handoff.h"
#include <cstdlib>
#include <iostream>
#include <limits>
namespace h=dingosdk::bmx::native_input_handoff;
namespace b=dingosdk::input::native_board_bridge;
unsigned checks{};
void check(bool ok,const char* why){++checks;if(!ok){std::cerr<<"FAIL: "<<why<<'\n';std::exit(1);}}
h::Ticket ticket(){return {{1,2,3,4,5,6},{7,8,9,10,11},{20,21,22,23,24,1,1},16000,10,1.0};}
h::Sample sample(std::uint64_t now=1016,std::uint64_t serial=11,double time=1.016) {
    h::Sample s;s.now=s.sampled_ms=now;s.serial=serial;s.native_time=time;s.owner=ticket().owner;s.body=ticket().body;
    s.counters=ticket().counters;s.physics=504;s.sdk_device=1;
    s.configuration=s.gates=s.canonical=s.clock_coherent=s.pool_released=s.sdk_idle=s.motion_released=true;
    s.publication_inactive=s.parts_zero=s.capture_released=s.original_slot0_fresh=s.original_quiet=s.only_slot0=true;
    s.sdk_quiet=s.gesture_released=s.offboard=s.standable=s.motion_supported=true;return s;
}
h::Session queued(){h::Session s;check(s.begin(ticket(),1000),"ticket accepted");check(!s.observe(sample()),"first advancing released observation waits");
    check(s.observe(sample(1032,12,1.032)),"second fresh released observation ready");check(s.pulse_queued(),"one pulse queued");return s;}
void baseline_and_one_shot(){
    h::Session s;auto bad=ticket();bad.body.index=0xffffffffU;check(!s.begin(bad,1000),"bad full body refused");
    bad=ticket();bad.owner[0]=0;check(!s.begin(bad,1000),"incomplete owner refused");
    bad=ticket();bad.deadline_ms=17001;check(!s.begin(bad,1000),"cannot extend 15s budget");
    check(s.begin(ticket(),1000),"valid ticket begins");check(!s.begin(ticket(),1000),"same process session cannot rebegin");
    auto v=sample();v.capture_released=false;check(!s.observe(v)&&s.phase()==h::Phase::cleanup_wait,"held independent North lease waits");
    v=sample(1032,12,1.032);check(!s.observe(v),"first fully released advancing sample waits");
    v=sample(1048,13,1.048);check(s.observe(v),"later advancing neutral qualifies");check(s.pulse_queued(),"queue once");
    check(!s.pulse_queued(),"no second pulse");check(s.callback_snapshot_ready(1048),"fresh proof usable");
    check(s.callback_snapshot_ready(1298),"proof freshness inclusive250ms");check(!s.callback_snapshot_ready(1299),"proof stale after250ms");
    check(!s.callback_snapshot_ready(1047),"future client proof not accepted");
}
void each_guard_aborts(){
    for(unsigned i=0;i<24;++i){auto s=queued();auto v=sample(1048,13,1.048);
        switch(i){
        case 0:v.configuration=false;break;case 1:v.gates=false;break;case 2:v.canonical=false;break;
        case 3:v.protected_owner=true;break;case 4:++v.owner[4];break;case 5:++v.body.generation;break;
        case 6:v.pool_released=false;break;case 7:++v.counters[3];break;case 8:v.clock_coherent=false;break;
        case 9:v.native_time=std::numeric_limits<double>::quiet_NaN();break;case 10:v.serial=11;break;
        case 11:v.sampled_ms=v.now+1;break;case 12:v.sampled_ms=v.now-251;break;
        case 13:v.original_slot0_fresh=false;break;case 14:v.only_slot0=false;break;case 15:v.sdk_device=0x424d5803;break;
        case 16:v.original_quiet=false;break;case 17:v.sdk_quiet=false;break;case 18:v.sdk_idle=false;break;
        case 19:v.motion_released=false;break;case 20:v.publication_inactive=false;break;case 21:v.parts_zero=false;break;
        case 22:v.capture_released=false;break;case 23:v.gesture_released=false;break;}
        check(!s.observe(v)&&s.phase()==h::Phase::aborted,"fresh invalidation permanently aborts");
        check(!s.observe(sample(1064,14,1.064))&&!s.begin(ticket(),1064),"neutral cannot defer or rearm aborted session");
    }
}
void native_transition_separate_from_input(){
    auto s=queued();b::Pulse pulse;check(pulse.queue(1032,0),"shared input core queues");
    auto r=pulse.poll(1032,0,true,true,true,true,504);check(r.rewrite&&!r.north,"observed baseline");
    r=pulse.poll(1052,0,true,true,true,true,504);check(r.rewrite&&r.north,"single press stage");
    r=pulse.poll(1132,0,true,true,true,true,504);check(r.rewrite&&r.north,"held stage no second rise");
    r=pulse.poll(1212,0,true,true,true,true,504);check(r.rewrite&&!r.north,"observed release");
    r=pulse.poll(1232,0,true,true,true,true,504);check(!r.rewrite&&pulse.phase==b::Phase::complete,"input complete at200ms");
    s.pulse_complete(true);check(s.phase()==h::Phase::observing,"input complete is not board complete");
    check(!s.observe(sample(1600,15,1.6))&&s.phase()==h::Phase::observing,"504 remains allowed after pulse250ms cap");
    auto v=sample(1719,16,1.719);v.physics=103;v.offboard=v.standable=v.motion_supported=false;
    check(!s.observe(v)&&s.saw103(),"native103 observed687ms after input start");
    v=sample(1797,17,1.797);v.physics=100;v.offboard=v.standable=v.motion_supported=false;
    check(!s.observe(v)&&s.saw100(),"native100 observed765ms after input start");
    v=sample(4047,18,4.047);v.physics=100;v.offboard=v.standable=v.motion_supported=false;
    check(!s.observe(v)&&s.phase()==h::Phase::complete,"separate native2250ms dwell completes");
}
void states_timeouts_and_packets(){
    for(unsigned p:{100u,200u,201u,500u}){auto s=queued();auto v=sample(1048,13,1.048);v.physics=p;
        check(!s.observe(v)&&s.phase()==h::Phase::aborted,"wrong route has no software fallback");}
    {auto s=queued();auto v=sample(16000,13,16);check(!s.observe(v)&&s.phase()==h::Phase::aborted,"existing switch deadline not renewed");}
    {auto s=queued();auto v=sample(1031,13,1.048);check(!s.observe(v)&&s.phase()==h::Phase::aborted,"wall rollback aborts");}
    {auto s=queued();s.pulse_complete(false);check(s.phase()==h::Phase::aborted,"failed offer cannot retry");}
    {auto s=queued();auto v=sample(1048,13,1.048);v.physics=103;check(!s.observe(v)&&s.saw103(),"boarding may precede pulse completion");
        v=sample(1064,14,1.064);check(!s.observe(v)&&s.phase()==h::Phase::aborted,"regression504 aborts");}
    b::PacketContinuity c;b::Pad raw;auto final=raw;
    check(c.translate(3,final)==4,"initial packet domain");final.buttons=0x8000;
    check(c.translate(3,final)==5,"North rise increments once");check(c.translate(3,final)==5,"North hold stable");
    final=raw;check(c.translate(3,final)==6,"release increments");check(c.translate(3,raw)==6,"terminal physical neutral stable");
    raw.buttons=1;raw.left_x=123;check(c.translate(3,raw)==7,"postterminal Dpad and axes retained in translated domain");
}
int main(){baseline_and_one_shot();each_guard_aborts();native_transition_separate_from_input();states_timeouts_and_packets();
    std::cout<<"Private BMX native input handoff policy: "<<checks<<" checks passed\n";}
