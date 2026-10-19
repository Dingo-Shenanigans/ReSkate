// Replay keyframe smoothing, apart from the game: at smoothness 0 the camera plays the game's own curve,
// at any smoothness it meets every key, at 1 its speed and acceleration carry on through the keys, and a
// framing segment rewritten for the game's own spline plays the tangents it was given.
// (The game's spline is copied here term for term, so these run without it.)
#include "Extension/ReplayKeyframes/replay_curve.h"
#include "Extension/ReplayKeyframes/replay_settings.h"
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace dingosdk::replay_keyframes;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool near(double a, double b, double within) { return std::abs(a - b) <= within * (1.0 + std::abs(b)); }

// Keys a second, two seconds and half a second apart: uneven, as the user places them.
const std::vector<double> times{0.0, 1.0, 3.0, 3.5, 6.0};
// The game's spline (0x602a20).
float game_spline(float p0, float p1, float p2, float p3, float s, float k1, float k2) {
    const float m1 = (1 - k1) * 0.5f * (p2 - p0), m2 = (1 - k2) * 0.5f * (p3 - p1);
    return (2 * s * s * s - 3 * s * s + 1) * p1 + (3 * s * s - 2 * s * s * s) * p2 + (s * s * s - 2 * s * s + s) * m1 + (s * s * s - s * s) * m2;
}
// Acceleration per second² at the start (s = 0) and end (s = 1) of a segment with velocities v per second.
double acceleration_at_start(double p1, double p2, double v1, double v2, double h) { return (6 * (p2 - p1) - (4 * v1 + 2 * v2) * h) / (h * h); }
double acceleration_at_end(double p1, double p2, double v1, double v2, double h) { return (-6 * (p2 - p1) + (2 * v1 + 4 * v2) * h) / (h * h); }

void blends_run_from_the_games_tangent_to_the_smooth_one() {
    check(blend(0.25f, 99.0, 0.0f) == 0.25f, "At smoothness 0 a tangent is the game's, exactly");
    check(blend(0.25f, 99.0, 1.0f) == 99.0f, "at 1 the smooth curve's");
    check(near(blend(1.0f, 3.0, 0.25f), 1.5, 1e-6), "and in between a mix");
}

void at_one_speed_and_acceleration_carry_on() {
    const std::vector<double> values{0.0, 4.0, -2.0, 1.0, 7.0};
    std::vector<double> v(values.size()), work;
    check(c2_velocities(times, values, v, work), "The spline is solved");
    for (std::size_t i = 1; i + 1 < values.size(); ++i) {
        const double before = times[i] - times[i - 1], after = times[i + 1] - times[i];
        check(near(acceleration_at_end(values[i - 1], values[i], v[i - 1], v[i], before),
                   acceleration_at_start(values[i], values[i + 1], v[i], v[i + 1], after), 1e-9),
              "The acceleration is the same either side of every inner key");
    }
    const auto last = values.size() - 1;
    check(near(acceleration_at_start(values[0], values[1], v[0], v[1], times[1]), 0, 1e-9) &&
              near(acceleration_at_end(values[last - 1], values[last], v[last - 1], v[last], times[last] - times[last - 1]), 0, 1e-9),
          "It starts and ends without accelerating");
    check(std::abs(v[0]) > 0.1 && std::abs(v.back()) > 0.1, "(the keys don't start or end at rest by themselves)");
}

void straight_lines_stay_straight() {
    std::vector<double> values;
    for (const double t : times) values.push_back(2.5 * t - 1.0);
    std::vector<double> v(values.size()), work;
    check(c2_velocities(times, values, v, work), "The spline is solved");
    for (const double velocity : v) check(near(velocity, 2.5, 1e-12), "Keys on a line at uneven times move along it evenly");
    const std::vector<double> one{2.0}, value{5.0};
    std::vector<double> still(1, 9.0);
    check(c2_velocities(one, value, still, work) && still[0] == 0, "One key holds still");
    const std::vector<double> backwards{0.0, 1.0, 1.0}, three{0.0, 1.0, 2.0};
    std::vector<double> none(3, 9.0);
    check(!c2_velocities(backwards, three, none, work) && none[0] == 9.0, "Keys that don't move on in time are refused");
}

