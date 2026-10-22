#include "Extension/BMX/bmx_selftest.h"
#include "Engine/Game/BMX/bmx_controls.h"
#ifdef _WIN32
#include "Extension/BMX/bmx_catalog_test.h"
#endif
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
namespace rehearsal = dingosdk::bmx::selftest;
using namespace dingosdk::bmx;
void check(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
void input_policy() {
    rehearsal::Readiness ready{true, false, true, true, false, false, false, true};
    check(rehearsal::input_allowed(rehearsal::Mode::focused, false, ready),
        "ordinary focused riding retains its existing readiness contract");
    auto background = ready; background.focused = background.input_available = false;
    check(!rehearsal::input_allowed(rehearsal::Mode::focused, true, background),
        "a development root alone never relaxes ordinary focus/device guards");
    check(rehearsal::input_allowed(rehearsal::Mode::unattended, true, background),
        "explicit isolated unattended mode can supply synthetic background input");
    check(!rehearsal::input_allowed(rehearsal::Mode::unattended, false, ready) &&
        !rehearsal::input_allowed(rehearsal::Mode::unattended, false, background),
        "unattended input requires the validated isolated root even when focused");
    for (unsigned reason = 0; reason < 6; ++reason) {
        auto blocked = background;
        if (reason == 0) blocked.native_ui_available = false;
        if (reason == 1) blocked.native_menu = true;
        if (reason == 2) blocked.overlay_ready = false;
        if (reason == 3) blocked.overlay_menu = true;
        if (reason == 4) blocked.other_capture = true;
        if (reason == 5) blocked.menu_requested = true;
        check(!rehearsal::input_allowed(rehearsal::Mode::unattended, true, blocked),
            "background mode preserves every native menu and capture guard");
    }
    for (unsigned reason = 0; reason < 2; ++reason) {
        auto blocked = ready;
        if (reason == 0) blocked.focused = false;
        else blocked.input_available = false;
        check(!rehearsal::input_allowed(rehearsal::Mode::focused, true, blocked),
            "focused mode independently requires focus and a physical input source");
    }
}
void lifetime_and_sequence() {
    check(!rehearsal::expired(1000, 120999) && rehearsal::expired(1000, 121000) &&
        rehearsal::expired(1000, 999), "synthetic lease expires at 120 wall seconds or clock reversal");
    check(rehearsal::expired(std::numeric_limits<std::uint64_t>::max() - 10, 5),
        "clock wrap cannot turn an expired request into an unbounded lease");
    for (const double elapsed : {-1.0, 0.0, 9.0, 100.0,
        std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        const auto raw = rehearsal::controls(elapsed);
        check(raw.available && raw.focused && raw.device == rehearsal::synthetic_device &&
            raw.left_x == 0 && raw.left_y == 0 && raw.right_x == 0 && raw.right_y == 0 &&
            raw.pedal == 0 && raw.brake == 0, "invalid/outside sequence time produces only neutral input");
    }
    check(rehearsal::controls(1).pedal == .6 && rehearsal::controls(4).left_x == .25 &&
        rehearsal::controls(5).brake == 1 && rehearsal::controls(6).right_y == -.95 &&
        rehearsal::controls(6.35).right_y == .95 && rehearsal::controls(7.8).brake == 1,
        "script boundary transitions preserve pedal/steer/brake/hop/recovery order");
}
void signed_native_turn() {
    for (const double initial : {-3.13, -1.5, 0.0, 1.5, 3.13}) {
        rehearsal::RightTurn turn;
        turn.observe(3.9, initial + 1);
        check(!turn.started, "heading outside the right-stick phase cannot establish turn evidence");
        turn.observe(4, initial);
        check(turn.started && !turn.passed(), "one native heading is not a turn measurement");
        turn.observe(4.5, std::remainder(initial - .1, 6.2831853071795864769));
        turn.observe(5.49, std::remainder(initial - .2, 6.2831853071795864769));
        check(turn.passed() && std::abs(turn.delta + .2) < 1e-12,
            "physical right input must decrease native yaw, including across the heading wrap");
        turn.observe(5.5, initial + 2);
        check(turn.passed(), "later airborne/recovery headings cannot change the completed right-turn observation");
        rehearsal::RightTurn reversed;
        reversed.observe(4, initial); reversed.observe(5, initial + .2);
        check(!reversed.passed(), "large absolute steering cannot pass when physical right turns native left");
    }
    rehearsal::RightTurn stationary;
    stationary.observe(4, -.5); stationary.observe(5, -.5);
    check(!stationary.passed(), "pre-existing negative yaw is not movement during the right-stick phase");
    stationary.observe(std::numeric_limits<double>::quiet_NaN(), -1);
    stationary.observe(5, std::numeric_limits<double>::infinity());
    check(!stationary.passed(), "nonfinite heading/time cannot create turn evidence");
}
void distinct_air_gestures() {
    const auto hop = rehearsal::chassis_trick_controls(6.4);
    const auto neutral = rehearsal::chassis_trick_controls(6.48);
    const auto flick = rehearsal::chassis_trick_controls(6.52);
    const auto rearm = rehearsal::chassis_trick_controls(6.55);
    const auto whip = rehearsal::chassis_trick_controls(6.59);
    const auto pitch = rehearsal::chassis_trick_controls(6.67);
    const auto catch_input = rehearsal::chassis_trick_controls(6.8);
    check(hop.right_y == .95 && neutral.right_x == 0 && neutral.right_y == 0,
        "air gestures follow a distinct neutral sample after the hop release");
    check(flick.left_x == 0 && flick.right_x == 0 && flick.right_y == -.95 && flick.left_shoulder && !flick.right_shoulder,
        "Streets LB plus rear down requests the named left barspin after airborne rearming");
    check(rearm.right_y==0&&!rearm.left_shoulder&&rearm.left_trigger==0&&rearm.right_trigger==0&&
        whip.right_x==.95&&whip.left_trigger==1&&whip.right_trigger==1&&!whip.left_shoulder,
        "distinct neutral release separates the barspin from both-trigger rear-right tailwhip");
    check(rearm.left_x == 0 && rearm.left_y == 0 && pitch.left_x == 0 && pitch.left_y == .55 && pitch.right_y == 0 &&
        catch_input.left_x == 0 && catch_input.right_x == 0 && catch_input.right_y == 0,
        "Streets left-stick pitch rearms after lateral gestures and leaves the rear trick stick neutral");
    for (double time : {-1.,9.,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()}) {
        const auto raw = rehearsal::chassis_trick_controls(time);
        check(raw.left_x == 0 && raw.right_x == 0 && raw.right_y == 0 && raw.pedal == 0,
            "trick rehearsal never extends beyond its valid input lease");
    }
}
void generic_catalog_gesture() {
#ifdef _WIN32
    check(catalog_test::configuration_selected(true,L"bmx-user-data-chassis-probe",L"superman"),
        "generic native catalog rehearsal requires its exact explicit variant and private profile");
    for (const auto profile : {L"bmx-user-data",L"bmx-user-data-chassis-probe-other",L"",L"../bmx-user-data-chassis-probe"})
        check(!catalog_test::configuration_selected(true,profile,L"superman"),"another profile cannot select catalog input");
    for (const auto variant : {L"",L"Superman",L"superman-extra",L"barspin"})
        check(!catalog_test::configuration_selected(true,L"bmx-user-data-chassis-probe",variant),"unknown catalog input variant fails closed");
    check(!catalog_test::configuration_selected(false,L"bmx-user-data-chassis-probe",L"superman"),
        "environment alone cannot enable catalog input without development isolation");
#endif
    for (const unsigned fps : {30u,60u,120u,240u}) {
        InputMapper mapper;
        unsigned movements{}, hops{};
        bool selected{}, held_without_repeat{}, neutral_after{};
        for (unsigned tick=0;tick<fps*9;++tick) {
            const double time=static_cast<double>(tick)/fps;
            const auto raw=rehearsal::catalog_controls(time);
            const auto action=mapper.update(raw,{time<6.4},1.0/fps);
            if(action.hop_pressed)++hops;
            if(action.movement!=catalog::TrickId::none) {
                ++movements;selected=action.movement==catalog::TrickId::superman;
                check(time>=6.50&&time<6.75,"catalog slot is requested only inside the explicit air gesture window");
            } else if(time>6.55&&time<6.75) held_without_repeat=true;
            if(time>=6.75&&time<7.8) {
                neutral_after=true;
                check(raw.right_x==0&&raw.right_y==0&&raw.left_x==0&&raw.left_y==0&&
                    !raw.left_shoulder&&!raw.right_shoulder&&raw.left_trigger==0&&raw.right_trigger==0,
                    "catalog presentation returns all gesture controls neutral before landing/catch");
            }
            check(action.air_pitch==0,"generic catalog rehearsal never adds a separate physical pitch pulse");
            check(action.handlebar==TrickDirection::none&&action.tailwhip==TrickDirection::none,
                "Superman cannot accidentally request the legacy barspin or tailwhip channels");
        }
        check(hops==1&&movements==1&&selected&&held_without_repeat&&neutral_after,
            "real ordinary mapper produces one hop then one neutral-rearmed held Superman event at each supported rate");
    }
    for (const double time : {-1.,9.,100.,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()}) {
        const auto raw=rehearsal::catalog_controls(time);
        check(raw.available&&raw.focused&&raw.device==rehearsal::synthetic_device&&
            raw.left_x==0&&raw.left_y==0&&raw.right_x==0&&raw.right_y==0&&raw.pedal==0&&raw.brake==0&&
            !raw.left_shoulder&&!raw.right_shoulder&&raw.left_trigger==0&&raw.right_trigger==0,
            "catalog invalid/outside native time never extends its synthetic gesture");
    }
    const auto before=rehearsal::catalog_controls(6.49),start=rehearsal::catalog_controls(6.50),end=rehearsal::catalog_controls(6.75);
    check(before.right_x==0&&before.right_y==0&&start.right_x==-.95&&start.right_y==.95&&
        start.left_shoulder&&start.right_trigger==1&&end.right_x==0&&end.right_y==0&&!end.left_shoulder&&end.right_trigger==0,
        "catalog input interval includes its exact start and excludes its exact neutral recovery boundary");
}
void render_evidence() {
    const rehearsal::RenderEvidence valid{9, 100, 400, 399, 0x120000, 0x120000, 5000, 4900, .01f, .02f, true};
    auto result = rehearsal::assess_render(valid);
    check(result.passed() && result.minimum_poses == 135,
        "a current owned completed render snapshot with sustained writes can pass");
    auto sample = valid;
    sample.total_count = sample.snapshot_count = 235;
    check(rehearsal::assess_render(sample).passed(), "the minimum sustained density includes its exact boundary");
    sample.simulation_seconds = 9.0001;
    result = rehearsal::assess_render(sample);
    check(!result.passed() && result.minimum_poses == 136, "density scales with measured native time, rounding up");
    sample = valid; sample.total_count = sample.snapshot_count = 101;
    check(!rehearsal::assess_render(sample).density, "a single early render cannot pass a nine-second rehearsal");
    sample = valid; sample.observed_ms = sample.now_ms - rehearsal::render_fresh_ms;
    check(rehearsal::assess_render(sample).passed(), "the final completed render can be at the freshness boundary");
    --sample.observed_ms;
    check(!rehearsal::assess_render(sample).fresh, "many old successful writes cannot replace a fresh final render");
    sample = valid; ++sample.snapshot_entity;
    check(!rehearsal::assess_render(sample).current_owner, "another actor's fresh render cannot pass for this rider");
    sample = valid; sample.expected_entity = sample.snapshot_entity = 0;
    check(!rehearsal::assess_render(sample).current_owner, "an empty owner is not a matching native rider");
    sample = valid; sample.ready = false;
    check(!rehearsal::assess_render(sample).passed(), "inactive/owner-change invalidation rejects otherwise plausible metrics");
    sample = valid; sample.observed_ms = sample.now_ms + 1;
    check(!rehearsal::assess_render(sample).fresh, "future timestamps cannot underflow the freshness age");
    sample.observed_ms = 0;
    check(!rehearsal::assess_render(sample).fresh, "an unobserved render timestamp cannot pass");
    sample = valid; sample.snapshot_count = sample.baseline_count;
    check(!rehearsal::assess_render(sample).coherent_count, "pre-request writes do not count toward this rehearsal");
    --sample.snapshot_count;
    check(!rehearsal::assess_render(sample).density, "a reset/wrapped counter cannot underflow into sufficient density");
    sample = valid; sample.snapshot_count = sample.total_count + 1;
    check(!rehearsal::assess_render(sample).coherent_count, "a snapshot cannot claim a write beyond the observed global count");
    for (const double seconds : {-1.0, 0.0, 8.99, 61.0, std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity()}) {
        sample = valid; sample.simulation_seconds = seconds;
        check(!rehearsal::assess_render(sample).passed(), "incomplete/nonfinite/out-of-lease native time cannot pass density");
    }
    for (const float contact_error : {-.01f, .18f, std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity()}) {
        sample = valid; sample.hand_error = contact_error;
        check(!rehearsal::assess_render(sample).contacts, "invalid or unsolved hand contact cannot pass");
        sample = valid; sample.foot_error = contact_error;
        check(!rehearsal::assess_render(sample).contacts, "invalid or unsolved foot contact cannot pass");
    }
}
void model_rehearsal(unsigned fps) {
    InputMapper mapper;
    Tuning tuning; tuning.forward_limit = 6; tuning.reverse_limit = 3;
    Model bike(tuning);
    const Contacts floor{{QueryState::hit, 0}, {QueryState::hit, 0}};
    const double seconds = 1.0 / fps;
    unsigned hops{};
    double height{}, turn{}, before_brake{}, after_brake = 1000;
    rehearsal::RightTurn right_turn;
    for (unsigned tick = 0; tick < fps * 9; ++tick) {
        const double elapsed = static_cast<double>(tick) / fps;
        const auto actions = mapper.update(rehearsal::controls(elapsed), {bike.state().grounded}, seconds);
        const auto result = bike.advance(seconds, riding_command(actions), floor);
        if (result.hopped) ++hops;
        height = std::max(height, bike.state().y);
        turn = std::max(turn, std::abs(bike.state().yaw));
        right_turn.observe(elapsed, bike.state().yaw);
        if (elapsed >= 4.5 && elapsed < 5) before_brake = std::max(before_brake, bike.state().speed);
        if (elapsed >= 5.8 && elapsed < 6) after_brake = std::min(after_brake, bike.state().speed);
    }
    check(std::hypot(bike.state().x, bike.state().z) > 1 && turn > .01,
        "rehearsal really pedals and steers through the ordinary mapper/model");
    check(right_turn.passed(), "the rehearsal's positive right-stick phase decreases actual model yaw at every sample rate");
    check(hops == 1 && height > .1, "rehearsal produces exactly one grounded hop at each supported sample rate");
    check(before_brake > .5 && after_brake < before_brake * .7 && std::abs(bike.state().speed) < .1,
        "rehearsal brakes and ends stationary through the ordinary model");
}
} // namespace

void actual_chassis_heading_qualification() {
    const auto q=[](double yaw){return std::array<float,4>{0,static_cast<float>(std::sin(yaw/2)),0,static_cast<float>(std::cos(yaw/2))};};
    for(double initial:{-3.13,-1.5,0.,1.5,3.13}) {
        rehearsal::ObservedBikeTurn turn;turn.observe(3.9,1,q(initial));turn.observe(4,2,q(initial));
        turn.observe(4.5,3,q(initial-.1));turn.observe(5.49,4,q(initial-.2));
        check(turn.passed()&&std::abs(turn.right_turn.delta+.2)<1e-6,"actual projected bike +Z heading retains signed-right and wrap semantics");
        turn.observe(5.5,5,q(initial+1));check(turn.passed(),"actual bike right-turn phase remains4..5.5 seconds");
    }
    for(unsigned reason=0;reason<10;++reason) {
        rehearsal::ObservedBikeTurn turn;turn.observe(4,1,q(0));auto sample=q(-.2);
        if(reason==0)sample={0,0,0,0};
        if(reason==1)sample={std::numeric_limits<float>::quiet_NaN(),0,0,1};
        if(reason==2)sample={1,0,0,0}; // upside-down
        if(reason==3)sample={.70710678f,0,0,.70710678f}; // forward vertical
        if(reason==4)for(auto& value:sample)value*=2;
        if(reason==5)sample=q(.2); // wrong sign
        if(reason==6)sample=q(0); // stationary
        if(reason==7)sample=q(-.005); // below original .01 threshold
        turn.observe(5,reason==8?1:reason==9?3:2,sample);
        check(!turn.passed(),"invalid/inverted/nearvertical/nonunit/wrongsign/stationary/tiny/stale/gapped native bike heading cannot manufacture a right turn");
        if(reason<5){turn.observe(5.4,3,q(-.3));check(!turn.passed(),"a later valid quaternion cannot erase a failed turn-window qualification");}
    }
    rehearsal::ObservedBikeTurn sign;auto a=q(.4),b=q(.4);for(auto& value:b)value=-value;
    sign.observe(4,1,a);sign.observe(5,2,b);check(!sign.passed(),"quaternion sign change is the same orientation and creates no turn");
}
int main() {
    actual_chassis_heading_qualification();
    input_policy(); lifetime_and_sequence(); signed_native_turn(); distinct_air_gestures(); generic_catalog_gesture(); render_evidence();
    for (const unsigned fps : {30u, 60u, 120u, 240u}) model_rehearsal(fps);
    std::cout << "BMX selftest policy and native-time control sequence tests passed.\n";
}
