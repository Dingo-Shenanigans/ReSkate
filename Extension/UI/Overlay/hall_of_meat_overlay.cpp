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

    ImVec2 previous{-1, -1};
    for (auto joint_index : body_axis) {
        const auto& joint = snapshot.joints[joint_index];
        if (!joint.valid) continue;
        const Vec3 delta{joint.position[0] - origin[0], joint.position[1] - origin[1],
            joint.position[2] - origin[2]};
        const float depth = -(delta[0] * back[0] + delta[1] * back[1] + delta[2] * back[2]);
        if (depth <= 0.1f) { previous = ImVec2(-1, -1); continue; }
        const float side = delta[0] * right[0] + delta[1] * right[1] + delta[2] * right[2];
        const float height = delta[0] * up[0] + delta[1] * up[1] + delta[2] * up[2];
        const ImVec2 at(centre.x + side * focal / depth, centre.y - height * focal / depth);
        if (previous.x >= 0) draw->AddLine(previous, at, axis_colour, 3.0f);
        previous = at;
    }

    // Red pulse at the head for the first moments of a fresh wipeout.
    const auto bails = dingosdk::hall_of_meat::recent();
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
}
