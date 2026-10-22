#include "Engine/Game/BMX/bmx_tricks.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
using namespace dingosdk::bmx;
constexpr double pi=std::numbers::pi,tau=2*pi;
void check(bool value,const char* reason) {
    if(!value){std::cerr<<reason<<'\n';std::exit(1);}
}
struct Ride {
    TrickController tricks;
    TrickInput input;
    Ride(){input.enabled=true;input.grounded=true;input.owner=17;input.device=2;input.bar_neutral=input.rear_neutral=true;tick();}
    TrickEvents tick(double seconds=.01){return tricks.update(seconds,input);}
    void air(){input.grounded=false;check(tick().took_off,"a grounded rider must enter one air attempt");}
    void bar(TrickDirection direction=TrickDirection::left){input.bar_neutral=false;input.barspin=direction;tick();input.barspin=TrickDirection::none;}
    void tail(TrickDirection direction=TrickDirection::right){input.rear_neutral=false;input.tailwhip=direction;tick();input.tailwhip=TrickDirection::none;}
    void wait(unsigned steps,bool neutral=true){input.bar_neutral=input.rear_neutral=neutral;for(unsigned i=0;i<steps;++i)tick();}
    TrickEvents land(){input.grounded=true;return tick();}
};
void air_only_and_preheld() {
    Ride ride;ride.input.barspin=TrickDirection::left;ride.input.tailwhip=TrickDirection::right;
    ride.input.bar_neutral=ride.input.rear_neutral=false;
    for(unsigned i=0;i<10;++i)ride.tick();
    check(!ride.tricks.state().bar_rotating&&!ride.tricks.state().tail_rotating,"ground requests never start air tricks");
    ride.air();for(unsigned i=0;i<10;++i)ride.tick();
    check(!ride.tricks.state().bar_rotating&&!ride.tricks.state().tail_rotating,"requests held through takeoff cannot ghost-start");
    ride.input.barspin=ride.input.tailwhip=TrickDirection::none;ride.wait(1);
    ride.bar();ride.tail();
    check(ride.tricks.state().bar_rotating&&ride.tricks.state().tail_rotating,"fresh gestures after airborne neutral may start both channels");
}
void usable_hop_and_landed_tally() {
    Ride ride;ride.air();ride.bar();ride.tail();
    double previous=ride.tricks.state().bar_angle;
    for(unsigned i=0;i<56;++i) {
        ride.input.bar_neutral=ride.input.rear_neutral=true;ride.tick();
        const auto& state=ride.tricks.state();
        check(state.bar_angle<=previous+1e-9&&state.bar_angle>=-tau-1e-9,"bar rotation is continuous through the full360");
        previous=state.bar_angle;
        check(state.tally.landed_units==0,"airborne catches never award landed combo credit");
    }
    const auto& caught=ride.tricks.state();
    check(caught.bar_caught&&caught.tail_caught&&caught.pending_units==2,"both45/55-centisecond rotations fit a short hop and catch");
    check(std::abs(caught.bar_angle+tau)<1e-9&&std::abs(caught.tail_angle-tau)<1e-9,"completed channels retain their unwrapped360 endpoints");
    check(caught.tail_release>0,"actual catch begins a bounded contact blend rather than snapping feet");
    ride.wait(6);check(ride.tricks.state().bar_release==0&&ride.tricks.state().tail_release==0,"neutral catches reattach contacts after the catch blend");
    const auto result=ride.land();
    check(result.combo_landed&&result.barspins==1&&result.tailwhips==1&&result.units==2,"only grounded landing commits the caught combo");
    check(ride.tricks.state().tally.landed_combos==1&&ride.tricks.state().tally.landed_units==2,"separate BMX tally records exactly one landed combo");
    check(ride.tricks.state().tally.landed_by_id[34]==1&&ride.tricks.state().tally.landed_by_id[52]==1,
        "legacy leftbar/rightwhip retain directional identities in the complete catalog tally");
    ride.tick();check(ride.tricks.state().tally.landed_combos==1,"a held grounded state cannot award the combo twice");
}
void no_late_catch_or_completion() {
    Ride incomplete;incomplete.air();incomplete.bar();incomplete.wait(38);
    incomplete.input.grounded=true;
    const auto early=incomplete.tick(.10);
    check(early.failed&&early.failure==TrickFailure::under_rotated&&early.units==0,"landing sample cannot spend post-contact time finishing a rotation");
    Ride no_catch;no_catch.air();no_catch.tail();no_catch.wait(56,false);
    check(no_catch.tricks.state().tail_waiting_catch&&no_catch.tricks.state().tail_release==1,"feet stay free through uncaught completion");
    no_catch.input.rear_neutral=true;
    const auto landing=no_catch.land();
    check(landing.failed&&landing.failure==TrickFailure::uncaught,"first neutral on the landing frame is too late to catch");
    check(no_catch.tricks.state().tally.landed_tailwhips==0,"uncaught tailwhip cannot produce landed credit");
    Ride expired;expired.air();expired.bar();expired.wait(60,false);
    check(expired.tricks.state().failure==TrickFailure::uncaught&&expired.tricks.state().bar_release==1,"missed catch window fails without automatically reattaching hands");
    expired.wait(2,true);check(expired.land().failed,"late neutral cannot revive an expired catch");
}
void catch_window_and_repeat_rearm() {
    Ride ride;ride.air();ride.bar();ride.wait(46,false);
    check(ride.tricks.state().bar_waiting_catch,"a held stick leaves a completed barspin awaiting catch");
    ride.input.bar_neutral=true;
    check(ride.tick().bar_caught,"returning neutral within the catch window catches the bar");
    ride.wait(6);ride.bar(TrickDirection::right);ride.wait(46);
    check(ride.tricks.state().pending_barspins==2,"a fresh neutral-rearmed gesture can repeat a rotation in one longer flight");
    const auto landed=ride.land();check(landed.combo_landed&&landed.barspins==2,"both caught rotations commit together on landing");
    Ride held;held.air();held.input.bar_neutral=false;held.input.barspin=TrickDirection::left;held.tick();
    for(unsigned i=0;i<46;++i)held.tick();
    check(held.tricks.state().bar_waiting_catch&&held.tricks.state().pending_barspins==0,"a repeated held request cannot restart or auto-catch the channel");
    held.input.barspin=TrickDirection::none;held.input.bar_neutral=true;held.tick();
    held.wait(10);check(held.tricks.state().pending_barspins==1&&!held.tricks.state().bar_rotating,"neutral alone rearms but never starts another spin");
}
void physical_spins_and_alignment() {
    for(double rotation:{pi,tau,-tau}) {
        Ride ride;ride.air();
        for(unsigned i=1;i<=80;++i) {
            ride.input.body_yaw=std::remainder(rotation*i/80,tau);ride.tick();
        }
        const auto result=ride.land();
        check(result.combo_landed&&result.spin_half_turns==(std::abs(rotation)>pi+1?2u:1u),"unwrapped applied yaw recognizes180,360 and negative360 with forward/fakie alignment");
    }
    Ride partial;partial.air();
    for(unsigned i=1;i<=50;++i){partial.input.body_yaw=.75*pi*i/50;partial.tick();}
    check(partial.land().failure==TrickFailure::under_rotated,"a135-degree attempted spin cannot claim180 credit");
    Ride sideways;sideways.air();sideways.bar();
    for(unsigned i=1;i<=50;++i){sideways.input.bar_neutral=true;sideways.input.body_yaw=.25*pi*i/50;sideways.tick();}
    const auto misaligned=sideways.land();
    check(misaligned.failed&&misaligned.failure==TrickFailure::unaligned_landing,"caught bars with a sideways body-to-momentum landing fail");
    Ride empty;empty.air();empty.wait(10);check(!empty.land().combo_landed&&empty.tricks.state().tally.landed_units==0,"ordinary hops and empty intent never earn trick points");
}
void interruption_and_ownership() {
    for(unsigned interruption=0;interruption<4;++interruption) {
        Ride ride;ride.air();ride.bar();ride.wait(46);
        if(interruption==0)ride.input.enabled=false;
        if(interruption==1)++ride.input.device;
        if(interruption==2)++ride.input.owner;
        if(interruption==3){ride.tricks.cancel();}
        const auto event=ride.tick();
        check(interruption==3||event.canceled,"focus/device/equipment ownership changes cancel a live trick");
        check(!ride.tricks.state().airborne&&ride.tricks.state().bar_release==0,"cancellation releases transient rotation/contact intent");
        ride.input.enabled=true;ride.input.barspin=TrickDirection::none;ride.wait(60);
        check(!ride.land().combo_landed&&ride.tricks.state().tally.landed_units==0,"resuming in air or landing later cannot resurrect canceled credit");
        check(ride.tricks.state().tally.canceled_combos==1,"an interrupted flight counts once, not every suppressed tick");
        ride.tick();ride.air();ride.bar();ride.wait(46);
        check(ride.land().combo_landed,"observed ground and a fresh neutral gesture may begin a new combo after cancellation");
    }
    Ride scored;scored.air();scored.bar();scored.wait(46);scored.land();scored.tricks.cancel();
    check(scored.tricks.state().tally.landed_barspins==1,"pause cancellation preserves already landed session tally");
    scored.tricks.reset();check(scored.tricks.state().tally.landed_units==0,"explicit session reset clears the separate BMX tally");
}
void bad_clock_and_heading() {
    for(double seconds:{-.01,.2,std::numeric_limits<double>::quiet_NaN()}) {
        Ride ride;ride.air();ride.bar();ride.wait(20);
        const auto event=ride.tick(seconds);
        check(event.canceled&&event.failure==TrickFailure::invalid_sample,"negative/hitch/nonfinite native time cancels instead of advancing a trick");
        ride.wait(60);check(!ride.land().combo_landed,"discarded clock time can never catch up into later landing credit");
    }
    Ride zero;zero.air();zero.bar();zero.wait(20);const auto before=zero.tricks.state().bar_angle;
    zero.input.grounded=true;zero.input.bar_neutral=true;zero.tick(0);
    check(zero.tricks.state().airborne&&zero.tricks.state().bar_angle==before,"zero native time cannot rotate, catch or commit a landing");
    check(zero.tick().failed,"a later real landing still sees the incomplete trick");
    Ride teleport;teleport.air();teleport.bar();teleport.input.body_yaw=pi;
    check(teleport.tick().failure==TrickFailure::invalid_sample,"an impossible heading discontinuity cannot create spin credit");
}
void quality_caught(Ride& ride) {
    ride.air();ride.bar();ride.wait(52);
    check(ride.tricks.state().bar_caught,"quality fixture starts with an airborne caught barspin");
    ride.input.landing_quality_available=true;
}
TrickEvents settle(Ride& ride,unsigned ticks=6) {TrickEvents result;for(unsigned i=0;i<ticks;++i)result=ride.tick();return result;}
void native_quality_settling_and_freeze() {
    Ride ride;quality_caught(ride);
    const auto first=ride.land();
    check(!first.landed&&!first.combo_landed&&ride.tricks.state().landing_pending&&ride.tricks.state().landing_support_seconds==0,
        "first actual contact freezes a quality landing; preceding air time cannot count as settled support");
    const auto angle=ride.tricks.state().bar_angle;
    ride.input.barspin=TrickDirection::right;ride.input.bar_neutral=false;
    check(!ride.tick(0).combo_landed&&ride.tricks.state().landing_support_seconds==0,
        "zero native time cannot advance landing settling");
    for(unsigned i=0;i<5;++i) {
        ride.input.body_yaw+=.1;ride.input.travel_yaw=ride.input.body_yaw;
        check(!ride.tick().combo_landed,"five support intervals remain below the60ms quality window");
        check(ride.tricks.state().bar_angle==angle&&ride.tricks.state().pending_barspins==1,
            "held gestures after contact cannot restart frozen rotations or catch another trick");
    }
    const auto landed=ride.tick();
    check(landed.combo_landed&&landed.barspins==1&&landed.spin_half_turns==0&&!ride.tricks.state().landing_pending,
        "six following quality support intervals commit one caught trick without adding ground spin credit");
    check(!ride.tick().combo_landed&&ride.tricks.state().tally.landed_combos==1,"settled grounded state never awards twice");
    Ride edge;quality_caught(edge);edge.input.body_up_alignment=.5;edge.input.support_up_alignment=.15;edge.input.closing_normal_speed=7;
    edge.land();check(settle(edge).combo_landed,"finite body/support alignment and impact speed thresholds are inclusive");
}
void native_quality_bounces_and_rejections() {
    Ride bounce;quality_caught(bounce);bounce.land();settle(bounce,3);
    bounce.input.grounded=false;settle(bounce,4);
    check(bounce.tricks.state().landing_pending&&bounce.tricks.state().landing_support_seconds==0&&
        std::abs(bounce.tricks.state().landing_gap_seconds-.04)<1e-9,"brief40ms bounce resets support time while keeping channels frozen");
    bounce.input.grounded=true;check(!bounce.tick().combo_landed&&bounce.tricks.state().landing_support_seconds==0,
        "first strict recontact starts at zero; the preceding unsupported interval earns no support");
    check(!settle(bounce,5).combo_landed,"pre-bounce support cannot accumulate across a gap");
    check(bounce.tick().combo_landed,"brief bounce requires a fresh complete60ms supported settling window");
    Ride lost;quality_caught(lost);lost.land();lost.input.grounded=false;
    const auto canceled=settle(lost,5);
    check(canceled.canceled&&canceled.failure==TrickFailure::unstable_landing&&!lost.tricks.state().landing_pending,
        "support loss longer than40ms cancels the candidate");
    lost.wait(60);check(!lost.land().combo_landed&&lost.tricks.state().tally.landed_units==0,
        "a later landing cannot resurrect the canceled candidate");
    for(unsigned reason=0;reason<8;++reason) {
        Ride invalid;quality_caught(invalid);
        if(reason==0)invalid.input.body_up_alignment=.4999;
        if(reason==1)invalid.input.body_up_alignment=-1;
        if(reason==2)invalid.input.support_up_alignment=.149;
        if(reason==3)invalid.input.support_up_alignment=-1;
        if(reason==4)invalid.input.closing_normal_speed=7.001;
        if(reason==5)invalid.input.body_up_alignment=std::numeric_limits<double>::quiet_NaN();
        if(reason==6)invalid.input.closing_normal_speed=-1;
        if(reason==7)invalid.input.support_up_alignment=1.01;
        const auto result=invalid.land();
        check(!result.combo_landed&&(result.failed||result.canceled)&&invalid.tricks.state().tally.landed_units==0,
            "tilted/inverted/ceiling/hard-impact/malformed quality cannot score a clean landing");
    }
    Ride tipped;quality_caught(tipped);tipped.land();tipped.input.body_up_alignment=-1;
    check(tipped.tick().failure==TrickFailure::unstable_landing&&tipped.tricks.state().tally.failed_combos==1,
        "losing upright alignment during settling fails instead of banking earlier contact");
    Ride unavailable;quality_caught(unavailable);unavailable.land();unavailable.input.landing_quality_available=false;
    check(unavailable.tick().canceled,"pending quality policy cannot silently fall back to immediate legacy scoring");
}
void native_quality_no_late_catch_or_unbounded_candidate() {
    Ride early;early.air();early.bar();early.wait(20);early.input.landing_quality_available=true;
    check(early.land().failure==TrickFailure::under_rotated&&!early.tricks.state().landing_pending,
        "settling time cannot finish an under-rotated trick after first contact");
    Ride late;late.air();late.bar();late.wait(46,false);late.input.landing_quality_available=true;late.input.bar_neutral=true;
    check(late.land().failure==TrickFailure::uncaught&&!late.tricks.state().landing_pending,
        "first neutral on contact is too late; quality settling cannot supply catch time");
    Ride teleported;quality_caught(teleported);teleported.land();teleported.input.body_yaw=pi;
    check(teleported.tick().failure==TrickFailure::invalid_sample,"a settling phase still rejects a heading teleport");
    Ride jitter;TrickSettings settings;settings.max_air_seconds=1;jitter.tricks=TrickController(settings);jitter.tick();quality_caught(jitter);jitter.land();
    bool expired=false;
    for(unsigned i=0;i<100&&!expired;++i) {
        jitter.input.grounded=i%3==0;
        expired=jitter.tick().canceled;
    }
    check(expired&&!jitter.tricks.state().landing_pending&&jitter.tricks.state().tally.landed_units==0,
        "repeated sub-threshold gaps cannot keep a frozen candidate alive beyond its finite attempt lease");
    Ride owner;quality_caught(owner);owner.land();++owner.input.owner;
    check(owner.tick().canceled&&owner.tricks.state().tally.landed_units==0,"ownership change cancels settling before any credit");
}
void catalog_identity_and_every_motion_lifecycle() {
    std::array<bool,80> ids{};unsigned shared=0,adapted=0;
    for(const auto& entry:catalog::entries) {
        const auto index=static_cast<unsigned>(entry.id);
        check(index<ids.size()&&!ids[index]&&!entry.name.empty()&&entry.source_object,
            "catalog identities are unique, bounded and carry exact source object provenance");
        ids[index]=true;shared+=entry.shared_clip_name?1U:0U;
        check(catalog::select(static_cast<int>(index/10),static_cast<int>(index%10))==entry.id,
            "catalog table keys preserve exact authored row and direction");
        if(catalog::legacy_rotation(entry.id))continue;
        ++adapted;
        Ride ride;ride.air();ride.input.movement=entry.id;ride.input.rear_neutral=false;ride.tick();
        ride.input.movement=catalog::TrickId::none;
        double max_phase=0,max_release=0;
        double max_bike_pitch=0,max_bike_roll=0,max_pose_bar=0;
        for(unsigned i=0;i<85;++i) {
            ride.input.rear_neutral=true;ride.tick();const auto& state=ride.tricks.state();
            max_phase=std::max(max_phase,state.movement_phase);max_release=std::max(max_release,state.movement_release);
            max_bike_pitch=std::max(max_bike_pitch,std::abs(state.movement_bike_pitch));
            max_bike_roll=std::max(max_bike_roll,std::abs(state.movement_bike_roll));
            max_pose_bar=std::max(max_pose_bar,std::abs(state.movement_bar_angle));
            check(state.movement_phase>=-1e-12&&state.movement_phase<=1.000001&&
                state.movement_release>=0&&state.movement_release<=1,"all catalog motion/contact envelopes stay finite and bounded");
            check(std::isfinite(state.movement_bike_pitch)&&std::isfinite(state.movement_bike_roll)&&
                std::abs(state.movement_bike_pitch)<=tau+1e-8&&std::abs(state.movement_bike_roll)<=tau+1e-8,
                "whole-bike presentation is bounded to the explicitly adapted pose or one complete cycle");
            check(state.tally.landed_movements==0,"catalog recovery in air never awards landed credit");
            check((entry.release_mask&catalog::left_hand)||state.release_left_hand==0,"a retained left hand never receives a free-hand flag");
            check((entry.release_mask&catalog::right_hand)||state.release_right_hand==0,"a retained right hand never receives a free-hand flag");
            check((entry.release_mask&catalog::left_foot)||state.release_left_foot==0,"a retained left foot never receives a free-foot flag");
            check((entry.release_mask&catalog::right_foot)||state.release_right_foot==0,"a retained right foot never receives a free-foot flag");
        }
        const auto state=ride.tricks.state();
        check(max_phase>.9&&max_release==1&&state.movement_caught&&state.pending_movements==1,
            "every new named entry actually advances an extension/release/recovery motion and catches once");
        check(std::abs(state.movement_bar_angle-entry.bar_turns*tau)<1e-8&&
            std::abs(state.movement_tail_angle-entry.tail_turns*tau)<1e-8&&
            std::abs(state.movement_crank_angle-entry.crank_turns*tau)<1e-8,
            "hybrid bar/frame/crank output completes its continuous adapted turns");
        if(entry.bike_pitch!=0)check(max_bike_pitch>.1,"pitch-articulated catalog motion cannot silently remain a static neutral bike");
        if(entry.bike_roll!=0)check(max_bike_roll>.1,"roll-articulated catalog motion cannot silently remain a static neutral bike");
        if(entry.bar_pose_radians!=0)check(max_pose_bar>2,"Xup/Turndown/Lookback actually articulate the bars while keeping their distinct pose identity");
        const auto result=ride.land();
        check(result.combo_landed&&result.movements==1&&result.units==1&&
            ride.tricks.state().tally.landed_by_id[index]==1,"only a subsequent clean landing banks the exact named catalog identity once");
        check(!ride.tick().combo_landed,"catalog landing cannot award the same motion twice");
    }
    check(shared==4&&adapted==54,"the complete table retains four positively bound shared clips and54 new motion paths beyond existing rotations");
    check(!catalog::find(static_cast<catalog::TrickId>(255))&&!catalog::find(static_cast<catalog::TrickId>(79)),
        "empty/invalid slots cannot acquire a motion descriptor");
}
void catalog_holds_catches_repeats_and_cancellation() {
    Ride hold;hold.air();hold.input.movement=catalog::TrickId::superman;hold.input.rear_neutral=false;hold.tick();
    hold.input.movement=catalog::TrickId::none;
    for(unsigned i=0;i<60;++i)hold.tick();
    check(hold.tricks.state().movement_holding&&hold.tricks.state().movement_phase==1&&
        hold.tricks.state().pending_movements==0,"a held Superman remains visibly extended without inventing a catch or score");
    hold.input.rear_neutral=true;
    check(hold.land().failure==TrickFailure::uncaught,"first neutral at contact is too late to recover a held pose");
    Ride repeat;repeat.air();
    for(auto id:{catalog::TrickId::one_hander_left,catalog::TrickId::can_right}) {
        repeat.input.movement=id;repeat.input.rear_neutral=false;repeat.tick();
        repeat.input.movement=catalog::TrickId::none;repeat.wait(40);
    }
    check(repeat.tricks.state().pending_movements==2,"neutral catches rearm distinct movement families within one flight");
    auto event=repeat.land();check(event.movements==2&&event.units==2,"different caught movement IDs bank together without duplicate legacy rotations");
    Ride cycle;cycle.air();cycle.input.movement=catalog::TrickId::crankflip;cycle.input.rear_neutral=false;cycle.tick();
    cycle.input.movement=catalog::TrickId::none;
    for(unsigned i=0;i<70;++i)cycle.tick();
    check(cycle.tricks.state().failure==TrickFailure::uncaught&&cycle.tricks.state().movement_release==1,
        "a completed cycle cannot automatically catch while the selecting stick stays held");
    cycle.input.rear_neutral=true;check(cycle.land().failed,"expired cycle catches cannot revive on contact");
    for(unsigned reason=0;reason<5;++reason) {
        Ride ride;ride.air();ride.input.movement=catalog::TrickId::table;ride.tick();ride.wait(10);
        if(reason==0)++ride.input.owner;
        if(reason==1)++ride.input.device;
        if(reason==2)ride.input.enabled=false;
        if(reason==3)ride.input.movement=static_cast<catalog::TrickId>(79);
        const auto canceled=ride.tick(reason==4?.2:.01);
        check(canceled.canceled&&ride.tricks.state().movement_id==catalog::TrickId::none&&
            ride.tricks.state().tally.landed_movements==0,"owner/device/pause/invalid ID/time atomically cancel all catalog presentation and pending credit");
        ride.input.enabled=true;ride.input.movement=catalog::TrickId::none;
        ride.wait(60);check(!ride.land().combo_landed,"canceled movement cannot earn late landing credit");
    }
    Ride recovery;recovery.air();recovery.input.movement=catalog::TrickId::tuck_no;recovery.tick();
    recovery.input.movement=catalog::TrickId::none;recovery.wait(22);
    check(recovery.tricks.state().movement_recovering,"pose catch has a real bounded airborne recovery interval");
    recovery.input.landing_quality_available=true;
    check(recovery.land().failure==TrickFailure::uncaught,"post-contact settling cannot finish an airborne recovery");
    Ride settled;settled.air();settled.input.movement=catalog::TrickId::nothing;settled.tick();
    settled.input.movement=catalog::TrickId::none;settled.wait(40);settled.input.landing_quality_available=true;
    check(!settled.land().combo_landed&&settled.tricks.state().landing_pending,"catalog credit honors the same native quality settling window");
    check(settle(settled).combo_landed&&settled.tricks.state().tally.landed_movements==1,"quality support commits exactly one fully recovered catalog movement");
}
std::array<double,4> axis_orientation(unsigned axis,double radians,double yaw=0) {
    std::array<double,4> q{0,0,0,std::cos(radians/2)};q[axis]=std::sin(radians/2);
    const double s=std::sin(yaw/2),c=std::cos(yaw/2);
    return {c*q[0]+s*q[2],c*q[1]+s*q[3],c*q[2]-s*q[0],c*q[3]-s*q[1]};
}
void measured_full_orientation_flips_and_rolls() {
    for(unsigned axis:{0U,2U})for(double direction:{-1.,1.})for(double yaw:{0.,1.7}) {
        Ride ride;ride.input.orientation_available=true;ride.input.body_yaw=ride.input.travel_yaw=yaw;
        ride.input.body_quaternion=axis_orientation(axis,0,yaw);ride.tick();ride.air();
        for(unsigned frame=1;frame<=100;++frame) {
            const double angle=direction*tau*frame/100;
            ride.input.body_quaternion=axis_orientation(axis,angle,yaw);
            // A native Euler heading can flip by pi when the bike passes vertical.
            // Full quaternion mode must follow the measured pose, not this branch.
            ride.input.body_yaw=std::remainder(yaw+(frame>25&&frame<75?pi:0),tau);
            if(frame%3==0)for(auto& component:ride.input.body_quaternion)component=-component;
            ride.tick();
            check(ride.tricks.state().tally.landed_units==0,"physical air orientation never creates airborne score");
        }
        const auto state=ride.tricks.state();
        check(std::abs((axis==0?state.flip_radians:state.roll_radians)-direction*tau)<1e-8,
            "shortest-arc quaternion deltas unwrap true local-axis360 despite sign flips, yaw basis and Euler heading jumps");
        check(std::abs(state.spin_radians)<1e-8,"a measured pure flip/roll does not invent a yawspin");
        ride.input.body_yaw=yaw;const auto result=ride.land();
        check(result.combo_landed&&result.units==1&&
            (axis==0?(direction>0?result.front_flips==1:result.back_flips==1):result.barrel_rolls==1),
            "front/backflip or barrelroll banks only after a complete measured turn and aligned landing");
    }
    Ride short_flip;short_flip.input.orientation_available=true;short_flip.tick();short_flip.air();
    for(unsigned i=1;i<=60;++i){short_flip.input.body_quaternion=axis_orientation(0,pi*i/60);short_flip.tick();}
    check(short_flip.land().failure==TrickFailure::under_rotated,"a physical180pitch cannot be called a complete flip");
    for(unsigned reason=0;reason<5;++reason) {
        Ride invalid;invalid.input.orientation_available=true;invalid.tick();invalid.air();
        for(unsigned i=1;i<=40;++i){invalid.input.body_quaternion=axis_orientation(0,tau*i/100);invalid.tick();}
        if(reason==0)invalid.input.body_quaternion[0]=std::numeric_limits<double>::quiet_NaN();
        if(reason==1)invalid.input.body_quaternion={0,0,0,0};
        if(reason==2)invalid.input.body_quaternion={0,0,0,2};
        if(reason==3)invalid.input.body_quaternion=axis_orientation(0,-pi/2);
        if(reason==4)invalid.input.orientation_available=false;
        check(invalid.tick().failure==TrickFailure::invalid_sample&&invalid.tricks.state().tally.landed_units==0,
            "nonunit/nonfinite pose, implausible angular jump and midair orientation-source loss cancel without late credit");
    }
    Ride pause;pause.input.orientation_available=true;pause.tick();pause.air();
    const auto before=pause.tricks.state().flip_radians;
    pause.input.body_quaternion=axis_orientation(0,.1);pause.tick(0);
    check(pause.tricks.state().flip_radians==before,"zero native time cannot count a quaternion orientation change");
}
}
int main() {
    air_only_and_preheld();usable_hop_and_landed_tally();no_late_catch_or_completion();catch_window_and_repeat_rearm();
    physical_spins_and_alignment();interruption_and_ownership();bad_clock_and_heading();
    native_quality_settling_and_freeze();native_quality_bounces_and_rejections();native_quality_no_late_catch_or_unbounded_candidate();
    catalog_identity_and_every_motion_lifecycle();catalog_holds_catches_repeats_and_cancellation();
    measured_full_orientation_flips_and_rolls();
    std::cout<<"BMX air tricks, neutral catches, landed combos and cancellation tests passed\n";
}
