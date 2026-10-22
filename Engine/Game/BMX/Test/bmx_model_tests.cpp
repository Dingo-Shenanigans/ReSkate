#include "Engine/Game/BMX/bmx_model.h"
#include <cmath>
#include <cstdio>
#include <limits>

namespace {
using namespace dingosdk::bmx;
int failures{};
void check(bool condition, const char* message) {
    if (!condition) { std::printf("FAIL: %s\n", message); ++failures; }
}
bool near(double a, double b, double epsilon = 1e-9) { return std::abs(a - b) < epsilon; }
bool finite_state(const State& s) {
    return std::isfinite(s.x) && std::isfinite(s.y) && std::isfinite(s.z) && std::isfinite(s.yaw) &&
        std::isfinite(s.speed) && std::isfinite(s.vertical_speed) && std::isfinite(s.steering_angle) &&
        std::isfinite(s.front_wheel_phase) && std::isfinite(s.rear_wheel_phase) && std::isfinite(s.crank_phase) &&
        std::isfinite(s.travel_yaw);
}
Contacts floor_at(double height = 0.0) { return {{QueryState::hit, height}, {QueryState::hit, height}}; }
Contacts clear_air() { return {{QueryState::clear, 0.0}, {QueryState::clear, 0.0}}; }
void run(Model& model, double seconds, unsigned fps, Command command = {}, Contacts contacts = floor_at()) {
    const auto frames = static_cast<unsigned>(std::round(seconds * fps));
    for (unsigned i = 0; i < frames; ++i) model.advance(1.0 / fps, command, contacts);
}
Tuning no_drag() { Tuning t; t.air_drag = t.rolling_drag = 0.0; return t; }
}

