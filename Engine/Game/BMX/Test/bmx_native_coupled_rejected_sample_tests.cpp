#include "Engine/Game/BMX/bmx_native_coupled_calibration.h"
#include <cstdio>
#include <cstdlib>
#include <string_view>
namespace c=dingosdk::bmx::native_coupled::calibration;
namespace p=dingosdk::bmx::native_coupled::policy;
unsigned checks{};void check(bool okay,const char* name){++checks;if(!okay){std::fprintf(stderr,"FAIL %s\n",name);std::exit(1);}}
c::Sample initial(){c::Sample s;s.epoch.owner_world={1,2,3,4,5,6,7,8};s.epoch.bodies={p::BodyKey{6,1,1},p::BodyKey{6,2,1}};
    s.epoch.token=1;s.epoch.serial=100;s.epoch.seconds=static_cast<float>(p::native_seconds);s.epoch.time=100*p::native_seconds;s.clearance={.1f,.1f};return s;}
int main(){
    constexpr std::array<const char*,6> names{"linear_speed","angular_speed","total_displacement","horizontal_displacement","horizontal_speed","proxy_plane_clearance"};
    for(unsigned role=0;role<2;++role)for(unsigned cap=0;cap<6;++cap){c::Monitor m;const auto start=initial();check(m.begin(c::Kind::world_contact,start),"neutral begin");
        auto sample=start;sample.epoch.serial++;sample.epoch.time+=sample.epoch.seconds;
        if(cap==0)sample.linear[role][1]=-6.1f;else if(cap==1)sample.angular[role][2]=8.1f;
        else if(cap==2)sample.position[role][1]=-1.41f;else if(cap==3)sample.position[role][0]=.121f;
        else if(cap==4)sample.linear[role][0]=1.01f;else sample.clearance[role]=-.041f;
        check(!m.observe(sample),"cap still rejects");const auto& r=m.report();
        check(r.phase==c::Phase::failed&&!r.neutral_response_candidate,"cap cannot qualify completed contact");
        check(r.count==1&&r.samples[0].epoch==start.epoch,"rejected sample does not advance accepted history");
        check(r.rejected_sample_available&&r.rejected_sample.epoch==sample.epoch,"exact rejected genuine epoch retained");
        check(r.rejected_role==role&&std::string_view(r.rejected_cap)==names[cap],"exact role/cap retained");
        check(cap==5?r.rejected_value<r.rejected_limit:r.rejected_value>r.rejected_limit,"exact violating value and unchanged limit retained");
        check(r.rejected_sample.position==sample.position&&r.rejected_sample.linear==sample.linear&&r.rejected_sample.angular==sample.angular,"offending actual vectors retained");
        check(!r.falling[role]&&!r.contact_candidate[role]&&!r.settled[role],"rejected sample cannot advance contact evidence");
    }
    for(unsigned mode=0;mode<2;++mode){c::Monitor m;const auto start=initial();check(m.begin(c::Kind::pair_filter,start),"pair begin");
        auto s=start;s.epoch.serial++;s.epoch.time+=s.epoch.seconds;if(mode)s.linear[1][1]=.0101f;else s.position[1][1]=.0011f;
        check(!m.observe(s),"pair response retains rejection");const auto& r=m.report();
        check(r.count==1&&r.rejected_role==0xffffffffU&&r.rejected_sample_available,"paired failure retains bounded unaccepted witness");
        check(std::string_view(r.rejected_cap)==(mode?"relative_linear_speed":"relative_displacement"),"paired exact cap retained");
    }
    c::Monitor invalid;const auto start=initial();check(invalid.begin(c::Kind::world_contact,start),"identity begin");auto wrong=start;wrong.epoch.owner_world[0]++;
    check(!invalid.observe(wrong)&&!invalid.report().rejected_sample_available,"cross-owner sample never accepted as actual cap witness");
    std::printf("coupled rejected sample diagnostics: %u checks PASS\n",checks);
}
