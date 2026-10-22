#include "Engine/Game/BMX/bmx_human_native_handoff.h"
#include <cstdlib>
#include <iostream>
#include <limits>
namespace h=dingosdk::bmx::human_native_handoff;
namespace n=dingosdk::bmx::native_input_handoff;
namespace b=dingosdk::input::native_board_bridge;
unsigned checks{};
void check(bool ok,const char* why){++checks;if(!ok){std::cerr<<"FAIL: "<<why<<'\n';std::exit(1);}}
n::Ticket ticket(){return {{1,2,3,4,5,6},{7,8,9,10,11},{20,21,22,23,24,1,1},16000,10,1.0};}
b::Pad chord(){b::Pad p;p.buttons=0x8100;return p;}
h::Guard guard(unsigned slot=0,std::uint64_t now=1032){return {now,now,ticket().owner,1u<<slot,true,true,true};}
n::Sample sample(std::uint64_t now=1032,std::uint64_t serial=12,double time=1.032){
    n::Sample s;s.now=s.sampled_ms=now;s.serial=serial;s.native_time=time;s.owner=ticket().owner;s.body=ticket().body;s.counters=ticket().counters;s.physics=504;
    s.configuration=s.gates=s.canonical=s.clock_coherent=s.pool_released=s.sdk_idle=s.motion_released=s.publication_inactive=s.parts_zero=s.capture_released=true;
    s.sdk_quiet=s.gesture_released=s.offboard=s.standable=s.motion_supported=true;return s;
}
void note(h::Intent& i,unsigned slot,std::uint64_t now,b::Pad p={}){i.note(true,slot,true,now,3,p);}
h::Intent admitted(unsigned slot=0){h::Intent i;note(i,slot,980);note(i,slot,1000,chord());check(i.admit(1000,1u<<slot,ticket().owner),"fresh actual original chord admitted");return i;}
h::Intent queued(unsigned slot=0){auto i=admitted(slot);check(i.transfer(ticket(),1000),"full cleanup ticket transferred");note(i,slot,1016);
    i.observe(sample(1016,11,1.016),guard(slot,1016));check(!i.pulse().used,"first advanced release sample waits");
    note(i,slot,1032);i.observe(sample(),guard(slot));check(i.pulse().live(),"second released sample queues one pulse");return i;}
