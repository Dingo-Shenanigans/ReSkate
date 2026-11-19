#pragma once
#include "Extension/Plugins/plugin_ui.h"
#include <imgui.h>
#include <string>

// Texts plugins draw in fonts of their own (a font file of the plugin's package that its manifest's
// "fonts" names; style.font in d:text). Dear ImGui's font atlas is not used for them: that would be
// stb_truetype parsing a plugin's file, and the atlas built again while the game runs. Windows reads
// the font instead (Extension/Plugins/plugin_font.h) and its glyphs are kept on pages of our own,
// which go to the GPU as plugins' pictures do. Render thread (plugin_fonts.cpp).
namespace dingosdk::overlay::detail {
struct OwnFont;
// Once a frame, before the plugins' shapes are drawn: the glyphs made since the last frame go onto
// their pages, the pages that reached the GPU take over, and those no longer drawn from are let go.
void own_fonts_begin();
// The font in a file (a layer's fonts: its full path in UTF-8), or nothing when it cannot be read:
// its texts are then drawn in "body", as the plugin measured them.
OwnFont* own_font(const std::string& file);
// A text shape drawn in the font at `where`: `k` is the window's height over 1080, as the shape's
// sizes are scaled, and `body` the face that draws what the font lacks, or has not got ready yet.
// Each character is where plugin_ui::set_text puts it, which is what d:measure measured.
void draw_own_text(ImDrawList* draw, OwnFont& font, ImFont* body, float k, ImVec2 where, const plugin_ui::Shape& shape);
// Once a frame, after them: the glyphs the frame's texts lacked are asked for.
void own_fonts_end();
}