// A framing segment as the game builds it (0x60c7a0) from four of the keys, starting at key `from`.
struct Framing {
    std::vector<float> slots;
    std::vector<double> key_times;
    alignas(16) unsigned char segment[layout::segment_size]{};
};
Framing framing_keys(std::size_t from) {
    constexpr auto floats = layout::slot_floats;
    Framing result;
    result.key_times = times;
    result.slots.assign(times.size() * floats, 0.0f);
    for (std::size_t key = 0; key < times.size(); ++key) {
        float* slot = result.slots.data() + key * floats;
        for (std::size_t lane = 0; lane < floats; ++lane)
            slot[lane] = static_cast<float>(std::cos(0.9 * static_cast<double>(lane) + 1.7 * static_cast<double>(key)) * 5.0);
        std::memcpy(reinterpret_cast<unsigned char*>(slot) + layout::slot_time, &times[key], sizeof(double));
    }
    const auto slot = [&](std::size_t key) { return reinterpret_cast<const unsigned char*>(result.slots.data() + key * floats); };
    const auto last = times.size() - 1;
    const auto previous = from ? from - 1 : from, to = from < last ? from + 1 : from, next = to < last ? to + 1 : to;
    std::memcpy(result.segment + layout::segment_from, slot(from), layout::slot_size);
    std::memcpy(result.segment + layout::segment_to, slot(to), layout::slot_size);
    std::memcpy(result.segment + layout::segment_previous, slot(previous), layout::slot_size);
    std::memcpy(result.segment + layout::segment_next, slot(next), layout::slot_size);
    // The neighbours moved to one segment's time away (0x624e50), and some braking.
    const double gap = times[to] - times[from];
    auto* const p0 = reinterpret_cast<float*>(result.segment + layout::segment_previous);
    auto* const p1 = reinterpret_cast<float*>(result.segment + layout::segment_from);
    auto* const p2 = reinterpret_cast<float*>(result.segment + layout::segment_to);
    auto* const p3 = reinterpret_cast<float*>(result.segment + layout::segment_next);
    for (std::size_t lane = 0; lane < floats; ++lane) {
        if (lane * sizeof(float) >= layout::slot_time && lane * sizeof(float) < layout::slot_time + sizeof(double)) continue;
        if (previous != from) p0[lane] = static_cast<float>(p1[lane] + (p0[lane] - p1[lane]) * gap / (times[from] - times[previous]));
        if (next != to) p3[lane] = static_cast<float>(p2[lane] + (p3[lane] - p2[lane]) * gap / (times[next] - times[to]));
    }
    auto* const k_to = reinterpret_cast<float*>(result.segment + layout::segment_k_to);
    auto* const k_from = reinterpret_cast<float*>(result.segment + layout::segment_k_from);
    for (std::size_t k = 0; k < layout::segment_k_count; ++k) {
        k_to[k] = 0.1f * static_cast<float>(k % 4);
        k_from[k] = k == 4 ? 1.0f : 0.05f * static_cast<float>(k);
    }
    const int behaviour = 1;
    std::memcpy(result.segment + layout::segment_behaviour, &behaviour, sizeof(behaviour));
    return result;
}
float segment_float(const unsigned char* segment, std::size_t offset) {
    float value;
    std::memcpy(&value, segment + offset, sizeof(value));
    return value;
}
// What the game's player makes of a lane of the segment at s.
float play(const unsigned char* segment, layout::Lane lane, float s) {
    const auto f = [&](std::size_t slot) { return segment_float(segment, slot + lane.slot_float * sizeof(float)); };
    return game_spline(f(layout::segment_previous), f(layout::segment_from), f(layout::segment_to), f(layout::segment_next), s,
                       segment_float(segment, layout::segment_k_from + lane.k * sizeof(float)),
                       segment_float(segment, layout::segment_k_to + lane.k * sizeof(float)));
}