void admission(){
    for(unsigned slot=0;slot<4;++slot){auto i=admitted(slot);check(i.slot()==slot,"actual slot mapping");check(i.generation()==1,"first generation");}
    for(unsigned device:{0u,3u,5u,15u,0x100u,0x424d5803u}){h::Intent i;note(i,0,980);note(i,0,1000,chord());check(!i.admit(1000,device,ticket().owner),"non-single-XInput source rejected");}
    {h::Intent i;note(i,0,1000,chord());check(!i.admit(1000,1,ticket().owner),"held-at-first-read is not a fresh chord");}
    {h::Intent i;note(i,0,980);auto p=chord();p.buttons=0x8000;note(i,0,990,p);note(i,0,1000,chord());check(!i.admit(1000,1,ticket().owner),"late LB on held North rejected");}
    {h::Intent i;i.note(false,0,true,980,3,{});i.note(false,0,true,1000,3,chord());check(!i.admit(1000,1,ticket().owner),"SDK/nested reads cannot prove source");}
    for(unsigned delta:{251u,1000u}){h::Intent i;note(i,0,980);note(i,0,1000,chord());check(!i.admit(1000+delta,1,ticket().owner),"stale actual chord rejected");}
    {h::Intent i;note(i,0,980);note(i,0,1001,chord());check(!i.admit(1000,1,ticket().owner),"future original rejected");}
    {h::Intent i;note(i,0,980);note(i,0,1000,chord());note(i,1,999);check(!i.admit(1000,1,ticket().owner),"fresh additional original slot rejected");}
    {h::Intent i;note(i,0,980);note(i,0,1000,chord());i.note(true,0,false,1001,0,{});check(!i.admit(1001,1,ticket().owner),"failed original never fabricates release/chord");}
    {auto i=admitted();i.cancel("test");check(!i.admit(1001,1,ticket().owner),"same held rise spent after cancellation");}
    {auto i=admitted(2);i.cancel("test");check(!i.admit(1001,1,ticket().owner)&&i.slot()==2&&i.generation()==1,"rejected other-slot request cannot relabel prior accepted generation");}
    {h::Intent i;note(i,0,980);auto p=chord();p.buttons|=1;note(i,0,1000,p);check(!i.admit(1000,1,ticket().owner),"D-pad during admission does not park");}
    {h::Intent i;note(i,0,980);auto p=chord();p.left_trigger=13;note(i,0,1000,p);check(!i.admit(1000,1,ticket().owner),"active trigger does not park");}
}
void release_and_context(){
    {auto i=admitted(2);check(i.transfer(ticket(),1000),"held ticket accepted");auto v=sample(1016,11,1.016);v.sdk_quiet=v.gesture_released=false;
        auto g=guard(2,1016);g.sdk_released=false;note(i,2,1016,chord());i.observe(v,g);check(i.active()&&!i.pulse().used,"held chord waits without fake quiet");
        note(i,2,1032);v=sample();v.capture_released=false;i.observe(v,guard(2));check(i.active()&&!i.pulse().used,"capture-through-release waits");
        note(i,2,1048);i.observe(sample(1048,13,1.048),guard(2,1048));check(!i.pulse().used,"first post-lease sample waits");
        note(i,2,1064);i.observe(sample(1064,14,1.064),guard(2,1064));check(i.pulse().live(),"next post-lease sample ready");}
    for(unsigned k=0;k<11;++k){auto i=admitted();auto g=guard(0,1016);g.sdk_released=false;
        switch(k){case 0:g.allowed=false;break;case 1:++g.owner[3];break;case 2:g.device=2;break;case 3:g.device=3;break;
        case 4:g.now=999;break;case 5:g.now=16000;break;case 6:g.sampled_ms=0;break;case 7:g.sampled_ms=g.now+1;break;
        case 8:g.sampled_ms=g.now-251;break;case 9:g.sdk_gesture_only=false;break;case 10:i.note(true,0,false,1016,0,{});break;}
        check(!i.guard(g)&&!i.active(),"changed context cannot defer intent");check(!i.guard(guard()),"later neutral does not retry");}
    {auto i=admitted();note(i,0,1016);note(i,0,1017,chord());check(!i.active(),"physical reentry before SDK release cancels");}
    {auto i=admitted();note(i,1,1016);check(!i.active(),"new second original device cancels");}
    {auto i=admitted();i.note(true,0,true,999,3,{});check(!i.active(),"source wall rollback cancels");}
    {auto i=admitted();auto t=ticket();++t.body.token;t.owner[1]++;check(!i.transfer(t,1001)&&!i.active(),"foreign cleanup ticket refuses with no fallback");}
    {auto i=admitted();auto t=ticket();t.deadline_ms=16002;check(!i.transfer(t,1001)&&!i.active(),"cleanup ticket cannot renew 15s");}
    {auto i=admitted();auto t=ticket();t.deadline_ms=16001;check(!i.transfer(t,1001)&&!i.active(),"one millisecond admission deadline extension rejected");}
    for(unsigned variant=0;variant<4;++variant){auto i=admitted();check(i.transfer(ticket(),1000),"held native clock ticket");
        auto s=sample(1016,20,1.20);s.sdk_quiet=s.gesture_released=false;auto g=guard(0,1016);g.sdk_released=false;
        note(i,0,1016,chord());i.observe(s,g);check(i.active(),"held coherent clock is observed");
        s=sample(1032,variant==0?19:variant==1?20:variant==2?21:19,variant==0?1.19:variant==1?1.21:variant==2?1.20:1.19);
        g=guard(0,1032);
        if(variant!=3){s.sdk_quiet=s.gesture_released=false;g.sdk_released=false;note(i,0,1032,chord());}else note(i,0,1032);
        i.observe(s,g);check(!i.active(),"held/first released native regression or unmatched serial-time pair rejected");}
}
void input_and_native(){
    for(unsigned slot=0;slot<4;++slot){auto i=queued(slot);
        auto r=i.poll(1032,slot,true,true,true,504,{});check(r.rewrite&&!r.north,"native baseline");
        r=i.poll(1052,slot,true,true,true,504,{});check(r.rewrite&&r.north,"single North rise");
        r=i.poll(1132,slot,true,true,true,504,{});check(r.rewrite&&r.north,"press hold");
        r=i.poll(1212,slot,true,true,true,504,{});check(r.rewrite&&!r.north,"genuine release offer");
        r=i.poll(1232,slot,true,true,true,504,{});check(!r.rewrite&&i.session().phase()==n::Phase::observing,"transport complete is not native success");
        auto v=sample(1700,20,1.7);v.physics=103;note(i,slot,1700);i.observe(v,guard(slot,1700));check(i.session().saw103(),"real observed native103");
        v=sample(1800,21,1.8);v.physics=100;note(i,slot,1800);i.observe(v,guard(slot,1800));check(i.active()&&i.session().saw100(),"native100 still needs dwell");
        v=sample(4051,22,4.051);v.physics=100;note(i,slot,4051);i.observe(v,guard(slot,4051));check(!i.active()&&i.session().phase()==n::Phase::complete,"native and wall dwell complete");
        const auto p=i.packet(slot,3,{},true);note(i,slot,4100);note(i,slot,4116,chord());check(i.admit(4116,1u<<slot,ticket().owner),"new real chord admits next generation");
        check(i.generation()==2&&i.packet(slot,3,{},false)==p,"request reset preserves packet domain");}
    for(unsigned k=0;k<5;++k){auto i=queued();auto p=b::Pad{};if(k==3)p.buttons=1;
        const auto r=i.poll(k==4?16000:1032,0,k!=0,k!=1,k!=2,504,p);check(!r.rewrite&&!i.active(),"callback identity focus capture activity deadline failures cancel");}
    {auto i=queued();auto r=i.poll(1032,0,true,true,true,504,{});check(r.rewrite,"first baseline");r=i.poll(1153,0,true,true,true,504,{});check(!r.rewrite&&!i.active(),"poll stall has no retry");}
    {auto i=queued();auto v=sample(1048,13,1.048);v.physics=200;note(i,0,1048);i.observe(v,guard(0,1048));check(!i.active(),"software200 route rejected");}
    for(unsigned k=0;k<17;++k){auto i=queued();auto v=sample(1048,13,1.048);note(i,0,1048);
        switch(k){case 0:v.configuration=false;break;case 1:v.gates=false;break;case 2:v.canonical=false;break;case 3:v.protected_owner=true;break;
        case 4:++v.body.generation;break;case 5:v.pool_released=false;break;case 6:++v.counters[3];break;case 7:v.clock_coherent=false;break;
        case 8:v.native_time=std::numeric_limits<double>::quiet_NaN();break;case 9:v.serial=11;break;case 10:v.sdk_idle=false;break;
        case 11:v.motion_released=false;break;case 12:v.publication_inactive=false;break;case 13:v.parts_zero=false;break;
        case 14:v.capture_released=false;break;case 15:v.sdk_quiet=false;break;case 16:v.gesture_released=false;break;}
        i.observe(v,guard(0,1048));check(!i.active(),"all cleanup and current-native proof gates retained after queue");}
}
void packet_domains(){
    h::Intent i;b::Pad p{};check(i.packet(0,3,p,false)==3,"default-off passthrough");
    const auto a=i.packet(2,0xffffffffu,p,true);check(a==0,"counter wraps naturally");
    p.buttons=0x8000;check(i.packet(2,0xffffffffu,p,true)==a+1,"press packet advances");
    p.buttons=0;check(i.packet(2,0xffffffffu,p,false)==a+2,"release advances even when original is unchanged");
    check(i.packet(2,0xffffffffu,p,false)==a+2,"unchanged return stable");
    p.buttons=1;check(i.packet(2,0xffffffffu,p,false)==a+3&&p.buttons==1,"later ordinary D-pad advances and is not swallowed");
    check(i.packet(1,3,{},false)==3,"unselected slot domain unchanged");
}

