#include "Extension/Style/style_edits.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace dingosdk::style::edits {
namespace {
float on_timeline(float time) noexcept { return std::clamp(std::isfinite(time) ? time : 0.0f, first_key_time, last_key_time); }
bool has_key(const Style &style, std::uint8_t trick, std::uint8_t key, std::string &error) {
    if (is_flip_trick(trick) && key < style.keys(trick).size()) return true;
    error = "That trick does not have that keyframe.";
    return false;
}
template <class Map, class TargetOf> void drop_key(Map &entries, std::uint8_t trick, std::uint8_t key, TargetOf target_of) {
    Map kept;
    for (const auto &[entry, value] : entries) {
        auto moved = entry;
        Target &target = target_of(moved);
        if (target.trick && target.id == trick && target.key == key) continue;
        if (target.trick && target.id == trick && target.key > key) --target.key;
        kept[moved] = value;
    }
    entries = std::move(kept);
}
} // namespace

bool set_joint(Style &style, Target target, std::string_view joint, float x, float y, float z, std::string &error) {
    const auto known = editable_joint(joint);
    if (!known || (target.trick ? !is_flip_trick(target.id) : target.id >= family_count) || (!target.trick && target.key)) {
        error = "A style cannot rotate that joint.";
        return false;
    }
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        error = "Angles must be numbers.";
        return false;
    }
    if (target.trick && !has_key(style, target.id, target.key, error)) return false;
    const RotationKey key{target, std::string(editable_joints[*known])};
    if (x == 0 && y == 0 && z == 0) style.rotations.erase(key);
    else style.rotations[key] = {x, y, z};
    return true;
}

void clear(Style &style, std::optional<Target> target) {
    if (target) {
        std::erase_if(style.rotations,
                      [&](const auto &entry) { return entry.first.target.trick == target->trick && entry.first.target.id == target->id; });
        if (target->trick) {
            style.times.erase(target->id);
            std::erase_if(style.blend_outs, [&](const auto &entry) { return entry.first.id == target->id; });
        }
    } else style = {};
}

int add_key(Style &style, std::uint8_t trick, float time, const Pace &pace, std::string &error) {
    auto times = style.keys(trick);
    if (!is_flip_trick(trick) || times.size() >= max_keys) {
        error = "That trick cannot have more keyframes.";
        return -1;
    }
    time = free_key_time(times, on_timeline(time));
    const auto added = static_cast<std::uint8_t>(times.size());
    std::vector<JointDelta> shown;
    evaluate(trick_keys(style, trick), time, shown, pace);
    for (const auto &delta : shown) {
        const auto degrees = to_degrees(delta.rotation);
        if (std::abs(degrees[0]) > 0.01f || std::abs(degrees[1]) > 0.01f || std::abs(degrees[2]) > 0.01f)
            style.rotations[{Target{true, trick, added}, std::string(editable_joints[delta.joint])}] = degrees;
    }
    times.push_back(time);
    style.times[trick] = std::move(times);
    return static_cast<int>(style.keys(trick).size()) - 1;
}

bool move_key(Style &style, std::uint8_t trick, std::uint8_t key, float time, std::string &error) {
    if (!has_key(style, trick, key, error)) return false;
    auto times = style.keys(trick);
    auto others = times;
    others.erase(others.begin() + key);
    times[key] = free_key_time(others, on_timeline(time));
    style.times[trick] = std::move(times);
    return true;
}

bool delete_key(Style &style, std::uint8_t trick, std::uint8_t key, std::string &error) {
    if (!has_key(style, trick, key, error)) return false;
    auto times = style.keys(trick);
    times.erase(times.begin() + key);
    style.times[trick] = std::move(times);
    drop_key(style.rotations, trick, key, [](RotationKey &entry) -> Target & { return entry.target; });
    drop_key(style.blend_outs, trick, key, [](Target &entry) -> Target & { return entry; });
    return true;
}

bool set_blend_out(Style &style, std::uint8_t trick, std::uint8_t key, float ms, std::string &error) {
    if (!has_key(style, trick, key, error)) return false;
    const Target target{true, trick, key};
    if (std::isfinite(ms) && ms > 0) style.blend_outs[target] = std::clamp(ms, min_blend_out_ms, max_blend_out_ms);
    else style.blend_outs.erase(target);
    return true;
}
} // namespace dingosdk::style::edits
