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

    // The verified spine: placement -> neck -> head. The parent array covers
    // all 395 joints, but the resource names only the physics subset, so
    // drawing every parent edge included control and gear joints; the body
    // reads as dots, the spine as a line.
    constexpr std::array<std::uint16_t, 6> spine_chain{1, 7, 42, 43, 44, 45};
    constexpr ImU32 bone_colour = IM_COL32(120, 220, 255, 170);
    ImVec2 previous{-1, -1};
    for (auto joint_index : spine_chain) {
        const auto& at = screen[joint_index];
        if (previous.x >= 0 && at.x >= 0) draw->AddLine(previous, at, bone_colour, 3.0f);
        if (at.x >= 0) draw->AddCircleFilled(at, 4.0f, bone_colour);
        previous = at;
    }

    // Whole-body hit flash: yellow fading over the five seconds after a bail.
    const auto bails = dingosdk::hall_of_meat::recent();
    float since_bail = 1e9f;
    if (!bails.empty())
        since_bail = static_cast<float>((GetTickCount64() - bails.front().at) / 1000ULL);
    const float flash = since_bail < hit_fade_seconds ? 1 - since_bail / hit_fade_seconds : 0;
    const ImU32 joint_colour = flash > 0
        ? IM_COL32(255, static_cast<int>(220 * flash + 35 * (1 - flash)), static_cast<int>(40 * flash + 220 * (1 - flash)),
              static_cast<int>(170 * flash + 110 * (1 - flash)))
        : IM_COL32(120, 220, 255, 110);
    for (unsigned index = 0; index < skater_pose::skeleton_joints; ++index) {
        if (screen[index].x < 0) continue;
        draw->AddCircleFilled(screen[index], 2.5f, joint_colour);
    }

    // Red pulse at the head for the first moments of a fresh wipeout.
}
}