void a_framing_segment_plays_as_meant() {
    const auto lanes_played = framing_lanes(1);
    check(lanes_played.size() == 7 && framing_lanes(7).empty(), "Behaviour 1 plays seven lanes, an unknown one none");
    Work work;
    for (std::size_t from = 0; from + 1 < times.size(); ++from) {
        // At smoothness 0 the game plays the segment as it built it.
        auto stock = framing_keys(from);
        const auto built = stock;
        check(smooth_segment(stock.segment, {stock.key_times, stock.slots, from}, lanes_played, 0.0f, work) == SegmentResult::smoothed,
              "A segment is rewritten");
        for (const auto lane : lanes_played)
            for (float s = 0; s <= 1.0f; s += 0.125f)
                check(near(play(stock.segment, lane, s), play(built.segment, lane, s), 1e-4), "At 0 it plays as the game built it");

        // Above 0 the game's spline plays the blended tangents (the C2 ones at 1), and still starts at one
        // key and ends at the next.
        const double gap = times[from + 1] - times[from];
        for (const float smoothness : {0.3f, 1.0f}) {
            auto smooth = framing_keys(from);
            check(smooth_segment(smooth.segment, {smooth.key_times, smooth.slots, from}, lanes_played, smoothness, work) ==
                      SegmentResult::smoothed,
                  "A segment is rewritten");
            for (const auto lane : lanes_played) {
                std::vector<double> values, v(times.size()), solver;
                for (std::size_t key = 0; key < times.size(); ++key) values.push_back(smooth.slots[key * layout::slot_floats + lane.slot_float]);
                c2_velocities(times, values, v, solver);
                const auto f = [&](const unsigned char* segment, std::size_t slot) {
                    return segment_float(segment, slot + lane.slot_float * sizeof(float));
                };
                const auto k = [&](const unsigned char* segment, std::size_t block) { return segment_float(segment, block + lane.k * sizeof(float)); };
                const float game_m1 = (1 - k(built.segment, layout::segment_k_from)) * 0.5f *
                                      (f(built.segment, layout::segment_to) - f(built.segment, layout::segment_previous));
                const float game_m2 = (1 - k(built.segment, layout::segment_k_to)) * 0.5f *
                                      (f(built.segment, layout::segment_next) - f(built.segment, layout::segment_from));
                check(near(0.5 * (f(smooth.segment, layout::segment_to) - f(smooth.segment, layout::segment_previous)),
                           blend(game_m1, v[from] * gap, smoothness), 1e-4) &&
                          near(0.5 * (f(smooth.segment, layout::segment_next) - f(smooth.segment, layout::segment_from)),
                               blend(game_m2, v[from + 1] * gap, smoothness), 1e-4),
                      "The game's spline plays the blended tangents");
                check(play(smooth.segment, lane, 0.0f) == values[from] && near(play(smooth.segment, lane, 1.0f), values[from + 1], 1e-5),
                      "and still goes from key to key");
            }
        }

        // At 1, the two segments the game plays either side of a key have the same speed per second there.
        if (from > 0) {
            auto into = framing_keys(from - 1), out = framing_keys(from);
            check(smooth_segment(into.segment, {into.key_times, into.slots, from - 1}, lanes_played, 1.0f, work) ==
                          SegmentResult::smoothed &&
                      smooth_segment(out.segment, {out.key_times, out.slots, from}, lanes_played, 1.0f, work) ==
                          SegmentResult::smoothed,
                  "Both segments at a key are rewritten");
            const double before = times[from] - times[from - 1];
            for (const auto lane : lanes_played) {
                const auto f = [&](const unsigned char* segment, std::size_t slot) {
                    return static_cast<double>(segment_float(segment, slot + lane.slot_float * sizeof(float)));
                };
                const double arriving = 0.5 * (f(into.segment, layout::segment_next) - f(into.segment, layout::segment_from)) / before;
                const double leaving = 0.5 * (f(out.segment, layout::segment_to) - f(out.segment, layout::segment_previous)) / gap;
                check(near(arriving, leaving, 1e-4), "The speed carries on through a key");
            }
        }
        auto smooth = framing_keys(from);
        check(smooth_segment(smooth.segment, {smooth.key_times, smooth.slots, from}, lanes_played, 1.0f, work) == SegmentResult::smoothed,
              "A segment is rewritten");
        // Lanes the Behaviour doesn't play, and their k, are left as they were.
        for (std::size_t offset = 0; offset < layout::segment_size; offset += sizeof(float)) {
            bool played = false;
            for (const auto lane : lanes_played)
                for (const auto slot : {layout::segment_previous, layout::segment_next})
                    played = played || offset == slot + lane.slot_float * sizeof(float);
            for (const auto lane : lanes_played)
                for (const auto block : {layout::segment_k_from, layout::segment_k_to})
                    played = played || offset == block + lane.k * sizeof(float);
            if (!played)
                check(std::memcmp(smooth.segment + offset, built.segment + offset, sizeof(float)) == 0 ||
                          offset == layout::segment_behaviour,
                      "Nothing else in the segment changes");
        }
    }
    auto last = framing_keys(times.size() - 1);
    check(smooth_segment(last.segment, {last.key_times, last.slots, times.size() - 1}, lanes_played, 1.0f, work) ==
              SegmentResult::stock,
          "The last key's segment holds still and is left alone");
    auto odd = framing_keys(1);
    reinterpret_cast<float*>(odd.segment + layout::segment_from)[12] += 1.0f;
    const auto changed = odd;
    check(smooth_segment(odd.segment, {odd.key_times, odd.slots, 1}, lanes_played, 1.0f, work) == SegmentResult::mismatch &&
              std::memcmp(odd.segment, changed.segment, layout::segment_size) == 0,
          "A segment that doesn't match its keys is left as the game built it");
}

void settings_are_kept_as_left() {
    const Settings fresh = from_text("");
    check(!fresh.on && fresh.amount == 1.0f, "With nothing saved, smoothing is off (on, it would be the smoothest)");
    const Settings left{true, 0.3f};
    const Settings read = from_text(to_text(left));
    check(read.on && read.amount == 0.3f, "Settings read back as they were written");
    const Settings odd = from_text("on = 1\r\namount=7\nease=0.5\nonly=0\nwho=me\n");
    check(odd.on && odd.amount == 1.0f,
          "Values out of range, and lines it doesn't know (an earlier build's ease and only too), keep the defaults");
    check(from_text("amount=1.5").amount == 1.0f && from_text("amount=0").amount == 0.0f, "The amount runs from 0 to 1");
}
} // namespace

int main() {
    try {
        blends_run_from_the_games_tangent_to_the_smooth_one();
        at_one_speed_and_acceleration_carry_on();
        straight_lines_stay_straight();
        a_framing_segment_plays_as_meant();
        settings_are_kept_as_left();
        std::cout << "replay keyframes tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "replay keyframes test failed: " << error.what() << '\n';
        return 1;
    }
}
