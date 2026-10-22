#include "Engine/Game/BMX/bmx_controls.h"
#include "Engine/Game/Input/bmx_input_sample.h"
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
using namespace dingosdk::bmx;
constexpr Contacts flat{{QueryState::hit, 0}, {QueryState::hit, 0}};
constexpr double dt = 1.0 / 240.0;
void check(bool valid, const char* message) {
    if (!valid) { std::cerr << message << '\n'; std::exit(1); }
}
struct Ride {
    InputMapper input;
    Model model;
    RawInput pad;
    Ride() { pad.available = true; sample(); }
    Actions sample(double seconds = dt) {
        const auto actions = input.update(pad, {model.state().grounded}, seconds);
        model.advance(seconds, riding_command(actions), flat);
        return actions;
    }
    void center() {
        pad.left_x = pad.left_y = pad.right_x = pad.right_y = pad.pedal = pad.brake = 0;
        pad.left_trigger=pad.right_trigger=0;pad.left_shoulder=pad.right_shoulder=false;
        sample(); sample();
    }
};
}

int main() {
    // A rear-stick hop survives the 240 Hz input -> 120 Hz physics boundary exactly once.
    {
        Ride ride;
        ride.center();
        ride.pad.right_y = -1;
        for (int i = 0; i < 60; ++i) ride.sample();
        ride.pad.right_y = 1;
        const auto hop = ride.sample();
        check(hop.hop_pressed, "Rear-stick down/up did not produce a hop event");
        ride.sample();
        check(!ride.model.state().grounded && ride.model.state().vertical_speed > 0,
            "High-frequency rear-stick hop did not reach physics");
        double highest = ride.model.state().y;
        bool landed = false;
        for (int i = 0; i < 360; ++i) {
            const auto action = ride.sample();
            check(!action.hop_pressed, "Held rear stick repeated the hop");
            highest = std::max(highest, ride.model.state().y);
            landed = landed || ride.model.state().grounded;
        }
        check(highest > 0.3 && landed && ride.model.state().grounded, "Hop failed its flat-floor landing cycle");
    }
    // Suppression cancels a charged flick instead of launching when the window regains focus.
    {
        Ride ride;
        ride.center();
        ride.pad.right_y = -1;
        for (int i = 0; i < 20; ++i) ride.sample();
        ride.pad.focused = false;
        check(!ride.sample().active, "Unfocused input remained active");
        ride.pad.focused = true;
        ride.pad.right_y = 1;
        for (int i = 0; i < 40; ++i) check(!ride.sample().active, "Held input rearmed after focus loss");
        check(ride.model.state().grounded && std::abs(ride.model.state().vertical_speed) < 1e-12,
            "Suppressed flick changed bike height");
        ride.center();
        ride.pad.right_y = -1;
        for (int i = 0; i < 20; ++i) ride.sample();
        ride.pad.right_y = 1;
        check(ride.sample().hop_pressed, "Fresh flick after release failed to rearm");
    }
    // Once a hop is emitted but before a fixed tick consumes it, every inhibition path cancels it.
    for (int reason = 0; reason < 4; ++reason) {
        InputMapper mapper;
        RawInput pad;
        pad.available = true;
        mapper.update(pad, {true}, dt);
        pad.right_y = -1;
        for (int i = 0; i < 60; ++i) mapper.update(pad, {true}, dt);
        pad.right_y = 1;
        const auto hop = mapper.update(pad, {true}, dt);
        check(hop.hop_pressed, "Pending-hop inhibition setup did not produce an event");
        Model model;
        model.advance(fixed_seconds, {}, flat);
        check(model.advance(fixed_seconds * 0.5, riding_command(hop), flat).steps == 0,
            "Pending-hop inhibition setup unexpectedly stepped");
        if (reason == 0) pad.focused = false;
        if (reason == 1) pad.menu_open = true;
        if (reason == 2) pad.available = false;
        if (reason == 3) ++pad.device;
        const auto inhibited = mapper.update(pad, {true}, dt);
        check(!inhibited.active, "Inhibition path failed to suppress mapper");
        const auto result = model.advance(fixed_seconds * 0.5, riding_command(inhibited), flat);
        check(!result.hopped && model.state().grounded && model.state().vertical_speed == 0,
            "Queued hop survived focus/menu/device suppression before its physics step");
    }
    // Sticks have separate front/rear ownership; airborne trick events grant no new propulsion.
    {
        InputMapper mapper;
        RawInput pad;
        pad.available = true;
        mapper.update(pad, {false}, dt);
        pad.right_x = 1;
        pad.left_trigger=pad.right_trigger=1;
        pad.pedal = 1;
        const auto rear = mapper.update(pad, {false}, dt);
        check(rear.tailwhip == TrickDirection::right && rear.handlebar == TrickDirection::none,
            "Streets trigger-pair rear-right tailwhip crossed into handlebar control");
        check(rear.steer == 0 && !rear.hop_pressed && riding_command(rear).pedal == 0,
            "Airborne rear gesture steered, hopped or added propulsion");
        pad.right_x = pad.pedal = 0;
        pad.left_trigger=pad.right_trigger=0;
        mapper.update(pad, {false}, dt);
        pad.left_shoulder=true;pad.right_y=-1;
        const auto front = mapper.update(pad, {false}, dt);
        check(front.handlebar == TrickDirection::left && front.tailwhip == TrickDirection::none,
            "Streets left-shoulder rear-down barspin crossed into tailwhip control");
    }
    // A takeoff cannot turn pre-held steering into a fresh barspin.
    {
        Ride ride;
        ride.center();
        ride.pad.left_x = 0.8;
        ride.pad.right_y = -1;
        for (int i = 0; i < 60; ++i) ride.sample();
        ride.pad.right_y = 1;
        ride.sample(); ride.sample();
        check(!ride.model.state().grounded, "Pre-held-steering setup did not take off");
        for (int i = 0; i < 25; ++i)
            check(ride.sample().handlebar == TrickDirection::none, "Takeoff converted steering hold to barspin");
        ride.pad.left_x = ride.pad.right_y = 0;
        ride.sample();
        ride.pad.left_x = 1;
        check(ride.sample().handlebar == TrickDirection::none, "Left steering never becomes a barspin in the Streets layout");
        ride.pad.right_shoulder=true;ride.pad.right_y=-1;
        check(ride.sample().handlebar == TrickDirection::right, "Fresh Streets modifier and rear-stick direction did not request barspin");
    }
    // Handlebar steering drives the simple supported model; rear input cannot secretly steer it.
    {
        Ride front, rear;
        front.center(); rear.center();
        front.pad.pedal = rear.pad.pedal = 1;
        front.pad.left_x = 0.5;
        rear.pad.right_x = 0.5;
        for (int i = 0; i < 480; ++i) { front.sample(); rear.sample(); }
        check(front.model.state().yaw < -0.1, "Rider-right front stick did not decrease native forward yaw");
        check(std::abs(rear.model.state().yaw) < 1e-12, "Rear stick steered bike in the riding model");
    }
    // The native transform uses forward={sin(yaw),0,cos(yaw)}. A camera following
    // behind that forward direction has right={-forward.z,0,forward.x}, matching
    // Hud/follow_camera.cpp. Verify physical stick direction in that view, not by
    // calling world +X "right" or merely checking that some yaw changed.
    for (const double initial_yaw : {-2.4, -.7, 0.0, .8, 2.5}) {
        for (const int side : {-1, 1}) {
            InputMapper mapper;
            RawInput pad; pad.available = true;
            mapper.update(pad, {true}, fixed_seconds);
            const auto axes = dingosdk::input::bmx_pad_axes(
                side < 0 ? std::int16_t{-32768} : std::int16_t{32767}, 0, 0, 0, 0);
            pad.left_x = axes.left_x;
            Tuning tuning; tuning.rolling_drag = 0;
            Model bike(tuning), fakie(tuning);
            State forward; forward.yaw = initial_yaw; forward.speed = 4; forward.grounded = true;
            bike.reset(forward); forward.speed = -4; fakie.reset(forward);
            for (unsigned tick = 0; tick < 60; ++tick) {
                const auto actions = mapper.update(pad, {true}, fixed_seconds);
                check(actions.steer * side > 0, "Physical left/right input was inverted before the model");
                bike.advance(fixed_seconds, riding_command(actions), flat);
                fakie.advance(fixed_seconds, riding_command(actions), flat);
            }
            const double camera_right_x = -std::cos(initial_yaw), camera_right_z = std::sin(initial_yaw);
            const double forward_x = std::sin(initial_yaw), forward_z = std::cos(initial_yaw);
            const auto projected_turn = [&](const State& state) {
                return (std::sin(state.yaw) - forward_x) * camera_right_x +
                    (std::cos(state.yaw) - forward_z) * camera_right_z;
            };
            check(projected_turn(bike.state()) * side > .1,
                "Physical stick right/left must turn right/left in the native following camera");
            check((bike.state().x * camera_right_x + bike.state().z * camera_right_z) * side > .1,
                "Forward riding must move into the same camera-side as the physical steering input");
            check(projected_turn(fakie.state()) * side < -.1,
                "The same handlebar direction must reverse native yaw while rolling fakie");
        }
    }
    // R3 uses the same ordinary fixed-step hop event, with no repeated or airborne impulse.
    {
        Ride ride; ride.center();
        ride.pad.quick_hop = true;
        const auto pressed = ride.sample();
        check(pressed.hop_pressed && pressed.hop_strength == 1, "R3 press did not request a full grounded quick hop");
        ride.sample();
        check(!ride.model.state().grounded, "R3 event was lost at the high-frequency fixed-step boundary");
        for (unsigned tick = 0; tick < 360; ++tick)
            check(!ride.sample().hop_pressed, "Held R3 repeated a hop through takeoff or landing");
        check(ride.model.state().grounded, "R3 hop did not land");
        ride.pad.quick_hop = false; ride.sample();
        ride.pad.quick_hop = true;
        check(ride.sample().hop_pressed, "R3 did not rearm after its actual release");
    }
    std::cout << "BMX controls/model integration regressions passed.\n";
}
