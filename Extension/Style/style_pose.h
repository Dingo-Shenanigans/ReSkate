#pragma once
#include "Extension/Style/style_model.h"
#include "Engine/Game/Abi/linear_transform.h"
#include "Engine/Game/Skater/first_person_spring.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/style.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <compare>
#include <cstddef>
#include <cstring>
#include <deque>
#include <map>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dingosdk::style {
using Quat = std::array<float, 4>; // x, y, z, w, as the native pose stores it
using first_person::conjugate;
using first_person::multiply;
using first_person::normalized;
using first_person::rotate;
using first_person::wrap_angle;
inline constexpr float pi = std::numbers::pi_v<float>;
inline constexpr float degrees_per_radian = static_cast<float>(180.0 / std::numbers::pi);
[[nodiscard]] inline float dot(const Quat &a, const Quat &b) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]; }

inline bool finite(const Quat &q) noexcept {
    return std::isfinite(q[0]) && std::isfinite(q[1]) && std::isfinite(q[2]) && std::isfinite(q[3]);
}
// A rotation about the joint's own X, then Y, then Z axis. Each angle is clamped to max_degrees.
inline Quat from_degrees(float x, float y, float z) noexcept {
    const auto axis = [](float degrees, std::size_t index) {
        if (!std::isfinite(degrees)) degrees = 0;
        const float half = std::clamp(degrees, -max_degrees, max_degrees) / degrees_per_radian / 2;
        Quat q{0, 0, 0, std::cos(half)};
        q[index] = std::sin(half);
        return q;
    };
    return normalized(multiply(multiply(axis(x, 0), axis(y, 1)), axis(z, 2)));
}

// The time from the last keyframe back to the game's pose.
inline constexpr float release_ms = 250.0f;
[[nodiscard]] inline float blend_out_ms(float own_ms, bool last) noexcept { return own_ms > 0 ? own_ms : last ? release_ms : 0.0f; }
[[nodiscard]] inline std::size_t key_place(std::span<const float> times, std::size_t key) noexcept {
    std::size_t place{};
    for (std::size_t i = 0; i < times.size(); ++i)
        if (times[i] < times[key] || (times[i] == times[key] && i < key)) ++place;
    return place;
}
inline constexpr float first_key_time = 0.02f, last_key_time = trick_end - 0.02f, key_nudge = 0.01f;
[[nodiscard]] inline float free_key_time(std::span<const float> others, float time) noexcept {
    const auto is_free = [&](float t) { return std::ranges::none_of(others, [&](float other) { return std::abs(other - t) < 0.001f; }); };
    if (is_free(time)) return time;
    for (std::size_t step = 1; step <= max_keys; ++step)
        for (const float t : {time + key_nudge * static_cast<float>(step), time - key_nudge * static_cast<float>(step)})
            if (t >= first_key_time && t <= last_key_time && is_free(t)) return t;
    return time; // Not reached with fewer than max_keys keyframes.
}
[[nodiscard]] inline std::vector<float> blend_outs_of(const BlendOuts &blend_outs, std::uint8_t trick, std::size_t keys) {
    std::vector<float> result(keys, 0.0f);
    for (const auto &[target, ms] : blend_outs)
        if (target.trick && target.id == trick && target.key < keys) result[target.key] = ms;
    return result;
}
// A timeline time (0 to 3) as ms at `pace`.
inline float paced(float time, const Pace &pace) noexcept {
    float result{};
    for (std::size_t part = 0; part < pace.size(); ++part)
        result += std::clamp(time - static_cast<float>(part), 0.0f, 1.0f) * std::max(pace[part], 1e-3f);
    return result;
}

// `joint` is spelled as in editable_joints.
struct RotationKey {
    Target target;
    std::string joint;
    auto operator<=>(const RotationKey &) const = default;
};
// Degrees about each joint's own X, Y and Z axes, for each target.
using Rotations = std::map<RotationKey, std::array<float, 3>>;
struct Style {
    Rotations rotations;
    KeyTimes times;
    BlendOuts blend_outs;
    bool operator==(const Style &) const = default;
    [[nodiscard]] std::vector<float> keys(std::uint8_t trick) const { return keys_of(times, trick); }
};

