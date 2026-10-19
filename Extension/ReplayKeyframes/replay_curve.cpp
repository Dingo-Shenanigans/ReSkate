#include "replay_curve.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace dingosdk::replay_keyframes {
namespace {
float read_float(const unsigned char* at) noexcept {
    float value;
    std::memcpy(&value, at, sizeof(value));
    return value;
}
void write_float(unsigned char* at, float value) noexcept { std::memcpy(at, &value, sizeof(value)); }
double read_double(const unsigned char* at) noexcept {
    double value;
    std::memcpy(&value, at, sizeof(value));
    return value;
}
// The same float, give or take rounding: a key the game and this code both built from one keyframe.
bool same(float a, float b) noexcept { return std::abs(a - b) <= 1e-4f * (1.0f + std::abs(a)); }

// The natural spline through two or more keys at rising times. One equation per key,
// a·v[i−1] + b·v[i] + c·v[i+1] = r: the acceleration either side of an inner key is the same, and at
// the two ends it is zero. Solved by the Thomas algorithm (the matrix is diagonally dominant): the
// forward sweep leaves c' in work and d' in velocities, and the back substitution turns d' into v.
void solve(std::span<const double> times, std::span<const double> values, std::span<double> velocities,
           std::span<double> work) {
    const auto n = times.size();
    const auto equation = [&](std::size_t i, double& a, double& b, double& c, double& r) {
        a = c = 0;
        if (i == 0) {
            b = 2;
            c = 1;
            r = 3 * (values[1] - values[0]) / (times[1] - times[0]);
        } else if (i == n - 1) {
            a = 1;
            b = 2;
            r = 3 * (values[i] - values[i - 1]) / (times[i] - times[i - 1]);
        } else {
            const double before = times[i] - times[i - 1], after = times[i + 1] - times[i];
            a = after;
            b = 2 * (before + after);
            c = before;
            r = 3 * (after * (values[i] - values[i - 1]) / before + before * (values[i + 1] - values[i]) / after);
        }
    };
    for (std::size_t i = 0; i < n; ++i) {
        double a{}, b{}, c{}, r{};
        equation(i, a, b, c, r);
        const double pivot = i ? b - a * work[i - 1] : b;
        work[i] = c / pivot;
        velocities[i] = (r - (i ? a * velocities[i - 1] : 0.0)) / pivot;
    }
    for (std::size_t i = n - 1; i-- > 0;)
        velocities[i] -= work[i] * velocities[i + 1];
}
} // namespace

bool c2_velocities(std::span<const double> times, std::span<const double> values, std::span<double> velocities,
                   std::vector<double>& work) {
    const auto n = times.size();
    if (!n || values.size() != n || velocities.size() != n) return false;
    for (std::size_t i = 0; i + 1 < n; ++i)
        if (!(times[i + 1] - times[i] > 1e-9)) return false;
    if (n == 1) {
        velocities[0] = 0;
        return true;
    }
    work.resize(n);
    solve(times, values, velocities, work);
    return true;
}

float blend(float stock, double c2, float smoothness) noexcept {
    if (!(smoothness > 0)) return stock;
    if (smoothness >= 1) return static_cast<float>(c2);
    return static_cast<float>((1.0 - smoothness) * stock + smoothness * c2);
}

SegmentResult smooth_segment(unsigned char* segment, const FramingKeys& keys, std::span<const layout::Lane> lanes,
                             float smoothness, Work& work) {
    constexpr auto slot_floats = layout::slot_floats;
    const auto n = keys.times.size(), from = keys.from;
    if (!segment || lanes.empty() || from + 1 >= n || keys.slots.size() != n * slot_floats)
        return SegmentResult::stock;
    const double gap = keys.times[from + 1] - keys.times[from];
    if (!(gap > layout::tiny_gap)) return SegmentResult::stock;
    if (read_double(segment + layout::segment_from + layout::slot_time) != keys.times[from] ||
        read_double(segment + layout::segment_to + layout::slot_time) != keys.times[from + 1])
        return SegmentResult::mismatch;
    const auto lane_at = [segment](std::size_t slot, std::size_t index) { return segment + slot + index * sizeof(float); };
    const auto key_at = [&](std::size_t key, std::size_t index) { return keys.slots[key * slot_floats + index]; };

    struct Change {
        std::size_t index, k;
        float previous, next;
    };
    std::array<Change, layout::segment_k_count> changes{};
    std::size_t count{};
    work.lane.resize(n);
    work.velocities.resize(n);
    for (const auto lane : lanes) {
        const std::size_t index = lane.slot_float, k = lane.k;
        if (index >= slot_floats || k >= layout::segment_k_count || count == changes.size())
            return SegmentResult::stock;
        const float p0 = read_float(lane_at(layout::segment_previous, index));
        const float p1 = read_float(lane_at(layout::segment_from, index));
        const float p2 = read_float(lane_at(layout::segment_to, index));
        const float p3 = read_float(lane_at(layout::segment_next, index));
        const float k1 = read_float(segment + layout::segment_k_from + k * sizeof(float));
        const float k2 = read_float(segment + layout::segment_k_to + k * sizeof(float));
        if (!same(p1, key_at(from, index)) || !same(p2, key_at(from + 1, index)))
            return SegmentResult::mismatch;
        for (std::size_t key = 0; key < n; ++key) {
            const float value = key_at(key, index);
            if (!std::isfinite(value)) return SegmentResult::stock;
            work.lane[key] = value;
        }
        if (!c2_velocities(keys.times, work.lane, work.velocities, work.solver)) return SegmentResult::stock;
        // The tangents the game's spline makes of the segment as it was built.
        const float m1 = blend((1.0f - k1) * 0.5f * (p2 - p0), work.velocities[from] * gap, smoothness);
        const float m2 = blend((1.0f - k2) * 0.5f * (p3 - p1), work.velocities[from + 1] * gap, smoothness);
        const Change change{index, k, p2 - 2.0f * m1, p1 + 2.0f * m2};
        if (!std::isfinite(change.previous) || !std::isfinite(change.next)) return SegmentResult::stock;
        changes[count++] = change;
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto& change = changes[i];
        write_float(lane_at(layout::segment_previous, change.index), change.previous);
        write_float(lane_at(layout::segment_next, change.index), change.next);
        write_float(segment + layout::segment_k_from + change.k * sizeof(float), 0.0f);
        write_float(segment + layout::segment_k_to + change.k * sizeof(float), 0.0f);
    }
    return SegmentResult::smoothed;
}

std::span<const layout::Lane> framing_lanes(int behaviour) noexcept {
    switch (behaviour) {
    case 0: return layout::behaviour_0_lanes;
    case 1: return layout::behaviour_1_lanes;
    case 2: return layout::behaviour_2_lanes;
    default: return {};
    }
}
} // namespace dingosdk::replay_keyframes
