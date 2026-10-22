#include "Engine/Game/BMX/bmx_native_coupled_calibration.h"
#include "Extension/BMX/bmx_collision_envelope.h"
#include <cstdlib>
#include <iostream>
namespace c=dingosdk::bmx::native_coupled::calibration;
namespace p=dingosdk::bmx::native_coupled::policy;
unsigned checks{};void check(bool ok,const char* why){++checks;if(!ok){std::cerr<<"FAIL "<<why<<'\n';std::exit(1);}}
c::Sample initial(){c::Sample s;s.epoch.owner_world={1,2,3,4,5,6,7,8};s.epoch.bodies={p::BodyKey{6,10,1},p::BodyKey{6,11,2}};
    s.epoch.token=1;s.epoch.serial=100;s.epoch.time=100*p::native_seconds;s.epoch.seconds=static_cast<float>(p::native_seconds);
    s.position={p::Vec{0,1,0},p::Vec{.58f,1.3f,.57f}};s.clearance={.53f,.5f};return s;}
c::Sample fall(unsigned step){auto s=initial();s.epoch.serial+=step;s.epoch.time+=step*p::native_seconds;const double t=step*p::native_seconds;
    for(unsigned i=0;i<2;++i){s.position[i][1]-=static_cast<float>(4.9*t*t);s.linear[i][1]=static_cast<float>(-9.8*t);s.clearance[i]-=static_cast<float>(4.9*t*t);}return s;}
