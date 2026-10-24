#pragma once
// The overlay's side of plugins (Extension/Plugins/plugin_ui.h): the shapes each plugin drew this
// tick, on the background draw list under ReSkate's own menus and chat, taking no input. World
// shapes are projected through the camera as the frame is drawn.
namespace dingosdk::overlay {
// Every presented frame: whether a plugin drew anything (so the overlay renders with the menu
// closed), then the drawing.
bool plugins_pending();
void draw_plugins();
// A plugin's open full-screen screen (reskate.ui.screen), over everything, with the input while it
// shows (State::plugin_screen_visible, set by plugins_pending). Esc closes it.
void draw_plugin_screen();
}
