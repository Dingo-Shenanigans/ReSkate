#include "Extension/BMX/bmx_ramp_rehearsal.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>
namespace r=dingosdk::bmx::ramp_rehearsal;
constexpr double h=1.0/60;
void check(bool good,const char* why) {if(!good){std::cerr<<why<<'\n';std::exit(1);}}
struct Ride {
    r::Route route{{0,0,0},{0,0,1},{0,0,-9},0,1,1.25};
    r::Driver driver{route};
    r::Observation o{true,true,0,h,{0,0,-9},{0,.9f,-9},{},1,1,0,7};
    dingosdk::bmx::RawInput last;
    const r::Tally& tick(float z,float y,bool ground,float vy=0,float vz=2,unsigned support_mask=3) {
        last=driver.controls(o.elapsed);o.elapsed+=h;++o.serial;
        o.root={0,y,z};o.com={0,y+.9f,z};o.velocity={0,vy,vz};o.grounded=ground;
        o.actual_wheel_support={ground&&(support_mask&1)!=0,ground&&(support_mask&2)!=0};
        const auto& tally=driver.observe(o);
        check(last.available&&last.focused&&!last.menu_open&&last.device==r::synthetic_device,"synthetic controls have explicit stable source identity");
        check(last.left_x==0&&last.left_y==0&&last.right_x==0&&last.right_y==0&&!last.quick_hop,
            "terrain rehearsal never supplies steering, hop, spin or rear-air-pitch input");
        return tally;
    }
    void neutral() {for(unsigned i=0;i<31;++i){tick(-9,0,true,0,0);check(last.pedal==0&&last.brake==0,"first half second stays neutral");}}
    void runup() {for(unsigned i=1;i<=180;++i){tick(-9+7.5f*i/180,0,true);check(last.pedal==.8&&last.brake==0,"supported runup receives the authored smaller-bike rehearsal input");}}
    void climb(bool tilted=true) {
        o.support_up_alignment=tilted?.9284767:1;
        for(unsigned i=1;i<=90;++i)tick(-1.5f+3.3f*i/90,float(i)/90,true,.6666667f,2.2f);
    }
    void natural_flight(unsigned count=38,float start_z=1.8f) {
        for(unsigned i=0;i<count;++i) {
            const double t=i*h;const float y=static_cast<float>(1+1.5*t-4.9*t*t);
            tick(start_z+static_cast<float>(2*t),y,false,static_cast<float>(1.5-9.8*t),2);
            if(i)check(last.pedal==0&&last.brake==0,"natural flight is entirely input-neutral including motor reaction");
        }
    }
    void land(unsigned intervals=4) {
        o.support_up_alignment=1;
        tick(o.root[2]+.03f,.005f,true,0,1);
        check(driver.tally().landing_support_seconds==0&&!driver.tally().landed,"first contact spends no pre-contact time on settling");
        for(unsigned i=0;i<intervals;++i)tick(o.root[2]+.02f,.005f,true,0,1);
    }
};
void genuine_sequence_and_inputs() {
    Ride ride;ride.neutral();ride.runup();ride.climb();
    check(ride.driver.tally().ascent&&!ride.driver.tally().flight&&ride.driver.tally().max_rise>.9,"actual tilted support and COM rise establish ramp ascent");
    ride.natural_flight();check(ride.driver.tally().flight&&ride.driver.tally().flight_seconds>.6&&
        ride.driver.tally().maximum_ballistic_seconds>.6,"native gravity response with clear owned force/torque proves sustained ballistic flight");
    ride.land();check(ride.driver.tally().landed&&!ride.driver.tally().passed,"settled landing precedes the stopping test");
    for(unsigned i=0;i<6;++i) {
        ride.tick(ride.o.root[2]+.01f,.005f,true,0,.5f);
        check(ride.last.brake==1&&ride.last.pedal==0,"post-landing controls brake without further pedal input");
    }
    check(ride.driver.tally().completed&&ride.driver.tally().passed&&ride.driver.tally().elapsed<9,
        "bounded ascent, natural flight, stable support and measured braking produce a pass");
    const auto input=ride.driver.controls(ride.o.elapsed+.1);
    check(input.brake==1&&input.pedal==0,"completed experiment remains stopped rather than resuming approach");
}
void support_and_flight_are_not_invented() {
    Ride flat;flat.neutral();flat.runup();flat.climb(false);flat.natural_flight();
    check(!flat.driver.tally().ascent&&!flat.driver.tally().flight,"height alone or flat-supported motion cannot claim verified ramp ascent");
    flat.driver.controls(9.05);check(flat.driver.tally().completed&&!flat.driver.tally().passed,"no ramp ascent fails at the bounded deadline");
    Ride early;early.neutral();early.runup();
    early.o.support_up_alignment=.9284767;
    for(unsigned i=1;i<=60;++i)early.tick(-1.5f+2.5f*i/60,.5f*i/60,true);
    check(early.driver.tally().ascent,"pre-lip support can establish real ascent");
    for(unsigned i=0;i<15;++i)early.tick(1,.5f,false);
    check(!early.driver.tally().flight&&early.driver.tally().flight_seconds==0,"a bump before the measured lip cannot count as ramp launch");
    check(early.last.pedal==0,"even unqualified pre-lip air receives no motor torque");
    Ride brief;brief.neutral();brief.runup();brief.climb();brief.natural_flight(5);
    // An intentionally short, measured flight onto another raised support.
    brief.o.support_up_alignment=1;brief.tick(brief.o.root[2]+.03f,brief.o.root[1],true);
    for(unsigned i=0;i<4;++i)brief.tick(brief.o.root[2]+.01f,brief.o.root[1],true);
    check(!brief.driver.tally().flight&&!brief.driver.tally().landed&&!brief.driver.tally().passed,
        "brief support flicker cannot claim a terrain jump and actual recontact can resume the ascent");
    brief.driver.controls(9.05);
    check(brief.driver.tally().completed&&!brief.driver.tally().passed&&std::string_view(brief.driver.tally().issue)=="no_qualified_natural_flight",
        "an unqualified short clear interval still fails at the bounded native deadline");
}
void settling_bounce_and_quality() {
    Ride ride;ride.neutral();ride.runup();ride.climb();ride.natural_flight();ride.land(2);
    check(!ride.driver.tally().landed,"two following support intervals are below the settling threshold");
    ride.tick(ride.o.root[2]+.01f,.01f,false);
    const auto frozen_flight=ride.driver.tally().flight_seconds;
    ride.tick(ride.o.root[2]+.01f,.005f,true);
    check(ride.driver.tally().landing_support_seconds==0&&!ride.driver.tally().landed,
        "first recontact after a bounce cannot count the preceding airborne interval as supported time");
    for(unsigned i=0;i<3;++i)ride.tick(ride.o.root[2]+.01f,.005f,true);
    check(!ride.driver.tally().landed,"settling requires the complete new supported window");
    ride.tick(ride.o.root[2]+.01f,.005f,true);
    check(ride.driver.tally().landed&&ride.driver.tally().flight_seconds==frozen_flight,
        "bounce resets support time without extending the original flight duration");
    Ride tipped;tipped.neutral();tipped.runup();tipped.climb();tipped.natural_flight();
    tipped.o.body_up_alignment=.49;tipped.tick(tipped.o.root[2]+.02f,.005f,true);
    check(tipped.driver.tally().completed&&!tipped.driver.tally().passed,"sideways/inverted contact cannot produce a clean ramp landing");
}
void stale_and_invalid_measurements() {
    Ride ride;ride.neutral();const auto count=ride.driver.tally().samples;
    ride.driver.observe(ride.o);check(ride.driver.tally().samples==count,"duplicate native serial cannot integrate experiment time twice");
    ++ride.o.body_token;ride.o.elapsed+=h;++ride.o.serial;ride.driver.observe(ride.o);
    check(ride.driver.tally().completed&&!ride.driver.tally().passed,"new physical body ownership cannot inherit the route attempt");
    Ride gap;gap.neutral();gap.o.serial+=2;gap.o.elapsed+=2*h;gap.driver.observe(gap.o);
    check(gap.driver.tally().completed&&!gap.driver.tally().passed,"missing native observations fail rather than filling in a trajectory");
    Ride sideways;sideways.neutral();sideways.o.root[0]=.401f;sideways.o.com[0]=.401f;
    sideways.o.elapsed+=h;++sideways.o.serial;sideways.driver.observe(sideways.o);
    check(sideways.driver.tally().completed&&!sideways.driver.tally().passed,"drifting outside the verified center corridor aborts");
    Ride malformed;malformed.neutral();malformed.o.body_up_alignment=std::numeric_limits<double>::quiet_NaN();
    malformed.o.elapsed+=h;++malformed.o.serial;malformed.driver.observe(malformed.o);
    check(malformed.driver.tally().completed&&!malformed.driver.tally().passed,"nonfinite quality cannot pass or keep driving");
    r::Route invalid;invalid.forward={};r::Driver bad(invalid);check(bad.tally().completed&&!bad.tally().passed,"unmeasured invalid route is rejected");
}
void provisional_pass_remains_guarded() {
    for(unsigned reason=0;reason<5;++reason) {
        Ride ride;ride.neutral();ride.runup();ride.climb();ride.natural_flight();ride.land();
        for(unsigned i=0;i<6;++i)ride.tick(ride.o.root[2]+.01f,.005f,true,0,.5f);
        check(ride.driver.tally().passed,"post-pass guard fixture first completes the physical route");
        ride.o.elapsed+=h;++ride.o.serial;
        if(reason==0)ride.o.root[0]=ride.o.com[0]=.401f;
        if(reason==1){ride.o.grounded=false;ride.o.actual_wheel_support={false,false};}
        if(reason==2)ride.o.support_up_alignment=.149;
        if(reason==3)++ride.o.body_token;
        if(reason==4)ride.o.velocity={0,0,.8f};
        ride.driver.observe(ride.o);
        check(ride.driver.tally().completed&&!ride.driver.tally().passed,
            "route/support/normal/owner/speed violation after an early pass revokes success before the fixed deadline");
    }
    Ride normal;normal.neutral();normal.runup();normal.climb();normal.natural_flight();normal.land();
    normal.o.support_up_alignment=.149;normal.tick(normal.o.root[2]+.01f,.005f,true,0,.5f);
    check(normal.driver.tally().completed&&!normal.driver.tally().passed,"braking cannot count a ceiling or wall as clean supported stopping");
}
void verified_runout_boundary() {
    Ride ride;ride.neutral();ride.runup();
    for(unsigned i=1;i<=65;++i)ride.tick(-1.5f+.1f*i,0,true,0,6);
    check(!ride.driver.tally().completed&&ride.driver.tally().progress==5,
        "five-meter bike-root boundary retains clearance for the pitched front capsule inside the six-meter corridor");
    ride.tick(5.001f,0,true,0,.06f);
    check(ride.driver.tally().completed&&!ride.driver.tally().passed&&
          std::string_view(ride.driver.tally().issue)=="bounded_route_or_motion_violation",
        "root progress beyond five meters aborts before any collider can leave the verified runout");
    r::Route shorter{{0,0,0},{0,0,1},{0,0,-7.5f},0,1,1.25};
    r::Driver adjusted(shorter);check(!adjusted.tally().completed,"freshly verified seven-and-a-half-meter runup is accepted without changing physics");
    r::Observation rollback{true,true,h,h,{0,0,-7.5f},{0,.9f,-7.5f},{},1,1,1,7};
    rollback.actual_wheel_support={true,true};
    adjusted.observe(rollback);rollback.elapsed+=h;++rollback.serial;
    rollback.root[2]=rollback.com[2]=-7.54f;adjusted.observe(rollback);
    check(!adjusted.tally().completed,"four-centimeter rollback boundary retains the rear capsule inside the verified approach");
    rollback.elapsed+=h;++rollback.serial;rollback.root[2]=rollback.com[2]=-7.541f;adjusted.observe(rollback);
    check(adjusted.tally().completed&&!adjusted.tally().passed&&
          std::string_view(adjusted.tally().issue)=="bounded_route_or_motion_violation",
        "rollback beyond four centimeters aborts before leaving the checked floor corridor");
}
void ray_clear_time_alone_is_not_ballistic_flight() {
    for(unsigned command=0;command<2;++command) {
        Ride ride;ride.neutral();ride.runup();ride.climb();
        if(command==0)ride.o.force={0,.02f,0};else ride.o.torque={.02f,0,0};
        ride.natural_flight(20);
        check(!ride.driver.tally().flight&&ride.driver.tally().maximum_ballistic_seconds==0&&
            ride.driver.tally().flight_seconds>.3,
            "clear geometry cannot prove ballistic motion while recorded owned force or torque remains active");
    }
    Ride split;split.neutral();split.runup();split.climb();
    float y=1,vy=1.5f;
    for(unsigned i=0;i<17;++i) {
        if(i)vy-=static_cast<float>(9.8*h);
        if(i==6||i==12)vy+=.05f; // unobserved native edge response despite clear wheel queries
        y+=vy*static_cast<float>(h);
        split.tick(1.8f+static_cast<float>(2*i*h),y,false,vy,2);
    }
    check(split.driver.tally().flight_seconds>.25&&split.driver.tally().maximum_ballistic_seconds>.08&&
        split.driver.tally().maximum_ballistic_seconds<.10&&!split.driver.tally().flight,
        "broken eighty-millisecond gravity stretches cannot accumulate into a qualified terrain flight");
    split.land();
    check(!split.driver.tally().flight&&!split.driver.tally().landed&&!split.driver.tally().passed,
        "subsequent stable contact cannot turn native edge interference into a passed ballistic launch");
    split.driver.controls(9.05);
    check(split.driver.tally().completed&&!split.driver.tally().passed,
        "native edge interference without a later qualified ballistic interval remains a deadline failure");
    Ride seed;seed.neutral();seed.runup();seed.climb();seed.natural_flight(1);
    check(seed.driver.tally().maximum_ballistic_seconds==0&&!seed.driver.tally().flight,
        "the first unsupported observation contributes no unknown preceding interval");
}
void unqualified_clear_recontact_can_resume_ascent_then_launch() {
    Ride ride;ride.neutral();ride.runup();ride.climb();ride.natural_flight(6);
    check(!ride.driver.tally().flight&&ride.driver.tally().maximum_ballistic_seconds>.08&&
        ride.driver.tally().maximum_ballistic_seconds<.10,"first short clear candidate has no qualified flight");
    ride.tick(2.0f,1,true,0,2);
    check(!ride.driver.tally().flight&&!ride.driver.tally().landed&&ride.driver.tally().ballistic_seconds==0&&
        ride.driver.tally().flight_seconds==0,"actual edge support resets an unqualified candidate without freezing touchdown");
    ride.tick(2.03f,1,true,0,2);
    check(ride.last.pedal==.8&&ride.last.brake==0,"verified renewed support resumes the same bounded rehearsal pedal input");
    ride.natural_flight(38,2.06f);
    check(ride.driver.tally().flight&&ride.driver.tally().maximum_ballistic_seconds>.6,
        "later continuous measured gravity flight can independently qualify after the earlier lip transition");
    ride.land();for(unsigned i=0;i<6;++i)ride.tick(ride.o.root[2]+.01f,.005f,true,0,.5f);
    check(ride.driver.tally().passed,"a later genuine launch, two-wheel settling and measured stop can pass normally");
}
void front_only_landing_must_wait_for_both_actual_wheels() {
    Ride ride;ride.neutral();ride.runup();ride.climb();ride.natural_flight();
    ride.o.support_up_alignment=1;ride.o.body_up_alignment=.55;
    for(unsigned i=0;i<16;++i) {
        ride.tick(ride.o.root[2]+.005f,.005f,true,0,1,1);
        check(!ride.driver.tally().landed&&ride.driver.tally().landing_support_seconds==0&&ride.last.brake==0,
            "front-only fifty-six-degree touchdown can settle naturally but cannot trigger landing credit or braking");
    }
    ride.o.body_up_alignment=.98;
    ride.tick(ride.o.root[2]+.005f,.005f,true,0,1,3);
    check(!ride.driver.tally().landed&&ride.driver.tally().landing_support_seconds==0&&ride.last.brake==0,
        "first actual two-wheel support starts a fresh window without banking the preceding front-only contact");
    for(unsigned i=0;i<4;++i)ride.tick(ride.o.root[2]+.005f,.005f,true,0,1,3);
    check(ride.driver.tally().landed&&ride.driver.tally().landing_support_seconds>.06,
        "only sixty following milliseconds of both actual wheel supports establish brake readiness");
    ride.tick(ride.o.root[2]+.005f,.005f,true,0,.5f,1);
    check(ride.last.brake==1&&ride.driver.tally().completed&&!ride.driver.tally().passed&&
        std::string_view(ride.driver.tally().issue)=="lost_support_during_stop",
        "losing either actual wheel after braking still fails instead of weakening the original stop gate");

    Ride mismatch;mismatch.neutral();mismatch.o.elapsed+=h;++mismatch.o.serial;
    mismatch.o.actual_wheel_support={false,false};mismatch.driver.observe(mismatch.o);
    check(mismatch.driver.tally().completed&&!mismatch.driver.tally().passed,
        "aggregate grounding inconsistent with the two actual measured support flags is rejected");
}
void measured_entry_must_stay_inside_cleared_approach() {
    const r::Vec start{307.40423584f,274.35232544f,-474.95669556f};
    const r::Vec forward{-.754918277f,0,-.65581888f};
    const r::Vec old_arrival{307.7008667f,274.3585205f,-474.7903137f};
    check(!r::entry_position_valid(old_arrival,start,forward),
        "Run75 arrival .333m behind the measured start cannot mount even inside the former .35m target radius");
    auto moved_arrival=old_arrival;
    for(unsigned i=0;i<3;++i)moved_arrival[i]+=.5f*forward[i];
    check(r::entry_position_valid(moved_arrival,start,forward),
        "same measured native arrival offset fits after moving only the teleport target inside the checked corridor");
    for(const r::Vec f:{r::Vec{0,0,1},r::Vec{0,0,-1},r::Vec{1,0,0},r::Vec{-1,0,0}}) {
        r::Vec p{.5f*f[0],10,.5f*f[2]};
        check(r::entry_position_valid(p,{},f),"entry geometry follows route yaw and ignores rider height");
        p={-.001f*f[0],0,-.001f*f[2]};
        check(!r::entry_position_valid(p,{},f),"preparation cannot spend the physical rollback allowance");
        p={1.001f*f[0],0,1.001f*f[2]};
        check(!r::entry_position_valid(p,{},f),"arrival cannot skip beyond the first metre of approach");
        p={.5f*f[0]+.091f*f[2],0,.5f*f[2]-.091f*f[0]};
        check(!r::entry_position_valid(p,{},f),"entry retains a lateral margin inside the driving corridor");
    }
    check(!r::entry_position_valid({}, {}, {}),"unmeasured direction cannot authorize a mount");
    check(!r::entry_position_valid({std::numeric_limits<float>::quiet_NaN(),0,0}, {}, {0,0,1}),
        "nonfinite entry is rejected");
}

