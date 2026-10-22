#include "Engine/Game/BMX/bmx_landing_presentation.h"
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
namespace p=dingosdk::bmx::landing_presentation;
namespace a=dingosdk::bmx::landing_approach;
constexpr double h=.01666666567325592;
void check(bool value,const char* why) {if(!value){std::cerr<<why<<'\n';std::exit(1);}}
bool near(double x,double y,double error=1e-6) {return std::abs(x-y)<error;}
a::Evidence touch(std::uint64_t serial,double scoring,double post=0) {
    a::Evidence e;e.valid=e.first_contact=e.post_available=true;
    e.contact_serial=serial;e.scoring_closing_speed=scoring;e.post_closing_speed=post;return e;
}
void frozen_scoring_is_one_presentation_pulse() {
    auto e=touch(100,4.44545841217041,3.7957205336547077);
    const auto impact=p::step(0,h,100,true,e);
    check(impact.valid&&impact.advanced&&near(impact.compression,.740909735361735),
        "native first-touch4.445m/s evidence produces a bounded absorption pulse");
    e.first_contact=false;e.post_closing_speed=.2642003339158499;
    auto result=p::step(impact.compression,h,101,true,e);
    check(result.valid&&near(result.compression,impact.compression-4*h),
        "following support decays even while conservative scoring retains the hard impact");
    e.post_closing_speed=0;
    for(std::uint64_t serial=102;serial<122;++serial) {
        result=p::step(result.compression,h,serial,true,e);
        check(result.valid&&result.compression>=0&&result.compression<=1,
            "continuous quiet support keeps a valid bounded posture");
    }
    check(result.compression==0&&e.contact_serial==100&&near(e.scoring_closing_speed,4.44545841217041),
        "stable ground recovers neutral posture without weakening frozen impact scoring");
    const double old_target=e.scoring_closing_speed/6;
    check(old_target>.74,"counterexample: the old per-tick scoring target would remain compressed");
}
void fresh_impacts_and_support_seams() {
    auto e=touch(10,1.2);
    auto r=p::step(0,h,10,true,e);check(near(r.compression,.2),"small first landing absorbs proportionally");
    e.first_contact=false;e.post_closing_speed=0;
    r=p::step(r.compression,h,11,true,e);
    // Same retained contact after a short unsupported seam may not repeat the
    // frozen1.2m/s impact; no fabricated new first-contact observation is used.
    a::Evidence air;air.valid=true;
    r=p::step(r.compression,h,12,false,air);
    r=p::step(r.compression,h,13,true,e);
    check(near(r.compression,0),"support seam/recontact cannot retrigger retained impact history");
    e=touch(14,6.6);r=p::step(r.compression,h,14,true,e);
    check(r.valid&&r.compression==1,"a distinct harder fresh landing can start a new bounded full pulse");
    e.first_contact=false;e.post_closing_speed=0;
    for(std::uint64_t serial=15;serial<35;++serial)r=p::step(r.compression,h,serial,true,e);
    e.post_closing_speed=1.8;r=p::step(r.compression,h,35,true,e);
    check(r.valid&&near(r.compression,.3),"fresh measured post-contact closing can absorb a later real jolt");
    e=touch(14,6.6);check(!p::step(.2f,h,36,true,e).valid,
        "an old contact mislabeled first-contact cannot replay a historic maximum");
}
void native_time_and_rejection_boundaries() {
    auto e=touch(100,3);auto r=p::step(0,h,100,true,e);
    // No wall-time input exists. A repeated/pause sample has no native delta.
    const auto paused=p::step(r.compression,0,100,true,e);
    check(paused.valid&&!paused.advanced&&paused.compression==r.compression,
        "pause/duplicate native time earns neither a new pulse nor recovery");
    a::Evidence air;air.valid=true;
    const auto airborne=p::step(r.compression,h,101,false,air);
    check(airborne.valid&&near(airborne.compression,r.compression-4*h),
        "advancing air may decay old compression but cannot create impact");
    check(p::step(0,h,102,false,air).compression==0,"clear air cannot create compression");
    for(double seconds:{-h,.10001,std::numeric_limits<double>::quiet_NaN(),
                        std::numeric_limits<double>::infinity()}) {
        const auto rejected=p::step(.5f,seconds,101,true,e);
        check(!rejected.valid&&!rejected.advanced&&rejected.compression==0,
            "rewound/nonfinite/unbounded native time fails closed instead of storing old posture");
    }
    for(float previous:{-.01f,1.01f,std::numeric_limits<float>::quiet_NaN()})
        check(!p::step(previous,h,100,true,e).valid,"malformed previous presentation fails closed");
    e.valid=false;check(!p::step(.5f,h,100,true,e).valid,"rejected contact evidence clears presentation only");
    e=touch(100,3);e.contact_serial=101;
    check(!p::step(.5f,h,100,true,e).valid,"future contact serial cannot generate an impact pulse");
    e=touch(100,3);e.post_closing_speed=std::numeric_limits<double>::quiet_NaN();
    check(!p::step(.5f,h,100,true,e).valid,"invalid current impact speed fails closed");
    // Actual bridge reset_controls clears the previous scalar at pause/owner
    // changes before any accepted new mount sample; the helper owns no token.
    e=touch(500,0);r=p::step(0,h,500,true,e);
    check(r.valid&&r.compression==0,"reset previous state cannot leak a prior mount compression");
}
void real_tracker_scoring_is_unchanged() {
    a::Tracker tracker;
    a::Motion before{7,100,10,h,{0,1,0},{0,-2,3},{},0,true};
    const auto clear=tracker.observe(before,{}, {0,-9.8,0});
    check(clear.valid&&!clear.post_available,"real tracker begins with an actual unsupported observation");
    const std::array<a::Support,2> floor{a::Support{true,{0,0,0},{0,1,0},{}},a::Support{}};
    auto current=before;++current.serial;current.simulation_time+=h;current.linear_velocity={0,0,3};
    auto e=tracker.observe(current,floor,{0,-9.8,0});
    const double score=2+9.8*h;const auto contact_serial=e.contact_serial;
    auto r=p::step(0,h,current.serial,true,e);
    check(e.valid&&e.first_contact&&e.approach_available&&near(e.scoring_closing_speed,score),
        "presentation retains original gravity-conservative first-touch classification");
    for(unsigned tick=0;tick<20;++tick) {
        ++current.serial;current.simulation_time+=h;e=tracker.observe(current,floor,{0,-9.8,0});
        r=p::step(r.compression,h,current.serial,true,e);
        check(e.valid&&!e.first_contact&&e.approach_available&&e.contact_serial==contact_serial&&
              e.approach.serial==before.serial&&near(e.scoring_closing_speed,score)&&e.post_closing_speed==0,
            "real tracker first-touch estimate/owner/serial stay frozen across all settle observations");
    }
    check(r.valid&&r.compression==0,"presentation decays while real tracker still preserves the scoring maximum");
}
void air_extension_uses_only_qualified_native_time() {
    float blend=0;
    for(unsigned tick=0;tick<15;++tick) {
        const auto next=p::step_air_extension(blend,h,true);
        check(next.valid&&next.advanced&&next.blend>=blend&&next.blend<=1&&
              next.blend-blend<=5*h+1e-6,
            "real air gradually reaches the existing4cm extension without a support-edge jump");
        blend=next.blend;
    }
    check(blend==1,"sustained real air retains the full existing extension");
    const auto duplicate=p::step_air_extension(blend,0,false);
    check(duplicate.valid&&!duplicate.advanced&&duplicate.blend==blend,
        "duplicate/paused native time cannot spend extension recovery or follow a new ground flag");
    const auto toggled=p::step_air_extension(blend,h,false);
    const auto reair=p::step_air_extension(toggled.blend,h,true);
    check(near(toggled.blend,1-5*h)&&near(reair.blend,1),
        "one-frame support toggle moves the presentation fraction gradually and returns without resetting");
    for(unsigned tick=0;tick<15;++tick)blend=p::step_air_extension(blend,h,false).blend;
    check(blend==0,"settled ground smoothly recovers neutral extension");
    for(double seconds:{-h,.10001,std::numeric_limits<double>::quiet_NaN(),
                        std::numeric_limits<double>::infinity()}) {
        const auto invalid=p::step_air_extension(.5f,seconds,true);
        check(!invalid.valid&&!invalid.advanced&&invalid.blend==0,
            "invalid/rewound native time clears presentation without catch-up or a synthetic takeoff");
    }
    for(float previous:{-.01f,1.01f,std::numeric_limits<float>::quiet_NaN()})
        check(!p::step_air_extension(previous,h,true).valid,"malformed extension state fails closed");
    // Bridge reset_controls explicitly clears both presentation scalars on
    // pause, rejection and mount teardown; a new State also starts from zero.
    blend=0;
    check(p::step_air_extension(blend,0,true).blend==0&&p::step_air_extension(blend,h,false).blend==0,
        "pause reset/new mount cannot inherit a previous ride's fully extended posture");
    const auto resumed=p::step_air_extension(blend,h,true);
    check(resumed.valid&&near(resumed.blend,5*h),"resumed air starts from fresh native time without a wall-time catch-up burst");
}
}
int main() {
    frozen_scoring_is_one_presentation_pulse();fresh_impacts_and_support_seams();
    native_time_and_rejection_boundaries();real_tracker_scoring_is_unchanged();
    air_extension_uses_only_qualified_native_time();
    std::cout<<"BMX landing presentation pulse, native-time recovery and scoring isolation tests passed\n";
}
