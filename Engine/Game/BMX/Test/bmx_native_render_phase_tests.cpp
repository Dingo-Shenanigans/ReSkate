#include "Engine/Game/BMX/bmx_native_render_phase.h"
#include "Extension/BMX/bmx_native_motion.h"
#include <cstdio>
#include <cstdlib>
namespace b=dingosdk::bmx;namespace r=b::native_render_phase;namespace ab=b::avatar_binding;
unsigned checks{};
void check(bool v,const char* issue){++checks;if(!v){std::fprintf(stderr,"FAIL %s\n",issue);std::exit(1);}}
int main(){
    ab::Frame f;f.requested=f.actual.available=true;auto& e=f.actual.epoch;
    e.owner_world={1,2,3,4,5,6,7,8};e.bodies={ab::np::BodyKey{6,10,1},ab::np::BodyKey{6,11,1}};
    e.token=1;e.serial=51;e.time=.8499999493360519;e.seconds=static_cast<float>(ab::np::native_seconds);f.native_motion_lease_revision=2;
    b::NativeMotionLeaseStatus l;l.available=l.active=l.details_available=l.epoch_valid=l.timing_available=l.owner_available=l.offboard=l.motion_supported=l.full_rotation=true;
    l.client=1;l.entity=2;l.core=3;l.context=4;l.rig=5;l.serial=e.serial;l.simulation_time=e.time;l.seconds=e.seconds;l.lease_revision=2;
    const r::Clock same{true,51,.8499999493360519},actual275{true,52,.8666666150093079};
    check(r::clock_matches(e,same,false)&&r::clock_matches(e,same,true),"exact source epoch retains animation/render support");
    const auto accepted=r::qualify(f,l,actual275,true);
    check(accepted.accepted&&accepted.one_step_latency,"actual275 one native step presentation handoff is admitted");
    check(accepted.sampled_epoch==e&&accepted.sampled_epoch.serial==51&&accepted.render_clock.serial==52&&accepted.source_lease_revision==2,"sampled actual pair epoch is never relabeled as newer physics");
    check(!r::qualify(f,l,actual275,false).accepted,"one-step latency is render-only, never animation or physics admission");
    for(const auto bad:{r::Clock{false,52,actual275.time},r::Clock{true,50,e.time-e.seconds},r::Clock{true,53,e.time+2*e.seconds},
        r::Clock{true,52,e.time},r::Clock{true,51,actual275.time},r::Clock{true,52,std::nextafter(actual275.time,1.)},
        r::Clock{true,52,std::numeric_limits<double>::quiet_NaN()},r::Clock{true,52,std::numeric_limits<double>::infinity()}})
        check(!r::qualify(f,l,bad,true).accepted,"missing/rewound/skipped/drifted/nonfinite presentation clock refuses");
    auto overflow=e;overflow.serial=std::numeric_limits<std::uint64_t>::max();
    check(!r::clock_matches(overflow,{true,0,overflow.time+overflow.seconds},true),"serial rollover cannot masquerade as one successor");
    auto huge=e;huge.time=std::numeric_limits<double>::max();check(!r::clock_matches(huge,{true,huge.serial+1,huge.time},true),"nonadvancing large-double time cannot masquerade as native step");
    for(float bad:{0.f,-1.f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),std::nextafter(e.seconds,1.f)}){
        auto wrong=f;wrong.actual.epoch.seconds=bad;auto lease=l;lease.seconds=bad;
        check(!r::qualify(wrong,lease,actual275,true).accepted,"nonfinite/non-native/source-changed h cannot qualify successor");}
    for(unsigned index=0;index<5;++index){auto wrong=l;if(index==0)++wrong.client;if(index==1)++wrong.entity;if(index==2)++wrong.core;if(index==3)++wrong.context;if(index==4)++wrong.rig;
        check(!r::qualify(f,wrong,actual275,true).accepted,"every current SDK owner identity field still exact");}
    const auto flags={&b::NativeMotionLeaseStatus::available,&b::NativeMotionLeaseStatus::active,&b::NativeMotionLeaseStatus::details_available,
        &b::NativeMotionLeaseStatus::epoch_valid,&b::NativeMotionLeaseStatus::timing_available,&b::NativeMotionLeaseStatus::owner_available,
        &b::NativeMotionLeaseStatus::offboard,&b::NativeMotionLeaseStatus::motion_supported,&b::NativeMotionLeaseStatus::full_rotation};
    for(auto flag:flags){auto wrong=l;wrong.*flag=false;check(!r::qualify(f,wrong,actual275,true).accepted,"missing/expired/source-unavailable lease never qualifies");}
    for(unsigned fault=0;fault<6;++fault){auto wrong=l;auto frame=f;
        if(fault==0)++wrong.lease_revision;if(fault==1)++wrong.serial;if(fault==2)wrong.simulation_time=actual275.time;
        if(fault==3)wrong.seconds=std::nextafter(e.seconds,1.f);if(fault==4)frame.native_motion_lease_revision=0;if(fault==5)frame.requested=false;
        check(!r::qualify(frame,wrong,actual275,true).accepted,"wrong revision/sourceepoch/h or unrequested frame cannot use render successor");}
    check(r::qualify(f,l,same,true).accepted&&!r::qualify(f,l,same,true).one_step_latency,"same-epoch render receipt remains explicit");
    check(f.actual.epoch==e&&l.serial==51&&l.simulation_time==e.time,"qualification never mutates frame/lease clocks");
    std::printf("sampled physics/render presentation phase: %u checks PASS\n",checks);return 0;
}
