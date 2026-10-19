#pragma once
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/replay_keyframes.h"
#include <cstddef>
#include <span>
#include <vector>

// The replay keyframes' smoother curve, apart from the game (Extension/ReplayKeyframes/replay_keyframes.h
// says where it goes in). The game's framing track plays a cubic Hermite segment between each pair of
// keys, from the tangents it chooses at the two keys. Smoothness σ chooses other tangents:
//   - at 0, the game's own;
//   - at 1, those of the natural cubic spline through every key, at the keys' real times: position,
//     speed and acceleration all carry on through a key, and the camera never brakes at one;
//   - in between, the two blended.
// Each segment still starts at one key and ends at the next: tangents never move the keys.
namespace dingosdk::replay_keyframes {
namespace layout = addr::replay_keyframes;

// The velocity (per second) at every key of the natural cubic spline through (times[i], values[i]):
// the camera starts and ends without accelerating. One key holds still. False, and velocities
// untouched, when the sizes differ, there are no keys or the times don't rise.
bool c2_velocities(std::span<const double> times, std::span<const double> values, std::span<double> velocities,
                   std::vector<double>& work);

// A segment tangent at smoothness σ: the game's at 0 (exactly), the C2 one at 1, blended between.
float blend(float stock, double c2, float smoothness) noexcept;

// Scratch space, kept between calls so that a rebuild allocates little.
struct Work {
    std::vector<double> lane, velocities, solver;
};

// The framing track's keys as one segment's Behaviour sees them: every key of the session as a slot
// (slot_floats floats each, key after key) and its slot time, angles unwrapped in sequence, and which
// key the segment starts at.
struct FramingKeys {
    std::span<const double> times;
    std::span<const float> slots;
    std::size_t from;
};
enum class SegmentResult { smoothed, stock, mismatch };
// Rewrites a segment the game built (layout::segment_size bytes) so that the game's own player plays
// it with σ's tangents on every lane the Behaviour plays: each tangent goes into the neighbour slots
// (p0 = p2 − 2·m1, p3 = p1 + 2·m2) and its k to 0, so (1 − k)·0.5·(p2 − p0) is m1 again. Left as it
// was (stock) for the last key's segment, a segment too short to play, a lane that isn't finite, or
// (mismatch) when the segment's own from and to don't match the keys.
SegmentResult smooth_segment(unsigned char* segment, const FramingKeys& keys, std::span<const layout::Lane> lanes,
                             float smoothness, Work& work);
// The lanes a Behaviour plays; empty for one the game doesn't have.
std::span<const layout::Lane> framing_lanes(int behaviour) noexcept;
} // namespace dingosdk::replay_keyframes