void pair(){
    c::Monitor monitor;check(monitor.begin(c::Kind::pair_filter,initial()),"one-shot neutral initial actual pair");
    for(unsigned step=1;step<=6;++step)check(monitor.observe(fall(step)),"six consecutive actual freefall epochs");
    check(monitor.report().phase==c::Phase::complete&&monitor.report().neutral_response_candidate&&monitor.report().count==7,"freefall and no relative collision response candidate recorded");
    check(!monitor.begin(c::Kind::world_contact,initial()),"same process monitor cannot run a second experiment");
    c::Monitor nofall;check(nofall.begin(c::Kind::pair_filter,initial()),"static pair starts");
    for(unsigned step=1;step<=6;++step){auto s=initial();s.epoch=fall(step).epoch;(void)nofall.observe(s);}
    check(nofall.report().phase==c::Phase::failed&&!nofall.report().neutral_response_candidate,"static/unintegrated bodies cannot count as pair filter evidence");
    c::Monitor impulse;check(impulse.begin(c::Kind::pair_filter,initial()),"impulse test starts");auto moved=fall(1);moved.linear[1][0]=.03f;
    check(!impulse.observe(moved)&&impulse.report().phase==c::Phase::failed,"small spurious horizontal pair impulse fails closed");
    c::Monitor angular;check(angular.begin(c::Kind::pair_filter,initial()),"angular test starts");moved=fall(1);moved.angular[0][2]=.1f;
    check(!angular.observe(moved),"small spurious pair angular response fails closed");
    c::Monitor relative;check(relative.begin(c::Kind::pair_filter,initial()),"relative test starts");moved=fall(1);moved.position[0][0]=.002f;
    check(!relative.observe(moved),"2mm pair correction is not hidden by loose displacement cap");
}
void world(){
    c::Monitor m;auto start=initial();start.clearance={.1f,.1f};check(m.begin(c::Kind::world_contact,start),"independent world contact starts");
    for(unsigned step=1;step<=45;++step){auto s=start;s.epoch=fall(step).epoch;
        if(step<=8){s.linear[0][1]=s.linear[1][1]=-.5f;s.position[0][1]-=.004f*step;s.position[1][1]-=.004f*step;
            s.clearance={.1f-.004f*step,.1f-.004f*step};}
        else {s.position[0][1]-=.1f;s.position[1][1]-=.1f;s.clearance={0,0};}
        check(m.observe(s),"finite world fall contact then settle samples");
    }
    check(m.report().phase==c::Phase::complete&&m.report().contact_candidate[0]&&m.report().contact_candidate[1]&&m.report().settled[0]>=3,"both role world contact and settled candidate required");
    c::Monitor falling;check(falling.begin(c::Kind::world_contact,initial()),"world freefall starts");
    for(unsigned step=1;step<=6;++step)check(falling.observe(fall(step)),"brief world freefall stays running");
    check(falling.report().phase==c::Phase::running&&!falling.report().neutral_response_candidate,"six ticks at half meter is never world contact proof");
    c::Monitor clipping;check(clipping.begin(c::Kind::world_contact,initial()),"clipping test starts");auto s=fall(1);s.clearance[1]=-.041f;
    check(!clipping.observe(s),"proxy penetration cap immediately terminates neutral test");
    c::Monitor unparked;check(unparked.begin(c::Kind::pair_filter,initial()),"cleanup proof test starts");unparked.parked(false);
    check(unparked.report().phase==c::Phase::failed&&!unparked.report().parked,"failed park cannot report a completed calibration");
}
void identity(){
    for(unsigned variant=0;variant<4;++variant){c::Monitor m;check(m.begin(c::Kind::pair_filter,initial()),"identity test starts");auto s=fall(1);
        if(variant==0)s.epoch.bodies[1].generation++;if(variant==1)s.epoch.serial++;if(variant==2)s.epoch.time+=.01;if(variant==3)s.epoch.token++;
        check(!m.observe(s)&&m.report().phase==c::Phase::failed,"body/token/missingepoch/clock mismatch fails closed");
    }
    c::Monitor m;check(m.begin(c::Kind::pair_filter,initial()),"duplicate test starts");check(m.observe(initial())&&m.report().count==1,"repeated same native epoch never fakes additional evidence");
}
void positive(){
    c::Monitor noresponse;check(noresponse.begin(c::Kind::pair_positive,initial()),"positive control starts same2mm geometry");
    for(unsigned step=1;step<=6;++step)(void)noresponse.observe(fall(step));
    check(noresponse.report().phase==c::Phase::failed&&!noresponse.report().pair_response,"absence alone fails a positive pair control");
    c::Monitor response;check(response.begin(c::Kind::pair_positive,initial()),"positive response experiment starts");
    for(unsigned step=1;step<=6;++step){auto sample=fall(step);sample.linear[0][0]=-.05f;sample.linear[1][0]=.05f;
        sample.position[0][0]-=.0008f*step;sample.position[1][0]+=.0008f*step;check(response.observe(sample),"bounded real pair response retained");}
    check(response.report().phase==c::Phase::complete&&response.report().pair_response&&response.report().neutral_response_candidate,"actual relative response plus genuine fall qualifies only positive candidate");
}
void geometry(){
    namespace e=dingosdk::bmx::collision_envelope;
    check(c::isolated_overlap(e::capsules),"only handlebar3/pelvis5 overlap2mm; all other cross-body capsules separated at least50mm");
    check(std::abs(c::segment_distance({0,0,0},{1,0,0},{0,1,0},{1,1,0})-1)<1e-7,"parallel segment separation");
    check(c::segment_distance({0,0,0},{1,0,0},{.5f,-1,0},{.5f,1,0})<1e-7,"crossed segment intersection");
    check(std::abs(c::segment_distance({0,0,0},{0,0,0},{1,0,0},{2,0,0})-1)<1e-7,"degenerate endpoint segment separation");
    check(c::fixture_supported(.01067f,-.0066f,{-.00372f,.999921f,-.011994f}),"actual265 measured plaza1.2percent grade admitted without flattening its plane");
    check(!c::fixture_supported(.0201f,0,{0,1,0}),"fixture pitch bound remains finite and explicit");
    check(!c::fixture_supported(0,0,{0,.9f,0}),"nonunit measured normal cannot masquerade as flat support");
    check(!c::fixture_supported(.019f,.019f,{.019f,.999639f,.019f}),"combined inclination must also remain within02rad");
    check(.5*9.8*std::sin(double(c::maximum_fixture_slope))*.75*.75<.12,"worst fixture tangent gravity displacement remains inside unchanged world horizontal safety cap");
}
int main(){geometry();pair();world();identity();positive();std::cout<<"PASS "<<checks<<" neutral calibration checks\n";}
