#pragma once
#include "Engine/Game/World/park_rotation.h"
#include "Engine/Game/World/park_editor.h"
#include "Engine/Game/World/world_layers.h"
#include "Engine/Game/World/world_controls.h"
#include "Engine/Game/Rendering/graphics_controls.h"
#include "Engine/Game/Profile/object_persistence_model.h"
#include "Engine/Game/Profile/player_card.h"
#include "Engine/Game/Profile/progression.h"
#include "Engine/Game/Input/controller_bindings.h"
#include "Engine/Game/Settings/named_settings.h"
#include "Engine/Game/Multiplayer/session_model.h"
#include "Engine/Game/UI/menu_scale.h"
#include "Engine/Game/Skater/first_person_spring.h"

#include "Engine/Core/Console/console_entry.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::plugins {
struct DialogSpec;
}
namespace dingosdk::overlay {
// The overlay's menu, console or a dialog has the input (input_capture.cpp).
bool interface_open() noexcept;
// A plugin dialog (reskate.ui.dialog) is open: the overlay keeps the input while it is.
void set_plugin_dialog_active(bool active) noexcept;
// Game input a plugin's game layer holds (input_capture.cpp): pad buttons kept from the game, held
// down for it, or all game input paused for half a second; the keyboard as the overlay reads it,
// and the mouse wheel's notches since the last call.
void hide_game_buttons(std::uint16_t buttons) noexcept;
// Keys (virtual key codes) the game does not see go down until this is called again: their presses
// are dropped from every way it reads the keyboard, releases pass. ReSkate's own reads see them. Any thread.
void hide_game_keys(std::span<const std::uint8_t> keys) noexcept;
void hold_game_buttons(std::uint16_t held) noexcept;
void pause_game_input(bool paused) noexcept;
bool key_down(int virtual_key) noexcept;
int take_mouse_wheel() noexcept;
// The mouse since the last call, whatever the game does with it: raw counts right and down, and the
// wheel in 120 a notch (away from you positive). False and zeros while the game is not in front or a
// menu, the console or chat is open (what moved meanwhile is dropped). One reader: the plugin runtime.
struct MouseLook {
    long x = 0, y = 0, wheel = 0;
};
bool read_mouse_look(MouseLook& look) noexcept;
// The mouse while a plugin's screen is open (plugin_overlay.cpp): where it is in the window's pixels,
// its buttons, the wheel's notches since the last read, and whether it is over the screen's panel.
// False while no plugin screen is open.
struct PluginCursor {
    float x{}, y{}, wheel{};
    bool left{}, right{}, middle{}, over_screen{};
};
bool read_plugin_cursor(PluginCursor& cursor) noexcept;
// What was typed (UTF-8, printable characters only) while a plugin's screen is open and it takes the
// typing, since the last call; `capture` keeps it taking the typing for the next 300 ms, during which
// Esc is the plugin's instead of closing the screen.
std::string take_plugin_text(bool capture) noexcept;
// reskate.ui.dialog (plugin_dialog.cpp): one dialog at a time over the game, input captured while it
// is open. The same plugin and id again updates it and keeps what was typed. Empty when shown, else
// why not. Answers reach the plugin as throwdown kit events (action "dialog", throwdown_kit.h).
std::string open_plugin_dialog(std::string_view plugin, const plugins::DialogSpec& spec);
// An empty id closes any of the plugin's.
void close_plugin_dialog(std::string_view plugin, std::string_view id);
struct Level {
    std::string asset;
    std::string display_name;
    std::vector<std::string> start_points;
    std::string manifest_start_point;
    // False only for a Studio manifest fallback which is valid as a detached
    // LM destination, not as the native request's base/root level.
    bool native_registered = true;
    bool can_load = true;
    std::string load_block_reason;
    bool custom = false; // Declared by a mod's level manifest.

