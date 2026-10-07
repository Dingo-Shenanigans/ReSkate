#pragma once
// Style layer maths: joint rotations added on top of the pose the game's animation evaluated.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <compare>
#include <cstring>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::style {
using Quat = std::array<float, 4>; // x, y, z, w, as the native pose stores it

// The physics state family that an adjustment applies to.
enum class Family : std::uint8_t { riding, grind, offboard, count };
inline constexpr std::size_t family_count = static_cast<std::size_t>(Family::count);
inline constexpr std::array<std::string_view, family_count> family_names{"riding", "grind", "offboard"};

// The game's flip tricks in the game's numbering (FlipTrickType). The n* names are nollie tricks.
inline constexpr std::array<std::string_view, 33> flip_trick_names{
    "none", "ollie", "kickflip", "heelflip", "popshuvit", "varialkickflip", "inwardheelflip", "fspopshuvit",
    "varialheelflip", "hardflip", "360popshuvit", "360flip", "360inwardheelflip", "fs360popshuvit", "laserflip",
    "360hardflip", "nollie", "nkickflip", "nheelflip", "npopshuvit", "nhardflip", "nvarialheelflip", "nfspopshuvit",
    "nvarialkickflip", "ninwardheelflip", "nfs360popshuvit", "n360hardflip", "nlaserflip", "n360popshuvit", "n360flip",
    "n360inwardheelflip", "olliepop", "nolliepop"};
inline constexpr std::array<std::string_view, 33> flip_trick_titles{
    "None", "Ollie", "Kickflip", "Heelflip", "Pop Shuvit", "Varial Kickflip", "Inward Heelflip", "FS Pop Shuvit",
    "Varial Heelflip", "Hardflip", "360 Pop Shuvit", "360 Flip", "360 Inward Heelflip", "FS 360 Pop Shuvit", "Laserflip",
    "360 Hardflip", "Nollie", "Nollie Kickflip", "Nollie Heelflip", "Nollie Pop Shuvit", "Nollie Hardflip",
    "Nollie Varial Heelflip", "Nollie FS Pop Shuvit", "Nollie Varial Kickflip", "Nollie Inward Heelflip",
    "Nollie FS 360 Pop Shuvit", "Nollie 360 Hardflip", "Nollie Laserflip", "Nollie 360 Pop Shuvit", "Nollie 360 Flip",
    "Nollie 360 Inward Heelflip", "Quick Ollie", "Quick Nollie"};
// A flip trick's timeline: 0 is the flick, 1 the catch, 2 the touchdown, 3 the end of the landing.
inline constexpr float trick_end = 3.0f;
inline constexpr std::size_t max_keys = 12;
// The target of a rotation: a state family, or one keyframe of one flip trick.
struct Target {
    bool trick{};
    std::uint8_t id{};
    std::uint8_t key{};
    auto operator<=>(const Target &) const = default;
};
// Equal when ASCII case is ignored. Preset names, trick names and joint names compare this way.
inline bool same_text(std::string_view a, std::string_view b) noexcept {
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
    return std::ranges::equal(a, b, [&](char x, char y) { return lower(x) == lower(y); });
}
inline std::optional<Target> parse_target(std::string_view name) noexcept {
    for (std::size_t i = 0; i < family_names.size(); ++i)
        if (same_text(family_names[i], name)) return Target{false, static_cast<std::uint8_t>(i)};
    for (std::size_t i = 1; i < flip_trick_names.size(); ++i)
        if (same_text(flip_trick_names[i], name)) return Target{true, static_cast<std::uint8_t>(i)};
    return std::nullopt;
}
// A preset's name is its file name on every computer: ASCII letters, digits, '-' and '_', and not a Windows device name.
inline bool preset_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 40) return false;
    for (const char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    for (const std::string_view device : {"con", "prn", "aux", "nul"})
        if (same_text(name, device)) return false;
    return !(name.size() == 4 && (same_text(name.substr(0, 3), "com") || same_text(name.substr(0, 3), "lpt")) && name[3] >= '0' && name[3] <= '9');
}

