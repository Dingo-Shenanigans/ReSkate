#include "overlay_internal.h"

#include "Extension/Skater/skater_pose.h"
#include "Extension/Skater/hall_of_meat.h"
#include "Engine/Game/UI/game_view.h"

#include <array>
#include <cmath>

// The local skater's skeleton, drawn over the world: the verified joint chain
// (root -> neck -> head) as a line, joints as dots, and a short red glow after
// a wipeout. Projection follows the nametag overlay (live camera, focal from
// the vertical field of view).
namespace dingosdk::overlay::detail {
namespace {
using Vec3 = std::array<float, 3>;
// The snapshot stays half a second; older ones mean the tick stopped publishing.
constexpr std::uint64_t snapshot_max_age_ms = 500;

ImVec2 project(const std::array<float, 3>& position, const std::array<float, 16>& camera,
               float focal, const ImVec2& centre) {
    const Vec3 origin{camera[12], camera[13], camera[14]};
    const Vec3 right{camera[0], camera[1], camera[2]}, up{camera[4], camera[5], camera[6]},
        back{camera[8], camera[9], camera[10]};
    const Vec3 delta{position[0] - origin[0], position[1] - origin[1], position[2] - origin[2]};
    const float depth = -(delta[0] * back[0] + delta[1] * back[1] + delta[2] * back[2]);
    const float side = delta[0] * right[0] + delta[1] * right[1] + delta[2] * right[2];
    const float height = delta[0] * up[0] + delta[1] * up[1] + delta[2] * up[2];
    if (depth <= 0.1f) return ImVec2(-1, -1);
    return ImVec2(centre.x + side * focal / depth, centre.y - height * focal / depth);
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

    std::array<ImVec2, skater_pose::head_chain.size()> screen{};
    std::size_t visible = 0;
    for (std::size_t index = 0; index < skater_pose::head_chain.size(); ++index) {
        screen[index] = project(snapshot.chain[index], view->world, focal, centre);
        if (screen[index].x >= 0) ++visible;
    }
    if (!visible) return;
    // Until the skeleton definition's parent array is mapped, only the two
    // verified points are drawn: the character placement (joint 1) and the
    // composed head. The chain's middle joints are animation controllers, not
    // anatomy, and drawing them read as a random line.
    constexpr std::size_t origin_index = 1, head_index = skater_pose::head_chain.size() - 1;
    constexpr ImU32 bone_colour = IM_COL32(120, 220, 255, 150);
    const auto& origin_point = screen[origin_index];
    const auto& head_point = screen[head_index];
    if (origin_point.x >= 0 && head_point.x >= 0) draw->AddLine(origin_point, head_point, bone_colour, 2.0f);
    if (origin_point.x >= 0) draw->AddCircleFilled(origin_point, 3.0f, bone_colour);
    if (head_point.x >= 0) draw->AddCircleFilled(head_point, 4.0f, bone_colour);

    // Short red glow at the character while a fresh wipeout is on record.
    const auto bails = dingosdk::hall_of_meat::recent();
    if (!bails.empty()) {
        const auto since = static_cast<float>((GetTickCount64() - bails.front().at) / 1000ULL);
        if (since < 2.5f) {
            if (head_point.x >= 0)
                draw->AddCircleFilled(head_point, 14.0f + since * 10.0f,
                    IM_COL32(255, 40, 40, static_cast<int>(120 * (1 - since / 2.5f))));
        }
    }
}
}
