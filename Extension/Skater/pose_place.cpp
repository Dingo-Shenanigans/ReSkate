#include "pose_place.h"
#include <algorithm>
#include <array>
#include <format>

namespace dingosdk::pose_place {
namespace {
using anim_studio::parent;

// One transform after another (parent, then child in the parent's frame); uniform scale.
Joint compose(const Joint& frame, const Joint& child) noexcept {
    const float k = (frame.scale[0] + frame.scale[1] + frame.scale[2]) / 3.0f;
    const auto moved = anim_studio::rotate(frame.rotation, {child.position[0] * k, child.position[1] * k, child.position[2] * k});
    Joint out;
    out.position = {frame.position[0] + moved[0], frame.position[1] + moved[1], frame.position[2] + moved[2]};
    out.rotation = anim_studio::normalised(anim_studio::multiply(frame.rotation, child.rotation));
    const float c = (child.scale[0] + child.scale[1] + child.scale[2]) / 3.0f;
    out.scale = {k * c, k * c, k * c};
    return out;
}
// `world` seen from `frame`.
Joint relative(const Joint& frame, const Joint& world) noexcept {
    const float k = std::max((frame.scale[0] + frame.scale[1] + frame.scale[2]) / 3.0f, 1e-4f);
    const auto back = anim_studio::conjugate(frame.rotation);
    const auto d = anim_studio::rotate(back, {world.position[0] - frame.position[0], world.position[1] - frame.position[1],
                                              world.position[2] - frame.position[2]});
    Joint out;
    out.position = {d[0] / k, d[1] / k, d[2] / k};
    out.rotation = anim_studio::normalised(anim_studio::multiply(back, world.rotation));
    const float c = (world.scale[0] + world.scale[1] + world.scale[2]) / 3.0f;
    out.scale = {c / k, c / k, c / k};
    return out;
}
// Whether `joint` hangs from `ancestor` (through any number of parents).
bool hangs_from(std::uint16_t joint, std::uint16_t ancestor) noexcept {
    for (int at = parent(joint); at >= 0; at = parent(static_cast<std::uint16_t>(at)))
        if (at == ancestor) return true;
    return false;
}
Joint world_of(std::span<const Joint> pose, int joint) noexcept {
    return joint >= 0 && joint < static_cast<int>(pose.size()) ? anim_studio::world(pose, static_cast<std::uint16_t>(joint)) : Joint{};
}
} // namespace

std::string order(std::span<const std::uint16_t> joints, std::span<const int> relative_to, std::vector<std::size_t>& out) {
    out.clear();
    const auto n = joints.size();
    std::vector<std::vector<std::size_t>> after(n); // after[i]: the entries i is posed after
    for (std::size_t i = 0; i < n; ++i) {
        const int to = i < relative_to.size() ? relative_to[i] : -1;
        if (to == joints[i]) return std::format("joint {} is placed relative to itself", joints[i]);
        if (to >= 0 && hangs_from(static_cast<std::uint16_t>(to), joints[i]))
            return std::format("joint {} is placed relative to joint {}, which hangs from it", joints[i], to);
        for (std::size_t k = 0; k < n; ++k) {
            if (k == i) continue;
            const bool above = hangs_from(joints[i], joints[k]);
            const bool frame = to >= 0 && (joints[k] == to || hangs_from(static_cast<std::uint16_t>(to), joints[k]));
            if (above || frame) after[i].push_back(k);
        }
    }
    // Each time, the lowest joint whose joints before it are all posed: the same order every frame.
    std::vector<bool> done(n);
    while (out.size() < n) {
        std::size_t next = n;
        for (std::size_t i = 0; i < n; ++i)
            if (!done[i] && std::all_of(after[i].begin(), after[i].end(), [&](std::size_t k) { return done[k]; }) &&
                (next == n || joints[i] < joints[next]))
                next = i;
        if (next == n) {
            std::string circle;
            for (std::size_t i = 0; i < n; ++i)
                if (!done[i]) circle += std::format("{}{}", circle.empty() ? "" : ", ", joints[i]);
            out.clear();
            return "joints " + circle + " are placed relative to each other in a circle";
        }
        done[next] = true;
        out.push_back(next);
    }
    return {};
}

void apply(std::span<Joint> pose, std::uint16_t joint, const Place& place, float weight) noexcept {
    if (joint >= pose.size()) return;
    const Joint now = pose[joint];
    Joint wanted = now;
    if (place.frame == Frame::parent) {
        if (place.position) wanted.position = *place.position;
        if (place.rotation) wanted.rotation = anim_studio::normalised(*place.rotation);
    } else {
        const Joint parent_world = world_of(pose, parent(joint));
        const Joint frame = place.frame == Frame::joint    ? world_of(pose, place.joint)
                            : place.frame == Frame::skater ? world_of(pose, anim_studio::placement_joint)
                                                           : Joint{};
        // Where it is now, in that frame; what is asked for replaces its part; back under its own parent.
        auto in_frame = relative(frame, compose(parent_world, now));
        if (place.position) in_frame.position = *place.position;
        if (place.rotation) in_frame.rotation = anim_studio::normalised(*place.rotation);
        wanted = relative(parent_world, compose(frame, in_frame));
        wanted.scale = now.scale;
    }
    const float w = std::clamp(weight, 0.0f, 1.0f);
    auto& out = pose[joint];
    for (std::size_t a = 0; a < 3; ++a) out.position[a] = now.position[a] + (wanted.position[a] - now.position[a]) * w;
    out.rotation = anim_studio::slerp(now.rotation, wanted.rotation, w);
}

void keep(std::span<const Joint> pose, std::uint16_t joint, std::vector<Kept>& out) {
    out.clear();
    for (std::size_t c = 0; c < pose.size(); ++c)
        if (parent(static_cast<std::uint16_t>(c)) == joint) out.push_back({static_cast<std::uint16_t>(c), world_of(pose, static_cast<int>(c))});
}

void put_back(std::span<Joint> pose, std::uint16_t joint, std::span<const Kept> kept) noexcept {
    const Joint now = world_of(pose, joint);
    for (const auto& k : kept)
        if (k.joint < pose.size()) pose[k.joint] = relative(now, k.world);
}
} // namespace dingosdk::pose_place
