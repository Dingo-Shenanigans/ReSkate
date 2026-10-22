#include "Engine/Game/BMX/bmx_native_coupled_calibration.h"
#include "Extension/BMX/bmx_collision_envelope.h"
#include <cstdio>
#include <cstdlib>
namespace c=dingosdk::bmx::native_coupled::calibration;
namespace p=dingosdk::bmx::native_coupled::policy;
namespace e=dingosdk::bmx::collision_envelope;
unsigned checks{};void check(bool okay,const char* name){++checks;if(!okay){std::fprintf(stderr,"FAIL %s\n",name);std::exit(1);}}
void near(double a,double b,double tolerance,const char* name){check(std::isfinite(a)&&std::abs(a-b)<=tolerance,name);}
c::Sample initial(){c::Sample s;s.epoch.owner_world={1,2,3,4,5,6,7,8};s.epoch.bodies={p::BodyKey{6,1,1},p::BodyKey{6,2,1}};
    s.epoch.token=1;s.epoch.serial=100;s.epoch.seconds=static_cast<float>(p::native_seconds);s.epoch.time=100*p::native_seconds;s.clearance={.08f,.08f};return s;}
void next(c::Sample& s){++s.epoch.serial;s.epoch.time+=s.epoch.seconds;}
int main(){
    for(unsigned scenario=0;scenario<3;++scenario){
        const float pitch=scenario==0?0:.01067f,roll=scenario==0?0:-.0066f,yaw=scenario==2?1.3f:0;
        const p::Quat x{std::sin(pitch/2),0,0,std::cos(pitch/2)},z{0,0,std::sin(roll/2),std::cos(roll/2)},y{0,std::sin(yaw/2),0,std::cos(yaw/2)};
        const auto q=p::multiply(p::multiply(y,x),z);const auto up=p::rotate(q,{0,1,0});
        check(c::fixture_supported(pitch,roll,up),"measured flat/reviewed native266 slope fixture supported");
        const p::Vec plane=scenario==2?p::Vec{100,20,-30}:p::Vec{};
        for(unsigned role=0;role<2;++role){const auto plan=c::world_placement(e::capsules,role,plane,q,up);
            check(plan.valid,"both genuine roles have bounded per-role placement");
            const auto measured=c::proxy_plane_clearance(e::capsules,role,plan.com,plan.native_orientation,plane,up);
            check(measured.has_value(),"readback support function valid");near(*measured,.08,.0001,"equal actual lowest capsule clearance");
            const auto shift=c::sub(plan.root,plane);near(c::dot(shift,up),plan.translation,.00001,"translation follows actual support normal");
            const auto tangent=c::sub(shift,c::add_scaled({},up,c::dot(shift,up)));near(p::length(tangent),0,.00001,"no world-horizontal flattening or arbitrary relocation");
            const auto native=p::native_orientation(role?p::multiply(q,p::rider_from_chassis):q,p::v2_roles[role].principal_to_model);
            check(p::pose_matches(plan.com,plan.native_orientation,plan.com,native),"native principal/model orientation unchanged");
            if(!scenario){float authored_lowest=100;
                for(unsigned shape=role?5u:0u;shape<(role?8u:5u);++shape)
                    authored_lowest=std::min(authored_lowest,std::min(e::capsules[shape].a[1],e::capsules[shape].b[1])-e::capsules[shape].radius);
                near(plan.original_clearance,authored_lowest,.00001,"principal-frame support agrees with independent authored neutral plane support");
                near(plan.translation,.08f-authored_lowest,.00001,"private world fixture role-specific normal offset");
                std::printf("world fixture role%u original=%.9g normal_translation=%.9g actual_planned=%.9g\n",role,plan.original_clearance,plan.translation,*measured);}
        }
    }
    check(!c::world_placement(e::capsules,2,{},{0,0,0,1},{0,1,0}).valid,"unknown role rejected");
    check(!c::world_placement(e::capsules,1,{},{0,0,0,1},{0,.5f,0}).valid,"nonunit measured support normal rejected");
    auto invalid=e::capsules;invalid[5].radius=-.1f;check(!c::world_placement(invalid,1,{},{0,0,0,1},{0,1,0}).valid,"invalid actual capsule cannot authorize clearance");
    // A role1 cap failure must not advance role0's fall, contact or settling
    // counters, even when role0 alone would have supplied that evidence.
    for(unsigned prelude=0;prelude<3;++prelude){c::Monitor m;auto sample=initial();check(m.begin(c::Kind::world_contact,sample),"counter atomicity begin");
        if(prelude){next(sample);for(unsigned role=0;role<2;++role){sample.position[role][1]=-.004f;sample.linear[role][1]=-.3f;sample.clearance[role]=.06f;}
            check(m.observe(sample),"genuine bounded falling accepted");}
        if(prelude==2){next(sample);sample.position[0][1]=-.005f;sample.linear[0][1]=0;sample.clearance[0]=.01f;
            check(m.observe(sample),"bounded contact/first settle accepted");check(m.report().contact_candidate[0]&&m.report().settled[0]==1,"settle precondition");}
        const auto before=m.report();next(sample);sample.position[0][1]=-.006f;sample.linear[0][1]=prelude?0.f:-.3f;sample.clearance[0]=prelude?.01f:.06f;sample.angular[1][0]=8.1f;
        check(!m.observe(sample),"role1 actual cap rejects whole pair");const auto& after=m.report();
        check(after.falling==before.falling&&after.contact_candidate==before.contact_candidate&&after.settled==before.settled,
            "rejected pair advances neither role's fall/contact/settle evidence");
        check(after.count==before.count&&after.rejected_sample_available&&after.rejected_role==1,"rejected actual sample remains separate from accepted history");
    }
    std::printf("coupled equal-clearance fixture/atomic evidence: %u checks PASS\n",checks);
}
