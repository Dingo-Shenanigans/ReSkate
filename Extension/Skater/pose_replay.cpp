#include "pose_replay.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dingosdk::pose_replay {
namespace {
using anim_studio::Joint;
void widen(Range& range, std::uint16_t joint) noexcept {
    if (range.last > range.first) {
        range.first = std::min(range.first, static_cast<std::uint32_t>(joint));
        range.last = std::max(range.last, static_cast<std::uint32_t>(joint) + 1u);
    } else range = {joint, static_cast<std::uint32_t>(joint) + 1u};
}
Joint joint_of(const NativeBone& b) noexcept {
    return {{b.scale[0], b.scale[1], b.scale[2]}, b.rotation, {b.position[0], b.position[1], b.position[2]}};
}
void store(NativeBone& b, const Joint& j) noexcept {
    std::memcpy(b.scale.data(), j.scale.data(), 12);
    b.rotation = j.rotation;
    std::memcpy(b.position.data(), j.position.data(), 12);
}
Range apply_entries(std::span<Joint> pose, std::span<const Entry> entries, float weight,
                    std::vector<std::uint16_t>* touched) {
    Range range;
    const float w = std::clamp(weight, 0.0f, 1.0f);
    thread_local std::vector<pose_place::Kept> kept;
    const auto mark = [&](std::uint16_t joint) {
        widen(range, joint);
        if (touched && std::find(touched->begin(), touched->end(), joint) == touched->end()) touched->push_back(joint);
    };
    for (const auto& entry : entries) {
        if (entry.joint < 2 || entry.joint >= pose.size()) continue;
        if (entry.keep_children) pose_place::keep(pose, entry.joint, kept);
        // Keep the publication range at zero weight too: the current baseline must replace a
        // previously sent pose. Do not round any native values through identity quaternion maths.
        if (w == 0) {
            if (entry.keep_children) for (const auto& k : kept) mark(k.joint);
            mark(entry.joint);
            continue;
        }
        auto& j = pose[entry.joint];
        j.rotation = anim_studio::normalised(anim_studio::multiply(j.rotation, anim_studio::slerp({0, 0, 0, 1}, entry.rotation, w)));
        for (std::size_t a = 0; a < 3; ++a) {
            j.scale[a] *= std::max(1.0f + (entry.scale[a] - 1.0f) * w, pose_offset::smallest_scale);
            j.position[a] += entry.offset[a] * w;
        }
        if (entry.place) pose_place::apply(pose, entry.joint, *entry.place, w);
        if (entry.keep_children) {
            pose_place::put_back(pose, entry.joint, kept);
            for (const auto& k : kept) mark(k.joint);
        }
        mark(entry.joint);
    }
    return range;
}
} // namespace

Range apply(std::span<Joint> pose, std::span<const Entry> entries, float weight) {
    return apply_entries(pose, entries, weight, nullptr);
}

bool draw(std::span<NativeBone> current, std::span<const Entry> entries, float weight,
          Written& written, std::vector<NativeBone>& before, Range& range) {
    if (current.size() != anim_studio::skeleton_joints || !std::isfinite(weight)) return false;
    thread_local std::vector<Joint> pose;
    thread_local std::vector<NativeBone> baseline;
    thread_local std::vector<std::uint16_t> touched;
    thread_local Written next;
    pose.resize(current.size());
    baseline.assign(current.begin(), current.end());
    Range recovered;
    for (std::size_t i = 0; i < current.size(); ++i) pose[i] = joint_of(current[i]);
    for (const auto& [joint, last] : written) {
        if (joint < 2 || joint >= pose.size()) continue;
        const auto original = pose_offset::before_delta(pose[joint], last);
        if (original.scale != pose[joint].scale || original.rotation != pose[joint].rotation || original.position != pose[joint].position)
            widen(recovered, joint);
        pose[joint] = original;
        store(baseline[joint], original);
    }
    touched.clear();
    auto changed = apply_entries(pose, entries, weight, &touched);
    next.clear();
    for (const auto joint : touched) {
        if (!anim_studio::finite(pose[joint])) return false;
        next.push_back({joint, {joint_of(baseline[joint]), pose[joint]}});
    }
    if (recovered.last > recovered.first) {
        widen(changed, static_cast<std::uint16_t>(recovered.first));
        widen(changed, static_cast<std::uint16_t>(recovered.last - 1u));
    }
    if (changed.last <= changed.first) return false;
    before.assign(baseline.begin() + changed.first, baseline.begin() + changed.last);
    // Start from the current native bones, including unrequested joints between the edits. Only
    // the old overlay's lanes are recovered; the fourth lanes are never replaced by pose values.
    for (std::uint32_t i = changed.first; i < changed.last; ++i) current[i] = baseline[i];
    for (const auto joint : touched) store(current[joint], pose[joint]);
    written = next;
    range = changed;
    return true;
}
} // namespace dingosdk::pose_replay
