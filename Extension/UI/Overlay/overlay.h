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
#include <string>
#include <vector>

namespace dingosdk::overlay {
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
    set_drone_follow,     // the drone flies itself behind the skater, and the view is its camera
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
// pad. The grind exit control's grab and lean guards (grind_exit_control.h).
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
    bool drone_follow = false;  // the drone flies itself behind the skater (Skater/drone_follow.h)
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
    float forward_velocity_speed = 20.0f;
    float up_velocity_speed = 20.0f;
    float offboard_up_velocity_speed = 20.0f;
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

// Hall of Meat's switch (Extension/HallOfMeat/hall_of_meat.h): started, and switched on.
struct HallOfMeatModel {
    bool available = false, enabled = false;
};

// Road Rash's switches (Extension/RoadRash/road_rash.h): started, switched on, and whether its
// worst wounds bleed.
struct RoadRashModel {
    bool available = false, enabled = false, blood = false;
};

// Board Break's switch (Extension/BoardBreak/board_break.h): started, switched on, and which of
// its decks is the one in the game (none: no deck pack loads, and no board can break).
struct BoardBreakModel {
    bool available = false, enabled = false, has_deck = false;
    std::string deck = "None";
    // Its settings (Extension/BoardBreak/board_break_feel.h) and how worn the board is (0 to 1, and
    // its word).
    bool always = false, rails = true, wear = true, crack_sounds = true, body_sounds = true;
    int sensitivity = 100, joined = 50, wear_rate = 100;
    float worn = 0;
    std::string condition = "new";
};

// Grind exit control's switches (Extension/Skater/grind_exit_control.h): started, switched on,
// LT/RT toe/heel (else left/right of travel), and the player's stance (0 not set, 1 regular,
// 2 goofy).
struct GrindExitModel {
    bool available = false, enabled = false, toe_heel = true;
    int stance = 0;
    int strength = 100; // pop-out strength, percent (10-100)
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
    HallOfMeatModel hall_of_meat;
    RoadRashModel road_rash;
    BoardBreakModel board_break;
    GrindExitModel grind_exit;
    KeyframeSmoothingModel keyframe_smoothing;
    ObjectPersistenceModel object_persistence;
    ParkEditorModel editor;
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
// A map the session needs being fetched (Extension/Assets/map_download.h), for its card.
struct MapDownloadCard {
    int stage{};                // 0 none, 1 asking, 2 downloading, 3 installing, 4 applying, 5 joining
    std::string map, package, author, version, description;
    std::string have;            // the version installed here, when the host's replaces it
    std::size_t step{}, steps{}; // applying: the merge's steps done of all
    std::string step_name;
    bool server{}, installed{}, moved{}, choice{true};
    std::uint64_t received{}, total{};
    std::uint32_t yes_bind{}, no_bind{};
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
    bool flying_drone{};              // flying the FPV drone: a FLYING DRONE label above the name
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
// Used by the FPV drone's controller selection (Skater/drone_radio.h); safe from any thread.
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
// the game from reading it (freecam or drone controller capture). Same thread, paired.
// The FPV drone reads its controller this way (Skater/drone_radio.h).
void begin_input_access() noexcept;
void end_input_access() noexcept;
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
