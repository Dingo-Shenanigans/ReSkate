#include "pose_offset.h"
#include <algorithm>

namespace dingosdk::pose_offset {
using anim_studio::multiply;
using anim_studio::normalised;
using anim_studio::slerp;

void add(std::vector<Delta>& out, std::uint16_t joint, const Quat& rotation, float scale, const Vec3& offset, float weight) {
    const float w = std::clamp(weight, 0.0f, 1.0f);
    const auto it = std::find_if(out.begin(), out.end(), [&](const Delta& d) { return d.joint == joint; });
    auto& d = it != out.end() ? *it : out.emplace_back(Delta{joint});
    // Turned after the turns before it; scales multiply and moves add, as they do applied one by one.
    d.rotation = normalised(multiply(d.rotation, slerp(Quat{0, 0, 0, 1}, rotation, w)));
    d.scale *= 1.0f + (scale - 1.0f) * w;
    for (std::size_t a = 0; a < 3; ++a) d.offset[a] += offset[a] * w;
}

Joint apply(const Joint& at, const Delta& delta) noexcept {
    Joint out = at;
    out.rotation = normalised(multiply(at.rotation, delta.rotation));
    const float s = std::max(delta.scale, smallest_scale);
    for (auto& k : out.scale) k *= s;
    for (std::size_t a = 0; a < 3; ++a) out.position[a] += delta.offset[a];
    return out;
}

Joint before_delta(const Joint& now, const std::optional<Written>& last) noexcept {
    if (last && now.scale == last->after.scale && now.rotation == last->after.rotation && now.position == last->after.position) return last->before;
    return now;
}
}
