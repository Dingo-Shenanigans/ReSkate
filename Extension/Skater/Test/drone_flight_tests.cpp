// The FPV drone's flight model (drone_flight.h): hover and height hold, thrust and top speed,
// motor and rate lag, angle mode, crashes against a wall and landing on a floor, and a gamepad's
// sticks. Flown in a world of one floor (y = 0) and one wall (x = 20).
#include "Extension/Skater/drone_beacon.h"
#include "Extension/Skater/drone_flight.h"
#include "Extension/Skater/drone_follow.h"
#include <cstdio>
using namespace dingosdk::drone;
using Vec3 = std::array<float, 3>;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { std::printf("FAIL: %s\n", msg); ++fails; } } while (0)

namespace {
constexpr float tick = 1.0f / 60.0f;
// The first point on the segment that lies on the floor or the wall.
bool cast(const Vec3& from, const Vec3& to, Vec3& hit) {
    float best = 2.0f;
    const auto plane = [&](int axis, float at) {
        const float a = from[axis] - at, b = to[axis] - at;
        if ((a > 0) == (b > 0) || a == b) return;
        const float t = a / (a - b);
        if (t >= 0 && t <= 1 && t < best) best = t;
    };
    plane(1, 0.0f);
    plane(0, 20.0f);
    if (best > 1.0f) return false;
    for (int i = 0; i < 3; ++i) hit[i] = from[i] + (to[i] - from[i]) * best;
    return true;
}
const World world{cast};
std::array<float, 16> at(float x, float y, float z) { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1}; }
Settings settings() {
    Settings s;
    s.hover_throttle = 0.22f;
    s.armed = armed();
    return s;
}
// Flies `seconds` of ticks, the command the same throughout.
void fly(std::array<float, 16>& camera, State& state, const Cmd& cmd, float seconds, Settings s, const World* w = &world, float speed = 10.0f) {
    for (float t = 0; t < seconds; t += tick) {
        s.armed = armed();
        camera = step_drone_cmd(camera, state, cmd, tick, speed, s, w);
    }
}
Vec3 position(const std::array<float, 16>& m) { return {m[12], m[13], m[14]}; }
float speed_of(const State& s) { return std::sqrt(s.velocity[0] * s.velocity[0] + s.velocity[1] * s.velocity[1] + s.velocity[2] * s.velocity[2]); }
float up_y(const State& s) { return s.body[5]; }
Cmd holding(float climb = 0.0f) { Cmd c; c.hold = true; c.climb = climb; return c; }
}

