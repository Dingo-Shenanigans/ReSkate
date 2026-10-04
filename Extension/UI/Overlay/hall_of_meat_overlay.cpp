#include "overlay_internal.h"

#include "Extension/Skater/skater_pose.h"
#include "Extension/Skater/hall_of_meat.h"
#include "Engine/Game/UI/game_view.h"

#include <array>
#include <cmath>

// The local skater's body axis, drawn over the world: the verified joint chain
// from the character's placement up through the neck to the head, with a short
// red pulse at the head when a wipeout records. Projection follows the nametag
// overlay (live camera, focal from the vertical field of view). Nothing else
// is drawn: the pose's remaining joints are control and gear joints, and the
// per-bone contact flags are not yet mapped to them.
namespace dingosdk::overlay::detail {
namespace {
using Vec3 = std::array<float, 3>;
// The snapshot stays half a second; older ones mean the tick stopped publishing.
constexpr std::uint64_t snapshot_max_age_ms = 500;
constexpr float pulse_seconds = 2.5f;
constexpr int hits_until_broken = 2;   // the same physics bone again -> broken
struct BoneState {
    int hits = 0;
    bool broken = false;
};
// Placement -> neck chain -> head; the first-person camera chain verified it.
constexpr std::array<std::uint16_t, 9> body_axis{1, 7, 42, 43, 44, 45, 101, 102, 103};
constexpr ImU32 axis_colour = IM_COL32(120, 220, 255, 170);
}

void draw_hall_of_meat_skeleton() {
    const auto snapshot = skater_pose::latest();
    if (!snapshot.valid || GetTickCount64() - snapshot.at > snapshot_max_age_ms) return;
    const auto view = latest_game_view();
    if (!view || !(view->vertical_fov > 1 && view->vertical_fov < 175)) return;
    const auto display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0 || display.y <= 0) return;
    const float focal = display.y / (2.0f * std::tan(view->vertical_fov * 3.14159265f / 360.0f));
    const ImVec2 centre(display.x * 0.5f, display.y * 0.5f);
    auto* draw = ImGui::GetBackgroundDrawList();
    const Vec3 origin{view->world[12], view->world[13], view->world[14]};
    const Vec3 right{view->world[0], view->world[1], view->world[2]},
        up{view->world[4], view->world[5], view->world[6]},
        back{view->world[8], view->world[9], view->world[10]};

    std::array<ImVec2, skater_pose::skeleton_joints> screen{};
    for (unsigned index = 0; index < skater_pose::skeleton_joints; ++index) {
        const auto& joint = snapshot.joints[index];
        if (!joint.valid) { screen[index] = ImVec2(-1, -1); continue; }
        const Vec3 delta{joint.position[0] - origin[0], joint.position[1] - origin[1],
            joint.position[2] - origin[2]};
        const float depth = -(delta[0] * back[0] + delta[1] * back[1] + delta[2] * back[2]);
        if (depth <= 0.1f) { screen[index] = ImVec2(-1, -1); continue; }
        const float side = delta[0] * right[0] + delta[1] * right[1] + delta[2] * right[2];
        const float height = delta[0] * up[0] + delta[1] * up[1] + delta[2] * up[2];
        screen[index] = ImVec2(centre.x + side * focal / depth, centre.y - height * focal / depth);
    }

    // The newest bail's body impacts light the joints nearest to each hit:
    // yellow fresh, red when the same physics bone took it again.
    const auto bails = dingosdk::hall_of_meat::recent();
    std::unordered_map<unsigned, BoneState> joint_hits;
    if (!bails.empty()) {
        for (std::size_t index = 0; index < bails.front().bone_hit_count; ++index) {
            const auto& hit = bails.front().bone_hits[index];
            if (!hit.has_position) continue;
            unsigned best = 0; float best_distance = 0.5f; bool found = false;
            for (unsigned j = 0; j < skater_pose::skeleton_joints; ++j) {
                const auto& joint = snapshot.joints[j];
                if (!joint.valid) continue;
                const float dx = joint.position[0] - hit.position[0],
                    dy = joint.position[1] - hit.position[1],
                    dz = joint.position[2] - hit.position[2];
                const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (distance < best_distance) { best_distance = distance; best = j; found = true; }
            }
            if (!found) continue;
            auto& bone = joint_hits[best];
            bone.hits += 1;
            bone.broken = bone.broken || bone.hits >= hits_until_broken;
        }
    }

    for (unsigned index = 1; index < skater_pose::skeleton_joints; ++index) {
        const auto parent = skater_pose::skeleton_parents[index];
        const auto& a = screen[parent];
        const auto& b = screen[index];
        if (a.x < 0 || b.x < 0) continue;
        const auto& pa = snapshot.joints[parent].position;
        const auto& pb = snapshot.joints[index].position;
        const float length = std::sqrt((pa[0] - pb[0]) * (pa[0] - pb[0]) + (pa[1] - pb[1]) * (pa[1] - pb[1]) +
            (pa[2] - pb[2]) * (pa[2] - pb[2]));
        if (length > 0.8f) continue;
        ImU32 colour = IM_COL32(120, 220, 255, 90);
        const auto hit = joint_hits.find(index);
        if (hit != joint_hits.end())
            colour = hit->second.broken ? IM_COL32(255, 30, 30, 230) : IM_COL32(255, 220, 40, 220);
        else if (joint_hits.find(parent) != joint_hits.end())
            colour = IM_COL32(255, 220, 40, 160);
        draw->AddLine(a, b, colour, 2.0f);
    }
    for (unsigned index = 0; index < skater_pose::skeleton_joints; ++index) {
        if (screen[index].x < 0) continue;
        ImU32 colour = IM_COL32(120, 220, 255, 110);
        const auto hit = joint_hits.find(index);
        if (hit != joint_hits.end())
            colour = hit->second.broken ? IM_COL32(255, 30, 30, 230) : IM_COL32(255, 220, 40, 220);
        draw->AddCircleFilled(screen[index], 2.5f, colour);
    }

    // Red pulse at the head for the first moments of a fresh wipeout.
    if (!bails.empty()) {
        const auto since = static_cast<float>((GetTickCount64() - bails.front().at) / 1000ULL);
        if (since < pulse_seconds) {
            const auto& head = snapshot.joints[skater_pose::head_joint];
            if (head.valid) {
                const Vec3 delta{head.position[0] - origin[0], head.position[1] - origin[1],
                    head.position[2] - origin[2]};
                const float depth = -(delta[0] * back[0] + delta[1] * back[1] + delta[2] * back[2]);
                if (depth > 0.1f) {
                    const float side = delta[0] * right[0] + delta[1] * right[1] + delta[2] * right[2];
                    const float height = delta[0] * up[0] + delta[1] * up[1] + delta[2] * up[2];
                    draw->AddCircleFilled(ImVec2(centre.x + side * focal / depth, centre.y - height * focal / depth),
                        14.0f + since * 10.0f,
                        IM_COL32(255, 40, 40, static_cast<int>(120 * (1 - since / pulse_seconds))));
                }
            }
        }
    }
}
} // namespace dingosdk::overlay::detail
