#include "overlay_internal.h"

#include "Extension/Skater/skater_pose.h"
#include "Extension/Skater/hall_of_meat.h"
#include "Engine/Game/UI/game_view.h"

#include <array>
#include <cmath>
#include <unordered_map>

// The local skater's skeleton, drawn over the world: every composed joint as a
// line to its parent, coloured by contact history — cyan untouched, yellow
// freshly hit, red where the same bone kept taking impacts (the current
// stand-in for "broken"). Projection follows the nametag overlay (live camera,
// focal from the vertical field of view).
namespace dingosdk::overlay::detail {
namespace {
using Vec3 = std::array<float, 3>;
// The snapshot stays half a second; older ones mean the tick stopped publishing.
constexpr std::uint64_t snapshot_max_age_ms = 500;
constexpr float hit_radius = 0.35f;   // a contact point marks joints within this range
constexpr float hit_fade_seconds = 5.0f;
constexpr int hits_until_broken = 2;   // same joint again inside the fade window -> red
constexpr float broken_seconds = 60.0f;

struct BoneState {
    double last_hit = 0;   // ImGui::GetTime() of the newest contact
    int hits = 0;
    bool broken = false;   // sticks until the hit history cools down
};
std::unordered_map<unsigned, BoneState> bone_states;

// Contacts light up the joints closest to them; called from the draw pass with
// the frame's composed positions.
void register_contacts(const skater_pose::Snapshot& snapshot, double now) {
    if (!snapshot.contacts_valid) return;
    for (const auto& contact : snapshot.contacts) {
        if (contact[0] == 0 && contact[1] == 0 && contact[2] == 0) continue;
        for (unsigned index = 0; index < skater_pose::skeleton_joints; ++index) {
            const auto& joint = snapshot.joints[index];
            if (!joint.valid) continue;
            const float dx = joint.position[0] - contact[0], dy = joint.position[1] - contact[1],
                dz = joint.position[2] - contact[2];
            if (dx * dx + dy * dy + dz * dz > hit_radius * hit_radius) continue;
            auto& bone = bone_states[index];
            if (now - bone.last_hit < hit_fade_seconds) ++bone.hits;
            else bone.hits = 1;
            bone.last_hit = now;
            bone.broken = bone.hits >= hits_until_broken;
        }
    }
}
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
    const auto now = ImGui::GetTime();
    register_contacts(snapshot, now);

    std::array<ImVec2, skater_pose::skeleton_joints> screen{};
    // Unused/gear joints sit at stale or scattered positions; anything far
    // from the character is not part of the visible body and its edges read
    // as random lines. Cull by distance to the character's placement.
    constexpr float max_body_distance = 2.5f;
    const auto body_distance = [&snapshot, &origin](const std::array<float, 3>& position) {
        const Vec3 delta{position[0] - origin[0] - (snapshot.origin[0] - origin[0]),
            position[1] - origin[1] - (snapshot.origin[1] - origin[1]),
            position[2] - origin[2] - (snapshot.origin[2] - origin[2])};
        return std::sqrt(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]);
    };
    for (unsigned index = 0; index < skater_pose::skeleton_joints; ++index) {
        const auto& joint = snapshot.joints[index];
        if (!joint.valid || body_distance(joint.position) > max_body_distance) {
            screen[index] = ImVec2(-1, -1);
            continue;
        }
        const Vec3 delta{joint.position[0] - origin[0], joint.position[1] - origin[1],
            joint.position[2] - origin[2]};
        const float depth = -(delta[0] * back[0] + delta[1] * back[1] + delta[2] * back[2]);
        if (depth <= 0.1f) { screen[index] = ImVec2(-1, -1); continue; }
        const float side = delta[0] * right[0] + delta[1] * right[1] + delta[2] * right[2];
        const float height = delta[0] * up[0] + delta[1] * up[1] + delta[2] * up[2];
        screen[index] = ImVec2(centre.x + side * focal / depth, centre.y - height * focal / depth);
    }

    // Contact history decays; bones that stopped being hit leave the set again.
    std::erase_if(bone_states, [now](const auto& entry) {
        return !entry.second.broken && now - entry.second.last_hit > hit_fade_seconds;
    });

    for (unsigned index = 1; index < skater_pose::skeleton_joints; ++index) {
        const auto parent = skater_pose::skeleton_parents[index];
        const auto& a = screen[parent];
        const auto& b = screen[index];
        if (a.x < 0 || b.x < 0) continue;
        const auto& pa = snapshot.joints[parent].position;
        const auto& pb = snapshot.joints[index].position;
        const float length = std::sqrt((pa[0] - pb[0]) * (pa[0] - pb[0]) + (pa[1] - pb[1]) * (pa[1] - pb[1]) +
            (pa[2] - pb[2]) * (pa[2] - pb[2]));
        if (length > 0.8f) continue; // no anatomical bone spans further on a human
        ImU32 colour = IM_COL32(120, 220, 255, 90);
        const auto hit = bone_states.find(index);
        if (hit != bone_states.end()) {
            if (hit->second.broken) colour = IM_COL32(255, 30, 30, 220);
            else {
                const float age = static_cast<float>(now - hit->second.last_hit);
                const float alpha = 1 - age / hit_fade_seconds;
                colour = IM_COL32(255, 220, 40, static_cast<int>(220 * alpha));
            }
        }
        draw->AddLine(a, b, colour, 2.0f);
    }
    for (unsigned index = 0; index < skater_pose::skeleton_joints; ++index) {
        if (screen[index].x < 0) continue;
        ImU32 colour = IM_COL32(120, 220, 255, 110);
        const auto hit = bone_states.find(index);
        if (hit != bone_states.end()) {
            if (hit->second.broken) colour = IM_COL32(255, 30, 30, 230);
            else {
                const float age = static_cast<float>(now - hit->second.last_hit);
                colour = IM_COL32(255, 220, 40, static_cast<int>(230 * (1 - age / hit_fade_seconds)));
            }
        }
        draw->AddCircleFilled(screen[index], 2.5f, colour);
    }

    // Short red glow at the character while a fresh wipeout is on record.
    const auto bails = dingosdk::hall_of_meat::recent();
    if (!bails.empty()) {
        const auto since = static_cast<float>((GetTickCount64() - bails.front().at) / 1000ULL);
        if (since < 2.5f) {
            const auto& head = screen[skater_pose::head_joint];
            if (head.x >= 0)
                draw->AddCircleFilled(head, 14.0f + since * 10.0f,
                    IM_COL32(255, 40, 40, static_cast<int>(120 * (1 - since / 2.5f))));
        }
    }
}
}
