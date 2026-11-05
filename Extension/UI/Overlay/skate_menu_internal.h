#pragma once
#include "skate_menu.h"
#include "Extension/Plugins/plugin_ui.h"
#include <cstdint>
#include <memory>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::overlay::detail {
// Whether a text box of the game's pause menu Multiplayer pages (hub_page.cpp) has the keyboard: it
// holds B and Esc, so the ReSkate menu leaves them alone meanwhile.
bool online_embedded_typing();
}

namespace dingosdk::overlay {
// The map grid's pictures (menu_map_pictures.cpp): asked for with the levels shown, then each
// level's square, once it is on the GPU.
void want_menu_map_pictures(std::vector<MenuMapWanted> levels);
bool menu_map_picture(const std::string& asset, ImTextureID& texture, ImVec2& uv0, ImVec2& uv1);
// When a custom map's mod was installed (seconds since 1970), or 0 while not known.
std::int64_t menu_map_installed(const std::string& asset);
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

// Page layout kit, so every page reads the same way in both layouts (the side sheet and the window),
// with a mouse or a controller. A control under the controller's or the keyboard's focus is white
// with dark text.
//   begin_card / end_card  a titled dark tile grouping related controls, sized to its content
//                          (always pair them). In a wide window a page's cards flow into two
//                          columns, shared out by last frame's heights so the columns end about
//                          level, while nothing comes between them; a run of one card spans the
//                          page, up to a width that stays readable.
//   field                  a label, then the next widget beside it filling the rest of the row
//                          (stacked under the label when the page is narrow)
//   info                   a status line: muted label column, value beside it (in a half-width
//                          card too)
//   note / warn            muted help text / a warning, wrapped
//   choice                 one of a few options as "< option >": left / right, A or a click on
//                          either half steps it
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
// Whether the shared controls draw for a controller. Always now: the menu has one look in both
// layouts. Pages still ask, and their window-only branches behind it wait for their redesign.
// console_hint is what the focused control does (a controller has no hover tooltips).
bool console_style();
const std::string& console_hint();
// For a control drawn by hand: left / right while it has the focus (-1, 0, 1), which then never
// moves the focus; and the line the menu shows for it.
int console_step(bool focused);
void set_console_hint(const char* hint);
// The step the next slider takes (left / right), in place of a fortieth of its range: for a value
// with no sensible range (nudged where it is), or one that moves in whole units.
void set_console_step(double step);
// A row of buttons: equal tiles across the row, wrapping onto more rows when their names would not
// fit (`on` draws one as chosen, in blue). Returns the one pressed, or -1.
struct RowButton {
    const char* label;
    bool enabled = true;
    bool on = false;
    const char* hint = nullptr;
};
int button_row(SkateMenu& menu, const char* id, std::initializer_list<RowButton> buttons);
// Many buttons as tiles like button_row's, at least 150 px wide, as many to a row as fit. `lit`
// (from 0, -1 for none) draws as chosen. Returns the one pressed, or -1.
int button_grid(SkateMenu& menu, const char* id, const std::vector<std::string>& names, int lit);
// One of a list on one row: the label, then "< option >" that left / right (or a click on either
// half) change. With `pressed`, A on the row sets it (for a choice applied by A, such as a park
// layout) instead of stepping on.
bool option_row(SkateMenu& menu, const char* label, const char* hint, int& selected, const std::vector<std::string>& options,
                bool enabled = true, bool* pressed = nullptr);
// One entry of a list (a song) as a row, `side` at its right end; true on A or a click.
bool list_row(SkateMenu& menu, const char* id, const std::string& text, const std::string& side, bool current, bool enabled = true);
// A value as a bar that left / right (or the mouse) nudge, with a number box beside it for an exact
// value (none with NoInput, which picks from named steps). ImGui's slider arguments;
// IsItemActive / IsItemDeactivatedAfterEdit afterwards cover the bar and the box together.
bool slider_float(const char* id, float* value, float low, float high, const char* format = "%.3f", ImGuiSliderFlags flags = 0);
bool slider_int(const char* id, int* value, int low, int high, const char* format = "%d", ImGuiSliderFlags flags = 0);
bool slider_double(const char* id, double* value, double low, double high, const char* format = "%.3f", ImGuiSliderFlags flags = 0);
bool primary_button(SkateMenu& menu, const char* label, bool enabled = true);
// Height left for a tab's body, keeping room for the feedback line below it.
float page_body_height(const SkateMenu& menu);
// Runs a console command and shows its reply as feedback.
void send_console(SkateMenu& menu, const CallbacksV3& callbacks, const std::string& command);
void debug_request(SkateMenu& menu, const CallbacksV3& callbacks, DebugRequest request);
// One thing of a list (a park mod, a saved object) as a row that opens: A or a click opens its
// actions under it, and again, or B, closes them. `open` is the id of the list's open row (empty for
// none), kept by the page. `detail` is a muted line under the title, `side` sits at the right end and
// `current` marks the row with a blue edge. Returns whether the row is open: the page then draws
// what goes under it, its actions as row_actions.
bool expand_row(SkateMenu& menu, const char* id, const std::string& title, const std::string& detail, const std::string& side,
                std::string& open, bool current = false);
// The actions of an open row: chips under its title, as many to a row as fit; `on` draws the main
// one in blue. Returns the one pressed, or -1.
int row_actions(SkateMenu& menu, const char* id, std::initializer_list<RowButton> actions);
// Whether a page had a row open last frame, so B is that row's to close and not the menu's Back.
bool page_holds_back();
// A page with something of its own open (a row's details it draws itself, a page within the page) says
// so each frame: B is then the page's to read and the menu's Back waits.
void hold_back();
// X on the controller pressed this frame (the menu keeps it from ImGui: see skate_menu.cpp).
bool pad_x_pressed();
// What A, X and Y do on this page, shown in the footer's prompts in place of Select (null: not shown).
// Set while the page draws, each frame it wants them.
void page_prompts(const char* a, const char* x = nullptr, const char* y = nullptr);
// One thing of a library (a mod, a plugin, a page to go back to) as a row: a picture or a colour square,
// its name, a muted line under it, side text, and its state as a tag at the right end. `edge` marks one
// that needs a look (amber, red); `open` one whose details the page draws under it. What the player
// did to it: A or a click; X, or a click on the tag; Y, or a right click.
struct LibraryRow {
    std::string name, detail;  // detail: the line under the name (empty: a one-line row)
    std::string side;          // before the tag, such as "Page >"
    ImU32 detail_colour = 0, side_colour = 0; // 0: muted
    const char* tag = nullptr; // ON, OFF, ASKS, UPDATE...
    ImU32 tag_fill = 0, tag_ink = 0; // 0: OFF's greys
    ImU32 edge = 0;
    ImU32 swatch = 0;          // a square before the name (0: none)...
    ImTextureID image{};       // ...or a picture in its place
    bool open = false;
    const char* hint = nullptr;
};
enum class RowPress { none, a, x, y };
RowPress library_row(SkateMenu& menu, const char* id, const LibraryRow& row);
// HOST shows the host form until a session exists, then that session (SESSION).
inline constexpr int multiplayer_session_tab = 1, multiplayer_voice_tab = 2, multiplayer_bans_tab = 3, multiplayer_chat_tab = 4;
void multiplayer_display_settings(SkateMenu&, const Model&);
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
// Settings > MUSIC (Extension/Music/music_page.cpp): the player, the sound and the sources.
void music_page(SkateMenu&, const Model&, const CallbacksV3&);
// Settings > REPLAYS (skate_menu_settings.cpp): the replay editor's keyframe smoothing and export quality.
void replays_page(SkateMenu&, const Model&, const CallbacksV3&);
// Extension/Trainer/trainer_page.cpp: the SANDBOX page (once the Trainer).
void trainer_page(SkateMenu&, const Model&, const CallbacksV3&);
bool trainer_page_wanted();
bool trainer_cheats_wanted(); // `trainer open movement`: Skater > Cheats
// Skater > CHEATS: noclip, No Bail and the boosts (skate_menu_skater.cpp).
void cheats_tab(SkateMenu&, const Model&, const CallbacksV3&);
// The time of day the menu shows: the one last asked for while the game is still changing to it
// (skate_menu_world.cpp). Setting it from a menu control goes through set_time_of_day, `quiet`
// leaving the game's reply off the page.
int time_of_day_shown(const Model&);
// The level being played, by name (a session's map, else the official map loaded); empty while not known.
std::string loaded_level_name(const Model&);
void set_time_of_day(SkateMenu&, const Model&, const CallbacksV3&, int next, bool quiet);
// The PLUGINS section (plugins_page.cpp): the plugins with a page, as last published (held for the
// frame); the one the menu shows (the first when its choice is gone; nullptr for none); and its page.
std::shared_ptr<const std::vector<plugin_ui::PageEntry>> plugin_pages();
// What menu.plugin_page holds while PLUGINS shows its list of plugins: no mod id can start with this.
inline constexpr const char* plugin_manager_entry = "" "plugin-manager";
// The plugin page shown, or null while that is the list (also when the page chosen is gone).
const plugin_ui::PageEntry* shown_plugin_page(SkateMenu& menu);
void plugins_page(SkateMenu& menu);
// The menu's first page (skate_menu_addons.cpp): where the player is, the quick switches, and the pages
// to pick up again.
void home_page(SkateMenu&, const Model&, const CallbacksV3&);
// Shuffle, on Settings > MUSIC (skate_menu_settings.cpp).
void music_playback_card(SkateMenu&);
// How the menu is laid out (Settings > Screen > Menu layout: menu_layout_sheet or menu_layout_hub),
// and changing it: saved with the profile (`ui layout`), shown at once.
int menu_layout(const SkateMenu&, const Model&);
void set_menu_layout(SkateMenu&, const CallbacksV3&, int layout);
// The pages, for the shells and the home tiles (skate_menu.cpp).
enum Page {
    page_home, page_map, page_build, page_skater, page_sandbox, page_plugins, page_online, page_progress, page_mods,
    page_settings, page_developer, page_count
};
// Settings' tabs, in the order they show.
inline constexpr int settings_controls_tab = 0, settings_screen_tab = 1, settings_performance_tab = 2, settings_music_tab = 3,
                     settings_replays_tab = 4;
// Opens a page, and with `tab` (0 and up) that tab of it, for a page with tabs (Map, Build, Skater, Settings).
void open_page(SkateMenu&, int page, int tab = -1);

// A list of things (players, servers, bans) as rows whose actions open inline under them: A or a click
// on a row opens them, closing any other row's; A on the row again, or B while the focus is in it,
// closes them. A controller's A moves the focus onto the first action; B puts it back on the row.
// While open_row returns true its id stays pushed: draw the actions (action_chips, sliders, notes),
// then end_open_row(), only then:
//     if (open_row(menu, "id", row)) { action_chips(...); end_open_row(); }
struct RowTag {
    const char* text;
    ImU32 colour;
};
struct OpenRow {
    std::string name, detail, side; // detail: a muted second line (none: a one-line row); side: at the right end
    std::vector<RowTag> tags;       // badges after the name
    ImU32 swatch = 0;               // a small square before the name (0: none)
    ImU32 name_colour = 0, side_colour = 0, edge = 0; // 0: the usual; edge: a stripe down the left side
    bool openable = true;           // false: shown, with nothing to open (and not focusable)
    const char* hint = nullptr;     // what A does here, for the controller's hint line
};
bool open_row(SkateMenu& menu, const char* id, const OpenRow& row);
void end_open_row();
// The actions of an open row: compact tiles like button_row's, as many to a line as fit. Returns the
// one pressed, or -1.
int action_chips(SkateMenu& menu, const char* id, std::initializer_list<RowButton> chips);
// B on the controller: closes the open row when the focus is in it. The menu asks before B goes back.
bool close_open_row();
}
