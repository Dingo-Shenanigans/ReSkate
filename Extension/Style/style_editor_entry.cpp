#include "style.h"
#include "menu_room.h"
#include "style_capture.h"
#include "style_editor.h"
#include "style_overlay.h"
#include "trick_pad.h"
#include "trick_recorder.h"
#include "trick_stage.h"
#include "Extension/Skater/editor_screen.h"

namespace dingosdk::style {
void start() noexcept {
    overlay::set_style_playhead_feed(style_editor::playhead);
    static const editor_screen::Screen screen{style_editor::camera_pose, style_editor::camera_fov,
                                              []() noexcept { style_editor::screen_open(true); }, style_editor::screen_closed,
                                              style_capture::camera_pose};
    editor_screen::set(screen);
}

void on_editor_tick(std::uintptr_t base, std::uintptr_t client, bool multiplayer_ready) noexcept {
    style_editor::tick(base, client, multiplayer_ready);
    menu_room::tick(base, multiplayer_ready);
    trick_pad::tick();
    trick_stage::tick(base, client, multiplayer_ready);
    trick_recorder::tick(base, multiplayer_ready);
    style_capture::tick(base, client, multiplayer_ready);
}

void fill_editor(StyleModel &model) { style_editor::fill(model); }
} // namespace dingosdk::style