void qualified_passive_rotation_is_distinct_from_ballistic_rotation() {
    for(unsigned reason=0;reason<20;++reason) {
        Ride ride;ride.neutral();ride.runup();ride.climb();
        ride.o.queried_clear={true,true};ride.o.neutral_air_damping_active=true;
        ride.o.angular_velocity={-1,0,0};ride.o.air_damping_torque=ride.o.torque={10,0,0};
        if(reason==1)ride.o.force={0,.02f,0};
        if(reason==2)ride.o.torque[1]=.02f;
        if(reason==3)ride.o.angular_velocity={1,0,0};
        if(reason==4)ride.o.air_damping_torque=ride.o.torque={160.01f,0,0};
        if(reason==5)ride.o.neutral_air_damping_active=false;
        if(reason==6)ride.o.queried_clear[0]=false;
        if(reason==7)ride.o.air_rotation_reserved=true;
        if(reason==8)ride.o.hop_applied=true;
        if(reason==9)ride.o.angular_velocity={};
        if(reason==10)ride.o.air_damping_torque[0]=std::numeric_limits<float>::quiet_NaN();
        if(reason==11)ride.o.angular_velocity[0]=std::numeric_limits<float>::infinity();
        if(reason==13)ride.o.torque[0]=11; // arbitrary torque cannot be relabeled passive
        if(reason==14)ride.o.queried_clear[1]=false;
        if(reason==15)ride.o.air_control_torque={0,1,0};
        if(reason==16)ride.o.motor_torque={0,1,0};
        if(reason==17)ride.o.contact_torque={0,1,0};
        if(reason==18)ride.o.balance_torque={0,1,0};
        if(reason==19){ride.o.air_control_torque={0,1,0};ride.o.motor_torque={0,-1,0};}
        if(reason==10||reason==11)ride.tick(1.8f,1,false,1.5f,2);
        else if(reason==12)for(unsigned i=0;i<20;++i)ride.tick(1.8f+.03f*i,1,false,0,2);
        else ride.natural_flight(reason?20:38);
        if(reason)check(!ride.driver.tally().flight,"unqualified/nonpassive/residual/excess/intent/hop/invalid/accelerated flight cannot qualify");
        else {
            check(ride.driver.tally().flight&&ride.driver.tally().maximum_natural_flight_seconds>.3&&
                ride.driver.tally().passive_angular_intervals>0&&ride.driver.tally().maximum_ballistic_seconds==0,
                "gravity-only translation with qualified passive rotation is counted separately from zero-torque ballistic motion");
            ride.o.neutral_air_damping_active=false;ride.o.air_damping_torque=ride.o.torque={};
            ride.land();check(ride.driver.tally().landed,"qualified natural translation retains unchanged actual two-wheel landing requirements");
        }
    }
    Ride split;split.neutral();split.runup();split.climb();
    split.o.queried_clear={true,true};split.o.neutral_air_damping_active=true;
    split.o.angular_velocity={-1,0,0};split.o.air_damping_torque=split.o.torque={10,0,0};
    split.natural_flight(6);
    split.o.queried_clear[1]=false;split.tick(split.o.root[2]+.03f,split.o.root[1],false,.52f,2);
    split.o.queried_clear[1]=true;
    for(unsigned i=0;i<6;++i)split.tick(split.o.root[2]+.03f,split.o.root[1],false,static_cast<float>(.52-9.8*h*(i+1)),2);
    check(!split.driver.tally().flight&&split.driver.tally().maximum_natural_flight_seconds<.12,
        "unknown/speculative query breaks adjacency; separated passive intervals never combine");
}
void captured_native196_runout_stays_failed_after_input_plan_change() {
    // Actual196 last-inside/failure pair from samples256/257, serials277/278.
    // Prefix below is authored only to establish a valid owner/clock and remain
    // inside the route. It is NOT a native replay or a simulation of the.8 input.
    r::Route route{{301.90631103515625f,274.3034973144531f,-480.0640563964844f},
        {-.7549182772636414f,0,-.6558188796043396f},
        {307.40423583984375f,274.3523254394531f,-474.9566955566406f},
        274.3523254394531,.987213134765625,1.2000000476837158};
    r::Driver driver(route);
    const r::Vec first_root{307.0267639160156f,274.3420104980469f,-475.28466796875f};
    const r::Vec first_com{307.0555114746094f,275.2428283691406f,-475.26971435546875f};
    const r::Vec inside_root{297.97052001953125f,274.309814453125f,-483.1231689453125f};
    const r::Vec inside_com{298.00537109375f,275.209716796875f,-483.0849914550781f};
    constexpr double native_h=.01666666567325592;
    r::Observation observed;observed.valid=observed.grounded=true;
    observed.step_seconds=native_h;observed.serial=20;observed.body_token=7;
    observed.actual_wheel_support={true,true};
    observed.velocity={-2.191392421722412f,.004197360016405582f,-1.8777786493301392f};
    for(unsigned i=0;i<=256;++i) {
        const float amount=static_cast<float>(i)/256;
        for(unsigned axis=0;axis<3;++axis) {
            observed.root[axis]=first_root[axis]+(inside_root[axis]-first_root[axis])*amount;
            observed.com[axis]=first_com[axis]+(inside_com[axis]-first_com[axis])*amount;
        }
        if(i==256){observed.root=inside_root;observed.com=inside_com;}
        observed.elapsed=(i+1)*native_h;++observed.serial;
        const auto input=driver.controls(observed.elapsed);
        if(i>31)check(input.pedal==.8&&input.brake==0,
            "new input plan remains only a reduced synthetic pedal request before this captured boundary");
        driver.observe(observed);
        check(!driver.tally().completed,"authored prefix and captured last-inside sample remain within original route bounds");
    }
    check(std::abs(driver.tally().progress-4.977424337541379)<1e-8&&driver.tally().last_serial==277,
        "independent captured world-space root remains inside unchanged five-meter boundary");
    observed.root={297.934814453125f,274.30987548828125f,-483.1538391113281f};
    observed.com={297.9696044921875f,275.20977783203125f,-483.11566162109375f};
    observed.velocity={-2.1462996006011963f,.004393959883600473f,-1.8393338918685913f};
    observed.body_up_alignment=.9999920725822449;observed.support_up_alignment=.9999948143959045;
    observed.elapsed+=native_h;++observed.serial;
    driver.observe(observed);
    check(driver.tally().completed&&!driver.tally().passed&&driver.tally().last_serial==278&&
        std::string_view(driver.tally().issue)=="bounded_route_or_motion_violation"&&
        std::abs(driver.tally().progress-5.024493196135154)<1e-8,
        "reduced generated pedal cannot retroactively turn native196's supported normal-speed route overrun into PASS");
}
int main() {
    captured_native196_runout_stays_failed_after_input_plan_change();
    qualified_passive_rotation_is_distinct_from_ballistic_rotation();
    measured_entry_must_stay_inside_cleared_approach();
    genuine_sequence_and_inputs();support_and_flight_are_not_invented();settling_bounce_and_quality();stale_and_invalid_measurements();provisional_pass_remains_guarded();verified_runout_boundary();
    ray_clear_time_alone_is_not_ballistic_flight();front_only_landing_must_wait_for_both_actual_wheels();
    unqualified_clear_recontact_can_resume_ascent_then_launch();
    std::cout<<"BMX measured ramp rehearsal, natural flight, stable landing and bounded controls tests passed\n";
}