// In a group, such as one drag, only the first edit makes an undo step.
class History {
public:
    static constexpr std::size_t depth = 100;
    void record(const Style &before, std::string what) {
        if (grouping_ && grouped_) return;
        undo_.push_back({before, std::move(what)});
        if (undo_.size() > depth) undo_.pop_front();
        redo_.clear();
        grouped_ = grouping_;
    }
    void group(bool open) noexcept {
        grouping_ = open;
        grouped_ = false;
    }
    [[nodiscard]] std::optional<Style> undo(const Style &current) { return step(undo_, redo_, current); }
    [[nodiscard]] std::optional<Style> redo(const Style &current) { return step(redo_, undo_, current); }
    [[nodiscard]] const std::string *undo_name() const noexcept { return undo_.empty() ? nullptr : &undo_.back().what; }
    [[nodiscard]] const std::string *redo_name() const noexcept { return redo_.empty() ? nullptr : &redo_.back().what; }
    void clear() noexcept {
        undo_.clear();
        redo_.clear();
        grouping_ = grouped_ = false;
    }

private:
    struct Step {
        Style style;
        std::string what;
    };
    std::optional<Style> step(std::deque<Step> &from, std::deque<Step> &to, const Style &current) {
        grouping_ = grouped_ = false;
        if (from.empty()) return std::nullopt;
        auto taken = std::move(from.back());
        from.pop_back();
        to.push_back({current, taken.what});
        return std::move(taken.style);
    }
    std::deque<Step> undo_, redo_;
    bool grouping_{}, grouped_{};
};

// A rotation of an editable joint: `joint` is its index in editable_joints.
struct JointDelta {
    std::uint8_t joint{};
    Quat rotation{0, 0, 0, 1};
};
// A rotation of a skeleton joint: `joint` is its index in the skater skeleton.
struct SkeletonDelta {
    std::uint16_t joint{};
    Quat rotation{0, 0, 0, 1};
};

inline constexpr Quat identity{0, 0, 0, 1};
inline Quat mix(const Quat &a, Quat b, float amount) noexcept {
    if (dot(a, b) < 0)
        for (auto &value : b) value = -value;
    Quat result;
    for (std::size_t i = 0; i < 4; ++i) result[i] = a[i] + (b[i] - a[i]) * amount;
    return normalized(result);
}
// The inverse of from_degrees. A rotation has two sets of angles: this gives the one with the smaller largest angle, so a tilt
// past 90 degrees does not come back with X and Z past max_degrees.
inline std::array<float, 3> to_degrees(const Quat &q) noexcept {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float tilt = std::clamp(2 * (x * z + y * w), -1.0f, 1.0f);
    const std::array<float, 3> direct{std::atan2(-2 * (y * z - x * w), 1 - 2 * (x * x + y * y)) * degrees_per_radian,
                                      std::asin(tilt) * degrees_per_radian,
                                      std::atan2(-2 * (x * y - z * w), 1 - 2 * (y * y + z * z)) * degrees_per_radian};
    const auto turned = [](float degrees) { return degrees > 0 ? degrees - 180 : degrees + 180; };
    const std::array<float, 3> flipped{turned(direct[0]), (direct[1] < 0 ? -180.0f : 180.0f) - direct[1], turned(direct[2])};
    const auto largest = [](const std::array<float, 3> &angles) {
        return std::max({std::abs(angles[0]), std::abs(angles[1]), std::abs(angles[2])});
    };
    return largest(flipped) < largest(direct) ? flipped : direct;
}
inline float ease_amount(float seconds, float duration) noexcept {
    if (duration <= 0) return 1.0f;
    return seconds <= 0 ? 0.0f : 1.0f - std::exp(-3.0f * seconds / duration);
}

// Eases each joint toward its current rotation, so that a pose does not snap.
class Blender {
public:
    explicit Blender(std::size_t joints) : slots_(joints) {}
    // Joints that are not in `targets` ease back to identity.
    const std::vector<SkeletonDelta> &step(const std::vector<SkeletonDelta> &targets, float amount) {
        ++pass_;
        out_.clear();
        for (const auto &target : targets) {
            auto &slot = slots_.at(target.joint);
            if (!slot.live) {
                slot.live = true;
                slot.rotation = identity;
                live_.push_back(target.joint);
            }
            slot.pass = pass_;
            slot.rotation = mix(slot.rotation, target.rotation, amount);
            out_.push_back({target.joint, slot.rotation});
        }
        std::erase_if(live_, [&](std::uint16_t joint) {
            auto &slot = slots_[joint];
            if (slot.pass == pass_) return false;
            slot.rotation = mix(slot.rotation, identity, amount);
            if (std::abs(slot.rotation[3]) > 0.999999f) {
                slot.live = false;
                return true;
            }
            out_.push_back({joint, slot.rotation});
            return false;
        });
        return out_;
    }

private:
    struct Slot {
        Quat rotation{identity};
        std::uint32_t pass{};
        bool live{};
    };
    std::vector<Slot> slots_;
    std::vector<std::uint16_t> live_;
    std::vector<SkeletonDelta> out_;
    std::uint32_t pass_{};
};

