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
inline std::optional<Target> parse_target(std::string_view name) noexcept {
    const auto same = [&](std::string_view known) {
        return std::ranges::equal(known, name, [](char a, char b) { return a == (b >= 'A' && b <= 'Z' ? b + 32 : b); });
    };
    for (std::size_t i = 0; i < family_names.size(); ++i)
        if (same(family_names[i])) return Target{false, static_cast<std::uint8_t>(i)};
    for (std::size_t i = 1; i < flip_trick_names.size(); ++i)
        if (same(flip_trick_names[i])) return Target{true, static_cast<std::uint8_t>(i)};
    return std::nullopt;
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

// Degrees about each joint's own X, Y and Z axes, for each target.
using Rotations = std::map<std::pair<Target, std::string>, std::array<float, 3>>;
// The timeline time of each keyframe of each flip trick, by key number.
using KeyTimes = std::map<std::uint8_t, std::vector<float>>;
struct Style {
    Rotations rotations;
    KeyTimes times;
    bool operator==(const Style &) const = default;
    // A trick starts with no keyframes.
    [[nodiscard]] std::vector<float> keys(std::uint8_t trick) const {
        const auto found = times.find(trick);
        return found != times.end() ? found->second : std::vector<float>{};
    }
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
    std::uint8_t preview{}; // the previewed flip trick, or 0
    float preview_time{};
    bool preview_playing{};
    bool editor_session_test{}; // the editor may open in a multiplayer session: a test switch
    std::string status;
    // Presets: named style files. `preset` is the one in use.
    std::string preset;
    std::vector<std::string> presets;
    std::vector<StyleRotation> rotations;
    KeyTimes times;
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
};
// The rotations at `time` for keyframes sorted by time. A joint that a keyframe omits has the game's rotation there.
inline void evaluate(const std::vector<Key> &keys, float time, std::vector<JointDelta> &out) {
    out.clear();
    if (keys.empty()) return;
    const auto after = std::ranges::find_if(keys, [&](const Key &key) { return key.time > time; });
    const Key *next = after != keys.end() ? &*after : nullptr;
    const Key *previous = after != keys.begin() ? &*(after - 1) : nullptr;
    const float from = previous ? previous->time : 0.0f, to = next ? next->time : trick_end;
    const float amount = to > from ? std::clamp((time - from) / (to - from), 0.0f, 1.0f) : 1.0f;
    const auto rotation_in = [](const Key *key, std::uint16_t joint) {
        if (key)
            for (const auto &delta : key->joints)
                if (delta.joint == joint) return delta.rotation;
        return identity;
    };
    if (previous)
        for (const auto &delta : previous->joints) out.push_back({delta.joint, mix(delta.rotation, rotation_in(next, delta.joint), amount)});
    if (next)
        for (const auto &delta : next->joints)
            if (!previous || std::ranges::find(previous->joints, delta.joint, &JointDelta::joint) == previous->joints.end())
                out.push_back({delta.joint, mix(identity, delta.rotation, amount)});
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

private:
    // Moves to the next part of the timeline and stores the duration of this part.
    void advance(float &learned, std::uint64_t now_ms) noexcept {
        if (now_ms > since_) learned = std::clamp(static_cast<float>(now_ms - since_), 80.0f, 1500.0f);
        ++part_;
        since_ = now_ms;
    }
    std::uint8_t trick_{}, part_{};
    std::uint64_t since_{};
    std::array<float, 33> pop_ms_ = filled(300.0f), fall_ms_ = filled(250.0f);
    static constexpr std::array<float, 33> filled(float value) {
        std::array<float, 33> result{};
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
        if (least > tolerance) search(0, frames_.size());
        if (distance) *distance = least;
        if (best >= frames_.size() || least > tolerance) return nullptr;
        last_ = best;
        return &frames_[best];
    }

private:
    std::deque<TakeFrame> frames_;
    std::size_t last_{};
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