int main() {
    // Thrust: a 22% hover is about ten times the drone's weight.
    {
        const auto s = settings();
        const float ratio = detail::max_thrust(s) / s.gravity;
        CHECK(ratio > 9.0f && ratio < 10.5f, "a 22% hover gives about a 10:1 thrust to weight");
        CHECK(std::abs(detail::output_for(s.gravity, s) - 0.22f) < 1e-3f, "the hover output holds the drone up");
    }
    // Height hold: let go and it stays where it is, upright or leaning.
    {
        set_armed(true);
        auto camera = at(0, 5, 0);
        State state;
        fly(camera, state, holding(), 5.0f, settings());
        CHECK(std::abs(position(camera)[1] - 5.0f) < 0.15f && speed_of(state) < 0.2f, "height hold hovers in place");
        auto s = settings();
        s.angle_mode = true;
        Cmd lean = holding();
        lean.roll = -0.6f;   // (away from the wall)
        fly(camera, state, lean, 3.0f, s);
        CHECK(std::abs(position(camera)[1] - 5.0f) < 0.6f, "height hold keeps the height while leaning");
        CHECK(std::abs(position(camera)[0]) > 3.0f, "leaning moves the drone sideways");
        fly(camera, state, holding(1.0f), 1.0f, s);
        CHECK(state.velocity[1] > 4.0f, "full stick up climbs at about the climb rate");
        CHECK(state.motor > 0.95f, "height hold never holds full stick back from full throttle");
        fly(camera, state, holding(-1.0f), 0.3f, s);
        CHECK(state.motor < 0.05f, "full stick down is no throttle, height hold or not");
    }
    // Motor and rate lag: neither jumps to what was asked.
    {
        set_armed(true);
        auto camera = at(0, 50, 0);
        State state;
        Cmd hover;
        hover.throttle = 0.22f;
        fly(camera, state, hover, 0.5f, settings(), nullptr);
        Cmd full;
        full.throttle = 1.0f;
        full.roll = 1.0f;
        fly(camera, state, full, tick, settings(), nullptr);
        CHECK(state.motor < 0.6f, "the motors take time to spool up");
        const float want = detail::actual_rate(1.0f, Settings{}.roll) * detail::kDegToRad;
        CHECK(state.rate[2] > 0.0f && state.rate[2] < want * 0.6f, "the roll rate takes time to build");
        fly(camera, state, full, 0.3f, settings(), nullptr);
        CHECK(state.motor > 0.95f, "the motors reach full output");
        CHECK(std::abs(state.rate[2] - want) < want * 0.05f, "the roll rate reaches the stick's rate");
    }
    // Top speed: flat out, drag holds it near the speed setting's (four times it, in m/s).
    {
        set_armed(true);
        auto camera = at(0, 500, 0);
        State state;
        auto s = settings();
        s.angle_mode = true;
        s.max_angle_deg = 75.0f;
        Cmd flat_out;
        flat_out.throttle = 1.0f;
        flat_out.pitch = -1.0f;
        fly(camera, state, flat_out, 15.0f, s, nullptr, 10.0f);
        const float top = speed_of(state);
        std::printf("Flat out at a speed setting of 10: %.1f m/s\n", top);
        CHECK(top > 26.0f && top < 46.0f, "flat out settles near the top speed");
    }
    // Angle mode: let go of the sticks and it levels out.
    {
        set_armed(true);
        auto camera = at(0, 50, 0);
        State state;
        auto s = settings();
        s.angle_mode = true;
        Cmd lean = holding();
        lean.pitch = 1.0f;
        lean.roll = -1.0f;
        fly(camera, state, lean, 1.0f, s);
        CHECK(up_y(state) < 0.85f, "angle mode leans with the sticks");
        fly(camera, state, holding(), 1.0f, s);
        CHECK(up_y(state) > 0.99f, "angle mode levels out when the sticks are let go");
    }
    // A hard hit: it bounces off, is knocked about, and the motors cut until it is armed again.
    {
        set_armed(true);
        auto camera = at(15, 5, 0);
        State state;
        fly(camera, state, holding(), tick, settings());
        state.velocity = {15.0f, 0.0f, 0.0f};
        Cmd ahead = holding();
        for (int i = 0; i < 60 && !state.crashed; ++i) fly(camera, state, ahead, tick, settings());
        CHECK(state.crashed && !armed(), "a hard hit cuts the motors");
        CHECK(state.velocity[0] < 0.0f, "a hard hit bounces off");
        CHECK(std::abs(state.rate[0]) + std::abs(state.rate[1]) + std::abs(state.rate[2]) > 1.0f, "a hard hit knocks the drone about");
        CHECK(position(camera)[0] < 20.0f, "it stays on its side of the wall");
        CHECK(telemetry().crashed.load() && telemetry().glitch.load() > 0.2f, "the crash shows and breaks up the picture");
        fly(camera, state, holding(), 4.0f, settings());
        CHECK(position(camera)[1] < 0.5f && position(camera)[1] >= 0.0f, "a crashed drone falls to the floor and stays on it");
        set_armed(true);
        fly(camera, state, holding(), tick, settings());
        CHECK(!state.crashed, "arming again puts it right");
    }
    // Crashes off: it just stops against the wall.
    {
        set_armed(true);
        auto camera = at(15, 5, 0);
        State state;
        auto s = settings();
        s.crash = false;
        fly(camera, state, holding(), tick, s);
        state.velocity = {15.0f, 0.0f, 0.0f};
        fly(camera, state, holding(), 1.0f, s);
        CHECK(!state.crashed && armed() && std::abs(state.velocity[0]) < 0.5f, "with crashes off it stops against the wall");
    }
    // A gentle landing is not a crash.
    {
        set_armed(true);
        auto camera = at(0, 3, 0);
        State state;
        fly(camera, state, holding(-0.25f), 4.0f, settings());
        CHECK(!state.crashed && armed(), "a gentle landing is not a crash");
        CHECK(position(camera)[1] >= 0.0f && position(camera)[1] < 0.5f, "it lands on the floor");
    }
    // A gamepad: a drifting stick reads nothing, the throttle stick springs back to a hold, and the
    // sensitivity scales full stick.
    {
        dingosdk::overlay::Gamepad pad;
        pad.valid = true;
        pad.left_x = 0.05f;
        pad.right_y = -0.06f;
        auto sticks = gamepad_sticks(pad, 0.03f, 0.6f, 0.45f, 0.22f);
        CHECK(sticks.valid && sticks.centred && sticks.yaw == 0.0f && sticks.pitch == 0.0f && sticks.climb == 0.0f,
              "a drifting gamepad stick reads nothing");
        CHECK(std::abs(sticks.throttle - 0.22f) < 1e-4f, "a gamepad's throttle at rest hovers");
        pad.right_x = 1.0f;
        pad.right_y = 0.0f;
        pad.left_y = 1.0f;
        sticks = gamepad_sticks(pad, 0.03f, 0.6f, 0.45f, 0.22f);
        CHECK(std::abs(sticks.roll - 0.6f) < 1e-4f && sticks.climb > 0.99f && sticks.throttle > 0.99f, "full gamepad sticks");
        pad.right_x = 0.3f;
        const auto soft = gamepad_sticks(pad, 0.03f, 1.0f, 0.45f, 0.22f).roll;
        const auto plain = gamepad_sticks(pad, 0.03f, 1.0f, 0.0f, 0.22f).roll;
        CHECK(soft < plain * 0.8f, "expo softens the middle of a gamepad stick");
        pad.right_x = 0.0f;
        pad.right_y = 1.0f;
        CHECK(gamepad_sticks(pad, 0.03f, 1.0f, 0.0f, 0.22f).pitch > 0.99f && gamepad_sticks(pad, 0.03f, 1.0f, 0.0f, 0.22f, true).pitch < -0.99f,
              "the right stick's pitch inverts");
        pad.right_x = 1.0f;
        pad.right_y = 0.0f;
        CHECK(gamepad_sticks(pad, 0.03f, 0.6f, 0.45f, 0.22f, false, true).roll > 0.99f &&
                  gamepad_sticks(pad, 0.03f, 0.6f, 0.45f, 0.22f, false, true).yaw == gamepad_sticks(pad, 0.03f, 0.6f, 0.45f, 0.22f).yaw,
              "in angle mode full stick is the full lean, and the sensitivity still slows turning");
        // Flown: let go of the throttle stick and the drone holds its height.
        set_armed(true);
        auto camera = at(0, 5, 0);
        State state;
        pad = {};
        pad.valid = true;
        for (float t = 0; t < 3.0f; t += tick) {
            auto s = settings();
            camera = step_drone_radio(camera, state, gamepad_sticks(pad, 0.1f, 0.6f, 0.45f, 0.22f), tick, 10.0f, s, &world);
        }
        CHECK(std::abs(position(camera)[1] - 5.0f) < 0.15f, "a gamepad at rest holds the height");
        // Height hold off: the throttle is the stick's alone, whatever the lean.
        {
            auto s = settings();
            s.angle_mode = true;
            s.alt_hold = false;
            State flat, leaning;
            auto a = at(0, 50, 0), b = at(0, 50, 0);
            dingosdk::overlay::Gamepad rest, lean;
            rest.valid = lean.valid = true;
            lean.right_x = -1.0f;
            for (float t = 0; t < 1.0f; t += tick) {
                a = step_drone_radio(a, flat, gamepad_sticks(rest, 0.1f, 0.6f, 0.45f, 0.22f, false, true), tick, 10.0f, s, &world);
                b = step_drone_radio(b, leaning, gamepad_sticks(lean, 0.1f, 0.6f, 0.45f, 0.22f, false, true), tick, 10.0f, s, &world);
            }
            CHECK(std::abs(flat.motor - 0.22f) < 0.01f && std::abs(leaning.motor - flat.motor) < 0.01f,
                  "without height hold the throttle does not change with the lean");
        }
    }
    // Another player's drone, heard: the throttle it seems to be on, from how it moves.
    {
        namespace beacon = dingosdk::drone::beacon;
        const float g = beacon::assumed_gravity, hover = beacon::assumed_hover;
        CHECK(std::abs(beacon::estimate_throttle({0, 0, 0}, beacon::no_rotation) - hover) < 1e-4f, "a hovering drone is on its hover throttle");
        CHECK(beacon::estimate_throttle({0, -g, 0}, beacon::no_rotation) < 1e-4f, "a falling drone is off the throttle");
        CHECK(std::abs(beacon::estimate_throttle({0, g, 0}, beacon::no_rotation) - hover * std::pow(2.0f, 2.0f / 3.0f)) < 1e-3f,
              "climbing at 1 g takes twice the hover thrust");
        // Leant 45 degrees and holding its height: the thrust is 1/cos(45) of its weight.
        const float half = 0.3926991f;
        const beacon::Rotation leant{std::sin(half), 0, 0, std::cos(half)};
        const auto up = beacon::basis_from_rotation(leant)[1];
        const beacon::Position level{up[0] / up[1] * g, 0, up[2] / up[1] * g};
        CHECK(std::abs(beacon::estimate_throttle(level, leant) - hover * std::pow(1.0f / std::cos(2 * half), 2.0f / 3.0f)) < 1e-3f,
              "a leant drone holding its height is on more than its hover throttle");
        // Through packets: still for a while, then climbing at 1 g.
        const auto start = beacon::Clock::now();
        const auto at_time = [&](int n) { return start + std::chrono::milliseconds(66 * n); };
        int n = 0;
        for (; n < 10; ++n) beacon::receive(7, "pilot", {0, 5, 0}, beacon::no_rotation, true, at_time(n));
        auto heard = beacon::snapshot(at_time(n - 1));
        CHECK(heard.size() == 1 && std::abs(heard[0].throttle - hover) < 0.01f && heard[0].speed < 0.01f, "a drone keeping still sounds like a hover");
        for (int k = 0; k < 15; ++k, ++n) {
            const float t = 0.066f * k;
            beacon::receive(7, "pilot", {0, 5 + 0.5f * g * t * t, 0}, beacon::no_rotation, true, at_time(n));
        }
        heard = beacon::snapshot(at_time(n - 1));
        CHECK(heard.size() == 1 && heard[0].throttle > 0.5f && heard[0].speed > 5.0f, "a drone climbing hard sounds busier and faster");
        beacon::forget(7);
    }
    // Follow mode (drone_follow.h): the autopilot flies the real model behind a skater.
    {
        namespace follow = dingosdk::drone::follow;
        using dingosdk::drone::detail::Vec3;
        const follow::Tuning tuning;
        const auto cfg = follow::flight_settings(settings(), tuning);
        Vec3 skater{0, 1, 0};
        Vec3 heading{0, 0, -1};
        auto camera = follow::pose_facing(follow::spot(skater, {0, 0, 0}, heading, tuning, 0.0f), skater);
        State drone;
        follow::Pilot pilot;
        float worst_frame = 0.0f;   // the skater furthest off the middle of the shot, degrees
        float worst_up = 0.0f, worst_down = 0.0f; // the skater above / below the middle of the shot, degrees
        float lowest = 1e9f, highest = -1e9f;     // the drone's height over the skater's ground
        float nose_up = 0.0f, nose_down = 0.0f;   // the drone's steepest lean back / forward, degrees
        float settle = 1.5f; // seconds of each run not measured (only the take-off)
        const auto run = [&](float seconds, float speed, float turn_rate, bool ollies = false) {
            for (float t = 0; t < seconds; t += tick) {
                heading = dingosdk::drone::detail::rotate(heading, {0, 1, 0}, turn_rate * tick);
                skater = dingosdk::drone::detail::add(skater, dingosdk::drone::detail::scale(heading, speed * tick));
                // An ollie every 1.5 s: up 0.8 m and down again over 0.6 s.
                const float phase = std::fmod(t, 1.5f);
                skater[1] = 1.0f + (ollies && phase < 0.6f ? 0.8f * std::sin(phase / 0.6f * 3.14159f) : 0.0f);
                const Vec3 at{camera[12], camera[13], camera[14]};
                const auto cmd = follow::fly(pilot, drone, at, skater, heading, tick, cfg, tuning);
                camera = step_drone_cmd(camera, drone, cmd, tick, 10.0f, cfg);
                if (t > settle) {
                    Vec3 look = dingosdk::drone::detail::scale(dingosdk::drone::detail::row(camera, 2), -1.0f);
                    Vec3 to{skater[0] - camera[12], skater[1] - camera[13], skater[2] - camera[14]};
                    look[1] = to[1] = 0.0f;
                    const float c = dingosdk::drone::detail::dot(dingosdk::drone::detail::normalize(look), dingosdk::drone::detail::normalize(to));
                    worst_frame = std::max(worst_frame, std::acos(std::clamp(c, -1.0f, 1.0f)) * 57.2958f);
                    const Vec3 view = dingosdk::drone::detail::normalize(dingosdk::drone::detail::scale(dingosdk::drone::detail::row(camera, 2), -1.0f));
                    const Vec3 body = dingosdk::drone::detail::normalize(Vec3{skater[0] - camera[12], skater[1] + 0.9f - camera[13], skater[2] - camera[14]});
                    const float above = (std::asin(body[1]) - std::asin(view[1])) * 57.2958f;
                    worst_up = std::max(worst_up, above);
                    worst_down = std::max(worst_down, -above);
                    const float nose = std::asin(std::clamp(-dingosdk::drone::detail::row(drone.body, 2)[1], -1.0f, 1.0f)) * 57.2958f;
                    nose_up = std::max(nose_up, nose);
                    nose_down = std::max(nose_down, -nose);
                    lowest = std::min(lowest, camera[13] - 1.0f);
                    highest = std::max(highest, camera[13] - 1.0f);
                }
            }
        };
        const auto behind = [&] {
            const Vec3 d{camera[12] - skater[0], camera[13] - skater[1], camera[14] - skater[2]};
            return std::array<float, 2>{-dingosdk::drone::detail::dot(d, heading), d[1]};
        };
        run(6.0f, 8.0f, 0.0f); // rolling straight at 8 m/s
        settle = 0.0f;         // from here on every moment counts
        std::printf("Follow: straight, worst framing %.1f deg [nose %.0f up %.0f down]\n", worst_frame, nose_up, nose_down);
        worst_frame = 0.0f;
        auto b = behind();
        CHECK(b[0] > 2.0f && b[0] < 9.0f && b[1] > 0.5f && b[1] < 4.0f, "follow: behind and above a skater rolling straight");
        CHECK(drone.velocity[2] < -5.0f, "follow: keeps up with the skater");
        run(2.0f, 8.0f, 0.785f); // a 90 degree turn over two seconds
        run(4.0f, 8.0f, 0.0f);
        std::printf("Follow: turn, worst framing %.1f deg [nose %.0f up %.0f down]\n", worst_frame, nose_up, nose_down);
        b = behind();
        CHECK(b[0] > 2.0f && b[0] < 9.0f, "follow: swings round behind after a turn");
        CHECK(worst_frame < 45.0f, "follow: keeps the skater in shot");
        worst_frame = 0.0f;
        std::printf("Follow: so far the skater sat up to %.1f deg above / %.1f below the middle [nose %.0f up %.0f down]\n", worst_up, worst_down, nose_up, nose_down);
        worst_frame = worst_up = worst_down = nose_up = nose_down = 0.0f;
        // Stops hard (a powerslide: 8 m/s to nothing in a second), then stands.
        for (int i = 1; i <= 10; ++i) run(0.1f, 8.0f - 0.8f * static_cast<float>(i), 0.0f);
        run(5.0f, 0.0f, 0.0f);
        std::printf("Follow: stop, worst framing %.1f deg, %.1f up / %.1f down [nose %.0f up %.0f down]\n", worst_frame, worst_up, worst_down, nose_up, nose_down);
        CHECK(worst_frame < 40.0f, "follow: keeps the skater in shot when they stop hard");
        b = behind();
        const float settled = std::abs(b[0] - tuning.distance) + std::abs(b[1] - tuning.height);
        CHECK(settled < 1.5f && dingosdk::drone::detail::length(drone.velocity) < 1.0f, "follow: settles behind a skater who stops");
        std::printf("Follow: settled %.2f m off [nose %.0f up %.0f down]\n", settled, nose_up, nose_down);
        // Ollies while rolling: the drone should not hop with every one.
        run(3.0f, 6.0f, 0.0f);
        worst_frame = worst_up = worst_down = nose_up = nose_down = 0.0f;
        lowest = 1e9f; highest = -1e9f;
        run(6.0f, 6.0f, 0.0f, true);
        std::printf("Follow: ollies, height %.2f..%.2f m, %.1f up / %.1f down [nose %.0f up %.0f down]\n", lowest, highest, worst_up, worst_down, nose_up, nose_down);
        CHECK(highest - lowest < 0.6f, "follow: does not hop with every ollie");
        // Falls behind (the skater pushes off hard), catches up and settles: no big nose-up brake.
        worst_frame = worst_up = worst_down = nose_up = nose_down = 0.0f;
        for (int i = 0; i < 10; ++i) run(0.15f, 2.0f + 1.0f * static_cast<float>(i), 0.0f);
        run(6.0f, 12.0f, 0.0f);
        run(2.0f, 6.0f, 0.0f); // and eases off
        std::printf("Follow: catch-up, worst framing %.1f deg, %.1f up / %.1f down [nose %.0f up %.0f down]\n", worst_frame, worst_up, worst_down, nose_up, nose_down);
        CHECK(worst_up < 25.0f && worst_down < 25.0f, "follow: keeps the skater in shot while catching up");
    }
    // Follow on a halfpipe: the skater rides wall to wall (across x, up to 4 m up each wall). The
    // drone should stop chasing the turns and hold a spot off to the side to film it.
    {
        namespace follow = dingosdk::drone::follow;
        namespace d = dingosdk::drone::detail;
        const follow::Tuning tuning;
        const auto cfg = follow::flight_settings(settings(), tuning);
        Vec3 skater{0, 1, 0};
        auto camera = follow::pose_facing(follow::spot(skater, {0, 0, 0}, {1, 0, 0}, tuning, 0.0f), skater);
        State drone;
        follow::Pilot pilot;
        float worst_side = 0.0f, worst_up = 0.0f, travel = 0.0f, lowest = 1e9f;
        Vec3 last{};
        for (float t = 0; t < 20.0f; t += tick) {
            const float phase = std::sin(t * 3.14159f / 2.0f); // wall to wall every two seconds
            skater = {6.0f * phase, 1.0f + 4.0f * std::max(std::abs(phase) - 0.6f, 0.0f) / 0.4f, 0.0f};
            const Vec3 at{camera[12], camera[13], camera[14]};
            const auto cmd = follow::fly(pilot, drone, at, skater, {std::cos(t * 3.14159f / 2.0f) >= 0 ? 1.0f : -1.0f, 0, 0}, tick, cfg, tuning, &world);
            camera = step_drone_cmd(camera, drone, cmd, tick, 10.0f, cfg, &world);
            lowest = std::min(lowest, camera[13]);
            if (t > 12.0f) {
                const Vec3 now{camera[12], camera[13], camera[14]};
                if (t > 12.0f + tick) travel += d::length(d::sub(now, last));
                last = now;
                Vec3 look = d::scale(d::row(camera, 2), -1.0f);
                const Vec3 to = d::sub(Vec3{skater[0], skater[1] + 0.9f, skater[2]}, now);
                Vec3 flat_look{look[0], 0, look[2]}, flat_to{to[0], 0, to[2]};
                worst_side = std::max(worst_side, std::acos(std::clamp(d::dot(d::normalize(flat_look), d::normalize(flat_to)), -1.0f, 1.0f)) * 57.2958f);
                worst_up = std::max(worst_up, std::abs(std::asin(d::normalize(to)[1]) - std::asin(d::normalize(look)[1])) * 57.2958f);
            }
        }
        std::printf("Follow: halfpipe, session %d, moved %.1f m in 8 s, worst %.1f deg off to the side, %.1f up or down\n",
            pilot.session ? 1 : 0, travel, worst_side, worst_up);
        CHECK(pilot.session, "follow: a halfpipe session is filmed from a spot, not chased");
        CHECK(travel < 8.0f, "follow: holds its spot through a halfpipe session");
        CHECK(worst_side < 40.0f && worst_up < 30.0f, "follow: keeps a halfpipe session in shot");
        CHECK(lowest > 0.5f, "follow: stays off the ground through a session");
    }
    // Follow into a wall: the skater rides at the wall (x = 20), stops short of it, turns round and
    // rides back. The spot behind it is then inside the wall; the drone keeps off it.
    {
        namespace follow = dingosdk::drone::follow;
        namespace d = dingosdk::drone::detail;
        const follow::Tuning tuning;
        const auto cfg = follow::flight_settings(settings(), tuning);
        Vec3 skater{0, 1, 5};
        Vec3 heading{1, 0, 0};
        auto camera = follow::pose_facing(follow::spot(skater, {0, 0, 0}, heading, tuning, 0.0f), skater);
        State drone;
        follow::Pilot pilot;
        float closest = 1e9f, worst_side = 0.0f;
        const auto step = [&](float speed) {
            skater = d::add(skater, d::scale(heading, speed * tick));
            const Vec3 at{camera[12], camera[13], camera[14]};
            const auto cmd = follow::fly(pilot, drone, at, skater, heading, tick, cfg, tuning, &world);
            camera = step_drone_cmd(camera, drone, cmd, tick, 10.0f, cfg, &world);
            closest = std::min(closest, 20.0f - camera[12]);
            Vec3 look = d::scale(d::row(camera, 2), -1.0f);
            Vec3 flat_look{look[0], 0, look[2]}, flat_to{skater[0] - camera[12], 0, skater[2] - camera[14]};
            if (d::length(flat_to) > 1.0f)
                worst_side = std::max(worst_side, std::acos(std::clamp(d::dot(d::normalize(flat_look), d::normalize(flat_to)), -1.0f, 1.0f)) * 57.2958f);
        };
        float t = 0;
        for (; skater[0] < 16.0f; t += tick) step(6.0f);                  // toward the wall
        for (float slow = 6.0f; slow > 0.0f; slow -= 6.0f * tick) step(slow); // stops a few metres short of it
        heading = {-1, 0, 0};
        for (float u = 0; u < 1.0f; u += tick) step(0.0f);                // turns round
        for (float u = 0; u < 6.0f; u += tick) step(6.0f);                // and rides away from it
        std::printf("Follow: wall, came within %.2f m of it, worst %.1f deg off to the side\n", closest, worst_side);
        CHECK(closest > 0.5f, "follow: keeps off a wall the spot behind the skater is in");
    }
    // Follow settings: the DRONE page's distance and aggression; the middle is the tuning above.
    {
        namespace follow = dingosdk::drone::follow;
        const follow::Tuning plain;
        const auto middle = follow::tuning_for(4.5f, 0.5f);
        CHECK(std::abs(middle.reaction - plain.reaction) < 1e-4f && std::abs(middle.speed_gain - plain.speed_gain) < 1e-4f &&
              std::abs(middle.gap_gain - plain.gap_gain) < 1e-4f && std::abs(middle.height - plain.height) < 0.01f &&
              std::abs(middle.session_distance - plain.session_distance) < 1e-4f, "follow: the middle settings are the usual tuning");
        const auto calm = follow::tuning_for(8.0f, 0.0f), hard = follow::tuning_for(8.0f, 1.0f);
        CHECK(calm.distance == 8.0f && calm.reaction > hard.reaction && calm.max_lean_deg < hard.max_lean_deg,
              "follow: aggression makes the pilot quicker and lean harder");
    }
    // Follow down a steep slope: the skater rides off an 8 m high top, down a 1:1 slope and on. The
    // spot behind the skater is then metres inside the slope; the drone keeps over the ground.
    {
        namespace follow = dingosdk::drone::follow;
        namespace d = dingosdk::drone::detail;
        const auto ground = [](float x) { return std::clamp(8.0f - x, 0.0f, 8.0f); };
        const World bank{[&](const Vec3& from, const Vec3& to, Vec3& hit) {
            for (int i = 0; i <= 64; ++i) {
                const float k = static_cast<float>(i) / 64.0f;
                const Vec3 at{from[0] + (to[0] - from[0]) * k, from[1] + (to[1] - from[1]) * k, from[2] + (to[2] - from[2]) * k};
                if (at[1] <= ground(at[0])) { hit = at; return true; }
            }
            return false;
        }};
        const follow::Tuning tuning;
        const auto cfg = follow::flight_settings(settings(), tuning);
        Vec3 skater{-8.0f, 9.0f, 0};
        const Vec3 heading{1, 0, 0};
        auto camera = follow::pose_facing(follow::spot(skater, {0, 0, 0}, heading, tuning, 0.0f), skater);
        State drone;
        follow::Pilot pilot;
        float lowest = 1e9f;
        for (float t = 0; t < 6.0f; t += tick) {
            skater[0] += 6.0f * tick;
            skater[1] = ground(skater[0]) + 1.0f;
            const Vec3 at{camera[12], camera[13], camera[14]};
            const auto cmd = follow::fly(pilot, drone, at, skater, heading, tick, cfg, tuning, &bank);
            camera = step_drone_cmd(camera, drone, cmd, tick, 10.0f, cfg, &bank);
            if (t > 1.0f) lowest = std::min(lowest, camera[13] - ground(camera[12]));
        }
        std::printf("Follow: down a slope, lowest %.2f m over the ground\n", lowest);
        CHECK(lowest > 0.6f, "follow: keeps over a slope its spot is inside");
        // A spot under the ground (as a bad height would give): it keeps its clearance instead.
        auto low = follow::tuning_for(4.5f, 0.5f);
        low.height = -3.0f;
        skater = {0, 1, 0};
        camera = follow::pose_facing(Vec3{-4.5f, 2.0f, 0.0f}, skater);
        drone = State{};
        pilot = follow::Pilot{};
        lowest = 1e9f;
        for (float t = 0; t < 6.0f; t += tick) {
            skater[0] += 4.0f * tick;
            const Vec3 at{camera[12], camera[13], camera[14]};
            const auto cmd = follow::fly(pilot, drone, at, skater, heading, tick, cfg, low, &world);
            camera = step_drone_cmd(camera, drone, cmd, tick, 10.0f, cfg, &world);
            lowest = std::min(lowest, camera[13]);
        }
        std::printf("Follow: spot underground, lowest %.2f m\n", lowest);
        CHECK(lowest > 1.1f, "follow: keeps its clearance when its spot is under the ground");
    }
    if (fails) return 1;
    std::puts("Drone flight tests passed.");
    return 0;
}