struct Key {
    float time{};
    std::vector<JointDelta> joints;
    float blend_out_ms{}; // 0: blends to the next keyframe
};
inline std::vector<Key> trick_keys(const Style &style, std::uint8_t trick) {
    const auto times = style.keys(trick);
    std::vector<Key> keys(times.size());
    const auto blends = blend_outs_of(style.blend_outs, trick, times.size());
    for (std::size_t i = 0; i < times.size(); ++i) {
        keys[i].time = times[i];
        keys[i].blend_out_ms = blends[i];
    }
    for (const auto &[key, degrees] : style.rotations) {
        if (!key.target.trick || key.target.id != trick || key.target.key >= keys.size()) continue;
        const auto joint = editable_joint(key.joint);
        if (!joint) continue;
        keys[key.target.key].joints.push_back({*joint, from_degrees(degrees[0], degrees[1], degrees[2])});
    }
    std::ranges::stable_sort(keys, {}, &Key::time);
    return keys;
}
// A rotation as its axis times its angle in radians, so that rotations add and scale.
using Turn = std::array<float, 3>;
using first_person::from_rotation_vector;
using first_person::rotation_vector;
// The points of a keyframe curve: each keyframe, and the game's pose at the flick and where a blend out ends.
struct CurvePoints {
    std::array<float, 2 * max_keys + 2> at{};
    std::array<const Key *, 2 * max_keys + 2> point{};
    std::size_t count{};
    void add(float moment, const Key *key) noexcept {
        if (count == at.size()) return;
        at[count] = moment;
        point[count++] = key;
    }
};
// `keys` sorted by time. A keyframe on an end replaces it.
inline CurvePoints curve_points(const std::vector<Key> &keys, const Pace &pace) noexcept {
    CurvePoints points;
    const float end_ms = paced(trick_end, pace);
    if (keys.front().time > 0) points.add(0, nullptr);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const float here = paced(keys[i].time, pace);
        points.add(here, &keys[i]);
        const bool last = i + 1 == keys.size();
        const float blend = blend_out_ms(keys[i].blend_out_ms, last);
        // A blend out that does not end before the next keyframe or the trick's end goes straight to it.
        const float next = last ? end_ms : paced(keys[i + 1].time, pace);
        if (blend > 0 && here + blend < next - 1e-3f) points.add(here + blend, nullptr);
        else if (last && keys[i].time < trick_end) points.add(end_ms, nullptr);
    }
    return points;
}
// `keys` sorted by time. A joint that a keyframe omits has the game's rotation there.
inline void evaluate(const std::vector<Key> &keys, float time, std::vector<JointDelta> &out, const Pace &pace = even_pace) {
    out.clear();
    if (keys.empty()) return;
    const auto points = curve_points(keys, pace);
    const float now = paced(std::clamp(time, 0.0f, trick_end), pace);
    std::size_t from = 0;
    while (from + 2 < points.count && now >= points.at[from + 1]) ++from;
    const std::size_t to = std::min(from + 1, points.count - 1);
    const float span = points.at[to] - points.at[from];
    const float u = span > 1e-6f ? std::clamp((now - points.at[from]) / span, 0.0f, 1.0f) : 1.0f;
    // Cubic Hermite weights for the two points and their slopes.
    const float u2 = u * u, u3 = u2 * u;
    const float start = 2 * u3 - 3 * u2 + 1, start_slope = u3 - 2 * u2 + u, end = 3 * u2 - 2 * u3, end_slope = u3 - u2;
    // The slope at a point on one axis: zero at the ends and where the joint turns back (Fritsch-Butland).
    const auto slope = [&](std::size_t index, float before, float here, float after) {
        if (index == 0 || index + 1 >= points.count) return 0.0f;
        const float left = points.at[index] - points.at[index - 1], right = points.at[index + 1] - points.at[index];
        if (left < 1e-6f || right < 1e-6f) return 0.0f;
        const float in = (here - before) / left, onward = (after - here) / right;
        if (in * onward <= 0) return 0.0f;
        const float w1 = 2 * right + left, w2 = right + 2 * left;
        return (w1 + w2) / (w1 / in + w2 / onward);
    };
    const auto turn_at = [&](std::size_t index, std::uint8_t joint) {
        if (index < points.count && points.point[index])
            for (const auto &delta : points.point[index]->joints)
                if (delta.joint == joint) return rotation_vector(delta.rotation);
        return Turn{};
    };
    const auto curve = [&](std::uint8_t joint) {
        if (std::ranges::find(out, joint, &JointDelta::joint) != out.end()) return;
        const std::array<Turn, 4> around{from ? turn_at(from - 1, joint) : Turn{}, turn_at(from, joint), turn_at(to, joint),
                                         turn_at(to + 1, joint)};
        Turn turn;
        for (std::size_t axis = 0; axis < 3; ++axis)
            turn[axis] = start * around[1][axis] + end * around[2][axis] +
                         span * (start_slope * slope(from, around[0][axis], around[1][axis], around[2][axis]) +
                                 end_slope * slope(to, around[1][axis], around[2][axis], around[3][axis]));
        out.push_back({joint, from_rotation_vector(turn)});
    };
    for (const auto index : {from, to})
        if (points.point[index])
            for (const auto &delta : points.point[index]->joints) curve(delta.joint);
}

