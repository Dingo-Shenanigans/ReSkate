#pragma once
#include <imgui.h>
#include <string>

// Board Break's deck preview: the deck the carousel is on, as a 3D model turning in a window of
// the BOARD BREAK DECK card (modding_menu.cpp).
//
// The model comes from the deck's own folder (mods::board_break_deck_folder): preview.obj, the
// deck's mesh as a Wavefront OBJ (any axes: the longest is the deck's length, the shortest its
// thickness, and the side its kicks turn up to is the grip), and preview.png beside it, optional,
// the deck's base colour texture by the OBJ's texture coordinates. SVG2Deck writes both into its
// Board Break folder. A deck without them (or not installed) shows a plain popsicle. The files are
// read on a thread of their own and looked at again every two seconds while the card shows, so a
// deck re-exported while the game runs shows its new shape.
//
// Drawn with HLSL on the overlay's own command list: into a texture of its own (4x MSAA, with
// depth) just before ImGui's draws, which then show that texture. Should the shaders not build on
// a machine, the model is drawn flat with ImGui's triangles instead.
//
// The model turns at one steady speed whatever the carousel does. Moving to another deck fades the
// model out, swaps it while it is hidden, and fades the new one in.
namespace dingosdk::overlay::deck_preview {
// Render thread, inside the menu's ImGui frame: the preview of `deck` (a Board Break deck name;
// empty for none) filling `a`..`z` of `draw`, on `background` (opaque, as the card's panel).
void draw(ImDrawList* draw, ImVec2 a, ImVec2 z, const std::string& deck, ImU32 background);
// Whether the deck in the model shown is a stand-in (no preview.obj for it).
bool showing_stand_in();
}

namespace dingosdk::overlay::detail {
struct State;
// Render thread, on the frame's command list once it is reset and before ImGui's draws: renders
// what draw() asked for this frame, if anything. And forgets every GPU object with the device.
void render_deck_preview(State& s);
void deck_preview_lost();
}