// The joints of Animation/Dingo/AnimBase_Default_Skeleton that a style can rotate.
inline constexpr std::array<std::string_view, 24> editable_joints{
    "Hips", "Spine", "Spine1", "Spine2", "Spine3", "Neck", "Neck1", "Head",
    "LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand", "RightShoulder", "RightArm", "RightForeArm", "RightHand",
    "LeftUpLeg", "LeftLeg", "LeftFoot", "LeftToeBase", "RightUpLeg", "RightLeg", "RightFoot", "RightToeBase"};
inline constexpr float max_degrees = 120.0f;

inline Quat multiply(const Quat &a, const Quat &b) noexcept {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}
inline bool finite(const Quat &q) noexcept {
    return std::isfinite(q[0]) && std::isfinite(q[1]) && std::isfinite(q[2]) && std::isfinite(q[3]);
}
inline Quat normalized(Quat q) noexcept {
    const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!std::isfinite(length) || length < 1e-6f) return {0, 0, 0, 1};
    for (auto &value : q) value /= length;
    return q;
}
// A rotation about the joint's own X, then Y, then Z axis. Each angle is clamped to max_degrees.
inline Quat from_degrees(float x, float y, float z) noexcept {
    const auto axis = [](float degrees, std::size_t index) {
        if (!std::isfinite(degrees)) degrees = 0;
        const float half = std::clamp(degrees, -max_degrees, max_degrees) * 0.00872664626f;
        Quat q{0, 0, 0, std::cos(half)};
        q[index] = std::sin(half);
        return q;
    };
    return normalized(multiply(multiply(axis(x, 0), axis(y, 1)), axis(z, 2)));
}

// The length of a trick's pop, fall and landing in milliseconds. Keyframes move at an even speed in real time, not in timeline parts.
using Pace = std::array<float, 3>;
inline constexpr Pace even_pace{500.0f, 500.0f, 500.0f};
// The time from the last keyframe back to the game's pose.
inline constexpr float release_ms = 250.0f;
// A timeline time (0 to 3) as time at `pace`.
inline float paced(float time, const Pace &pace) noexcept {
    float result{};
    for (std::size_t part = 0; part < pace.size(); ++part)
        result += std::clamp(time - static_cast<float>(part), 0.0f, 1.0f) * std::max(pace[part], 1e-3f);
    return result;
}
// The inverse of paced: the timeline time at `ms` into the trick.
inline float timeline_at(float ms, const Pace &pace) noexcept {
    float time{};
    for (std::size_t part = 0; part < pace.size(); ++part) {
        const float length = std::max(pace[part], 1e-3f);
        time += std::clamp(ms / length, 0.0f, 1.0f);
        ms -= length;
    }
    return time;
}

// Degrees about each joint's own X, Y and Z axes, for each target.
using Rotations = std::map<std::pair<Target, std::string>, std::array<float, 3>>;
// The timeline time of each keyframe of each flip trick, by key number.
using KeyTimes = std::map<std::uint8_t, std::vector<float>>;
// The blend out of each keyframe that has one: the ms from the keyframe back to the game's pose. Others blend to the next keyframe.
using BlendOuts = std::map<Target, float>;
inline constexpr float min_blend_out_ms = 50.0f, max_blend_out_ms = 2000.0f;
struct Style {
    Rotations rotations;
    KeyTimes times;
    BlendOuts blend_outs;
    bool operator==(const Style &) const = default;
    // A trick starts with no keyframes.
    [[nodiscard]] std::vector<float> keys(std::uint8_t trick) const {
        const auto found = times.find(trick);
        return found != times.end() ? found->second : std::vector<float>{};
    }
};

