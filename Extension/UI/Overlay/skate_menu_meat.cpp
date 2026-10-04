#include "skate_menu_internal.h"

#include "Extension/Skater/hall_of_meat.h"

#include <Windows.h>
#include <cmath>
#include <format>
#include <string>
#include <vector>

// The SKATER page's MEAT tab. The skater figure is placeholder art drawn with
// ImGui primitives until real art replaces it; the numbers come from the
// game's own bail causes (Extension/Skater/hall_of_meat.cpp).
namespace dingosdk::overlay::menu {
namespace {
// A simple stick figure. `heat` (0-1) tints the bones red for a moment after a
// bail; the newest bail's severity decides how long the colour lingers.
void draw_skater(ImDrawList* draw, ImVec2 at, float scale, float heat) {
    const ImU32 bone = ImGui::ColorConvertFloat4ToU32({1 - heat, 1 - heat * .6f, 1 - heat * .6f, 1});
    const auto joint = [&](float x, float y) { return ImVec2(at.x + x * scale, at.y + y * scale); };
    const auto bone_line = [&](float ax, float ay, float bx, float by) {
        draw->AddLine(joint(ax, ay), joint(bx, by), bone, 3 * scale);
    };
    draw->AddCircleFilled(joint(0, -36), 9 * scale, bone);
    bone_line(0, -27, 0, 6);   // spine
    bone_line(0, -20, -14, -6); // left arm
    bone_line(-14, -6, -20, 12);
    bone_line(0, -20, 14, -6);  // right arm
    bone_line(14, -6, 20, 12);
    bone_line(0, 6, -9, 26);   // left leg
    bone_line(-9, 26, -12, 46);
    bone_line(0, 6, 9, 26);    // right leg
    bone_line(9, 26, 12, 46);
}

std::string ago(std::uint64_t at) {
    const auto seconds = static_cast<double>((GetTickCount64() - at) / 1000ULL);
    if (seconds < 60) return "just now";
    if (seconds < 3600) return std::format("{} min ago", static_cast<int>(seconds / 60));
    return std::format("{} h ago", static_cast<int>(seconds / 3600));
}
}

void meat_controls(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    (void)model;
    (void)callbacks;
    begin_card(menu, "hall-of-meat", "HALL OF MEAT");
    const auto bails = dingosdk::hall_of_meat::recent();
    float heat = 0;
    if (!bails.empty()) {
        const auto since = static_cast<float>((GetTickCount64() - bails.front().at) / 1000ULL);
        heat = std::max(0.0f, 1 - since / 4);
    }
    const ImVec2 figure_origin = ImGui::GetCursorScreenPos();
    const float figure_width = ImGui::GetFontSize() * 5;
    draw_skater(ImGui::GetWindowDrawList(), {figure_origin.x + figure_width * .5f, figure_origin.y + 4},
        ImGui::GetFontSize() / 11, heat);
    ImGui::Dummy(ImVec2(figure_width, ImGui::GetFontSize() * 7.5f));
    ImGui::SameLine();
    ImGui::BeginGroup();
    if (bails.empty())
        note("No bails recorded yet. The feed fills in when the local skater wipes out.");
    else {
        const auto& last = bails.front();
        info(menu, "Last bail", ago(last.at));
        info(menu, "Total impact", std::format("{:.1f}", last.magnitude));
        if (last.body_contact) note("A body bone hit something solid.");
        for (std::size_t index = 0; index < last.impact_count; ++index)
            info(menu, std::format("Cause {}", index + 1).c_str(),
                std::format("reason {}, magnitude {:.1f}", last.impacts[index].reason, last.impacts[index].magnitude));
    }
    ImGui::EndGroup();
    if (bails.size() > 1) {
        section(menu, "EARLIER BAILS");
        for (std::size_t index = 1; index < bails.size(); ++index)
            info(menu, ago(bails[index].at).c_str(),
                std::format("impact {:.1f}{}", bails[index].magnitude,
                    bails[index].body_contact ? ", body contact" : ""));
    }
    if (ImGui::Button("Clear history", ImVec2(-FLT_MIN, 0))) dingosdk::hall_of_meat::forget();
    end_card();
}
}
