#pragma once
#include "skate_menu.h"
#include <initializer_list>
#include <string>

namespace dingosdk::overlay::menu {
// The menu chrome is laid out in pixels; px() applies the user's menu scale to
// one of those measurements. Only valid while the menu is drawing.
float px(float value);
void section(SkateMenu& menu, const char* text);
void feedback(SkateMenu& menu, const char* text);
bool toggle_row(SkateMenu& menu, const char* label, const char* hint, bool& value,
                bool available = true, const char* unavailable = "N/A");
void category_tabs(SkateMenu& menu, int& selected, std::initializer_list<const char*> tabs, const char* id);

// Page layout kit, so every page reads the same way:
//   begin_card / end_card  a titled tile grouping related controls, sized to its
//                          content (always pair them)
//   field                  a label in the left column; the next widget fills the
//                          rest of the row (stacked under the label when narrow)
//   info                   a status line: muted label column, value beside it
//   note / warn            muted help text / a warning, wrapped
//   choice                 one of a few options as a strip of tiles (not radio buttons)
//   tag                    a small coloured badge, e.g. HOST or IN GAME
//   primary_button         a card's main action: blue, full width
void begin_card(SkateMenu& menu, const char* id, const char* title = nullptr, const char* subtitle = nullptr);
void end_card();
void field(SkateMenu& menu, const char* label, const char* tooltip = nullptr);
void info(SkateMenu& menu, const char* label, const std::string& value);
void note(const char* text);
void warn(const char* text);
// width 0 fills the rest of the row.
bool choice(SkateMenu& menu, const char* id, int& selected, std::initializer_list<const char*> options,
            bool enabled = true, float width = 0.0f);
void tag(SkateMenu& menu, const char* text, ImU32 colour);
bool primary_button(SkateMenu& menu, const char* label, bool enabled = true);
// Height left for a tab's body, keeping room for the feedback line below it.
float page_body_height(const SkateMenu& menu);
// Runs a console command and shows its reply as feedback.
void send_console(SkateMenu& menu, const CallbacksV3& callbacks, const std::string& command);
void debug_request(SkateMenu& menu, const CallbacksV3& callbacks, DebugRequest request);
// HOST shows the host form until a session exists, then that session (SESSION).
inline constexpr int multiplayer_session_tab = 1, multiplayer_voice_tab = 2, multiplayer_bans_tab = 3;
void multiplayer_display_settings(SkateMenu&, const Model&);
void multiplayer_network_page(SkateMenu&, const Model&, const CallbacksV3&);
void atmosphere_menu(SkateMenu&, const Model&, const CallbacksV3&);
void progression_page(SkateMenu&, const Model&, const CallbacksV3&);
void multiplayer_page(SkateMenu&, const Model&, const CallbacksV3&);
// Sidebar pages.
void map_page(SkateMenu&, const Model&, const CallbacksV3&);
void world_page(SkateMenu&, const Model&, const CallbacksV3&);
void build_page(SkateMenu&, const Model&, const CallbacksV3&);
void skater_page(SkateMenu&, const Model&, const CallbacksV3&);
// The SKATER page's MEAT tab (skate_menu_meat.cpp).
void meat_controls(SkateMenu&, const Model&, const CallbacksV3&);
void settings_page(SkateMenu&, const Model&, const CallbacksV3&);
// Settings > Post FX (skate_menu_world.cpp).
void graphics_page(SkateMenu&, const Model&, const CallbacksV3&);
void developer_page(SkateMenu&, const Model&, const CallbacksV3&);
}