// The edits to undo and redo. In a group, such as one drag, only the first edit makes a step.
class History {
public:
    static constexpr std::size_t depth = 100;
    // `before` is the style before the edit named `what`.
    void record(const Style &before, std::string what) {
        if (grouping_ && grouped_) return;
        undo_.push_back({before, std::move(what)});
        if (undo_.size() > depth) undo_.pop_front();
        redo_.clear();
        grouped_ = grouping_;
    }
    void group(bool open) noexcept { grouping_ = open, grouped_ = false; }
    // The style to show instead of `current`, or nothing.
    [[nodiscard]] std::optional<Style> undo(const Style &current) { return step(undo_, redo_, current); }
    [[nodiscard]] std::optional<Style> redo(const Style &current) { return step(redo_, undo_, current); }
    [[nodiscard]] const std::string *undo_name() const noexcept { return undo_.empty() ? nullptr : &undo_.back().what; }
    [[nodiscard]] const std::string *redo_name() const noexcept { return redo_.empty() ? nullptr : &redo_.back().what; }
    void clear() noexcept {
        undo_.clear(), redo_.clear();
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

// What the menus show of the style layer.
struct StyleRotation {
    Target target;
    std::uint8_t joint{}; // index into editable_joints
    std::array<float, 3> degrees{};
    bool operator==(const StyleRotation &) const = default;
};
struct StyleModel {
    bool enabled{}, share{true}, saved{true};
    bool auto_save{true}, unsaved{};  // unsaved: without auto save, edits that are not in the file
    std::string save_issue;           // why the preset could not be read or saved. Empty: no problem
    std::string undo_name, redo_name; // the edit that undo or redo changes. Empty: none
    std::uint8_t preview{}; // the previewed flip trick, or 0
    float preview_time{};
    bool preview_playing{};
    bool editor_session_test{}; // the editor may open in a multiplayer session: a test switch
    // Presets: named style files. `preset` is the one in use.
    std::string preset;
    std::vector<std::string> presets;
    std::vector<StyleRotation> rotations;
    KeyTimes times;
    BlendOuts blend_outs;
    Pace pace{even_pace}; // the shown clip's pop, fall and landing
    std::uint64_t clips{}; // the tricks that have a clip, one bit for each trick
    std::string editor_note; // the last message from the editor
    bool operator==(const StyleModel &) const = default;
};

struct JointDelta {
    std::uint16_t joint{};
    Quat rotation{0, 0, 0, 1};
};

inline constexpr Quat identity{0, 0, 0, 1};
// `a` moved by `amount` (0 to 1) toward `b` on the shorter arc.
inline Quat mix(const Quat &a, Quat b, float amount) noexcept {
    if (a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0)
        for (auto &value : b) value = -value;
    Quat result;
    for (std::size_t i = 0; i < 4; ++i) result[i] = a[i] + (b[i] - a[i]) * amount;
    return normalized(result);
}
// The inverse of from_degrees: angles about X, then Y, then Z.
inline std::array<float, 3> to_degrees(const Quat &q) noexcept {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float tilt = std::clamp(2 * (x * z + y * w), -1.0f, 1.0f);
    constexpr float degrees = 57.2957795f;
    return {std::atan2(-2 * (y * z - x * w), 1 - 2 * (x * x + y * y)) * degrees, std::asin(tilt) * degrees,
            std::atan2(-2 * (x * y - z * w), 1 - 2 * (y * y + z * z)) * degrees};
}
// The blend fraction after `seconds`, for a blend of approximately `duration`.
inline float ease_amount(float seconds, float duration) noexcept {
    if (duration <= 0) return 1.0f;
    return seconds <= 0 ? 0.0f : 1.0f - std::exp(-3.0f * seconds / duration);
}

// Eases each joint toward its current rotation, so that a pose does not snap.
class Blender {
public:
    explicit Blender(std::size_t joints) : slots_(joints) {}
    // The rotations to write now: `targets` approached by `amount`, and joints that ease back to identity.
    const std::vector<JointDelta> &step(const std::vector<JointDelta> &targets, float amount) {
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
    std::vector<JointDelta> out_;
    std::uint32_t pass_{};
};

// One keyframe as the layer applies it.
struct Key {
    float time{};
    std::vector<JointDelta> joints;
    float blend_out_ms{}; // 0: blends to the next keyframe
};
// A rotation as its axis times its angle in radians, so that rotations add and scale.
using Turn = std::array<float, 3>;
inline Turn turn_of(Quat q) noexcept {
    if (q[3] < 0)
        for (auto &value : q) value = -value;
    const float sine = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
    const float scale = sine < 1e-7f ? 2.0f : 2.0f * std::atan2(sine, q[3]) / sine;
    return {q[0] * scale, q[1] * scale, q[2] * scale};
}
inline Quat quat_of(const Turn &turn) noexcept {
    const float angle = std::sqrt(turn[0] * turn[0] + turn[1] * turn[1] + turn[2] * turn[2]);
    if (angle < 1e-7f) return normalized({turn[0] * 0.5f, turn[1] * 0.5f, turn[2] * 0.5f, 1.0f});
    const float scale = std::sin(angle * 0.5f) / angle;
    return {turn[0] * scale, turn[1] * scale, turn[2] * scale, std::cos(angle * 0.5f)};
}
// The rotations at `time` for keyframes sorted by time. A joint that a keyframe omits has the game's rotation there.
// The curve goes through each keyframe with no corner, and slows to a stop where a joint turns back, so it does not overshoot.
inline void evaluate(const std::vector<Key> &keys, float time, std::vector<JointDelta> &out, const Pace &pace = even_pace) {
    out.clear();
    if (keys.empty()) return;
    // The points of the curve: each keyframe, and the game's pose at the flick and where a keyframe's blend out ends.
    // A keyframe on an end replaces it. The last keyframe blends out in release_ms if it does not set a time.
    std::array<float, 2 * max_keys + 2> at{};
    std::array<const Key *, 2 * max_keys + 2> point{};
    std::size_t count{};
    const auto add = [&](float moment, const Key *key) {
        if (count < at.size()) at[count] = moment, point[count++] = key;
    };
    const float end_ms = paced(trick_end, pace);
    if (keys.front().time > 0) add(0, nullptr);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const float here = paced(keys[i].time, pace);
        add(here, &keys[i]);
        const bool last = i + 1 == keys.size();
        const float blend = keys[i].blend_out_ms > 0 ? keys[i].blend_out_ms : last ? release_ms : 0.0f;
        // A blend out that does not end before the next keyframe or the trick's end goes straight to it.
        const float next = last ? end_ms : paced(keys[i + 1].time, pace);
        if (blend > 0 && here + blend < next - 1e-3f) add(here + blend, nullptr);
        else if (last && keys[i].time < trick_end) add(end_ms, nullptr);
    }
    const float now = paced(std::clamp(time, 0.0f, trick_end), pace);
    std::size_t from = 0;
    while (from + 2 < count && now >= at[from + 1]) ++from;
    const std::size_t to = std::min(from + 1, count - 1);
    const float span = at[to] - at[from];
    const float u = span > 1e-6f ? std::clamp((now - at[from]) / span, 0.0f, 1.0f) : 1.0f;
    // Cubic Hermite weights for the two points and their slopes.
    const float u2 = u * u, u3 = u2 * u;
    const float start = 2 * u3 - 3 * u2 + 1, start_slope = u3 - 2 * u2 + u, end = 3 * u2 - 2 * u3, end_slope = u3 - u2;
    // The slope at a point on one axis: zero at the ends and where the joint turns back (Fritsch-Butland).
    const auto slope = [&](std::size_t index, float before, float here, float after) {
        if (index == 0 || index + 1 >= count) return 0.0f;
        const float left = at[index] - at[index - 1], right = at[index + 1] - at[index];
        if (left < 1e-6f || right < 1e-6f) return 0.0f;
        const float in = (here - before) / left, onward = (after - here) / right;
        if (in * onward <= 0) return 0.0f;
        const float w1 = 2 * right + left, w2 = right + 2 * left;
        return (w1 + w2) / (w1 / in + w2 / onward);
    };
    const auto turn_at = [&](std::size_t index, std::uint16_t joint) {
        if (index < count && point[index])
            for (const auto &delta : point[index]->joints)
                if (delta.joint == joint) return turn_of(delta.rotation);
        return Turn{};
    };
    const auto curve = [&](std::uint16_t joint) {
        if (std::ranges::find(out, joint, &JointDelta::joint) != out.end()) return;
        // The joint's rotation at the point before `from`, at `from`, at `to` and at the point after `to`.
        const std::array<Turn, 4> around{from ? turn_at(from - 1, joint) : Turn{}, turn_at(from, joint), turn_at(to, joint), turn_at(to + 1, joint)};
        Turn turn;
        for (std::size_t axis = 0; axis < 3; ++axis)
            turn[axis] = start * around[1][axis] + end * around[2][axis] +
                         span * (start_slope * slope(from, around[0][axis], around[1][axis], around[2][axis]) +
                                 end_slope * slope(to, around[1][axis], around[2][axis], around[3][axis]));
        out.push_back({joint, quat_of(turn)});
    };
    for (const auto index : {from, to})
        if (point[index])
            for (const auto &delta : point[index]->joints) curve(delta.joint);
}

// Follows one flip trick along its timeline from the game's trick number and ground state. The last pop and fall durations set the pace.
class TrickTracker {
public:
    static constexpr std::uint64_t landing_ms = 450;
    struct Moment {
        std::uint8_t trick{}; // 0: none
        float time{};         // on the trick's timeline
    };
    static constexpr std::uint64_t longest_ms = 4000; // a pop or a fall is always shorter
    // `flip`: the game's trick number, below 1 for none. `valid`: a style applies to the state. `riding`: on the board, no grind.
    Moment step(int flip, bool grounded, bool valid, bool riding, std::uint64_t now_ms) noexcept {
        const bool named = flip >= 1 && flip < static_cast<int>(flip_trick_names.size());
        if (named && (!trick_ || part_ != 0 || trick_ != flip)) {
            trick_ = static_cast<std::uint8_t>(flip);
            part_ = 0;
            since_ = now_ms;
        } else if (trick_ && !named) {
            if (part_ == 0) advance(pop_ms_[trick_], now_ms);
            if (!valid) trick_ = 0;
            // A grind or a step off the board is the landing of this trick.
            else if ((grounded || !riding) && part_ == 1) advance(fall_ms_[trick_], now_ms);
        }
        if (trick_ && part_ == 2 && now_ms - since_ > landing_ms) trick_ = 0;
        if (trick_ && part_ < 2 && now_ms - since_ > longest_ms) trick_ = 0;
        if (!trick_) return {};
        const float expected = part_ == 0 ? pop_ms_[trick_] : part_ == 1 ? fall_ms_[trick_] : static_cast<float>(landing_ms);
        const float within = std::min(static_cast<float>(now_ms - since_) / expected, part_ == 2 ? 1.0f : 0.999f);
        return {trick_, static_cast<float>(part_) + within};
    }
    // The last measured lengths of this trick's pop and fall, and the landing, in milliseconds.
    [[nodiscard]] Pace pace(std::uint8_t trick) const noexcept {
        return trick < pop_ms_.size() ? Pace{pop_ms_[trick], fall_ms_[trick], static_cast<float>(landing_ms)} : even_pace;
    }

private:
    // Moves to the next part of the timeline and stores the duration of this part.
    void advance(float &learned, std::uint64_t now_ms) noexcept {
        if (now_ms > since_) learned = std::clamp(static_cast<float>(now_ms - since_), 80.0f, 1500.0f);
        ++part_;
        since_ = now_ms;
    }
    std::uint8_t trick_{}, part_{};
    std::uint64_t since_{};
    std::array<float, flip_trick_names.size()> pop_ms_ = filled(300.0f), fall_ms_ = filled(250.0f);
    static constexpr std::array<float, flip_trick_names.size()> filled(float value) {
        std::array<float, flip_trick_names.size()> result{};
        for (auto &entry : result) entry = value;
        return result;
    }
};

// Shown flip trick frames, kept so that a replay is recognised by its pose.
inline constexpr std::array<std::string_view, 8> signature_joints{"Hips",    "Spine1",   "LeftUpLeg", "RightUpLeg",
                                                                  "LeftLeg", "RightLeg", "LeftArm",   "RightArm"};
using Signature = std::array<Quat, signature_joints.size()>;
// 0 for the same pose. Approximately 0.0003 for each joint that is 2 degrees off.
inline float difference(const Signature &a, const Signature &b) noexcept {
    float total{};
    for (std::size_t i = 0; i < a.size(); ++i)
        total += 1.0f - std::abs(a[i][0] * b[i][0] + a[i][1] * b[i][1] + a[i][2] * b[i][2] + a[i][3] * b[i][3]);
    return total;
}
struct TakeFrame {
    std::uint8_t trick{};
    float time{};
    Signature shown{};
    std::vector<JointDelta> written;
};
class Takes {
public:
    static constexpr std::size_t capacity = 20000; // several minutes of continuous tricks
    static constexpr float tolerance = 0.003f;
    void add(TakeFrame frame) {
        if (frames_.size() >= capacity) frames_.pop_front();
        frames_.push_back(std::move(frame));
    }
    [[nodiscard]] std::size_t size() const noexcept { return frames_.size(); }
    // The recorded frame that showed this pose, or null. `distance` gets the distance of the nearest frame.
    const TakeFrame *find(const Signature &shown, float *distance = nullptr) {
        std::size_t best = frames_.size();
        float least = 1e9f;
        const auto search = [&](std::size_t from, std::size_t to) {
            for (std::size_t i = from; i < to; ++i)
                if (const float d = difference(frames_[i].shown, shown); d < least) least = d, best = i;
        };
        // Playback moves one frame at a time, so search near the last match first.
        const std::size_t around = std::min(last_, frames_.size());
        search(around > 120 ? around - 120 : 0, std::min(frames_.size(), around + 120));
        // After a full search that found nothing, only every 8th call searches all frames: a skater that is no replay costs little.
        if (least > tolerance && (!unmatched_ || ++skipped_ % 8 == 0)) {
            search(0, frames_.size());
            unmatched_ = least > tolerance;
        }
        if (distance) *distance = least;
        if (best >= frames_.size() || least > tolerance) return nullptr;
        last_ = best;
        unmatched_ = false;
        return &frames_[best];
    }

private:
    std::deque<TakeFrame> frames_;
    std::size_t last_{}, skipped_{};
    bool unmatched_{};
};
// The rotations that change a frame shown with `before` into a frame shown with `now`.
inline void restyle(const std::vector<JointDelta> &before, const std::vector<JointDelta> &now, std::vector<JointDelta> &out) {
    out.clear();
    const auto undo = [](const Quat &q) { return Quat{-q[0], -q[1], -q[2], q[3]}; };
    for (const auto &old : before) {
        const auto same = std::ranges::find(now, old.joint, &JointDelta::joint);
        out.push_back({old.joint, normalized(multiply(undo(old.rotation), same != now.end() ? same->rotation : identity))});
    }
    for (const auto &added : now)
        if (std::ranges::find(before, added.joint, &JointDelta::joint) == before.end()) out.push_back(added);
}
// The position of a replay or a clip on a flip trick's timeline, for the menu playhead.
struct Playhead {
    std::uint8_t trick{}; // 0: no recognised trick on screen
    float time{};
    bool editor{};  // the stand-in shows a clip of this trick
    bool playing{}; // the clip plays and is not held
    bool wanted{};  // the editor screen is wanted, with or without a shown clip
};

// Stores the last write to each joint. The game does not rewrite the pose on every update, so no joint is adjusted twice.
class PoseTracker {
public:
    explicit PoseTracker(std::size_t joints) : slots_(joints) {}
    void begin() noexcept { ++pass_; }
    // The rotation to write for `joint`, whose pose entry now holds `current`.
    Quat adjust(std::uint16_t joint, const Quat &current, const Quat &delta, bool *reused = nullptr) {
        auto &slot = slots_.at(joint);
        const bool ours = slot.valid && same(current, slot.written);
        if (reused) *reused = ours;
        if (!ours) slot.base = current;
        if (!slot.valid) live_.push_back(joint);
        slot.valid = true;
        slot.pass = pass_;
        slot.written = normalized(multiply(slot.base, delta));
        return slot.written;
    }
    // For each joint not adjusted since begin(), calls restore(joint, base) if its entry still holds our write.
    template <class Current, class Restore> void end(Current &&current, Restore &&restore) {
        std::erase_if(live_, [&](std::uint16_t joint) {
            auto &slot = slots_[joint];
            if (slot.pass == pass_) return false;
            if (same(current(joint), slot.written)) restore(joint, slot.base);
            slot.valid = false;
            return true;
        });
    }
    // The game's own rotation of a joint being adjusted, or null.
    [[nodiscard]] const Quat *base(std::uint16_t joint) const noexcept {
        return joint < slots_.size() && slots_[joint].valid ? &slots_[joint].base : nullptr;
    }
    // The same, but only while the pose entry `current` still holds the written rotation.
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
} // namespace dingosdk::style