int main() {
    {
        Model a, b, c;
        const Command pedal{0.2, 0.8, 0.0, false};
        run(a, 5.0, 30, pedal); run(b, 5.0, 60, pedal); run(c, 5.0, 144, pedal);
        check(near(a.state().speed, b.state().speed) && near(a.state().x, c.state().x) &&
            near(a.state().z, c.state().z) && near(a.state().yaw, c.state().yaw),
            "fixed simulation gives the same ride at 30, 60 and 144 render FPS");
        const State before = a.state();
        for (unsigned i = 0; i < 100; ++i) a.advance(0.0, {1.0, 1.0, 0.0, true}, floor_at());
        check(near(a.state().speed, before.speed) && near(a.state().z, before.z) && near(a.state().y, before.y),
            "paused simulation time does not move or accelerate the bike");
        check(!a.advance(fixed_seconds, {0.0, 0.0, 0.0, true}, floor_at()).hopped,
            "a held hop during pause does not fire on resume");
        Model rebased;
        check(rebased.advance(fixed_seconds * 0.6, {}, floor_at()).steps == 0,
            "a partial step waits for more simulation time");
        rebased.advance(0.0, {}, floor_at());
        check(rebased.advance(fixed_seconds * 0.6, {}, floor_at()).steps == 0,
            "paused time discards the old partial step rather than triggering an early resumed tick");
        rebased.advance(-1.0, {}, floor_at());
        check(rebased.advance(fixed_seconds * 0.6, {}, floor_at()).steps == 0,
            "invalid negative time also rebases a pending partial step");
    }
    {
        Model model;
        const auto first = model.advance(fixed_seconds * 0.25, {0.0, 0.0, 0.0, true}, floor_at());
        const auto second = model.advance(fixed_seconds * 0.25, {}, floor_at());
        const auto third = model.advance(fixed_seconds * 0.5, {}, floor_at());
        check(first.steps == 0 && second.steps == 0 && third.steps == 1 && third.hopped,
            "a short hop event survives zero-step render frames until the next fixed step");
        check(!model.advance(fixed_seconds, {}, floor_at()).hopped,
            "a latched short event is consumed only once");
        Model weak, strong;
        weak.advance(fixed_seconds * 0.5, {0.0, 0.0, 0.0, true, 0.5}, floor_at());
        const auto weak_event = weak.advance(fixed_seconds * 0.5, {}, floor_at());
        strong.advance(fixed_seconds, {0.0, 0.0, 0.0, true, 1.0}, floor_at());
        check(weak_event.hopped && near(weak.state().vertical_speed,
            weak.tuning().hop_speed * 0.5 - weak.tuning().gravity * fixed_seconds),
            "a queued hop retains its original strength across a zero-step release frame");
        check(strong.state().vertical_speed > weak.state().vertical_speed,
            "greater hop strength produces a greater takeoff speed");
        Model zero;
        check(!zero.advance(fixed_seconds, {0.0, 0.0, 0.0, true, 0.0}, floor_at()).hopped && zero.state().grounded,
            "zero-strength hop is consumed without a jump");
    }
    {
        Model model;
        model.advance(fixed_seconds, {}, floor_at());
        model.advance(fixed_seconds * 0.5, {0.0, 0.0, 0.0, true}, floor_at());
        Command inhibited{1.0, 1.0, 1.0, true, 1.0, false};
        const auto result = model.advance(fixed_seconds * 0.5, inhibited, floor_at());
        check(result.steps == 1 && !result.hopped && model.state().grounded && near(model.state().vertical_speed, 0.0),
            "input suppression cancels a hop queued before the next physics step");
        check(!model.advance(fixed_seconds, {}, floor_at()).hopped,
            "canceled hop strength/event cannot escape on a later active neutral frame");
        Model coast, ignored;
        State moving; moving.speed = 4.0; coast.reset(moving); ignored.reset(moving);
        run(coast, 0.5, 60);
        run(ignored, 0.5, 60, inhibited);
        check(near(coast.state().speed, ignored.state().speed) && near(coast.state().z, ignored.state().z) &&
            near(coast.state().yaw, ignored.state().yaw) && ignored.state().speed > 0.0,
            "suppressed pedal/brake/steer inputs leave the same passive coasting as active neutral input");
        State airborne; airborne.y = 2.0; ignored.reset(airborne);
        run(ignored, 0.1, 60, inhibited, {{QueryState::clear, 0.0}, {QueryState::clear, 0.0}});
        check(ignored.state().y < 2.0 && ignored.state().vertical_speed < 0.0,
            "input suppression permits passive gravity instead of freezing the world");
    }
    {
        Model model;
        run(model, 2.0, 60, {0.0, 1.0, 0.0, false});
        check(model.state().speed > 5.0 && model.state().z > 4.0, "pedaling builds forward speed and distance");
        const double speed = model.state().speed;
        const double crank = model.state().crank_phase;
        run(model, 0.5, 60);
        check(model.state().speed > 0.0 && model.state().speed < speed, "coasting loses speed gradually");
        check(near(model.state().crank_phase, crank), "coasting does not turn the cranks");
        check(model.state().front_wheel_phase > 0.0 && near(model.state().front_wheel_phase, model.state().rear_wheel_phase),
            "rolling animates both wheel phases");
        run(model, 3.0, 60, {0.0, 0.0, 1.0, false});
        check(near(model.state().speed, 0.0), "brakes stop the bike without making it reverse");
        const double stopped = model.state().z;
        run(model, 2.0, 60, {0.0, 0.0, 1.0, false});
        check(near(model.state().z, stopped) && near(model.state().speed, 0.0), "braking at rest remains at rest");
    }
    {
        Model forward(no_drag()), fakie(no_drag());
        State f; f.speed = 4.0; forward.reset(f);
        State r; r.speed = -4.0; fakie.reset(r);
        run(forward, 0.5, 60, {0.5, 0.0, 0.0, false});
        run(fakie, 0.5, 60, {0.5, 0.0, 0.0, false});
        check(forward.state().z > 0.0 && fakie.state().z < 0.0, "signed rolling speed preserves forward and fakie travel");
        check(forward.state().yaw < 0.0 && fakie.state().yaw > 0.0,
            "rider-right steering decreases native forward yaw and reverses yaw while rolling fakie");
        run(fakie, 1.0, 60, {0.0, 0.0, 1.0, false});
        check(near(fakie.state().speed, 0.0), "braking fakie stops without pushing forward");
        Model slow(no_drag()), fast(no_drag());
        f.speed = 1.0; slow.reset(f); f.speed = 12.0; fast.reset(f);
        run(slow, 1.0, 60, {1.0, 0.0, 0.0, false}); run(fast, 1.0, 60, {1.0, 0.0, 0.0, false});
        check(std::abs(slow.state().steering_angle) > std::abs(fast.state().steering_angle),
            "handlebar travel decreases as riding speed rises");
    }
    {
        Model still(no_drag()), right(no_drag()), left(no_drag());
        State airborne; airborne.y = 10.0; airborne.yaw = 0.7; airborne.speed = 4.0;
        airborne.travel_yaw = -2.0;
        still.reset(airborne); right.reset(airborne); left.reset(airborne);
        Command spin; spin.air_spin = 1.0; spin.steer = 1.0;
        run(still, 0.5, 60, {}, clear_air()); run(right, 0.5, 60, spin, clear_air());
        spin.air_spin = -1.0; run(left, 0.5, 60, spin, clear_air());
        check(near(still.state().travel_yaw, airborne.yaw) && near(right.state().travel_yaw, airborne.yaw),
            "reset initializes momentum bearing from facing rather than an incoming stale travel bearing");
        check(near(right.state().x, still.state().x) && near(right.state().z, still.state().z) &&
            near(left.state().x, still.state().x) && near(left.state().z, still.state().z) &&
            near(still.state().x, std::sin(airborne.yaw) * 2.0) &&
            near(still.state().z, std::cos(airborne.yaw) * 2.0),
            "air spins and held steering never curve the takeoff momentum path");
        check(near(model_detail::angle(right.state().yaw - airborne.yaw), model_detail::angle(-4.0)) &&
            near(model_detail::angle(left.state().yaw - airborne.yaw), model_detail::angle(4.0)),
            "positive rider-right air input decreases actual native yaw and left reverses it");
        Model a(no_drag()), b(no_drag()), c(no_drag());
        a.reset(airborne); b.reset(airborne); c.reset(airborne); spin.air_spin = 0.5;
        run(a, 0.5, 30, spin, clear_air()); run(b, 0.5, 60, spin, clear_air());
        run(c, 0.5, 144, spin, clear_air());
        check(near(a.state().yaw, b.state().yaw) && near(a.state().yaw, c.state().yaw) &&
            near(a.state().x, c.state().x) && near(a.state().y, c.state().y) && near(a.state().z, c.state().z),
            "air rotation and momentum are invariant at 30,60 and144 render FPS");
    }
    {
        Tuning tuning = no_drag(); tuning.hop_speed = 4.2;
        Model forward(tuning), fakie(tuning);
        State moving; moving.speed = 4.0; forward.reset(moving);
        moving.speed = -4.0; fakie.reset(moving);
        Command hop; hop.hop = true;
        check(forward.advance(fixed_seconds, hop, floor_at()).hopped &&
            fakie.advance(fixed_seconds, hop, floor_at()).hopped, "both forward and fakie can take off");
        Command spin; spin.air_spin = pi / (tuning.air_spin_rate * 0.4);
        run(forward, 0.4, 120, spin); run(fakie, 0.4, 120, spin);
        check(near(forward.state().yaw, -pi) && near(forward.state().travel_yaw, 0.0) &&
            near(forward.state().x, 0.0) && forward.state().z > 1.6 &&
            near(fakie.state().x, 0.0) && fakie.state().z < -1.6,
            "a real180 rotates facing while both signed takeoff paths remain straight");
        unsigned remaining = 240;
        while ((!forward.state().grounded || !fakie.state().grounded) && remaining-- > 0) {
            forward.advance(fixed_seconds, {}, floor_at()); fakie.advance(fixed_seconds, {}, floor_at());
        }
        check(forward.state().grounded && fakie.state().grounded &&
            near(forward.state().speed, -4.0) && near(fakie.state().speed, 4.0),
            "landing a forward180 produces fakie and a fakie180 produces forward rolling");
        check(near(forward.state().travel_yaw, forward.state().yaw) &&
            near(fakie.state().travel_yaw, fakie.state().yaw), "landing rebases the signed travel bearing once");
        const State f = forward.state(), r = fakie.state();
        forward.advance(fixed_seconds, {}, floor_at()); fakie.advance(fixed_seconds, {}, floor_at());
        check(forward.state().z > f.z && fakie.state().z < r.z &&
            near(forward.state().speed, -4.0) && near(fakie.state().speed, 4.0),
            "postlanding ground motion preserves world momentum without projecting speed again");
    }
    {
        for (double rotation : {pi / 3.0, pi / 2.0, pi}) {
            Model model(no_drag()); State moving; moving.y = 10.0; moving.speed = 4.0;
            model.reset(moving); Command spin; spin.air_spin = rotation / (model.tuning().air_spin_rate * 0.4);
            run(model, 0.4, 120, spin, clear_air());
            check(model.reconcile_position(model.state().x, 0.0, model.state().z),
                "native position reconciliation accepts a verified touchdown");
            model.advance(fixed_seconds, {}, floor_at());
            check(model.state().grounded && near(model.state().speed, 4.0 * std::cos(rotation)) &&
                near(model.state().travel_yaw, model.state().yaw),
                "reconciled landing projects momentum onto facing including sideways and fakie outcomes");
        }
        Model edge(no_drag()); State moving; moving.yaw = 1.1; moving.speed = -3.0; edge.reset(moving);
        run(edge, 0.2, 120, {0.5, 0.0, 0.0, false});
        const State departure = edge.state(); Command spin; spin.air_spin = 1.0;
        run(edge, 0.1, 120, spin, clear_air());
        check(near(edge.state().travel_yaw, departure.yaw) &&
            near(edge.state().x - departure.x, std::sin(departure.yaw) * -0.3) &&
            near(edge.state().z - departure.z, std::cos(departure.yaw) * -0.3),
            "leaving an edge pins current steering heading and retains negative fakie momentum");
        Model grounded_a(no_drag()), grounded_b(no_drag()); moving.yaw = 0.0; moving.speed = 3.0;
        grounded_a.reset(moving); grounded_b.reset(moving);
        Command turn{0.5, 0.0, 0.0, false}; run(grounded_a, 0.5, 60, turn);
        turn.air_spin = 1.0; run(grounded_b, 0.5, 60, turn);
        check(near(grounded_a.state().yaw, grounded_b.state().yaw) &&
            near(grounded_a.state().x, grounded_b.state().x) && near(grounded_a.state().z, grounded_b.state().z),
            "air spin input leaves existing grounded steering and movement unchanged");
    }
    {
        Model model(no_drag()); State airborne; airborne.y = 10.0; airborne.speed = 4.0; model.reset(airborne);
        Command spin; spin.air_spin = 1.0;
        model.advance(fixed_seconds * 0.6, spin, clear_air());
        model.advance(0.0, spin, clear_air());
        check(model.advance(fixed_seconds * 0.6, spin, clear_air()).steps == 0 && near(model.state().yaw, 0.0),
            "paused air time rebases partial ticks without rotating on resume");
        model.advance(-1.0, spin, clear_air());
        model.advance(fixed_seconds, spin, {{QueryState::unavailable, 0.0}, {QueryState::clear, 0.0}});
        check(near(model.state().yaw, 0.0) && near(model.state().travel_yaw, 0.0),
            "unavailable contacts safely freeze facing and momentum heading");
        spin.input_enabled = false; model.advance(fixed_seconds, spin, clear_air());
        check(near(model.state().yaw, 0.0) && model.state().y < airborne.y && model.state().z > 0.0,
            "suppressed air spin permits passive flight while ignoring rotation input");
        spin.input_enabled = true; const auto stalled = model.advance(1000.0, spin, clear_air());
        check(stalled.steps == max_steps_per_advance && near(model.state().yaw, -0.8) &&
            stalled.dropped_seconds > 999.0, "air rotation is bounded by accepted fixed ticks during a stall");
        const double yaw = model.state().yaw;
        spin.air_spin = std::numeric_limits<double>::infinity(); model.advance(fixed_seconds, spin, clear_air());
        check(near(model.state().yaw, yaw) && finite_state(model.state()), "nonfinite air input is neutral and state remains finite");
        Tuning tuning = no_drag(); tuning.air_spin_rate = 1000.0; Model capped(tuning); capped.reset(airborne);
        spin.air_spin = 1000.0; capped.advance(fixed_seconds, spin, clear_air());
        check(near(capped.tuning().air_spin_rate, 12.0) && near(capped.state().yaw, -0.1),
            "air rate and command are capped independently");
        tuning.air_spin_rate = std::numeric_limits<double>::quiet_NaN(); capped.set_tuning(tuning);
        check(near(capped.tuning().air_spin_rate, 8.0), "nonfinite air rate restores its finite default");
    }
    {
        Model model;
        unsigned hops{};
        double peak{};
        for (unsigned i = 0; i < 180; ++i) {
            hops += model.advance(1.0 / 60.0, {0.0, 0.0, 0.0, true}, floor_at()).hopped ? 1U : 0U;
            peak = std::max(peak, model.state().y);
        }
        check(hops == 1 && peak > 0.4 && peak < 0.6 && model.state().grounded,
            "one held hop has a bounded arc, lands and never repeats");
        model.advance(fixed_seconds, {}, floor_at());
        check(model.advance(fixed_seconds, {0.0, 0.0, 0.0, true}, floor_at()).hopped,
            "release and press permits another grounded hop");
        model.advance(fixed_seconds, {}, floor_at());
        check(!model.advance(fixed_seconds, {0.0, 0.0, 0.0, true}, floor_at()).hopped,
            "a fresh hop press in the air cannot apply another impulse");
        Model empty;
        const Contacts clear{{QueryState::clear, 0.0}, {QueryState::clear, 0.0}};
        check(!empty.advance(fixed_seconds, {0.0, 1.0, 0.0, true}, clear).hopped &&
            empty.state().y < 0.0 && near(empty.state().speed, 0.0), "clear space falls without pedaling or hopping");
        Model single;
        check(!single.advance(fixed_seconds, {0.0, 0.0, 0.0, true},
            {{QueryState::hit, 0.0}, {QueryState::clear, 0.0}}).hopped,
            "one wheel alone cannot qualify for this initial two-wheel hop model");
    }
    {
        Model model;
        run(model, 1.0, 60, {0.0, 1.0, 0.0, false});
        const State before = model.state();
        const auto suspended = model.advance(10.0, {0.0, 1.0, 0.0, true},
            {{QueryState::unavailable, 0.0}, {QueryState::hit, 0.0}});
        check(suspended.suspended && suspended.steps == 0 && near(model.state().z, before.z) &&
            near(model.state().speed, before.speed) && near(model.state().y, before.y),
            "an unavailable wheel query safely suspends all motion");
        const auto resumed = model.advance(fixed_seconds, {0.0, 0.0, 0.0, true}, floor_at());
        check(!resumed.suspended && resumed.steps == 1 && !resumed.hopped,
            "resuming has no accumulated-time burst or stale held hop");
        const auto stall = model.advance(1000.0, {0.0, 1.0, 0.0, false}, floor_at());
        check(stall.steps == max_steps_per_advance && stall.dropped_seconds > 999.0,
            "a long stall has bounded catch-up and reports discarded simulation time");
        check(model.advance(fixed_seconds, {}, floor_at()).steps == 1, "discarded stall time is not caught up later");
    }
    {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double inf = std::numeric_limits<double>::infinity();
        Tuning malformed; malformed.wheelbase = nan; malformed.wheel_radius = 0.0;
        malformed.acceleration = inf; malformed.gravity = -1.0; malformed.slow_steer = inf;
        Model model(malformed);
        run(model, 10.0, 60, {inf, 100.0, nan, false});
        check(finite_state(model.state()) && model.state().speed <= model.tuning().forward_limit &&
            model.state().front_wheel_phase >= 0.0 && model.state().front_wheel_phase < tau,
            "malformed tuning/commands remain finite, speed-capped and phase-wrapped");
        const State before = model.state();
        check(model.advance(nan, {}, floor_at()).steps == 0 && model.advance(inf, {}, floor_at()).steps == 0 &&
            model.advance(-1.0, {}, floor_at()).steps == 0 && near(model.state().z, before.z),
            "nonfinite and negative simulation time cannot advance the model");
        check(model.advance(fixed_seconds, {}, {{QueryState::hit, nan}, {QueryState::hit, 0.0}}).suspended,
            "invalid support height is unavailable rather than an empty world");
        State invalid; invalid.x = inf; model.reset(invalid);
        check(finite_state(model.state()) && near(model.state().x, 0.0), "invalid reset state returns to finite defaults");
        State oversized; oversized.speed = 100000.0; oversized.x = 1e300; model.reset(oversized);
        check(model.state().speed <= model.tuning().forward_limit && model.state().x == 1000000.0,
            "finite but excessive reset state is bounded");
    }
    if (failures) { std::printf("BMX model: %d failures\n", failures); return 1; }
    std::puts("BMX model: fixed time, riding, steering, pinned air momentum, fakie landings, hops and safety passed.");
    return 0;
}
