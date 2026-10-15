#pragma once
#include "Extension/UI/Overlay/skate_menu.h"

namespace dingosdk::overlay {
using StylePlayheadFeed = style::Playhead (*)() noexcept;
void set_style_playhead_feed(StylePlayheadFeed) noexcept;
void draw_style_editor(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks, bool exit_requested);
// True while the style editor screen is asked for or open.
bool style_editor_wanted() noexcept;
namespace menu {
void style_page(SkateMenu&, const Model&, const CallbacksV3&);
}
} // namespace dingosdk::overlay
