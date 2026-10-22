#include "Engine/Game/BMX/bmx_input.h"
#include "Engine/Game/BMX/bmx_chassis_control.h"
#include <cmath>
#include <cstdio>
#include <limits>

using namespace dingosdk::bmx;
namespace {
int failures{};
void check(bool condition, const char* message) {
    if (!condition) { std::printf("FAIL: %s\n", message); ++failures; }
}
bool near(double a, double b, double tolerance = 1e-9) { return std::abs(a - b) <= tolerance; }
RawInput pad() { RawInput p; p.available = true; p.device = 1; return p; }
constexpr InputContext ground{true}, air{false};
constexpr double tick = 1.0 / 60.0;
Actions step(InputMapper& mapper, RawInput p, InputContext context = ground, double seconds = tick) {
    return mapper.update(p, context, seconds);
}
void ready(InputMapper& mapper, InputContext context = ground) {
    check(step(mapper, pad(), context).active, "neutral available input arms mapper");
}
chassis_control::Result pitch_response(const Actions& action) {
    auto body = chassis_control::proposed_body();
    body.center_of_mass = {0,2,0};
    const auto wheels = chassis_control::proposed_wheels();
    std::array<chassis_control::ContactSample,2> contacts;
    for (auto& sample : contacts) {
        sample.contact.status = tire::ContactStatus::clear;
        sample.contact.point = body.center_of_mass;
        sample.separation = 1;
    }
    chassis_control::Controls controls;
    controls.enabled = action.active;
    // Exact bridge mapping. Manual intent remains grounded-only in Actions;
    // this separate left-stick Flip channel drives the existing airborne torque.
    controls.manual = std::max(0.0,-action.air_pitch);
    controls.nose_manual = std::max(0.0,action.air_pitch);
    return chassis_control::Controller{}.step(chassis_control::verified_native_seconds,body,wheels,contacts,controls);
}
void streets_left_air_pitch_rearm_and_real_torque() {
    InputMapper mapper; ready(mapper);
    auto p = pad(); p.right_y = -1;
    for (int i = 0; i < 20; ++i) step(mapper,p);
    p.right_y = 1;
    check(step(mapper,p).hop_pressed,"air pitch change preserves grounded back-forward hop");
    for (int i = 0; i < 20; ++i) {
        const auto action = step(mapper,p,air);
        const auto solved = pitch_response(action);
        check(near(action.air_pitch,0) && solved.valid && near(solved.torque[0],0),
            "held hop forward release cannot produce airborne nose-down torque");
    }
    ready(mapper,air);
    check(near(step(mapper,p,air).air_pitch,0),"right-stick hop/trick input never becomes Flip even after neutral");
    p=pad();p.left_y=1;
    auto action = step(mapper,p,air);
    auto solved = pitch_response(action);
    check(near(action.air_pitch,1) && near(action.manual,0) && near(action.nose_manual,0),
        "air neutral arms full forward pitch without claiming wheel-supported manual");
    check(solved.valid && near(solved.torque[0],160) && near(solved.force[0],0) && near(solved.force[1],0) && near(solved.force[2],0),
        "actual mapper-to-clear-air controller gives nose-down torque and excludes native gravity");
    p.left_y = -1; action = step(mapper,p,air); solved = pitch_response(action);
    check(near(action.air_pitch,-1) && solved.valid && near(solved.torque[0],-160),
        "backward airborne left input gives real front-up torque");
    p.left_y = .55; action = step(mapper,p,air);
    check(action.air_pitch > 0 && action.air_pitch < 1,"partial airborne left Y produces bounded proportional pitch");
    ready(mapper,air);
    p=pad();p.right_x = 1; action = step(mapper,p,air);
    check(near(action.air_pitch,0) && action.tailwhip == TrickDirection::none,
        "bare rear stick neither selects a named trick nor pitches");
    p.right_x = 0; p.right_y = 1;
    check(near(step(mapper,p,air).air_pitch,0),"tailwhip vertical endpoint cannot become a nose dive before neutral");
    p=pad();p.left_y=1;
    check(near(step(mapper,p,air).air_pitch,1),"left Flip remains independent of rear trick endpoints");
    action = step(mapper,p,ground);
    check(near(action.air_pitch,0) && near(action.nose_manual,0),"landing held input clears air pitch and requires ground balance rearm");
    p=pad();p.right_y = -.55; ready(mapper); action = step(mapper,p);
    check(action.manual > 0 && near(action.air_pitch,0),"grounded partial manual behavior remains unchanged");
    for (int i = 0; i < 20; ++i) check(near(step(mapper,p,air).air_pitch,0),
        "preheld partial ground manual cannot leak through takeoff");
}
void streets_left_air_pitch_inhibition_and_arc() {
    InputMapper mapper; ready(mapper,air);
    auto p = pad(); p.left_y = -1;
    check(step(mapper,p,air).air_pitch < 0,"initial air neutral permits deliberate left-stick pitch");
    for (int reason = 0; reason < 6; ++reason) {
        auto blocked = p;
        if (reason == 0) blocked.focused = false;
        if (reason == 1) blocked.menu_open = true;
        if (reason == 2) blocked.available = false;
        if (reason == 3) blocked.device = 20;
        if (reason == 4) blocked.left_y = std::numeric_limits<double>::quiet_NaN();
        const auto stopped = step(mapper,blocked,air,reason == 5 ? 0 : tick);
        check(!stopped.active && near(stopped.air_pitch,0),"inhibited or changed-device air pitch is immediately suppressed");
        check(near(step(mapper,p,air).air_pitch,0),"held left pitch cannot survive source recovery without release");
        ready(mapper,air);
        check(step(mapper,p,air).air_pitch < 0,"fresh air neutral rearms pitch after source recovery");
    }
    mapper.reset(); check(near(step(mapper,p,air).air_pitch,0),"equipment reset suppresses held air pitch");
    ready(mapper,air);
    p=pad();
    p.right_y = -.7; step(mapper,p,air);
    p.right_x = .7; p.right_y = 0; step(mapper,p,air);
    p.right_x = 0; p.right_y = .7;
    const auto arc = step(mapper,p,air);
    check(arc.tailwhip == TrickDirection::none && near(arc.air_pitch,0),
        "legacy rear arc without modifiers does not select a named trick or pitch");
    ready(mapper,air); p=pad();p.left_y = .1;
    check(near(step(mapper,p,air).air_pitch,0),"radial deadzone suppresses airborne left-stick drift");
    p.left_y = .27;
    check(near(step(mapper,p,air).air_pitch,0),"air pitch threshold suppresses small longitudinal movement");
    ready(mapper,air);p=pad();p.left_x=1;step(mapper,p,air);p.left_x=0;p.left_y=1;
    check(step(mapper,p,air).air_pitch > 0,"after airborne neutral a deliberate left-stick axis change may combine yaw and Flip");
    ready(mapper,air);p=pad();p.left_x=.8;p.left_y=.6;
    const auto diagonal=step(mapper,p,air);
    check(diagonal.steer>.7 && diagonal.air_pitch>.4 && diagonal.handlebar==TrickDirection::none,
        "left diagonal produces combined yaw/Flip intent without a barspin");
}
bool event(const Actions& a) {
    return a.hop_pressed || a.handlebar != TrickDirection::none || a.tailwhip != TrickDirection::none;
}

void normalization_and_ownership() {
    InputMapper mapper;
    check(!step(mapper, {}).active, "unavailable default input suppressed");
    auto p = pad(); p.pedal = 1;
    check(!step(mapper, p).active, "held pedal at activation requires release");
    ready(mapper);
    p = pad(); p.left_x = 4; p.left_y = -4; p.right_y = 0.05; p.pedal = 2; p.brake = -1;
    auto a = step(mapper, p);
    check(a.active && near(a.steer, std::sqrt(0.5)) && near(a.front_weight, -std::sqrt(0.5)),
        "sticks clamp to unit disc without diagonal gain");
    check(near(a.rear_weight, 0) && near(a.manual, 0), "radial deadzone suppresses stick drift");
    check(near(a.pedal, 1) && near(a.brake, 0), "pedal and brake clamp to normalized range");
    p = pad(); p.pedal = 0.04; p.brake = 0.03;
    a = step(mapper, p);
    check(near(a.pedal, 0) && near(a.brake, 0), "trigger deadzone prevents pedal and brake drift");
    p = pad(); p.left_x = -1; p.right_y = 0.5;
    a = step(mapper, p);
    check(near(a.steer, -1) && a.rear_weight > 0, "left steering and rear weight remain independent");
    p.pedal = 1;
    a = step(mapper, p, air);
    check(near(a.pedal, 0), "pedal does not create airborne propulsion");
}

void hop_preload_flick_and_latch() {
    InputMapper mapper; ready(mapper);
    auto p = pad(); p.right_y = -1;
    for (int i = 0; i < 30; ++i) {
        const auto a = step(mapper, p);
        check(!a.hop_pressed && a.preload > 0 && a.preload <= 1, "back hold preloads without repeated hop");
    }
    p.right_y = 1;
    auto a = step(mapper, p);
    check(a.hop_pressed && near(a.hop_strength, 1) && near(a.preload, 0), "back to forward flick releases fully charged hop");
    for (int i = 0; i < 40; ++i) check(!step(mapper, p).hop_pressed, "holding forward never repeats hop");
    p.right_y = -1;
    check(!step(mapper, p).hop_pressed, "return directly to back does not bypass neutral rearm");
    p.right_y = 1;
    check(!step(mapper, p).hop_pressed, "a second unarmed back-forward sequence is rejected");
    ready(mapper);
    p.right_y = -1; step(mapper, p);
    step(mapper, pad()); // sampled neutral is allowed within one flick
    p.right_y = 1; a = step(mapper, p);
    check(a.hop_pressed && a.hop_strength >= 0.25 && a.hop_strength < 1, "quick hop works through sampled neutral with bounded strength");
}

void hop_cancellation_and_context() {
    InputMapper mapper; ready(mapper);
    auto p = pad(); p.right_y = -1; step(mapper, p);
    for (int i = 0; i < 30; ++i) check(!step(mapper, pad()).hop_pressed, "neutral release alone is not a hop");
    p.right_y = 1;
    check(!step(mapper, p).hop_pressed, "a slow forward movement after preload timeout does not hop");
    ready(mapper);
    p.right_y = -1; step(mapper, p);
    p.right_x = 0.9; p.right_y = 0.6;
    check(!step(mapper, p).hop_pressed, "sideways movement cancels pending hop");
    p.right_x = 0; p.right_y = 1;
    check(!step(mapper, p).hop_pressed, "canceled hop cannot resume on forward hold");
    ready(mapper);
    p.right_y = -1; step(mapper, p);
    check(!event(step(mapper, p, air)), "leaving support discards grounded preload");
    p.right_y = 1;
    check(!step(mapper, p, air).hop_pressed, "airborne back-forward sequence never hops");
    p.right_y = -1; step(mapper, p, ground);
    p.right_y = 1;
    check(!step(mapper, p, ground).hop_pressed, "landing with preheld back cannot create ghost hop");
    ready(mapper);
    p.right_y = -1; step(mapper, p); p.right_y = 1;
    check(step(mapper, p).hop_pressed, "landing then neutral correctly rearms hop");
}

void partial_stick_balance() {
    InputMapper mapper; ready(mapper);
    auto p = pad(); p.right_y = -0.55;
    auto a = step(mapper, p);
    check(a.manual > 0 && a.manual < 1 && near(a.nose_manual, 0) && near(a.preload, 0), "partial rear stick requests manual without preload");
    p.right_y = 0.55; a = step(mapper, p);
    check(a.nose_manual > 0 && a.nose_manual < 1 && near(a.manual, 0), "partial forward stick requests nose manual");
    p.right_x = 0.9;
    a = step(mapper, p);
    check(near(a.manual, 0) && near(a.nose_manual, 0), "strong sideways input does not enter manual balance");
    p = pad(); p.right_y = -0.55;
    a = step(mapper, p, air);
    check(near(a.manual, 0) && near(a.nose_manual, 0), "airborne stick does not claim wheel-supported manual");
    a = step(mapper, p, ground);
    check(near(a.manual, 0), "preheld partial stick on landing needs neutral before manual");
    ready(mapper); a = step(mapper, p);
    check(a.manual > 0, "manual rearms after landing neutral");
}

void streets_named_selector_and_modifier_priority() {
    // Independent oracle from authored BMX_TrickDataSets46147, not the implementation's branch order.
    // Bitmask LB=1/RB=2/LT=4/RT=8. Additional LT permits LB, additional RT permits RB;
    // all other combinations select unsupported authored sets rather than falling through.
    constexpr std::array<TrickDirection,16> down_bar{
        TrickDirection::none,TrickDirection::left,TrickDirection::right,TrickDirection::none,
        TrickDirection::none,TrickDirection::left,TrickDirection::none,TrickDirection::none,
        TrickDirection::none,TrickDirection::none,TrickDirection::right,TrickDirection::none,
        TrickDirection::none,TrickDirection::none,TrickDirection::none,TrickDirection::none};
    const double d=std::sqrt(.5);
    const std::array<std::array<double,2>,8> slots{{{0,1},{d,d},{1,0},{d,-d},{0,-1},{-d,-d},{-1,0},{-d,d}}};
    for (unsigned mask=0;mask<16;++mask) for (unsigned slot=0;slot<8;++slot) {
        InputMapper mapper;ready(mapper,air);auto p=pad();
        p.left_shoulder=(mask&1)!=0;p.right_shoulder=(mask&2)!=0;
        p.left_trigger=(mask&4)?1:0;p.right_trigger=(mask&8)?1:0;
        p.right_x=slots[slot][0];p.right_y=slots[slot][1];
        const auto a=step(mapper,p,air);
        const auto expected_bar=slot==4?down_bar[mask]:TrickDirection::none;
        const auto expected_whip=mask==12&&slot==2?TrickDirection::right:
            mask==12&&slot==6?TrickDirection::left:TrickDirection::none;
        check(a.active && a.handlebar==expected_bar && a.tailwhip==expected_whip,
            "all16 modifier combinations x8 authored sectors select only implemented named requests");
        for(int i=0;i<8;++i) check(!event(step(mapper,p,air)),"held modifier/stick selects at most once");
        p.right_x=-p.right_x;p.right_y=-p.right_y;
        check(!event(step(mapper,p,air)),"opposite direction without full release cannot retrigger");
    }
}
void streets_rearm_takeoff_and_unsupported_attempts() {
    InputMapper mapper;ready(mapper,air);auto p=pad();p.left_shoulder=true;p.right_y=-1;
    check(step(mapper,p,air).handlebar==TrickDirection::left,"LB plus RS down requests authored left barspin34");
    p.right_y=0;step(mapper,p,air);p.right_y=-1;
    check(step(mapper,p,air).handlebar==TrickDirection::left,"centering RS rearms a second barspin while LB stays held");
    p.left_shoulder=false;step(mapper,p,air);p.left_shoulder=true;
    check(!event(step(mapper,p,air)),"modifier-only release with RS held cannot rearm");
    ready(mapper,air);
    p=pad();p.left_trigger=p.right_trigger=1;p.right_x=-1;
    check(step(mapper,p,air).tailwhip==TrickDirection::left,"both triggers plus RS left requests authored WhipLeft56");
    p.right_x=0;step(mapper,p,air);p.right_x=1;
    check(step(mapper,p,air).tailwhip==TrickDirection::right,"centering RS permits opposite whip while both triggers stay held");
    check(!event(step(mapper,p,air)),"held trigger pair and deflected RS never repeat a whip");
    ready(mapper,air);p=pad();p.left_shoulder=true;p.right_y=1;
    check(!event(step(mapper,p,air)),"unsupported LB plus RS up never aliases to a barspin");
    p.right_y=-1;
    check(!event(step(mapper,p,air)),"moving from unsupported to supported sector still requires full release");
    ready(mapper,air);p=pad();p.left_shoulder=p.right_shoulder=true;p.right_y=-1;
    check(!event(step(mapper,p,air)),"two shoulders selects its own unsupported set, never left barspin fallback");
    p.right_shoulder=false;
    check(!event(step(mapper,p,air)),"releasing one held modifier does not turn an old unsupported attempt into a new trick");
    ready(mapper,ground);p=pad();p.left_shoulder=true;p.right_y=-1;
    check(!event(step(mapper,p,ground)),"named air trick has no grounded request path");
    for(int i=0;i<10;++i)check(!event(step(mapper,p,air)),"held takeoff modifier+stick cannot synthesize a trick");
    p=pad();p.left_shoulder=true;
    step(mapper,p,air);p.right_y=-1;
    check(step(mapper,p,air).handlebar==TrickDirection::left,"fresh airborne RS neutral rearms held modifier after takeoff");
    ready(mapper,air);p=pad();p.right_shoulder=true;p.right_y=-1;
    check(step(mapper,p,air).handlebar==TrickDirection::right,"complete airborne release rearms authored right barspin44");
    check(!event(step(mapper,p,ground)) && !event(step(mapper,p,air)),"landing and retakeoff clear a held selector");
}
void selector_thresholds_and_no_legacy_arcs() {
    InputSettings cfg;cfg.deadzone=0;
    for(double radius:{.699,.7,.701}) {
        InputMapper mapper(cfg);ready(mapper,air);auto p=pad();p.left_shoulder=true;p.right_y=-radius;
        check((step(mapper,p,air).handlebar==TrickDirection::left)==(radius>=.7),
            "radius.7 threshold is tested on normalized selector input");
    }
    for(double trigger:{.2,.2001}) {
        InputMapper mapper(cfg);ready(mapper,air);auto p=pad();p.left_trigger=p.right_trigger=trigger;p.right_x=1;
        check((step(mapper,p,air).tailwhip==TrickDirection::right)==(trigger>.2),
            "trigger modifier threshold follows explicitly chosen strictgreater fallback convention");
    }
    // Wide native backward sector includes170/190deg; adjacent authored diagonals stayunsupported.
    for(double degrees:{145.,170.,190.,215.}) {
        InputMapper mapper;ready(mapper,air);auto p=pad();p.left_shoulder=true;
        p.right_x=std::sin(degrees*std::numbers::pi/180);p.right_y=std::cos(degrees*std::numbers::pi/180);
        check((step(mapper,p,air).handlebar==TrickDirection::left)==(degrees>150&&degrees<210),
            "named barspin uses actual backward sector, not a lateral flick");
    }
    for(bool front:{false,true}) {
        InputMapper mapper;ready(mapper,air);auto p=pad();
        for(const auto& v:std::array<std::array<double,2>,5>{{{0,-1},{1,0},{0,1},{-1,0},{0,-1}}}) {
            if(front){p.left_x=v[0];p.left_y=v[1];}else{p.right_x=v[0];p.right_y=v[1];}
            check(!event(step(mapper,p,air)),"unmodified full stick arcs and flicks never invoke legacy barspin/whip");
        }
    }
}
void modifier_mount_reset_and_invalid_gates() {
    for (int modifier=0;modifier<4;++modifier) {
        InputMapper mapper;auto p=pad();
        if(modifier==0)p.left_shoulder=true;
        if(modifier==1)p.right_shoulder=true;
        if(modifier==2)p.left_trigger=1;
        if(modifier==3)p.right_trigger=1;
        check(!step(mapper,p,air).active,"each held Streets modifier blocks activation until released");
        ready(mapper,air);p=pad();p.left_shoulder=true;p.right_y=-1;
        check(step(mapper,p,air).handlebar==TrickDirection::left,"released modifier arms a fresh named trick");
        mapper.reset();
        check(!step(mapper,p,air).active,"equipment reset cannot inherit a held named selection");
    }
    for(int reason=0;reason<6;++reason) {
        InputMapper mapper;ready(mapper,air);auto p=pad();p.left_shoulder=true;p.right_y=-1;
        if(reason==0)p.focused=false;
        if(reason==1)p.menu_open=true;
        if(reason==2)p.available=false;
        if(reason==3)++p.device;
        if(reason==4)p.left_trigger=std::numeric_limits<double>::quiet_NaN();
        if(reason==5)p.right_trigger=std::numeric_limits<double>::infinity();
        check(!step(mapper,p,air).active,"source/gate/nonfinite modifier suppresses the complete selection");
        p=pad();p.left_shoulder=true;p.right_y=-1;
        check(!step(mapper,p,air).active,"recovering a source still requires stick and modifiers released together");
        ready(mapper,air);
        check(step(mapper,p,air).handlebar==TrickDirection::left,"full release permits a genuinely new request after inhibition");
    }
}
void inhibition_device_change_and_invalid_samples() {
    for (int reason = 0; reason < 3; ++reason) {
        InputMapper mapper; ready(mapper, air);
        auto p = pad(); p.right_x = 1;
        if (reason == 0) p.focused = false;
        if (reason == 1) p.menu_open = true;
        if (reason == 2) p.available = false;
        check(!step(mapper, p, air).active, "focus/menu/device loss suppresses all actions");
        p = pad(); p.right_x = 1;
        check(!step(mapper, p, air).active, "held stick cannot reactivate after suppression");
        p.right_x = 0; p.pedal = 1;
        check(!step(mapper, p, air).active, "held pedal also blocks release-to-rearm");
        ready(mapper, air);
        p = pad(); p.right_x = 1; p.left_trigger=p.right_trigger=1;
        check(step(mapper, p, air).tailwhip == TrickDirection::right, "fresh named trick works after suppression and full release");
    }
    InputMapper mapper; ready(mapper, air);
    auto p = pad(); p.device = 2; p.left_x = 1;
    check(!step(mapper, p, air).active, "source identity change requires neutral");
    p.left_x = 0; check(step(mapper, p, air).active, "replacement source arms at neutral");
    p.right_y=-1;p.right_shoulder=true;check(step(mapper,p,air).handlebar==TrickDirection::right,"replacement source can make a fresh named selection");
    p = pad(); p.left_x = std::numeric_limits<double>::quiet_NaN();
    check(!step(mapper, p).active, "NaN axis suppresses and resets rather than becoming neutral");
    p = pad(); p.brake = std::numeric_limits<double>::infinity();
    check(!step(mapper, p).active, "infinite trigger suppresses and resets");
    for (double seconds : {0.0, -1.0, 1.0, std::numeric_limits<double>::quiet_NaN()}) {
        ready(mapper, air);
        check(!step(mapper, pad(), air, seconds).active, "invalid or stalled sample time suppresses interpretation");
        p = pad(); p.right_x = 1;
        check(!step(mapper, p, air).active, "invalid sample requires neutral rearm");
    }
}

void seconds_not_frame_counts() {
    for (double hz : {30.0, 60.0, 120.0}) {
        InputMapper mapper; ready(mapper);
        auto p = pad(); p.right_y = -1;
        for (int i = 0; i < static_cast<int>(hz * 0.5); ++i) step(mapper, p, ground, 1 / hz);
        p.right_y = 1;
        const auto a = step(mapper, p, ground, 1 / hz);
        check(a.hop_pressed && near(a.hop_strength, 1), "same simulated preload duration produces same hop across sample rates");
        ready(mapper, air);p=pad();p.left_shoulder=true;
        int requests=0;
        for(int i=1;i<=10;++i) {
            p.right_y=-i/10.;
            const auto selected=step(mapper,p,air,1/hz);
            if(selected.handlebar==TrickDirection::left)++requests;
        }
        check(requests==1,"a slow deliberate namedselection is positional rather than an invented fastflick timer");
    }
    InputSettings settings;
    settings.deadzone = 2; settings.neutral_threshold = -1;
    settings.trick_threshold = std::numeric_limits<double>::quiet_NaN();
    settings.modifier_threshold = std::numeric_limits<double>::quiet_NaN();
    InputMapper mapper(settings);
    check(near(mapper.settings().deadzone, 0.45) && near(mapper.settings().neutral_threshold, 0.05) &&
        near(mapper.settings().trick_threshold,.7) && near(mapper.settings().modifier_threshold,.2), "settings are bounded and nonfinite values use defaults");
    ready(mapper);
    mapper.configure({});
    auto p = pad(); p.right_y = -1;
    check(!step(mapper, p).active, "settings changes require fresh neutral rather than inheriting held gesture");
}
void quick_hop_edges_and_suppression() {
    InputMapper mapper; ready(mapper);
    auto p = pad(); p.quick_hop = true;
    auto action = step(mapper, p);
    check(action.hop_pressed && near(action.hop_strength, 1), "R3 requests a full-strength grounded hop");
    check(!step(mapper, p, air).hop_pressed && !step(mapper, p, ground).hop_pressed,
        "held R3 cannot retrigger through takeoff and landing");
    p.quick_hop = false; step(mapper, p, air);
    p.quick_hop = true;
    check(!step(mapper, p, air).hop_pressed && !step(mapper, p, ground).hop_pressed,
        "an airborne R3 press is not buffered into a landing hop");
    for (int reason = 0; reason < 5; ++reason) {
        ready(mapper); p = pad(); p.quick_hop = true;
        if (reason == 0) p.focused = false;
        if (reason == 1) p.available = false;
        if (reason == 2) p.menu_open = true;
        if (reason == 3) ++p.device;
        check(!step(mapper, p, ground, reason == 4 ? 0 : 1.0 / 60).active,
            "focus, menu, device or paused-time suppression rejects a held quick-hop button");
        p.focused = p.available = true; p.menu_open = false;
        check(!step(mapper, p).active, "R3 must be released to rearm after suppression");
        p.quick_hop = false;
        check(step(mapper, p).active, "released R3 rearms a neutral input source");
        p.quick_hop = true;
        check(step(mapper, p).hop_pressed, "a fresh R3 press works after suppression and release");
    }
}
void complete_authored_catalog() {
    // Independent transcription of the exact BMX table's row/slot identities.
    constexpr int slots[8][8]={{0,1,2,3,4,5,6,7},{10,11,12,13,14,15,16,17},
        {20,21,22,23,24,25,26,27},{255,31,32,33,34,35,36,37},
        {255,41,42,43,44,45,46,47},{50,51,52,53,54,55,56,57},
        {255,61,62,63,255,65,66,67},{255,71,72,73,255,75,76,77}};
    unsigned selections=0;
    for(unsigned set=0;set<8;++set)for(unsigned slot=0;slot<8;++slot) {
        InputMapper mapper;ready(mapper,air);auto p=pad();
        p.left_shoulder=set==0||set==2||set==3;
        p.right_shoulder=set==1||set==2||set==4;
        p.left_trigger=(set==1||set==5||set==6)?1:0;
        p.right_trigger=(set==0||set==5||set==7)?1:0;
        const double radians=slot*std::numbers::pi/4;
        p.right_x=std::sin(radians);p.right_y=std::cos(radians);
        const auto a=step(mapper,p,air);
        check(static_cast<int>(a.movement)==slots[set][slot],"every authored modifier/direction retains its own exact catalog identity");
        if(a.movement!=catalog::TrickId::none)++selections;
        check(step(mapper,p,air).movement==catalog::TrickId::none,"held named catalog slot cannot repeatedly trigger");
        p.right_x=p.right_y=0;step(mapper,p,air);
        p.right_x=std::sin(radians);p.right_y=std::cos(radians);
        check(static_cast<int>(step(mapper,p,air).movement)==slots[set][slot],"RS neutral rearms all catalog entries with modifiers held");
        p.menu_open=true;check(step(mapper,p,air).movement==catalog::TrickId::none,"menu capture cancels named movement input");
        p.menu_open=false;check(!step(mapper,p,air).active,"held catalog controls cannot bypass the all-neutral ownership reset gate");
    }
    check(selections==58,"all58 nonempty authored BMX bindings are reachable, including four shared animation references");
    for(int mask=0;mask<16;++mask) {
        InputMapper mapper;ready(mapper,air);auto p=pad();
        p.left_shoulder=(mask&1)!=0;p.right_shoulder=(mask&2)!=0;
        p.left_trigger=(mask&4)?1:0;p.right_trigger=(mask&8)?1:0;p.right_x=1;
        const int row=(mask&1)&&(mask&8)?0:(mask&2)&&(mask&4)?1:
            (mask&1)&&(mask&2)?2:(mask&1)?3:(mask&2)?4:
            (mask&4)&&(mask&8)?5:(mask&4)?6:(mask&8)?7:-1;
        const int expected=row<0?255:slots[row][2];
        check(static_cast<int>(step(mapper,p,air).movement)==expected,"all16 modifier combinations obey native priority without fallback to another trick");
    }
}
} // namespace

int main() {
    normalization_and_ownership();
    hop_preload_flick_and_latch();
    hop_cancellation_and_context();
    partial_stick_balance();
    streets_named_selector_and_modifier_priority();
    streets_rearm_takeoff_and_unsupported_attempts();
    selector_thresholds_and_no_legacy_arcs();
    modifier_mount_reset_and_invalid_gates();
    inhibition_device_change_and_invalid_samples();
    seconds_not_frame_counts();
    quick_hop_edges_and_suppression();
    streets_left_air_pitch_rearm_and_real_torque();
    streets_left_air_pitch_inhibition_and_arc();
    complete_authored_catalog();
    if (failures) std::printf("%d BMX input check(s) failed\n", failures);
    else std::printf("BMX input checks passed\n");
    return failures ? 1 : 0;
}