h::AdmissionContext pending_context(unsigned slot=0,std::uint64_t now=1000,std::uint64_t serial=10,double time=1.0){
    return {now,now,serial,time,ticket().owner,1u<<slot,true,true};
}
void pending_note(h::Intent& i,h::AdmissionWait& w,unsigned slot,std::uint64_t now,b::Pad p={},bool success=true){
    i.note(true,slot,success,now,4,p);w.note(slot,success,now,i);
}
void pending_admission(){
    for(unsigned slot=0;slot<4;++slot)for(unsigned initial:{0u,0x100u}){
        h::Intent i;h::AdmissionWait w;auto raw=chord();raw.buttons=static_cast<std::uint16_t>(initial);note(i,slot,1000,raw);
        check(!i.admit(1000,1u<<slot,ticket().owner),"SDK precedes genuine original North: strict admission refuses");
        check(w.begin(pending_context(slot),i),"only neutral/LB armed original begins bounded wait");
        check(!i.active()&&!i.pulse().used&&i.generation()==0,"pending cannot acquire body or Pulse");
        auto c=pending_context(slot,1008,11,1.008);check(w.update(c,i)==h::AdmissionWaitResult::waiting,"unchanged original waits without another SDK edge");
        pending_note(i,w,slot,1016,chord());c=pending_context(slot,1016,12,1.016);
        check(w.update(c,i)==h::AdmissionWaitResult::ready&&w.active(),"source-ready wait stays guarded until atomic consumption");
        check(w.consume(c,i)&&!w.active(),"fresh original witness consumes once before strict admission");
        check(i.admit(c.now,c.device,c.owner)&&i.generation()==1,"same unchanged strict policy admits after genuine proof");
        check(w.update(c,i)==h::AdmissionWaitResult::idle&&!w.begin(c,i),"consumed wait cannot admit or rearm active Intent");
    }
    for(unsigned initial:{0u,0x100u})for(unsigned delta:{249u,250u,251u}){
        h::Intent i;h::AdmissionWait w;auto raw=chord();raw.buttons=static_cast<std::uint16_t>(initial);note(i,0,1000,raw);check(w.begin(pending_context(),i),"deadline wait begins");
        pending_note(i,w,0,1000+delta,chord());const auto c=pending_context(0,1000+delta,11,1.3);
        check((w.update(c,i)==h::AdmissionWaitResult::ready)==(delta<=250),"original 250ms boundary is inclusive and never renewed");
    }
    for(unsigned variant=0;variant<17;++variant){
        h::Intent i;h::AdmissionWait w;note(i,0,1000);check(w.begin(pending_context(),i),"guard negative setup");
        auto c=pending_context(0,1016,11,1.016);
        switch(variant){case 0:c.allowed=false;break;case 1:c.sdk_chord_held=false;break;case 2:++c.owner[2];break;case 3:c.device=2;break;
        case 4:c.device=3;break;case 5:c.now=999;c.sampled_ms=999;break;case 6:c.sampled_ms=999;break;
        case 7:c.sampled_ms=0;break;case 8:c.sampled_ms=c.now+1;break;case 9:c.now=1251;c.sampled_ms=c.now;break;
        case 10:c.serial=9;break;case 11:c.native_time=.9;break;case 12:c.serial=10;break;case 13:c.native_time=1;break;
        case 14:c.native_time=std::numeric_limits<double>::quiet_NaN();break;case 15:c.serial=0;break;case 16:c.owner={};break;}
        check(w.update(c,i)==h::AdmissionWaitResult::canceled&&!w.active(),"menu/focus/release/input/owner/device/native and SDK clocks cancel");
        pending_note(i,w,0,1020,chord());check(w.update(pending_context(0,1020,12,1.02),i)==h::AdmissionWaitResult::idle,"cancellation never defers later held chord");
    }
    for(unsigned variant=0;variant<12;++variant){
        h::Intent i;h::AdmissionWait w;auto raw=chord();raw.buttons=0x100;note(i,0,1000,raw);check(w.begin(pending_context(),i),"sticky original negative setup");
        raw=chord();
        switch(variant){case 0:raw.buttons=0;break;case 1:raw.buttons=0x8000;break;case 2:raw.buttons|=1;break;case 3:raw.left_trigger=13;break;
        case 4:raw.right_trigger=13;break;case 5:raw.left_x=1639;break;case 6:raw.right_y=-1639;break;
        case 7:pending_note(i,w,0,1010,{},false);break;case 8:pending_note(i,w,1,1010);break;
        case 9:pending_note(i,w,0,999,raw);break;case 10:pending_note(i,w,0,1010,raw);raw.buttons=0x100;break;
        case 11:pending_note(i,w,0,1010,raw);raw.buttons=0;break;}
        if(variant<7||variant>=10)pending_note(i,w,0,1011,raw);
        check(!w.active(),"genuine transient release/new activity/disconnect/extra slot cancels sticky");
        pending_note(i,w,0,1016,chord());check(w.update(pending_context(0,1016,12,1.016),i)==h::AdmissionWaitResult::idle,"later raw reentry cannot revive same gesture");
    }
    {h::Intent i;h::AdmissionWait w;note(i,0,1000);check(w.begin(pending_context(),i),"simultaneous starts from original neutral");
        auto raw=chord();raw.buttons=0x100;pending_note(i,w,0,1008,raw);pending_note(i,w,0,1016,chord());
        check(w.update(pending_context(0,1016,11,1.016),i)==h::AdmissionWaitResult::ready,"neutral then original LB then North is supported");}
    {h::Intent i;h::AdmissionWait w;note(i,0,1000);check(w.begin(pending_context(),i),"Y-first negative setup");
        auto raw=chord();raw.buttons=0x8000;pending_note(i,w,0,1008,raw);pending_note(i,w,0,1016,chord());
        check(!w.active()&&w.update(pending_context(0,1016,11,1.016),i)==h::AdmissionWaitResult::idle,"late original LB on held Y never satisfies pending");}
    {h::Intent i;h::AdmissionWait w;note(i,0,1000);check(w.begin(pending_context(),i),"nonrenewable wait setup");
        check(w.update(pending_context(0,1249,11,1.249),i)==h::AdmissionWaitResult::waiting,"late waiting poll preserves start");
        pending_note(i,w,0,1251,chord());check(w.update(pending_context(0,1251,12,1.251),i)!=h::AdmissionWaitResult::ready&&w.started_ms()==1000,"poll never renews original wait deadline");}
    for(unsigned variant=0;variant<9;++variant){h::Intent i;h::AdmissionWait w;auto c=pending_context();auto raw=chord();raw.buttons=0x100;
        switch(variant){case 0:note(i,0,1000,chord());break;case 1:raw.buttons=0x8000;note(i,0,1000,raw);break;
        case 2:note(i,0,1000,raw);note(i,1,1000);break;case 3:note(i,0,749,raw);break;case 4:note(i,0,1001,raw);break;
        case 5:i.note(false,0,true,1000,4,raw);break;case 6:note(i,0,1000,raw);c.allowed=false;break;
        case 7:note(i,0,1000,raw);c.sdk_chord_held=false;break;case 8:note(i,0,1000,raw);c.device=0;break;}
        check(!w.begin(c,i),"already-ready/held Y/foreign/stale/future/SDK-only/invalid context cannot arm wait");}
}

int main(){pending_admission();admission();release_and_context();input_and_native();packet_domains();std::cout<<checks<<" human native handoff checks passed\n";}
