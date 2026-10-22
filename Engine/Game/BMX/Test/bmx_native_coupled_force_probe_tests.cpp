#include "Engine/Game/BMX/bmx_native_coupled_force_probe.h"
#include "Extension/BMX/bmx_collision_envelope.h"
#include <cstdio>
#include <cstdlib>
#include <string_view>
namespace f=dingosdk::bmx::native_coupled::force_probe;
namespace n=dingosdk::bmx::native_coupled::policy;
unsigned checks{};void check(bool okay,const char* name){++checks;if(!okay){std::fprintf(stderr,"FAIL %s\n",name);std::exit(1);}}
f::Sample initial(){f::Sample s;s.epoch.owner_world={1,2,3,4,5,6,7,8};s.epoch.bodies={n::BodyKey{6,1,2},n::BodyKey{6,3,4}};
    s.epoch.token=1;s.epoch.serial=100;s.epoch.time=100*n::native_seconds;s.epoch.seconds=static_cast<float>(n::native_seconds);
    for(unsigned role=0;role<2;++role){s.mass[role]=n::v2_roles[role].mass;s.inertia[role]=n::v2_roles[role].principal_moments;
        s.principal_orientation[role]=n::native_orientation(role?n::rider_from_chassis:n::Quat{0,0,0,1},n::v2_roles[role].principal_to_model);
        s.clearance[role]=.5f;s.maximum_angular_speed[role]=15;s.integrates_angular_accumulator[role]=true;s.position[role]={float(role)*2.1f,1,0};}
    return s;}
n::Receipt receipt(const f::Sample& before){n::Receipt r;r.state=n::ReceiptState::submitted;r.epoch=before.epoch;r.token=before.epoch.token;
    r.serial=before.epoch.serial;r.attempted=r.completed=15;r.inertia_before_wrenches=true;return r;}
// Explicit synthetic host fixture for policy tests ONLY. It is never compiled
// into the native operator or used as measured game evidence.
f::Sample advance(f::Sample s,const n::Command& cmd){const auto h=s.epoch.seconds;++s.epoch.serial;s.epoch.time+=h;s.collides_with.fill(0x101);
    for(unsigned role=0;role<2;++role){auto& q=s.principal_orientation[role];const auto axis=n::rotate(q,{0,1,0});
        for(unsigned a=0;a<3;++a){s.linear[role][a]+=cmd.roles[role].force[a]/s.mass[role]*h;
            s.angular[role][a]+=static_cast<float>(axis[a]*f::dot(cmd.roles[role].torque,axis)/s.inertia[role][1]*h);}
        s.linear[role][1]-=9.8f*h;for(unsigned a=0;a<3;++a)s.position[role][a]+=s.linear[role][a]*h;
        s.clearance[role]+=s.linear[role][1]*h;const auto speed=n::length(s.angular[role]);
        if(speed>1e-10){const auto scale=std::sin(speed*h/2)/speed;
            q=n::multiply({static_cast<float>(s.angular[role][0]*scale),static_cast<float>(s.angular[role][1]*scale),static_cast<float>(s.angular[role][2]*scale),static_cast<float>(std::cos(speed*h/2))},q);}
    }return s;}