// Follows a flip trick along its timeline. The last measured pop and fall durations set the pace.
class TrickTracker {
public:
    static constexpr std::uint64_t landing_ms = 450;
    struct Moment {
        std::uint8_t trick{}; // 0: none
        float time{};
    };
    static constexpr std::uint64_t longest_ms = 4000; // a pop or a fall is always shorter
    // `flip`: the game's trick number, below 1 for none. `valid`: a style applies to the state. `riding`: on the board, no grind.
    Moment step(int flip, bool grounded, bool valid, bool riding, std::uint64_t now_ms) noexcept {
        const bool named = is_flip_trick(flip);
        if (named && (!trick_ || part_ != Part::pop || trick_ != flip)) {
            trick_ = static_cast<std::uint8_t>(flip);
            part_ = Part::pop;
            since_ = now_ms;
        } else if (trick_ && !named) {
            if (part_ == Part::pop) advance(pop_ms_[trick_], now_ms);
            if (!valid) trick_ = 0;
            // A grind or a step off the board is the landing of this trick.
            else if ((grounded || !riding) && part_ == Part::fall) advance(fall_ms_[trick_], now_ms);
        }
        if (trick_ && part_ == Part::landing && now_ms - since_ > landing_ms) trick_ = 0;
        if (trick_ && part_ != Part::landing && now_ms - since_ > longest_ms) trick_ = 0;
        if (!trick_) return {};
        const float expected =
            part_ == Part::pop ? pop_ms_[trick_] : part_ == Part::fall ? fall_ms_[trick_] : static_cast<float>(landing_ms);
        const float within = std::min(static_cast<float>(now_ms - since_) / expected, part_ == Part::landing ? 1.0f : 0.999f);
        return {trick_, part_start(part_) + within};
    }
    [[nodiscard]] Pace pace(std::uint8_t trick) const noexcept {
        return trick < pop_ms_.size() ? Pace{pop_ms_[trick], fall_ms_[trick], static_cast<float>(landing_ms)} : even_pace;
    }

private:
    void advance(float &learned, std::uint64_t now_ms) noexcept {
        if (now_ms > since_) learned = std::clamp(static_cast<float>(now_ms - since_), 80.0f, 1500.0f);
        part_ = next_part(part_);
        since_ = now_ms;
    }
    std::uint8_t trick_{};
    Part part_{};
    std::uint64_t since_{};
    std::array<float, flip_trick_count> pop_ms_ = filled(300.0f), fall_ms_ = filled(250.0f);
    static constexpr std::array<float, flip_trick_count> filled(float value) {
        std::array<float, flip_trick_count> result{};
        for (auto &entry : result) entry = value;
        return result;
    }
};

