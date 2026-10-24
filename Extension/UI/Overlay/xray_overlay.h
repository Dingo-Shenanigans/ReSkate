#pragma once
// The local skater's x-ray (Extension/Skater/xray.h) as a plugin's draw handler paints it
// (d:skeleton): each body in its colour, the rest left out.
#include "Engine/Game/Skater/skater_body.h"
#include <array>
#include <imgui.h>

namespace dingosdk::overlay {
// Render thread. `paint` by body (skater_body.h's order; 0 leaves a body out), `alpha` the whole.
void draw_xray(ImDrawList* draw, const std::array<ImU32, skater_body::count>& paint, float alpha);
}
