#pragma once
#include "skate_menu.h"
#include "Extension/Plugins/plugin_ui.h"
#include <memory>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::overlay::detail {
// The game's pause menu Multiplayer pages (hub_page.cpp), drawn into the full screen menu's ONLINE
// page: in `a`..`z` of `draw`, their own focus taking the D-pad, A and LT / RT. And whether one of
// their text boxes has the keyboard.
void draw_online_embedded(ImDrawList* draw, ImVec2 a, ImVec2 z, const Model& model, const CallbacksV3& callbacks, bool input);
bool online_embedded_typing();
}

namespace dingosdk::overlay {
// The map grid's pictures (menu_map_pictures.cpp): asked for with the levels shown, then each
// level's square, once it is on the GPU.
void want_menu_map_pictures(std::vector<MenuMapWanted> levels);
bool menu_map_picture(const std::string& asset, ImTextureID& texture, ImVec2& uv0, ImVec2& uv1);
}

namespace dingosdk::overlay::menu {
// The menu chrome is laid out in pixels; px() applies the user's menu scale to
// one of those measurements. Only valid while the menu is drawing.
float px(float value);
void section(SkateMenu& menu, const char* text);
void feedback(SkateMenu& menu, const char* text);
bool toggle_row(SkateMenu& menu, const char* label, const char* hint, bool& value,
                bool available = true, const char* unavailable = "N/A");
void category_tabs(SkateMenu& menu, int& selected, std::span<const char* const> tabs, const char* id);
inline void category_tabs(SkateMenu& menu, int& selected, std::initializer_list<const char*> tabs, const char* id) {
    category_tabs(menu, selected, std::span<const char* const>(tabs.begin(), tabs.size()), id);
}

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
// A card that folds: a click on its title row (or A on the controller) closes it down to that row and opens it again.
// Returns whether the body is to be drawn; end_card() follows either way:
//     if (fold_card(menu, "id", "TITLE")) { ... }
//     end_card();
// Which cards are open is kept by id for as long as the game runs. The owner of a page can seed it (set_fold) and
// collect what the player changed (take_fold_changes) to keep it for longer.
bool fold_card(SkateMenu& menu, const char* id, const char* title, const char* subtitle = nullptr, bool open_by_default = true);
bool fold_open(std::string_view id, bool fallback);
void set_fold(std::string_view id, bool open);
bool take_fold_change(std::string& id, bool& open);
// The little arrow of a fold, drawn at the cursor's row; `open` points down.
void fold_arrow(ImVec2 at, float row_height, bool open, ImU32 colour);
void field(SkateMenu& menu, const char* label, const char* tooltip = nullptr);
void info(SkateMenu& menu, const char* label, const std::string& value);
void note(const char* text);
void warn(const char* text);
// width 0 fills the rest of the row. tooltips, when given, are shown on hovering the option at the
// same position (one per option; nullptr for none).
bool choice(SkateMenu& menu, const char* id, int& selected, std::initializer_list<const char*> options,
            bool enabled = true, float width = 0.0f, std::initializer_list<const char*> tooltips = {});
void tag(SkateMenu& menu, const char* text, ImU32 colour);
// Whether the menu is drawing full screen this frame: its shared controls (toggle_row, choice, the
// sliders, cards) draw for a controller then. console_hint is what the focused one does.
bool console_style();
const std::string& console_hint();
// For a full screen control drawn by hand: left / right while it has the focus (-1, 0, 1), which
// then never moves the focus; and the line the bottom bar shows for it.
int console_step(bool focused);
void set_console_hint(const char* hint);
// The step the next full screen slider takes (left / right), in place of a fortieth of its range:
// for a value with no sensible range (nudged where it is), or one that moves in whole units.
void set_console_step(double step);
// A row of buttons. In a window, ImGui's own side by side; full screen, equal tiles across the
// row, the pause menu's look (`on` draws one as chosen). Returns the one pressed, or -1.
struct RowButton {
    const char* label;
    bool enabled = true;
    bool on = false;
    const char* hint = nullptr;
};
int button_row(SkateMenu& menu, const char* id, std::initializer_list<RowButton> buttons);
// One of a list, under a label. In a window, a drop-down after a field() label; full screen, one
// row: the label, then "< option >" that left / right change. With `pressed`, A on the row sets it
// (for a choice applied by A, such as a park layout) instead of stepping on.
bool option_row(SkateMenu& menu, const char* label, const char* hint, int& selected, const std::vector<std::string>& options,
                bool enabled = true, bool* pressed = nullptr);
// Full screen: one entry of a list (a song) as a tall row, `side` at its right end; true on A.
bool list_row(SkateMenu& menu, const char* id, const std::string& text, const std::string& side, bool current, bool enabled = true);
// ImGui's sliders with a number box beside them for an exact value (none with NoInput, which
// picks from named steps). Same arguments; IsItemActive / IsItemDeactivatedAfterEdit afterwards
// cover the slider and the box together.
bool slider_float(const char* id, float* value, float low, float high, const char* format = "%.3f", ImGuiSliderFlags flags = 0);
bool slider_int(const char* id, int* value, int low, int high, const char* format = "%d", ImGuiSliderFlags flags = 0);
bool slider_double(const char* id, double* value, double low, double high, const char* format = "%.3f", ImGuiSliderFlags flags = 0);
bool primary_button(SkateMenu& menu, const char* label, bool enabled = true);
// Height left for a tab's body, keeping room for the feedback line below it.
float page_body_height(const SkateMenu& menu);
// Runs a console command and shows its reply as feedback.
void send_console(SkateMenu& menu, const CallbacksV3& callbacks, const std::string& command);
void debug_request(SkateMenu& menu, const CallbacksV3& callbacks, DebugRequest request);
// HOST shows the host form until a session exists, then that session (SESSION).
inline constexpr int multiplayer_session_tab = 1, multiplayer_voice_tab = 2, multiplayer_bans_tab = 3, multiplayer_chat_tab = 4;
void multiplayer_display_settings(SkateMenu&, const Model&);
void special_page(SkateMenu&, const Model&, const CallbacksV3&);
void multiplayer_network_page(SkateMenu&, const Model&, const CallbacksV3&);
void atmosphere_menu(SkateMenu&, const Model&, const CallbacksV3&);
void progression_page(SkateMenu&, const Model&, const CallbacksV3&);
void multiplayer_page(SkateMenu&, const Model&, const CallbacksV3&);
// Sidebar pages.
void map_page(SkateMenu&, const Model&, const CallbacksV3&);
void build_page(SkateMenu&, const Model&, const CallbacksV3&);
void skater_page(SkateMenu&, const Model&, const CallbacksV3&);
void dark_pop_controls(SkateMenu&, const Model&, const CallbacksV3&); // shown on SANDBOX > TUNE > TRICKLINING
void settings_page(SkateMenu&, const Model&, const CallbacksV3&);
// Settings > Post FX (skate_menu_world.cpp).
void graphics_page(SkateMenu&, const Model&, const CallbacksV3&);
void developer_page(SkateMenu&, const Model&, const CallbacksV3&);
// Extension/Music/music_page.cpp
void music_page(SkateMenu&, const Model&, const CallbacksV3&);
// Extension/Trainer/trainer_page.cpp: the SANDBOX page (once the Trainer).
void trainer_page(SkateMenu&, const Model&, const CallbacksV3&);
bool trainer_page_wanted();
bool trainer_cheats_wanted(); // `trainer open movement`: Skater > Cheats
// Skater > CHEATS: noclip, No Bail and the boosts (skate_menu_skater.cpp).
void cheats_tab(SkateMenu&, const Model&, const CallbacksV3&);
// The addons' own pages (skate_menu_skater.cpp), and the ADDONS page itself (skate_menu_addons.cpp).
void addons_page(SkateMenu&, const Model&, const CallbacksV3&);
// The time of day the menu shows: the one last asked for while the game is still changing to it
// (skate_menu_world.cpp). Setting it from a menu control goes through set_time_of_day, `quiet`
// leaving the game's reply off the page.
int time_of_day_shown(const Model&);
void set_time_of_day(SkateMenu&, const Model&, const CallbacksV3&, int next, bool quiet);
// The PLUGINS section (plugins_page.cpp): the plugins with a page, as last published (held for the
// frame); the one the menu shows (the first when its choice is gone; nullptr for none); and its page.
std::shared_ptr<const std::vector<plugin_ui::PageEntry>> plugin_pages();
const plugin_ui::PageEntry* shown_plugin_page(SkateMenu& menu);
void plugins_page(SkateMenu& menu);
// The fullscreen menu's first page: a tile for each part of the menu (skate_menu_addons.cpp).
void home_page(SkateMenu&, const Model&, const CallbacksV3&);
// Shuffle, on the Music addon (skate_menu_settings.cpp).
void music_playback_card(SkateMenu&);
// Whether the menu fills the screen (Settings > Screen > Menu style), and changing it: saved
// with the profile (`ui fullscreen`), shown at once.
bool menu_fullscreen(const SkateMenu&, const Model&);
void set_menu_fullscreen(SkateMenu&, const CallbacksV3&, bool fullscreen);
// The pages, for the shells and the home tiles (skate_menu.cpp).
enum Page {
    page_home, page_map, page_build, page_skater, page_sandbox, page_addons, page_plugins, page_online, page_progress, page_mods,
    page_settings, page_developer, page_count
};
enum Addon { addon_all, addon_music, addon_count, addon_first = addon_music };
void open_page(SkateMenu&, int page, int addon = -1);
}