    const std::string* automatic_start_point() const noexcept {
        if (!manifest_start_point.empty()) return &manifest_start_point;
        return !native_registered && start_points.size() == 1 ? &start_points.front() : nullptr;
    }
};

enum class DebugAction {
    set_free_camera,
    set_game_ui_hidden,
    set_camera_speed,
    restore_debug,
    set_noclip,
    add_forward_velocity,
    set_forward_velocity_speed,
    set_no_bail,
    set_park_editor,
    add_up_velocity,
    set_up_velocity_speed,
    add_offboard_up_velocity,
    set_offboard_up_velocity_speed,
    set_first_person,
    set_first_person_fov,
    set_first_person_spring,
    set_first_person_offset_x,
    set_first_person_offset_y,
    set_first_person_offset_z,
    set_first_person_pitch,
    set_first_person_yaw,
    set_first_person_roll,
    set_first_person_spring_up,
    set_first_person_spring_down,
    set_first_person_spring_left,
    set_first_person_spring_right,
    set_first_person_stabilize,
    set_first_person_follow_flips,
    set_first_person_smoothing,
    set_first_person_head_pitch,
    set_first_person_head_roll,
    set_first_person_bob,
    set_first_person_board_only,
    set_first_person_camera_cycle,
    reset_first_person_arm,
    set_free_camera_fov,  // 0 = the game's own FOV
    set_revert_boost_enabled,
    set_revert_boost_speed,
    set_probe_enabled,
    set_dark_pop_enabled,
    set_dark_pop_unpatch,
    set_dark_pop_hold,
    set_dark_pop_one_press,
    set_dark_pop_route_stay,
    set_dark_pop_route_guard,
    set_dark_pop_keep_speed,
    set_dark_pop_catch,
    set_dark_pop_feet,
    set_dark_pop_adaptive,
    set_dark_pop_timeout,
    set_dark_pop_strength,
    set_dark_pop_hold_landing,
    set_dark_pop_dark_exit,
    set_dark_pop_full_flight,
    set_dark_pop_hold_flight,
    set_dark_pop_stay_physical,
    set_dark_pop_early_physics,
    set_dark_pop_reaim,
    set_dark_pop_hide_dpad,
    // Keep the last action in sync with the bound in request_scheduler.h.
};

struct DebugRequest {
    DebugAction action = DebugAction::restore_debug;
    bool enabled = false;
    float value = 0.0f;
};

struct FlightInput {
    bool active = false;
    bool boost = false;
    float right = 0, up = 0, forward = 0;
    float look_x = 0, look_y = 0;
};

// May change what the GAME reads from an XInput pad (never what ReSkate itself reads). `user` is
// the XInput slot. Called on whatever thread the game polls from, once per poll that returns a
// pad. Plugins' hidden pad controls (Extension/Skater/pad_hide.h, reskate.input.hide).
struct GamepadValues {
    std::uint16_t buttons = 0;
    std::uint8_t left_trigger = 0, right_trigger = 0;
    std::int16_t left_x = 0, left_y = 0, right_x = 0, right_y = 0;
};
using GamepadFilter = void (*)(unsigned user, GamepadValues& pad) noexcept;

struct DebugModel {
    bool available = false;
    bool camera_available = false;
    bool ui_available = false;
    bool free_camera = false;
    bool first_person = false;
    // Vertical degrees applied to the first-person view; 0 keeps the camera's own.
    float first_person_fov = 0;
    float free_camera_fov = 0;  // 0 = the game's own FOV
    first_person::Settings first_person_arm;
    bool park_editor = false;
    bool camera_transform_valid = false;
    std::array<float, 16> camera_transform{};
    float camera_fov = 55;
    bool noclip = false, noclip_available = false;
    bool forward_velocity_available = false;
    bool up_velocity_available = false;
    bool offboard_up_velocity_available = false;
    std::string camera_unavailable = "Waiting for local controls.";
    std::string noclip_unavailable = "Waiting for local controls.";
    std::string forward_velocity_unavailable = "Waiting for local controls.";
    std::string up_velocity_unavailable = "Waiting for local controls.";
    std::string offboard_up_velocity_unavailable = "Waiting for local controls.";
    bool no_bail = false, no_bail_available = false, no_bail_active = false;
    std::uint64_t noclip_velocity_updates = 0, noclip_motion_updates = 0;
    std::uint64_t forward_velocity_updates = 0;
    std::uint64_t up_velocity_updates = 0;
    std::uint64_t offboard_up_velocity_updates = 0;
    bool game_ui_hidden = false;
    bool settings_owned = false;
    // ReSkate free-flight speed in world units per second.
    float camera_speed = 15.0f;
    // Boost speeds; 0 is off (the stock setting).
    float forward_velocity_speed = 0.0f;
    float up_velocity_speed = 0.0f;
    float offboard_up_velocity_speed = 0.0f;
    // Revert Boost: a forward speed added when the game auto reverts a landing.
    bool revert_boost = false;
    float revert_boost_speed = 1.0f; // strength multiplier
    std::uint64_t revert_boost_count = 0;
    // Dark Pop: the pre-0.32.0 dark pop (see no_bail.cpp). The rest are its advanced settings.
    bool dark_pop = false;
    bool dark_pop_unpatch = true;   // placement entered the pre-0.32.0 way at the dark landing
    bool dark_pop_hold = true;      // the old hold (foot-attach mode 2) in the landing window
    bool dark_pop_one_press = true; // the D-pad must be let go before the next arming
    float dark_pop_route_stay = 0.0f;   // ticks of 601 before the stay bit is cancelled (the sink length; 1.9: 0, the 1.0 setting, by default)
    float dark_pop_route_guard = 12.0f; // pop ticks guarded after a 601 exit
    bool dark_pop_keep_speed = true;    // the landing's horizontal speed put back at the 601 exit
    bool dark_pop_catch = true;         // 1.0: a dismount straight from the air on an armed dark flight is routed too
    bool dark_pop_feet = false;         // 1.7: feet-only hold (1.9: off by default) (the plant's own foot attaches cleared in the sink, a two-tick one requested)
    bool dark_pop_adaptive = true;      // 1.7: the sink ends the tick the board starts rolling over
    bool dark_pop_timeout = true;       // 1.9: a landing 201 timed out on (board dark, resting) is routed to the sink too
    float dark_pop_strength = 8.0f;     // 2.4: the launch (m/s) the bodies' vertical velocity is lifted to on the pop's second and third ticks; 0 = off
    bool dark_pop_hold_landing = true;  // 2.0: +0x13cc bit 20 raised around the chooser so a timed-out landing can be staged by the game
    bool dark_pop_dark_exit = true;     // 2.1: the board put back as it landed on the sink's exit tick
    bool dark_pop_full_flight = true;   // 2.1: the landing countdown floored at the ballistic time on a pop's flight
    bool dark_pop_hold_flight = true;   // 2.3: contact landings blocked on a pop's flight while the countdown runs
    float dark_pop_stay_physical = 6.0f; // 2.5: ticks after the exit the pop stays in the physics air state (the jump-fired bit hidden); 0 = off
    bool dark_pop_early_physics = true;  // 2.6: a pop whose jump fired on its first ticks (180 landings) stays physical for the whole flight
    bool dark_pop_reaim = true;
    bool dark_pop_hide_dpad = true;      // 3.1: in the air the game's polls see the D-pad let go (no Quick Drop radial)          // 2.7: instead, after Stay physical ticks the animation's flight curve is re-aimed from where the body is and handed over
    std::uint64_t dark_pop_count = 0;
    bool probe = false; // Dark pop probe: writes logs\dark-pop-probe.csv
    bool camera_position_valid = false;
    bool skater_position_valid = false;
    std::array<float, 3> camera_position{};
    std::array<float, 3> skater_position{};
    std::uintptr_t skater_identity = 0;
    std::string status;
};

enum class OfflineFeatureGroup {
    activities,
    fast_travel,
    progression,
    developer_menus,
    board_wear,
    player_collision,
    restore_all,
};

struct OfflineFeatureRequest {
    OfflineFeatureGroup group = OfflineFeatureGroup::restore_all;
    bool enabled = false;
};

struct OfflineFeatureGroupModel {
    bool available = false;
    bool effective = false;
    bool override_active = false;
};

struct EngineVariableModel {
    std::string name;
    bool available = false;
    bool value = false;
    bool override_active = false;
};

struct OfflineFeatureModel {
    bool available = false;
    bool settings_owned = false;
    OfflineFeatureGroupModel activities;
    OfflineFeatureGroupModel fast_travel;
    OfflineFeatureGroupModel progression;
    OfflineFeatureGroupModel developer_menus;
    OfflineFeatureGroupModel board_wear;
    OfflineFeatureGroupModel player_collision;
    bool skater_slots_available = false;
    bool skater_slots_override_active = false;
    bool skater_slot_manager_available = false;
    bool skater_slot_selectors_enabled = false;
    std::uint32_t skater_slot_count = 0;
    std::uint32_t skater_slot_target = 10;
    std::string skater_slot_status;
    bool main_missions_available = false;
    bool main_missions_override_active = false;
    bool main_missions_authored_offline_route = false;
    std::uint32_t main_mission_claim_leases = 0;
    std::uint32_t main_mission_target = 20;
    std::uint32_t progression_unlock_claim_leases = 0;
    std::uint32_t progression_unlock_target = 2;
    std::uint32_t onboarding_dependency_claim_leases = 0;
    std::uint32_t onboarding_dependency_target = 2;
    std::uint32_t mission_pending_restore = 0;
    std::string main_mission_status;
    std::string status;
    // Exact-build, reflection-validated boolean settings. The console exposes
    // only this allowlist; arbitrary addresses and unvalidated fields are never
    // accepted as engine variables.
    std::vector<EngineVariableModel> variables;
};

using ConsoleLogLine = dingosdk::ConsoleLogLine;

struct MissionRow {
    std::string id, group;
    int completed = -1;
};

// Keyframe smoothing (Extension/ReplayKeyframes/replay_keyframes.h): started, switched on, and how
// smooth (0..1) when on.
struct KeyframeSmoothingModel {
    bool available = false, enabled = false;
    float amount = 1.0f;
};
// The smoothness as the menus show it, in percent.
inline int keyframe_smoothness_percent(const KeyframeSmoothingModel& k) { return static_cast<int>(k.amount * 100.0f + 0.5f); }

struct Model {
    std::string state = "Waiting for native state";
    std::string detail;
    std::vector<Level> levels;
    bool can_queue_load = false;
    std::string load_block_reason = "Native load handler is not connected.";
    DebugModel debug;
    OfflineFeatureModel offline;
    std::vector<NamedSettingModel> engine_settings;
    std::vector<ConsoleLogLine> console_log;
    bool missions_available = false;
    std::vector<MissionRow> missions;
    std::string mission_feedback;
    ParksModel parks;
    WorldLayersModel world;
    WorldControlsModel world_controls;
    ProgressionModel progression;
    PlayerCardModel player_card;
    KeyframeSmoothingModel keyframe_smoothing;
    ObjectPersistenceModel object_persistence;
    ParkEditorModel editor;
    // The console commands plugins added (Extension/Plugins/plugin_runtime.h command_hints), for
    // the console's lookup beside ReSkate's own.
    struct PluginCommand {
        std::string name, args, help, plugin; // name: one to four words
    };
    std::vector<PluginCommand> plugin_commands;
    float menu_scale = default_menu_scale;
    float menu_scale_fullscreen = default_menu_scale; // the full screen menu's own size (`ui scalefull`)
    // The ReSkate menu fills the screen, made for a controller, instead of a window (`ui fullscreen`).
    bool menu_fullscreen = false;
    // Offline mode: no Steam, so every multiplayer page and control is hidden.
    bool steam_offline = false;
    ControllerBindingsModel bindings;
    GraphicsControlsModel graphics;
    MultiplayerModel multiplayer;
    // Host bookkeeping: the revision each part of this copy was taken at (zero:
    // never). A reader that hands its previous copy back to read_model has only
    // the parts that changed since copied; the rest are left as they are.
    struct Revisions {
        std::uint64_t model{}, debug{}, offline{}, engine_settings{};
    } revisions;
};

// Callbacks run on the presentation thread. They must be bounded and thread
// safe; queue callbacks must enqueue requests, never call engine code here.
struct Callbacks {
    void* user = nullptr;
    void (*read_model)(void* user, Model& output) = nullptr;
    bool (*queue_load)(void* user, const char* asset, const char* start_point,
                       const char* lm_level, const char* lm_start_point,
                       char* result, std::size_t result_size) = nullptr;
    bool (*queue_debug)(void* user, const DebugRequest& request,
                        char* result, std::size_t result_size) = nullptr;
};

// Extended callback table for hosts that expose process-local offline controls.
// Keep Callbacks and DingoSDKOverlayStart unchanged for binary compatibility with
// hosts built against the original four-field table.
struct CallbacksV2 {
    void* user = nullptr;
    void (*read_model)(void* user, Model& output) = nullptr;
    bool (*queue_load)(void* user, const char* asset, const char* start_point,
                       const char* lm_level, const char* lm_start_point,
                       char* result, std::size_t result_size) = nullptr;
    bool (*queue_debug)(void* user, const DebugRequest& request,
                        char* result, std::size_t result_size) = nullptr;
    bool (*queue_offline_feature)(void* user, const OfflineFeatureRequest& request,
                                 char* result, std::size_t result_size) = nullptr;
};

// Extended callback table for the in-game command console. Keep V1 and V2
// unchanged so previously built hosts remain binary compatible.
// Internal private action queue, separate from the stable V3 callback ABI and console history.
using MultiplayerQueue = bool (*)(const char *action, const char *argument, const char *password,
                                 char *result, std::size_t size);
void set_multiplayer_queue(MultiplayerQueue) noexcept;
// What the session needs being fetched, a map and plugins (Extension/Assets/map_download.h), for its card.
struct MapDownloadCard {
    int stage{};                // 0 none, 1 asking, 2 downloading, 3 installing, 4 applying, 5 joining
    std::string map, package, author, version, description;
    std::string have;            // the version installed here, when the host's replaces it
    std::size_t step{}, steps{}; // applying: the merge's steps done of all
    std::string step_name;
    bool server{}, installed{}, moved{}, choice{true};
    std::uint64_t bytes{};       // asking: the package's size; 0 when it is not known
    std::uint64_t received{}, total{};
    std::uint32_t yes_bind{}, no_bind{};
    // Everything asked for, the map first when it is needed, then a dedicated server's plugins.
    // One map alone has the map's own card; anything more is listed.
    struct Item {
        std::string name, author, version, have;
        bool plugin{}, installed{};
        std::uint64_t bytes{};
    };
    std::vector<Item> items;
    std::size_t current{}; // downloading, installing: the one in hand
};
using MapDownloadFeed = MapDownloadCard (*)();
void set_map_download_feed(MapDownloadFeed) noexcept;
using MapDownloadAnswer = void (*)(bool download) noexcept;
void set_map_download_answer(MapDownloadAnswer) noexcept;
// The card's icon: the package's own picture as PNG bytes, or nothing to show a plain tile.
// Any thread; decoded here and uploaded when the overlay next draws.
void set_map_download_icon(const std::string& png) noexcept;
// The pictures for a dedicated server's vote for the next map (Extension/Assets/map_icons.h):
// each map's name in the vote and its picture: a mod's icon as PNG bytes, or one of the game's
// own maps' as pixels (straight-alpha RGBA, a square of `side`). Neither: it has none. Any thread.
struct MapVoteIcon {
    std::string name, png;
    std::vector<unsigned char> rgba;
    unsigned side{};
};
void set_map_vote_icons(const std::vector<MapVoteIcon>& icons) noexcept;
// The menu's map grid (Map > Travel): the levels it shows ask for their pictures, which
// Assets/map_icons.cpp finds off the game's thread (MapVoteIcon::name is then the level's asset).
struct MenuMapWanted {
    std::string asset, name;
};
std::vector<MenuMapWanted> take_menu_map_request() noexcept;
void set_menu_map_pictures(const std::vector<MapVoteIcon>& pictures) noexcept;
// That vote's popup (map_download_card.cpp), for the runtime, which reads the controller and
// holds the game's input while it is up: whether it is, moving the pick, voting for the pick
// (the popup then closes), and closing it without a vote. Any thread.
// A message the player has to see and say OK to, in the middle of the screen (message_card.cpp):
// that they were kicked or banned from a server, and why. One at a time: a new one takes the
// place of one still up. For the runtime, which reads the controller and holds the game's input
// while it is up, as for the vote below: whether it is, and closing it. Any thread.
void show_message(std::string title, std::string text) noexcept;
bool message_asking() noexcept;
void message_dismiss() noexcept;
bool map_vote_asking() noexcept;
void map_vote_move(int by) noexcept;
void map_vote_take() noexcept;
void map_vote_dismiss() noexcept;
// The session's text chat, read by the chat panel every frame (thread-safe, cheap).
using ChatFeed = MultiplayerChat (*)();
void set_chat_feed(ChatFeed) noexcept;
// The game's pause menu, as far as ReSkate's own pages in it go: whether one of them is the
// page on screen, and which (`section` 0: Multiplayer, 1: Mod Options). The overlay draws
// everything in such a page itself, its tabs too (hub_page.cpp). Read every presented frame
// (thread-safe, cheap).
struct HubPage {
    bool visible{};
    int section{};
};
using HubPageFeed = HubPage (*)();
void set_hub_page_feed(HubPageFeed) noexcept;
// Text the game draws with its debug text natives, where retail draws nothing (the
// S.K.A.T.E. throwdown HUD). One frame of lines, positioned in a width x height screen;
// color is R, G, B, A bytes (ImGui's IM_COL32 layout). Read every presented frame
// (thread-safe, cheap; empty = nothing).
struct GameTextLine {
    float x{}, y{}, scale{1};
    std::uint32_t color{0xffffffffU};
    bool centered{}; // x is the middle of the line
    std::string text;
};
struct GameText {
    float width{}, height{};
    std::vector<GameTextLine> lines;
};
using GameTextFeed = GameText (*)();
void set_game_text_feed(GameTextFeed) noexcept;
// The S.K.A.T.E. throwdown's HUD as ReSkate draws it (skate_hud_overlay.cpp), read from what
// the game's own debug HUD lays out each frame: the messages at the top (what to do, the
// trick to copy, how an attempt went) and every player's letters. Read every presented
// frame; no messages and no players = nothing to draw.
struct SkateHudMessage {
    enum class Kind : std::uint8_t { prompt, trick, success, failure } kind{};
    std::string text;
};
struct SkateHudPlayer {
    std::string name; // empty when the game has none
    int letters{};    // 0-5 of S.K.A.T.E.
    bool up{};        // whose turn it is
    bool out{};       // eliminated
};
struct SkateHud {
    std::vector<SkateHudMessage> messages; // top to bottom
    std::vector<SkateHudPlayer> players;   // in the game's order
    std::vector<std::string> done_tricks;  // set so far this game, oldest first (none repeat)
};
using SkateHudFeed = SkateHud (*)();
void set_skate_hud_feed(SkateHudFeed) noexcept;
// ReSkate's own nametags: one per other player, placed over the world with the camera the
// client last used. Close ones show a name and distance, far ones a dot, and players off
// screen a dot at the screen's edge. Empty = nothing to draw (off, or the game hides its UI).
// One line in a player's bubble stack: the text (masked when the chat filter is on), the line
// as sent when the filter changed it (same length; emote names are taken back from it), how
// far its pop-in has come (0 just arrived, 1 settled) and how opaque it still is (1 down to 0).
struct NametagBubble {
    std::string text, raw;
    float appear{1}, fade{1};
    bool hint{}; // grey typing discs, not a chat line. text is ".", "..", or "..." for 1, 2, or 3 discs
};
struct Nametag {
    std::array<float, 3> position{}; // above the skater's head, world space
    std::string name;
    std::uint32_t color{0xffffffffU}; // R, G, B, A bytes (IM_COL32)
    std::string tag;                  // role badge before the name ("Dev", "ReSkate Admin", "Content Creator", "Centrix", "Homie", "Admin", "Host", "Friend")
    float distance{};                 // metres from the local skater
    bool talking{};
    std::string extra;                // a plugin's short tag above the name ("" for none)
    std::uint32_t extra_color{0xffffffffU};
    // Recent chat lines to show as bubbles above the head, oldest first ("" = none).
    std::vector<NametagBubble> bubbles;
    bool self{};                      // the local player: bubbles only, never a name or dot
    bool nameless{};                  // another player whose name is not shown: bubbles only too
};
struct Nametags {
    std::array<float, 16> camera{}; // world matrix: right, up, back, position rows
    float vertical_fov{};
    bool show_names{true};          // draw the name, distance and role badge
    bool show_bubbles{};            // draw chat bubbles above the heads
    float bubble_distance{40.f};    // furthest a player may be and still show a bubble (metres)
    float name_distance{120.f};     // furthest a player's name shows; past it they are a dot
    bool dots{true};                // draw those dots, and the ones at the edge for off-screen players
    std::vector<Nametag> tags;
};
using NametagFeed = Nametags (*)();
void set_nametag_feed(NametagFeed) noexcept;
// The skater item grids' favorites, search and filter (item_browser_overlay.cpp): what the host's
// grids show now, and what the keys ask of them. The view is read while a grid is open; every
// function is thread-safe and only queues work for the client thread.
struct ItemBrowserView {
    bool open{};                // an item grid has the focus
    std::string search, filter; // the search text and the filter's name
    unsigned shown{}, total{};  // tiles in the grid that has the focus
    bool focused_favorite{};    // the highlighted item is a favorite
    // The mod the highlighted item is from (empty title: the game's own), for its card: its icon
    // file (UTF-8), how many items it has, and whether it only changes one of the game's.
    std::string mod_title, mod_author, mod_version, mod_icon;
    unsigned mod_items{};
    bool mod_changed{};
};
struct ItemBrowserHost {
    ItemBrowserView (*view)(){};
    void (*toggle_favorite)(){};
    void (*set_search)(const char *text){};
    void (*cycle_filter)(){};
};
// `host` must stay valid for the life of the process.
void set_item_browser(const ItemBrowserHost *host) noexcept;
// Where the item grids' own search bar and prompts end, as the overlay last drew them, in pixels: a
// plugin's own prompts go under them. `unit` is the prompts' scale (a keycap is 20 units high). All
// zero while no grid is open. Any thread.
struct ItemPrompts {
    float left{}, top{}, width{}, unit{};
};
ItemPrompts item_browser_prompts() noexcept;
// A presented frame as the game drew it, before ReSkate's own UI goes on top: 8-bit RGBA rows
// (BGRA when `bgra`), `pitch` bytes apart. Valid only during the call it is handed to.
struct GrabbedFrame {
    const std::uint8_t *data{};
    std::uint32_t width{}, height{}, pitch{};
    bool bgra{};
    // The game's depth for the same frame, when it was found (depth_probe.cpp): 32-bit floats,
    // reversed (1 near, 0 at the far plane), `depth_pitch` bytes a row, the size of the frame.
    const std::uint8_t *depth{};
    std::uint32_t depth_pitch{};
};
// Screen captures (Extension/Rendering/Capture, reskate.capture): asked on every presented frame, on
// the render thread, whether they want its pixels, and handed them when they do. While `active` says
// so the overlay keeps working even with nothing of its own on screen.
struct FrameGrabHost {
    bool (*active)() noexcept {};
    bool (*wants_frame)() noexcept {};
    void (*take_frame)(const GrabbedFrame &frame) noexcept {};
};
// `host` must stay valid for the life of the process.
void set_frame_grab(const FrameGrabHost *host) noexcept;
// Research aid for captures (`capture depth`): the game's depth buffers over the next two frames are
// copied and written to `folder` as pictures, with depth-probe.txt (depth_probe.cpp).
void request_depth_probe(const wchar_t *folder) noexcept;
using ParkSurfaceQueue = bool (*)(const EditorSurfaceRequest &);
void set_park_surface_queue(ParkSurfaceQueue) noexcept;
using ParkPreviewQueue = bool (*)(const EditorPreviewRequest &);
void set_park_preview_queue(ParkPreviewQueue) noexcept;
using ParkSelectionQueue = bool (*)(const EditorSelectionRequest &);
void set_park_selection_queue(ParkSelectionQueue) noexcept;
using ParkPasteQueue = bool (*)(const EditorPasteRequest &);
void set_park_paste_queue(ParkPasteQueue) noexcept;
using ParkObjectSetQueue = bool (*)(const EditorObjectSetRequest &);
void set_park_object_set_queue(ParkObjectSetQueue) noexcept;
using ParkDeleteQueue = bool (*)(const EditorDeleteRequest &);
void set_park_delete_queue(ParkDeleteQueue) noexcept;
struct CallbacksV3 {
    void* user = nullptr;
    void (*read_model)(void* user, Model& output) = nullptr;
    bool (*queue_load)(void* user, const char* asset, const char* start_point,
                       const char* lm_level, const char* lm_start_point,
                       char* result, std::size_t result_size) = nullptr;
    bool (*queue_debug)(void* user, const DebugRequest& request,
                        char* result, std::size_t result_size) = nullptr;
    bool (*queue_offline_feature)(void* user, const OfflineFeatureRequest& request,
                                 char* result, std::size_t result_size) = nullptr;
    bool (*queue_console_command)(void* user, const char* command,
                                  char* result, std::size_t result_size) = nullptr;
};

// Plays one of the game's own menu sounds, by its number in the game's list (ui_sound.h: 3
// moving the focus, 7 pressing something, 56 changing tab). Any thread.
using UiSound = void (*)(std::uint32_t sound) noexcept;
void set_ui_sound(UiSound) noexcept;
// One of the game's menu sounds, by its number (UI/NativeMenu/ui_sound.h): 3 moving the focus,
// 7 pressing, 56 changing tab, as the pause menu's own pages make them.
void play_ui_sound(std::uint32_t sound) noexcept;
// What those pages read the game with and act through: callbacks as the overlay's own, with a
// read_model that takes a snapshot and leaves the console log alone (it is asked a few times a
// second while a page is drawn).
void set_hub_callbacks(const CallbacksV3 &callbacks) noexcept;

struct Status {
    bool started = false;
    bool bound = false;
    bool ready = false;
    bool visible = false;
    bool stopping = false;
    bool failed = false;
    std::uint64_t rendered_frames = 0;
};
bool keyboard_shortcuts_allowed() noexcept;

// A short message in the top-left corner of the game window: a title line and
// optional detail, stacked under earlier ones and fading out on their own
// (info a few seconds, warnings and errors longer). Safe from any thread and
// before the overlay has started: queued notices show once the first frame
// draws. They take no input and never open the menu.
enum class NoticeLevel { info, warning, error };
void notify(NoticeLevel level, std::string title, std::string text = {}) noexcept;
}

