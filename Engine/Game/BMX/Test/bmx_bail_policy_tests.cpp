#include "Engine/Game/BMX/bmx_bail_policy.h"
#include <cstdlib>
#include <iostream>
#include <limits>
namespace b=dingosdk::bmx::bail_policy;
void check(bool value,const char* message) {if(!value){std::cerr<<message<<'\n';std::exit(1);}}
b::Sample sample() {
    b::Sample s;s.owner={1,2,3,4,5,6,0,1,1};s.serial=1;s.seconds=b::native_seconds;
    s.mounted=true;s.body_up={0,1,0};return s;
}
void hit(b::Sample& s,b::Region region,double closing=0,b::Vec normal={0,1,0}) {
    s.contact_count=1;s.contacts[0]={region,true,true,s.serial,{0,0,0},normal,0,closing};
}
void next(b::Sample& s) {++s.serial;for(std::size_t i=0;i<s.contact_count;++i)s.contacts[i].query_serial=s.serial;}
void natural_air_and_landing() {
    b::Policy p;auto s=sample();s.body_up={0,-1,0};
    for(unsigned i=0;i<60;++i) {check(!p.step(s).bail,"airborne inversion alone must never bail");next(s);}
    s.body_up={0,1,0};hit(s,b::Region::frame,2.5);s.residual_available=true;s.residual_impulse_ns=53.02;
    const auto landing=p.step(s);check(landing.valid&&!landing.bail,"known successful Run87 landing residual is below frame bail threshold");
    next(s);s.residual_impulse_ns=300;s.contacts[0].separation=.02;
    check(!p.step(s).bail,"full query skin is not actual guard contact");
    next(s);s.contact_count=0;check(!p.step(s).bail,"large proxy without actual guard contact is not a collision claim");
}
void rider_and_frame_impacts() {
    for(auto region:{b::Region::head,b::Region::torso}) {
        b::Policy p;auto s=sample();hit(s,region,2.01);
        const auto result=p.step(s);check(result.valid&&result.bail&&result.reason==b::Reason::hard_rider_contact,"fresh head/torso closing contact bails immediately");
        next(s);check(!p.step(s).bail&&p.step(s).latched,"one native request per mount");
        p.pause();next(s);check(!p.step(s).bail,"pause must not replay a latched bail");
        ++s.owner.mount_token;next(s);check(p.step(s).bail,"explicit new mount token rearms a current fresh contact");
    }
    b::Policy p;auto s=sample();hit(s,b::Region::frame,2.1);s.residual_available=true;s.residual_impulse_ns=201;
    check(p.step(s).reason==b::Reason::large_frame_impact_proxy,"frame-only hard event requires explicit impulse proxy plus measured closing contact");
}
void slow_tilt_continuity() {
    b::Policy p;auto s=sample();s.body_up={1,0,0};hit(s,b::Region::torso);
    check(!p.step(s).bail&&p.step(s).tilt_steps==0,"duplicate serial cannot add another tilt frame");
    next(s);auto result=p.step(s);check(result.bail&&result.tilt_steps==2,"two fresh slow severe contact frames bail");
    for(unsigned variant=0;variant<7;++variant) {
        p.reset();s=sample();s.body_up={1,0,0};hit(s,b::Region::torso);check(!p.step(s).bail,"first tilt frame seeds only");
        next(s);
        switch(variant) {
        case 0: p.pause();break;
        case 1: ++s.owner.entity;break;
        case 2: ++s.owner.world;break;
        case 3: next(s);break;
        case 4: s.contacts[0].region=b::Region::head;break;
        case 5: s.contacts[0].normal={0,0,1};break;
        case 6: s.contacts[0].closing_speed=-.1;break;
        }
        check(!p.step(s).bail,"pause/owner/world/gap/region/normal/opening changes cannot retain old tilt credit");
    }
    p.reset();s=sample();s.body_up={1,0,0};hit(s,b::Region::frame,.6);
    check(p.step(s).reason==b::Reason::severe_contact_tilt,"fast severe contact tilt does not wait for rest debounce");
}
void invalid_and_stale_samples() {
    for(unsigned variant=0;variant<10;++variant) {
        b::Policy p;auto s=sample();hit(s,b::Region::head,3);
        switch(variant) {
        case 0: s.contacts[0].query_serial=0;break;
        case 1: s.contacts[0].normal={0,0,0};break;
        case 2: s.contacts[0].point[0]=std::numeric_limits<double>::quiet_NaN();break;
        case 3: s.contacts[0].separation=-.2;break;
        case 4: s.contacts[0].available=false;break;
        case 5: s.contacts[0].region=b::Region::none;break;
        case 6: s.body_up={0,2,0};break;
        case 7: s.seconds=.02;break;
        case 8: s.contact_count=9;break;
        case 9: s.owner.world=0;break;
        }
        const auto result=p.step(s);check(!result.valid&&!result.bail,"invalid/stale geometry or ownership must fail without a bail request");
    }
    b::Policy p;auto s=sample();hit(s,b::Region::head,3);s.contact_count=2;s.contacts[1]=s.contacts[0];s.contacts[1].query_serial=0;
    check(!p.step(s).bail,"later stale contact must invalidate the complete sample before an earlier hard contact fires");
    s=sample();s.body_up={1,0,0};hit(s,b::Region::torso);check(!p.step(s).bail,"valid slow contact starts a fresh history");
    next(s);s.seconds=.02;check(!p.step(s).valid,"unsupported timing rejects and clears prior contact history");
    next(s);s.seconds=b::native_seconds;check(!p.step(s).bail,"invalid timing cannot lend a later frame old tilt credit");
}
void witnessed_swept_crossings() {
    b::Policy p;auto s=sample();check(!p.step(s).bail,"crossing starts with a prior current-owner sample");
    next(s);hit(s,b::Region::frame,5,{0,0,-1});s.contacts[0].separation=-1.0;s.contacts[0].swept_crossing=true;
    const auto crossing=p.step(s);
    check(crossing.valid&&crossing.bail&&crossing.reason==b::Reason::swept_body_crossing,
          "proven consecutive face crossing bails even beyond ordinary overlap depth and independent of tilt");
    for(unsigned variant=0;variant<6;++variant) {
        p.reset();s=sample();check(!p.step(s).bail,"seed crossing history");next(s);
        hit(s,b::Region::frame,5,{0,0,-1});s.contacts[0].separation=-1.0;s.contacts[0].swept_crossing=true;
        switch(variant) {
        case 0: s.contacts[0].swept_crossing=false;break;
        case 1: s.contacts[0].separation=-1.51;break;
        case 2: s.contacts[0].separation=.01;break;
        case 3: next(s);break;
        case 4: ++s.owner.world;break;
        case 5: s.contacts[0].query_serial=0;break;
        }
        const auto result=p.step(s);check(!result.valid&&!result.bail,"unproven/deep/opening/stale/foreign sweep cannot bypass ordinary contact guards");
    }
}
int main() {
    natural_air_and_landing();rider_and_frame_impacts();slow_tilt_continuity();invalid_and_stale_samples();witnessed_swept_crossings();
    std::cout<<"BMX bail policy contact provenance, native-time debounce, owner/latch and successful-landing controls passed\n";
}