int main(){
    auto before=initial();f::Monitor m;check(m.begin(before),"actual initial config admitted");
    for(unsigned i=0;i<f::command_count;++i){const auto cmd=m.pending_command();check(n::prepare(cmd,n::native_seconds).valid,"existing SDK budgets unchanged for every deterministic group");
        const auto after=advance(before,cmd);check(m.accept(receipt(before),after),"exact successor all-four receipt and small signed response accepted");before=after;}
    check(m.report().phase==f::Phase::complete&&m.report().count==7&&m.report().completed==6&&m.report().bounded_application_candidate,"finite six-command seven-observation candidate");
    for(unsigned i=1;i<=4;++i)check(m.report().linear_response[i]>0&&m.report().angular_response[i]>0,"both roles and both signs require actual force and torque responses");
    for(unsigned fault=0;fault<7;++fault){f::Monitor bad;const auto start=initial();check(bad.begin(start),"rejection setup");auto after=advance(start,bad.pending_command());auto r=receipt(start);
        if(fault==0)r.completed=7;else if(fault==1)r.epoch.owner_world[7]++;else if(fault==2)after.epoch.bodies[1].generation++;
        else if(fault==3)after.maximum_angular_speed[1]=14;else if(fault==4)after.integrates_angular_accumulator[0]=false;
        else if(fault==5){after.principal_orientation[1]=n::multiply({.1f,0,0,.994987437f},after.principal_orientation[1]);after.angular[1]={};}
        else after.epoch.serial++;
        check(!bad.accept(r,after)&&bad.report().phase==f::Phase::failed&&bad.report().count==1,"refused sample cannot advance accepted history");
        if(fault==5)check(std::string_view(bad.report().rejected_check)=="probe_actual_quaternion_step_cap","quaternion jump rejected despite harmless final angular velocity");
    }
    f::Monitor noresponse;before=initial();check(noresponse.begin(before),"missing response setup");
    auto after=advance(before,noresponse.pending_command());check(noresponse.accept(receipt(before),after),"baseline accepted");before=after;
    after=advance(before,{});check(!noresponse.accept(receipt(before),after),"producer receipt alone cannot prove force application");
    for(unsigned role=0;role<2;++role){const auto plan=f::placement(dingosdk::bmx::collision_envelope::capsules,role,{},{0,0,0,1},{0,1,0});
        check(plan.valid,"finite private air fixture");const auto actual=f::cal::proxy_plane_clearance(dingosdk::bmx::collision_envelope::capsules,role,plan.com,plan.native_orientation,{},{0,1,0});
        check(actual&&std::abs(*actual-.5f)<.0001f,"both actual proxy support minima start at half metre");}
    n::Once once;const auto owner=initial().epoch;once.reset(owner.token);bool armed=true;unsigned calls{};
    const auto interrupted=once.run(owner,{},[&](){return armed;},[&](unsigned,bool,const n::Vec&){if(++calls==2)armed=false;return true;});
    check(interrupted.state==n::ReceiptState::terminal&&interrupted.attempted==3&&interrupted.completed==3&&calls==2,"private disarm between native writes is terminal and skips remaining roles");
    armed=true;const auto replay=once.run(owner,{},[&](){return armed;},[&](unsigned,bool,const n::Vec&){++calls;return true;});
    check(once.terminal()&&replay.state!=n::ReceiptState::submitted&&calls==2,"rearming cannot retry partially submitted group");
    for(unsigned role=0;role<2;++role)for(unsigned kind=0;kind<2;++kind){f::Monitor limited;const auto start=initial();check(limited.begin(start),"diagonal norm setup");
        auto next=advance(start,limited.pending_command());if(kind)next.angular[role]={.2f,.2f,.2f};else next.linear[role]={2.5f,2.5f,0};
        check(!f::config(next),"diagonal speed over norm cap refuses valid individual components");
        check(!limited.accept(receipt(start),next)&&limited.report().count==1&&limited.report().phase==f::Phase::failed,"norm refusal cannot advance accepted producer history");
    }
    for(unsigned role=0;role<2;++role)for(float invalid:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}){
        f::Monitor strict;const auto start=initial();check(strict.begin(start),"nonfinite mass policy setup");auto next=advance(start,strict.pending_command());next.mass[role]=invalid;
        check(!f::config(next),"pure config rejects nonfinite mass independently of native producer constraints");
        check(!strict.accept(receipt(start),next)&&strict.report().count==1,"nonfinite mass cannot advance receipt history");
    }
    std::printf("coupled finite small-wrench probe policy: %u checks PASS\n",checks);
}