// Call outside DllMain, before the game's first DXGI factory is created. No
// remote-process access and no image-specific patching happen in this component.
extern "C" __declspec(dllexport) bool DingoSDKOverlayStart(
    const dingosdk::overlay::Callbacks* callbacks);
extern "C" __declspec(dllexport) bool DingoSDKOverlayStartV2(
    const dingosdk::overlay::CallbacksV2* callbacks);
extern "C" __declspec(dllexport) bool DingoSDKOverlayStartV3(
    const dingosdk::overlay::CallbacksV3* callbacks);

namespace dingosdk::overlay {
// The first Xbox (XInput) or PlayStation (DualShock 4 / DualSense) controller, read directly.
// Sticks are -1..1 (up and right positive), triggers 0..1; `buttons` is XInput's wButtons bit mask.
// Plugins read it (reskate.input.gamepad); safe from any thread.
struct Gamepad {
    bool valid = false;
    bool playstation = false;
    float left_x = 0, left_y = 0, right_x = 0, right_y = 0;
    float left_trigger = 0, right_trigger = 0;
    unsigned buttons = 0;
};
Gamepad read_gamepad();
// The full screen menu's page draws its own controls with their own focus (ONLINE: the pause
// menu's pages): the controller then drives that, not ImGui's navigation.
inline std::atomic<bool>& menu_page_owns_pad() {
    static std::atomic<bool> value{false};
    return value;
}
// The controller is in charge of the full screen menu: the pointer is hidden (and points at
// nothing) until the mouse moves or clicks (overlay_render.cpp sets it, cursor.cpp clears it).
inline std::atomic<bool>& menu_pad_in_charge() {
    static std::atomic<bool> value{false};
    return value;
}
// The menu was last drawn full screen (its controller pace, overlay_render.cpp).
inline std::atomic<bool>& menu_fullscreen_shown() {
    static std::atomic<bool> value{false};
    return value;
}
// `menu` (and the controller bind for it) asks for the ReSkate menu to open; the overlay takes it.
inline std::atomic<bool>& menu_open_flag() {
    static std::atomic<bool> value{false};
    return value;
}
// Reads of a controller between these two calls see the real device even while ReSkate keeps
// the game from reading it (freecam, or a plugin's input capture). Same thread, paired.
// Plugins read controllers this way (Extension/Plugins/plugin_joystick.h).
void begin_input_access() noexcept;
void end_input_access() noexcept;
// When the player last worked the controls (GetTickCount64 ms; 0: not since the game started): a
// key or a mouse button or move the game read, a gamepad's buttons, sticks or triggers, or a
// joystick a plugin reads. A multiplayer server is told while they do (PacketKind::present), so a
// player whose skater stands still (flying a plugin's drone) is not taken for away. Any thread.
std::uint64_t controls_touched_at() noexcept;
// A joystick a plugin read (Extension/Plugins/plugin_joystick.h): its buttons and six axes.
void note_joystick(std::uint32_t buttons, const std::array<float, 6>& axes) noexcept;
}

