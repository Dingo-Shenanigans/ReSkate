#include "Engine/Game/BMX/bmx_mount_transition.h"
#include <cstdlib>
#include <iostream>
#include <limits>

namespace m=dingosdk::bmx::mount_transition;
namespace c=dingosdk::bmx::chassis;
void check(bool value,const char* message){if(!value){std::cerr<<message<<'\n';std::exit(1);}}
m::Quat yaw(float angle){return {0,std::sin(angle*.5f),0,std::cos(angle*.5f)};}
std::array<float,16> matrix(m::Quat q){std::array<float,16> out{};out[15]=1;
    for(unsigned i=0;i<3;++i){m::Vec axis{};axis[i]=1;const auto v=c::rotate_unit(q,axis);
        for(unsigned j=0;j<3;++j)out[i*4+j]=v[j];}return out;}
void native_orientation(){
    for(const auto q:{yaw(0),yaw(.7f),yaw(3.14159265f),m::Quat{1,0,0,0},m::Quat{0,0,1,0},m::Quat{.5f,.5f,.5f,.5f}}){
        auto input=matrix(q);input[12]=627;input[13]=199;input[14]=1062;m::Quat output{};
        check(m::native_rotation(input,output)&&m::angle(q,output)<.001f,"native matrix retains root quaternion basis on every trace branch");
    }
    for(unsigned which=0;which<4;++which){auto bad=matrix(yaw(.4f));
        if(which==0)bad[0]*=2;
        if(which==1)for(unsigned i=0;i<3;++i)bad[i]=-bad[i];
        if(which==2)bad[4]+=.1f;
        if(which==3)bad[10]=std::numeric_limits<float>::quiet_NaN();
        m::Quat output{.5f,.5f,.5f,.5f},before=output;
        check(!m::native_rotation(bad,output)&&output==before,"invalid native scale/reflection/shear/nonfinite matrix cannot partially replace orientation");
    }
}
void timing_and_bounds(){
    for(float distance:{0.f,.35f,.65f,.7f})for(float angle:{0.f,1.57f,3.14159265f})for(double h:{1./30,1./60,1./120}){
        m::Transition state;const m::Vec origin{-distance,0,0},target{};const auto q=yaw(angle);
        check(state.capture(origin,yaw(0)),"fresh standing pose captured");auto sample=state.sample(target,q,0);
        check(sample.ready&&sample.root==origin&&sample.progress==0&&m::length(sample.velocity)==0,"first lease exactly preserves standing root and velocity");
        check(sample.duration>=.75f,"ordinary mounts cannot finish inside old .4 second interval");
        auto prior=sample;
        while(sample.progress<1){sample=state.sample(target,q,h);check(sample.ready,"valid native time advances mount");
            check(m::length(sample.velocity)<=m::relative_speed_limit+.001f,"approach contribution obeys native-root speed bound");
            check(m::angle(prior.rotation,sample.rotation)/static_cast<float>(h)<=m::angular_speed_limit+.01f,"shortest arc obeys alignment speed bound");
            m::Quat unit;check(c::unit_quaternion(sample.rotation,unit),"every submitted native orientation is normalized");prior=sample;}
        check(m::length(m::sub(sample.root,target))<.00001f&&m::angle(sample.rotation,q)<.001f&&m::length(sample.velocity)<.00001f,"mount exactly converges with continuous zero correction velocity");
    }
    m::Transition distant;check(distant.capture({-1.8f,0,0},yaw(0))&&!distant.sample({},yaw(0),0).ready,
        "old1.8m interaction eligibility cannot drag an unreachable planted limb into a mount");
    m::Transition a,b;check(a.capture({0,0,0},yaw(3.1f))&&b.capture({0,0,0},yaw(3.1f)),"wrap origins accepted");
    auto q=yaw(-3.1f),negative=q;for(auto& v:negative)v=-v;a.sample({},q,0);b.sample({},negative,0);
    for(unsigned i=0;i<24;++i){const auto x=a.sample({},q,1./60),y=b.sample({},negative,1./60);
        check(m::angle(x.rotation,y.rotation)<.001f&&m::angle(yaw(3.1f),x.rotation)<.09f,"antipodes and pi wrap take the short route");}
}
void moving_target_and_revocation(){
    m::Transition state;check(state.capture({-.4f,0,0},yaw(0)),"moving target origin captured");
    auto sample=state.sample({},yaw(0),0);double time=0;const double steps[]{1./120,1./30,1./60,1./60};
    for(unsigned i=0;i<120;++i){const double h=steps[i%4];time+=h;const m::Vec target{static_cast<float>(time*.3),static_cast<float>(time*.1),0};
        sample=state.sample(target,yaw(0),h);check(sample.ready,"irregular native steps track the measured rider point");
        const float root_t=std::min(1.f,sample.progress/m::root_phase_end);
        const float rate=6*root_t*(1-root_t)/(sample.duration*m::root_phase_end);
        const float expected=.3f*sample.blend+(target[0]+.4f)*rate;
        check(std::abs(sample.velocity[0]-expected)<.00002f,"root derivative includes current moving target and blend derivative");
        if(sample.progress==1)check(std::abs(sample.velocity[0]-.3f)<.00002f&&std::abs(sample.velocity[1]-.1f)<.00002f,"completion continues actual measured rider-point velocity, including rotating offsets");
    }
    const auto paused=state.sample({50,0,0},yaw(2),0);
    check(paused.root==sample.root&&paused.rotation==sample.rotation&&paused.progress==sample.progress,"zero native time freezes the entire transition");
    check(!state.sample({50,0,0},yaw(2),1./60).ready,"teleporting target rejects instead of dragging the old rider");
    check(!state.sample({},yaw(0),.2).ready&&!state.sample({},yaw(0),-.01).ready,"discontinuous or backwards time cannot advance mount");
    check(state.capture({4,0,0},yaw(1))&&state.sample({4,0,0},yaw(1),0).root==m::Vec{4,0,0},"a new acquired owner/pose starts without stale elapsed time or anchors");
    check(!state.capture({},m::Quat{})&&!state.sample({},yaw(0),.016).ready,"invalid replacement acquisition revokes the former entry");
    state={};check(!state.sample({},yaw(0),.016).ready,"released transition cannot submit a native pose");
    m::Transition rotating;check(rotating.capture({-.1f,.5f,0},yaw(0)),"rotating rider point captured");
    rotating.sample({0,.5f,0},yaw(0),0);time=0;
    for(unsigned i=0;i<180;++i){const double h=steps[i%4];time+=h;const float theta=static_cast<float>(time*.25);
        const m::Quat q{std::sin(theta*.5f),0,0,std::cos(theta*.5f)};
        const auto offset=c::rotate_unit(q,{0,.5f,0});m::Vec target{static_cast<float>(time*.1),offset[1],offset[2]};
        const auto value=rotating.sample(target,q,h);check(value.ready,"actual rotating height offset is a valid moving rider target");
        if(value.progress==1){const float midpoint=static_cast<float>((time-h*.5)*.25);
            check(std::abs(value.velocity[1]+.125f*std::sin(midpoint))<.0001f&&
                std::abs(value.velocity[2]-.125f*std::cos(midpoint))<.0001f,"endpoint follows angular rider-offset velocity rather than COM velocity alone");}
    }
}
void remount_identity_and_reach(){
    const m::BodyIdentity parked{{1,2,3,4,5},6,7,8,9,10};auto acquired=parked;++acquired.token;
    check(m::remount_matches(parked,acquired),"same parked body receives the next mount token rather than reusing its old lease");
    for(unsigned which=0;which<7;++which){auto changed=acquired;
        if(which==0)changed.token=parked.token;
        if(which==1)++changed.token;
        if(which==2)++changed.world;
        if(which==3)++changed.state;
        if(which==4)++changed.index;
        if(which==5)++changed.generation;
        if(which==6)++changed.owner[2];
        check(!m::remount_matches(parked,changed),"changed predecessor lease/world/body generation/owner cannot become current entry identity");
    }
    check(m::interaction_distance(.65f,false)&&m::interaction_distance(.7f,false)&&
        !m::interaction_distance(.71f,false)&&m::interaction_distance(1.8f,true)&&
        !m::interaction_distance(1.8f,false),"set-down and nearby mount remain eligible while farther pickup does not imply drag-to-mount");
}
int main(){native_orientation();timing_and_bounds();moving_target_and_revocation();remount_identity_and_reach();std::cout<<"BMX native mount root/orientation, timing, moving-target derivative, remount identity and revocation tests passed\n";}