// Stores the last write to each joint. The game does not rewrite the pose on every update, so no joint is adjusted twice.
class PoseTracker {
public:
    explicit PoseTracker(std::size_t joints) : slots_(joints) {}
    void begin() noexcept { ++pass_; }
    Quat adjust(std::uint16_t joint, const Quat &current, const Quat &delta) {
        auto &slot = slots_.at(joint);
        const bool ours = slot.valid && same(current, slot.written);
        if (!ours) slot.base = current;
        if (!slot.valid) live_.push_back(joint);
        slot.valid = true;
        slot.pass = pass_;
        slot.written = normalized(multiply(slot.base, delta));
        return slot.written;
    }
    // Restores each joint not adjusted since begin() whose entry still holds our write; an unreadable joint is dropped.
    template <class Current, class Restore> void end(Current &&current, Restore &&restore) noexcept {
        std::erase_if(live_, [&](std::uint16_t joint) noexcept {
            auto &slot = slots_[joint];
            if (slot.pass == pass_) return false;
            if (const std::optional<Quat> now = current(joint); now && same(*now, slot.written)) restore(joint, slot.base);
            slot.valid = false;
            return true;
        });
    }
    // Forgets every adjusted joint without restoring it, for a pose that is gone.
    void clear() noexcept {
        for (const auto joint : live_) slots_[joint].valid = false;
        live_.clear();
    }
    // The game's own rotation of a joint being adjusted, while the pose entry `current` still holds the written rotation; else null.
    [[nodiscard]] const Quat *base(std::uint16_t joint, const Quat &current) const noexcept {
        return joint < slots_.size() && slots_[joint].valid && same(current, slots_[joint].written) ? &slots_[joint].base : nullptr;
    }
    [[nodiscard]] const std::vector<std::uint16_t> &adjusted() const noexcept { return live_; }

private:
    struct Slot {
        Quat base{}, written{};
        std::uint32_t pass{};
        bool valid{};
    };
    static bool same(const Quat &a, const Quat &b) noexcept { return !std::memcmp(a.data(), b.data(), sizeof(Quat)); }
    std::vector<Slot> slots_;
    std::vector<std::uint16_t> live_;
    std::uint32_t pass_{};
};
enum class RigKind : std::uint8_t { other, skater, board };
// A board's rig has 2 to 32 joints. No rig is read past 512 joints.
inline constexpr std::size_t max_board_joints = 32, max_rig_joints = 512;
[[nodiscard]] constexpr RigKind rig_kind(std::size_t joints) noexcept {
    if (joints == addr::style::skeleton_joints) return RigKind::skater;
    if (joints >= 2 && joints <= max_board_joints) return RigKind::board;
    return RigKind::other;
}

// The editor's helpers: distances, the skater's and the board's joints, and the keyframe lookups of its timeline.
using Vec3 = game::Vec3;
[[nodiscard]] inline float distance(const Vec3 &a, const Vec3 &b) noexcept { return game::length({a[0] - b[0], a[1] - b[1], a[2] - b[2]}); }
// x and z only; y is up.
[[nodiscard]] inline float ground_distance(const Vec3 &a, const Vec3 &b) noexcept { return std::hypot(a[0] - b[0], a[2] - b[2]); }
// The skater joint with the world position (AITrajectory).
inline constexpr std::size_t skater_world = 1;
// A board pose: the board entity at 0, then its rig joints; the anchor is pose index 2. In the game board rig, the deck is joint 1.
inline constexpr std::size_t board_entity = 0, board_anchor = 2, rig_deck = 1;
// Last in time, not by number.
[[nodiscard]] inline bool last_key(std::span<const float> times, std::size_t key) noexcept {
    return std::ranges::none_of(times, [&](float time) { return time > times[key]; });
}
[[nodiscard]] inline float next_key_time(std::span<const float> times, float at) noexcept {
    float next = trick_end;
    for (const auto time : times)
        if (time > at && time < next) next = time;
    return next;
}
[[nodiscard]] inline float previous_key_time(std::span<const float> times, float at) noexcept {
    float previous = 0;
    for (const auto time : times)
        if (time < at && time > previous) previous = time;
    return previous;
}
[[nodiscard]] inline std::optional<std::size_t> key_at_time(std::span<const float> times, float at) noexcept {
    for (std::size_t i = 0; i < times.size(); ++i)
        if (std::abs(times[i] - at) < 0.001f) return i;
    return std::nullopt;
}
// The ID to select after keyframe `key` is deleted: the keyframe before it in time, else the earliest one left.
[[nodiscard]] inline std::optional<std::size_t> selection_after_delete(std::span<const float> times, std::size_t key) noexcept {
    if (times.size() < 2) return std::nullopt;
    const auto place = key_place(times, key);
    const auto wanted = place ? place - 1 : 1;
    for (std::size_t i = 0; i < times.size(); ++i)
        if (key_place(times, i) == wanted) return i > key ? i - 1 : i;
    return std::nullopt;
}
// The inverse of paced.
inline float timeline_at(float ms, const Pace &pace) noexcept {
    float time{};
    for (std::size_t part = 0; part < pace.size(); ++part) {
        const float length = std::max(pace[part], 1e-3f);
        time += std::clamp(ms / length, 0.0f, 1.0f);
        ms -= length;
    }
    return time;
}
} // namespace dingosdk::style