// Nonblocking. A later Present releases GPU resources after its fence completes.
// The DLL stays pinned until process exit so no thread can return into unloaded
// detours. Input is released immediately, even if rendering has stopped.
extern "C" __declspec(dllexport) void DingoSDKOverlayRequestStop();
extern "C" __declspec(dllexport) void DingoSDKOverlayGetStatus(dingosdk::overlay::Status* status);
// Poll only on the client update thread. Returns no input while either overlay
// surface is open, the game is unfocused, or the overlay has stopped.
extern "C" void DingoSDKOverlayReadFlightInput(dingosdk::overlay::FlightInput* input, bool flight_active, bool player_flight = false);
extern "C" void DingoSDKOverlaySetFreecamInputCapture(bool active);
// The same hold on the game's input while a card asks something of the player and a
// controller answers it (the map download's): the buttons that answer must not also skate.
extern "C" void DingoSDKOverlaySetPromptInputCapture(bool active);
namespace dingosdk::overlay {
// Route gameplay Start/Escape through the custom challenge's single menu
// request. Our independent controller/key readers continue seeing the press.
void capture_challenge_menu_input(bool active,bool native_panel=false,bool opening_key_down=false) noexcept;
}
// The keyboard keys such a card answers to, read past that hold: bit 0 left, 1 right, 2 Enter,
// 3 Esc. Nothing while the game is not in front or the menu, console or chat has the keys.
extern "C" unsigned DingoSDKOverlayReadPromptKeys();
// allow_menu is used only by the Binds page while recording. Focus and lifetime
// gates still apply. Gameplay callers leave it false.
extern "C" void DingoSDKOverlayReadControllerInput(dingosdk::ControllerInput* input, bool allow_menu = false);
// Installs (or, with nullptr, removes) the one gamepad filter. Any thread.
extern "C" void DingoSDKOverlaySetGamepadFilter(dingosdk::overlay::GamepadFilter filter);

// Optional direct integration for a host that already knows its exact pair.
struct IDXGISwapChain;
struct ID3D12CommandQueue;
extern "C" __declspec(dllexport) bool DingoSDKOverlayBindDx12(
    IDXGISwapChain* swapchain, ID3D12CommandQueue* command_queue);
