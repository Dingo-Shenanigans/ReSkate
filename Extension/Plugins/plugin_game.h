#pragma once
#include "plugin_wave.h"
#include "Engine/Core/Json/json.h"
#include "Extension/AnimationStudio/anim_core.h"
#include "Extension/Throwdowns/throwdown_kit.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The game behind the wider set of the plugin API (docs: ReSkate Plugin System Design): what plugins
// may read and change of the skater, the world, the session and the rest. Every function has an
// "unavailable" default, so the host runs without a game (the tests give it a fake one) and the
// game side fills in what it can (plugin_game_runtime.cpp). Client thread throughout.
//
// A string a function returns is empty when it worked, and otherwise says why not, for the plugin.
namespace dingosdk::plugins {
using Vec3 = std::array<float, 3>;
inline constexpr char unavailable[] = "unavailable here";

// The game's camera as it is now: where it is, the way it looks, and its vertical field of view.
struct CameraView {
    Vec3 position{}, forward{};
    float fov{};
    Vec3 right{}, up{};
    bool free_camera{}; // the view is Freecam's, verified selected (not just asked for)
};
// What a game layer reads of the local skater each tick (reskate.skater.telemetry): the trainer's
// telemetry and its last measured jump, the physics state the skater's selector chose (with No
// Bail's wipeout and runout counts), the Bone Cam's slams and where the body is during a bail.
struct SkaterTelemetry {
    bool skater{}, airborne{};
    Vec3 position{};
    float heading{}; // the trainer's degrees: 0 faces +Z, forward (sin, 0, cos)
    float speed{}, vertical{}, air_time{}, height{}, top_speed{};
    std::uint32_t physics_state{};
    struct Jump {
        std::uint64_t serial{};
        float takeoff_speed{}, takeoff_angle{}, air_time{}, height{}, distance{}, drop{}, landing_speed{};
        float spin{}, spin_rate{}, flip{}, board_turn{};
        std::uint32_t state{};
        Vec3 takeoff{}, landing{};
    } last;
    struct Watch {
        bool valid{}, ragdoll_known{}, ragdoll{};
        std::uint32_t state{}, previous{};
        float previous_seconds{};
        std::uint64_t changes{}, wipeouts{}, runouts{};
    } watch;
    bool teleport_pending{}, slam{}, struck{};
    std::optional<Vec3> body; // the hips, during a bail (Bone Cam)
};
struct TrickNote {
    std::uint64_t entry{};
    char kind{}; // 'n' name, 's' side, 'l' landing, 'b' body spin, 'r' board spin, 'k' stance, 'm' modifier
    std::string text;
};
// Input for a plugin that flies something: the flight keys and mouse look (as Freecam reads them),
// a gamepad's sticks, triggers and buttons (XInput's bits), a joystick or radio's axes and buttons.
struct FlightInput {
    float right{}, up{}, forward{}, look_x{}, look_y{};
    bool boost{};
};
struct PadInput {
    float left_x{}, left_y{}, right_x{}, right_y{}, left_trigger{}, right_trigger{};
    std::uint32_t buttons{};
    bool playstation{};
};
struct StickInput {
    std::array<float, 6> axes{};
    std::uint32_t buttons{};
    std::string name;
};
// A sound a plugin plays (reskate.audio): its wave, or the motor synth when it has none.
struct SoundStart {
    std::shared_ptr<const Wave> wave;
    float volume{1}, pitch{1}, range{40}, throttle{}, speed{};
    bool loop{};
    std::optional<Vec3> position; // empty: at the local skater
};
struct SoundChange {
    std::optional<float> volume, pitch, range, throttle, speed;
    std::optional<std::optional<Vec3>> position;
};
// What the local skater's physics did since the last tick (events.skater).
struct SkaterNews {
    struct Landing {
        float down_speed{}, speed{};
        bool rail{};
    };
    struct Hit {
        std::string bone; // as the game names its body ("Left forearm")
        float impact{};
        bool vehicle{}, board{};
    };
    struct Fall {
        float seconds{}, left{}, right{};
        struct Bone {
            std::string name;
            float impact{}, slide{};
        };
        std::vector<Bone> bones; // those that hit or slid
    };
    std::vector<Landing> landings;
    std::vector<Hit> hits;
    std::vector<Fall> falls;
};
// One physics step of the local skater (events.physics): when (the steady clock, ms, as
// reskate.time.now), how long it simulated, and what the body did; only the bodies touching something.
struct PhysicsStep {
    double at{};
    float seconds{};
    bool wipeout{}, airborne{};
    std::optional<bool> ragdoll;
    std::optional<Vec3> velocity;
    std::optional<float> spin;
    struct Body {
        std::string name; // as body_hit names it
        float impact{}, slide{};
        bool board{}, vehicle{}, world{}; // world: anything but the board and vehicles
    };
    std::vector<Body> bodies;
};
// What a plugin sets of a launch into the air, m/s, each if given: across the direction of travel
// (+ its left), along it, and up. And a launch as it went (Extension/Skater/launches.h).
struct LaunchShape {
    std::optional<float> across, along, up;
};
struct LaunchNews {
    std::string from;      // "grind", "ollie", "no_comply", "boneless" or "other"
    Vec3 game{}, velocity{}; // as the game made it, as it left
    std::optional<std::array<float, 2>> travel; // the horizontal direction of travel (x, z), when moving
    float travel_speed{};
    std::string reason; // "applied", "none", "slow", "not_ours"
    std::string by;     // the plugin whose shape applied
};
// The last slam: its force and the game's cause number, and how long ago (ms).
struct SlamNews {
    float force{};
    int cause{-1};
    double ago_ms{};
};
struct CatalogItem {
    std::string key, title, category;
};
// A park editor object on the map being played: its layout id, the item, where and how it sits.
struct GameObject {
    std::uint64_t id{};
    std::string item;
    Vec3 position{};
    std::array<float, 4> rotation{0, 0, 0, 1}; // x, y, z, w
    float scale{1};
};
struct Destination {
    std::string id, name, map; // fast travel
};
struct LevelInfo {
    std::string asset, name;
    bool can_load{};
};
// A skater's body as capsules: `part` indexes body_part_names; a and b are its ends in the world.
struct BodyCapsule {
    std::uint8_t part{};
    Vec3 a{}, b{};
    float radius{};
};
struct Body {
    std::uint64_t player{}; // 0: the local skater
    std::vector<BodyCapsule> capsules;
};
inline constexpr std::array<const char*, 7> body_part_names{"head", "body", "left_arm", "right_arm", "left_leg", "right_leg", "board"};
// Another player's skater as the session last heard it.
struct PeerSkater {
    std::uint64_t id{};
    Vec3 position{};
    float heading{}; // as the local skater's
    double age_ms{}; // since its pose arrived
};
struct ChatLine {
    std::uint64_t sequence{}, sender{};
    std::string name, text;
    bool local{}, server{};
};
struct Track {
    bool available{}, playing{};
    std::string app, title, artist, album;
    std::uint64_t revision{};
};
struct ThrowdownState {
    bool active{}, host{};
    struct Player {
        std::string name;
        int letters{};
        bool up{}, out{};
    };
    std::vector<Player> skate; // S.K.A.T.E.'s players and letters, while one is shown
};
// A throwdown mode on the game's Throwdowns page: a card on its picker, the game's own setup page
// (relabelled from the plugin's setup document) and the game's start flag. The plugin runs it.
struct ThrowdownMode {
    std::string id, name, description;
    std::string art;  // the logo: a texture or image asset the plugin's mod adds, by path; empty: the card's own
    std::string flag; // the placement-preview prefab its native flag shows, by path; empty: Spot Battle's
    std::string card = "spot"; // the stock card it is cloned from: "skate", "spot" or "jam"
    std::string replaces; // the stock throwdown whose place it takes: "skate", "spot", "jam" or empty
    std::uint32_t color{};
    int order{};          // among the plugin modes on the picker
    int min_players = 1, max_players = 6, players = 4;
    float cursor_height{}; // > 0: its flag's placement cursor may rise this far (metres)
};
// reskate.ui.dialog: an overlay dialog over the game (plugin_dialog.cpp). Its answers come back to
// the plugin as throwdown_ui events with action "dialog" (throwdown_kit.h).
struct DialogSpec {
    std::string id, title, message;
    struct Field {
        bool present{};
        std::string text;
        std::size_t max{64};
    } field;
    struct Row {
        std::string text, sub;
        std::vector<std::string> buttons; // each answers with its name and the row
    };
    bool list{};
    std::vector<Row> rows;
    std::string empty; // shown in a list with no rows
    struct Button {
        std::string id, label;
    };
    std::vector<Button> buttons;
    struct File {
        bool present{};
        std::string label, filter; // filter: "*.json" or "*.json;*.txt"
    } file;
};
struct Progress {
    bool available{};
    struct District {
        std::string id, name;
        int rank{};
    };
    std::vector<District> districts;
    struct Challenge {
        std::string id, type, neighborhood;
        int attempts{}, completed_goals{}, goal_count{};
    };
    std::vector<Challenge> challenges;
    double score{}, score_cap{};
    int score_level{};
};

class Game {
public:
    virtual ~Game() = default;
    // ---- the local skater ----------------------------------------------------------------------
    // Its physics state (100s riding, 200s in the air, 300 a wipeout, 400s grinding, 504 off the
    // board), or empty without a skater; and how many wipeouts it has had this session.
    virtual std::optional<std::uint32_t> physics_state() { return std::nullopt; }
    virtual std::uint64_t wipeouts() { return 0; }
    // "noclip_on", "noclip_off", "boost_forward", "boost_up", "boost_offboard": queued as the
    // console's own are, under the host's rules.
    virtual std::string control(std::string_view) { return unavailable; }
    // The game's HUD hidden while any plugin asks; given back when the last one stops asking.
    virtual void hide_hud(std::string_view /*plugin*/, bool) {}
    // ---- the world -------------------------------------------------------------------------------
    virtual std::vector<CatalogItem> object_catalog() { return {}; }
    virtual std::vector<GameObject> objects() { return {}; }
    // "place" (item and pose), "move" (id and pose) or "delete" (id): through the park editor,
    // under the session's object rules and limits. One at a time: it is busy until done.
    virtual std::string edit_object(std::string_view, const GameObject&) { return unavailable; }
    // A plugin's own world objects (world_entities.h): an entity of a world object blueprint at a
    // full transform (rows: right, up, forward axes with their scale, then the position), kept by the
    // plugin's id for it. Only this player's game shows them. Status: 0 none, 1 waiting, 2 loading,
    // 3 missing, 4 ready, 5 hidden.
    // With `attach`, kept on a joint of a skater (player 0: the local one) and the transform in that
    // joint's frame, moved with it every frame.
    struct EntityAttach {
        std::uint64_t player{};
        std::uint16_t joint{};
        bool upright{}; // level at the joint, turned only by its heading
        bool turn{true}; // false: the joint's position only, the transform's rotation in the world
        float ground{};  // metres below the joint to find the floor in, 0 none
    };
    virtual std::string entity_set(std::string_view /*plugin*/, std::uint32_t, std::string_view, const std::array<float, 16>&, bool,
                                   std::optional<EntityAttach> /*attach*/, bool /*solid*/) {
        return unavailable;
    }
    virtual void entity_remove(std::string_view /*plugin*/, std::uint32_t) {}
    virtual void entity_clear(std::string_view /*plugin*/) {}
    virtual int entity_status(std::string_view /*plugin*/, std::uint32_t) { return 0; }
    virtual bool entities_ready(std::string_view /*plugin*/) { return false; }
    // The world object blueprints loaded now, those starting with `prefix` (case ignored), sorted
    // (Extension/World/world_entities.h).
    virtual std::vector<std::string> entity_blueprints(std::string_view /*prefix*/, bool /*meshes_only*/) { return {}; }
    // A world object blueprint's mesh box in its own frame (min, max), when it has a loaded mesh.
    virtual std::optional<std::array<Vec3, 2>> entity_bounds(std::string_view /*blueprint*/) { return std::nullopt; }
    // The world raycast and the object it meets (Extension/Objects/ParkEditor/park_editor_runtime.h,
    // local_world_pick): where; and the object's transform (rows right, up, forward with their
    // scale, then position) and blueprint, when known.
    struct WorldPick {
        Vec3 at{};
        bool object{};
        std::optional<std::array<float, 16>> matrix;
        std::string blueprint;
    };
    virtual std::optional<WorldPick> world_pick(const Vec3& /*from*/, const Vec3& /*to*/) { return std::nullopt; }
    // kind "tod" (key: morning, noon, afternoon, evening, night, weatherday, weathernight, default),
    // "control" (key: fog, fog_distance, sky_brightness, clouds, wind_strength, wind_direction,
    // traffic, pedestrians; value a number, -1 the default) or "park" (key: a lot, value a layout).
    virtual std::string set_environment(std::string_view, std::string_view, std::string_view) { return unavailable; }
    virtual std::vector<Destination> destinations() { return {}; }
    virtual std::vector<LevelInfo> levels() { return {}; }
    // kind "location" (a destination's id) or "level" (a level's asset); a guest is refused.
    virtual std::string travel(std::string_view, std::string_view) { return unavailable; }
    // "spawn" (count) or "clear"; refused in a session, where others would never see them.
    virtual std::string ai(std::string_view, int) { return unavailable; }
    virtual int ai_count() { return 0; }
    // ---- the session ----------------------------------------------------------------------------
    virtual std::vector<PeerSkater> peer_skaters() { return {}; }
    // Every skater's body in the session as this game drew it last (plugin_session_feed.h body_of):
    // the other players' (hidden ones too) and the local skater's, as player 0. Asking keeps them read
    // for two seconds, so the first ask comes back empty.
    virtual std::vector<Body> bodies() { return {}; }
    // Another player's skater, board, sound, nametag and map marker not shown on this screen while
    // any plugin hides them (at most 256 between them).
    virtual std::string hide_player(std::string_view /*plugin*/, std::uint64_t, bool /*hidden*/) { return unavailable; }
    virtual std::vector<std::uint64_t> hidden_players(std::string_view /*plugin*/) { return {}; }
    // A short tag beside a player's nametag, the plugin's own (empty text takes it away).
    virtual void set_nametag(std::string_view /*plugin*/, std::uint64_t, std::string, std::uint32_t) {}
    // The names over the other players held off while any plugin asks (chat bubbles still show).
    virtual void hide_nametags(std::string_view /*plugin*/, bool /*hidden*/) {}
    // 0 mutes, 1 is as heard, up to 10. The local player's ears only.
    virtual std::string voice_volume(std::uint64_t, float) { return unavailable; }
    // Chat lines after `sequence`, as this player sees them (filtered when they filter).
    virtual std::vector<ChatLine> chat_since(std::uint64_t) { return {}; }
    virtual std::string send_chat(std::string_view) { return unavailable; }
    // A line in this player's chat only, under the plugin's name with a "Plugin" tag.
    virtual std::string local_chat(std::string_view /*plugin*/, std::string_view, std::uint32_t /*colour, 0 the usual*/) { return unavailable; }
    virtual ThrowdownState throwdowns() { return {}; }
    // A drop the local player hosts where the skater stands, for chosen players of the session
    // (`series` "JamSession", "SpotBattle" or "ThrowdownSkate"): they see it and join it with the
    // game's own Join, and it starts once all have. Its status: "", "asked", "spawning", "open",
    // "started", "cancelled", or why it failed.
    virtual std::string host_throwdown(std::string_view /*plugin*/, std::string_view /*series*/, std::vector<std::uint64_t> /*players*/,
                                       int /*max players*/) {
        return unavailable;
    }
    virtual void cancel_throwdown(std::string_view /*plugin*/) {}
    virtual std::string throwdown_status(std::string_view /*plugin*/) { return {}; }
    // Adds the mode to the Throwdowns page, or updates it (by id).
    virtual std::string set_throwdown_mode(std::string_view /*plugin*/, const ThrowdownMode&) { return unavailable; }
    virtual void remove_throwdown_mode(std::string_view /*plugin*/, std::string_view /*id*/) {}
    // The throwdown kit (throwdown_kit.h): the plugin's documents for the native UI, and the one
    // native flag registration (`mode` is the SDK's id, "<plugin>/<id>"). Leave lets go of the
    // registration only when it is one of the plugin's modes.
    virtual std::string publish_throwdown_ui(std::string_view /*plugin*/, const Json&) { return unavailable; }
    virtual bool throwdown_flag_begin(std::string_view /*mode*/, std::uint32_t /*game*/, const throwdowns::kit::flag::Options&) { return false; }
    virtual throwdowns::kit::flag::State throwdown_flag_state() { return {}; }
    virtual void throwdown_flag_leave(std::string_view /*plugin*/) {}
    // ---- general services for a game layer in Lua (plugin_api_services.cpp) ----------------------
    // `plugin`: whose read it is (slam and struck are news since that plugin's last read).
    virtual std::optional<SkaterTelemetry> skater_telemetry(std::string_view /*plugin*/) { return std::nullopt; }
    // The pad's XInput button bits, LT 0x10000 and RT 0x20000 (empty: no pad). `menu`: read while
    // ReSkate's menu is open too.
    virtual std::optional<std::uint32_t> pad_bits(bool /*menu*/) { return std::nullopt; }
    virtual bool key_down(int /*virtual_key*/) { return false; }
    virtual int mouse_wheel() { return 0; }
    // The mouse since this plugin's last read (reskate.input.mouse): raw counts right and down, the
    // wheel in notches (away from you positive). Empty while the game is not in front or a menu is open.
    struct MouseMotion {
        double x{}, y{}, wheel{};
    };
    virtual std::optional<MouseMotion> mouse_motion(std::string_view /*plugin*/) { return std::nullopt; }
    // The mouse over the game window while a plugin's screen is open (reskate.input.cursor): where
    // (pixels from the top left), its buttons, the wheel's notches since the last read, and whether
    // it is over the screen's own panel.
    struct Cursor {
        float x{}, y{}, wheel{};
        bool left{}, right{}, middle{}, over_screen{};
    };
    virtual std::optional<Cursor> cursor() { return std::nullopt; }
    // What was typed since the last call while a plugin's screen is open (reskate.input.text);
    // `capture` keeps the typing the plugin's for 300 ms more.
    virtual std::string typed_text(bool /*capture*/) { return {}; }
    // The game menu's character room (Extension/World/character_room.h; reskate.room): walked to, the
    // local skater shown in it once there, and walked back from. One plugin at a time. state: "idle",
    // "opening", "open", "leaving" or "failed" (with why).
    virtual std::string room_open(std::string_view /*plugin*/) { return unavailable; }
    virtual void room_leave(std::string_view /*plugin*/) {}
    struct RoomState {
        std::string state, why;
    };
    virtual RoomState room_state(std::string_view /*plugin*/) { return {"idle", {}}; }
    // The game menu's preview (the skater on the Skaters page, the board on a Board tab) turned by a
    // stick of a stand-in pad: `value` -1..1 of the stick held for 300 ms (call it every tick), 0 lets
    // go; `stick` 0 left x, 1 left y, 2 right x, 3 right y. Only while the game's menu is open.
    virtual std::string room_turn(std::string_view /*plugin*/, float /*value*/, int /*stick*/) { return unavailable; }
    // The game's menu: whether it is open, whether it shows the Skaters page, and the navigations of
    // the open page's buttons (how the game names where each goes: "CASPresetPurchase", ...).
    struct GameMenu {
        bool open{}, skaters{};
        std::vector<std::string> buttons;
    };
    virtual GameMenu game_menu() { return {}; }
    // The customization grid that has the focus (Extension/Customization/item_browser.h), when one does.
    struct MenuGrid {
        std::string focused, title, category; // the highlighted item: key, title, catalog category
        std::vector<std::string> items;        // every item's key in the grid
        unsigned shown{}, total{};
        bool showing{}, held{};                // a plugin's list is shown; it has nothing in it (grid_show)
        float prompts_left{}, prompts_top{}, prompts_width{}, prompts_unit{}; // where the grid's own prompts end
    };
    virtual std::optional<MenuGrid> menu_grid() { return std::nullopt; }
    // A plugin's own list shown in the grid that has the focus (item_browser::show_items), nullopt to
    // give it back; and the tiles it marks with the game's check. Empty when taken, else why not.
    virtual std::string grid_show(std::string_view /*plugin*/, std::optional<std::vector<std::string>> /*keys*/) { return unavailable; }
    virtual void grid_mark(std::string_view /*plugin*/, std::vector<std::string> /*keys*/) {}
    // ---- capture: the screen as the game draws it (Extension/Rendering/Capture) ---------------------
    // One plugin records at a time; frames are numbered from 0 as they are taken.
    struct CaptureStart {
        float left{}, top{}, right{1}, bottom{1};
        bool menu{};            // fractions of the game's menu frame (at most 21:9), else of the screen
        bool depth{true};
        std::uint32_t height{}, skip{}, most{2400};
        double seconds{60};
    };
    virtual std::string capture_start(std::string_view /*plugin*/, const CaptureStart&) { return unavailable; }
    virtual void capture_stop(std::string_view /*plugin*/) {}
    virtual void capture_discard(std::string_view /*plugin*/) {}
    struct CaptureStatus {
        std::string state; // "idle", "recording", "stopped", "saving", "saved", "failed"
        std::uint32_t taken{}, kept{};
        bool depth{};
        std::string message, file; // file: its path in the plugin's data folder
    };
    // Another plugin's capture reads as idle.
    virtual CaptureStatus capture_status(std::string_view /*plugin*/) { return {"idle"}; }
    virtual float capture_compare(std::string_view /*plugin*/, std::uint32_t, std::uint32_t) { return -1; }
    virtual std::vector<std::uint32_t> capture_kept(std::string_view /*plugin*/) { return {}; }
    struct CaptureSave {
        std::vector<std::uint32_t> frames;
        std::string format{"gif"}; // "gif", "apng", "png", "webm"
        std::uint32_t fps{30}, size{512};
        std::optional<std::uint32_t> background; // 0xRRGGBB; none: transparent
        std::string cutout{"auto"};               // "auto", "depth", "colour", "none"
        bool solid{true};
        std::string card_title;
        std::vector<std::pair<std::string, std::string>> card; // label, value
        std::string path;                        // in the plugin's data folder
    };
    virtual std::string capture_save(std::string_view /*plugin*/, const CaptureSave&) { return unavailable; }
    // Game buttons kept from the game while asked (overlay::hide_game_buttons/hold_game_buttons),
    // and all game input paused for half a second at a time.
    virtual void game_buttons(std::string_view /*plugin*/, std::uint16_t /*hidden*/, std::uint16_t /*held*/, bool /*paused*/) {}
    // Keys (virtual key codes) whose presses the game does not see until the next call (overlay::hide_game_keys).
    virtual void game_keys(std::string_view /*plugin*/, std::vector<std::uint8_t> /*keys*/) {}
    // The game window has the focus, ReSkate's menu is shut and the native UI lets the game play.
    virtual bool input_allowed() { return false; }
    virtual std::string free_camera(std::string_view /*plugin*/, bool) { return unavailable; }
    // The free camera flown from the pad while placing from the air (Extension/Skater/pad_flight.h);
    // one plugin holds it at a time.
    virtual std::string pad_flight(std::string_view /*plugin*/, bool) { return unavailable; }
    // The stock physics while the local player competes (set_competition_stock_physics).
    virtual void competition(std::string_view /*plugin*/, bool) {}
    virtual bool stock_physics_ready() { return false; }
    // physics_tuning::prepare(): the game's stock tuning read, without switching anything.
    virtual void prepare_physics() {}
    // The session's world (it changes with the map or session), whether the local player's mods bar
    // them from scoring, and the map's name.
    virtual std::string world_map() { return {}; }
    virtual std::uint64_t world_id() { return 0; }
    virtual bool world_barred() { return false; }
    virtual std::optional<bool> stance_goofy() { return std::nullopt; }
    virtual void trick_feed(std::string_view /*plugin*/, bool) {}
    virtual std::vector<TrickNote> take_trick_notes() { return {}; }
    // The Bone Cam (Extension/Skater/bone_cam.h): no words for its setting, "status", "bail",
    // "ringing", "game on|off" (the plugin's game asks for it), or the bonecam console words.
    virtual std::string bone_cam(std::string_view /*plugin*/, const std::vector<std::string>& /*words*/) { return {}; }
    // Three focus checks: the game window has focus, ReSkate's overlay is open, and the throwdown
    // flow lets gameplay input through.
    struct InputFocus {
        bool focused{}, interface_open{}, gameplay{};
    };
    virtual InputFocus input_focus() { return {}; }
    // Watch another player's skater the way the native party menu does (0: back to our own).
    virtual void spectate(std::uint64_t /*player*/) {}
    // The flick diagram of a flip trick as skate. names it (trick_gestures.h): read in the
    // background the first time; empty until then, or for a trick without one.
    virtual std::vector<std::array<float, 2>> trick_gesture(std::string_view /*trick*/) { return {}; }
    // What the game shows and records of the local skater's tricks (Extension/Skater/trick_rewrite.h):
    // the ticker's label `from` shown as `to` (empty: that label is the game's again), and the next
    // trick with one of `from`'s ids, within `within_ms`, recorded as `to`. Empty when done, else why not.
    virtual std::string trick_label(std::string_view /*plugin*/, std::string_view /*from*/, std::string_view /*to*/) { return unavailable; }
    // ---- content: a folder of every enabled mod, as the plugin's manifest names it ("content") -------
    struct ContentFile {
        std::string mod, title; // the mod's folder name and its own name
        std::string path;       // under <mod>/<folder>/, with / between folders
        std::uint64_t size{};
    };
    // The files there, mod by mod in load order; `folder` is checked against the manifest already.
    virtual std::string content_list(std::string_view /*folder*/, std::vector<ContentFile>& /*files*/) { return unavailable; }
    // One of them: `path` is checked already (safe_relative_path). Empty when read, else why not.
    virtual std::string content_read(std::string_view /*mod*/, std::string_view /*folder*/, std::string_view /*path*/, std::string& /*text*/) {
        return unavailable;
    }
    virtual std::string trick_record_as(std::string_view /*plugin*/, std::vector<std::uint32_t> /*from*/, std::uint32_t /*to*/, int /*within_ms*/) {
        return unavailable;
    }
    // ---- files: the plugin's own data folder, <game>/Mods/.reskate_plugin_data/<plugin>/ -----------
    // Paths are relative to it and already checked (plugin_manifest.h's safe_relative_path). A listing
    // names the files and folders in `folder` (empty: the top), folders ending in "/".
    virtual std::string files_list(std::string_view /*plugin*/, std::string_view /*folder*/, std::vector<std::string>& /*names*/) { return unavailable; }
    virtual std::string files_read(std::string_view /*plugin*/, std::string_view /*path*/, std::string& /*text*/) { return unavailable; }
    virtual std::string files_write(std::string_view /*plugin*/, std::string_view /*path*/, std::string_view /*text*/) { return unavailable; }
    virtual std::string files_remove(std::string_view /*plugin*/, std::string_view /*path*/) { return unavailable; }
    // The data folder (or a folder in it) opened in the player's file manager.
    virtual std::string files_reveal(std::string_view /*plugin*/, std::string_view /*folder*/) { return unavailable; }
    // The same, for a file up to `most` bytes (an animation clip's, say).
    virtual std::string files_read_bytes(std::string_view plugin, std::string_view path, std::string& out, std::size_t /*most*/) {
        return files_read(plugin, path, out);
    }
    virtual std::string files_write_bytes(std::string_view plugin, std::string_view path, std::string_view bytes, std::size_t /*most*/) {
        return files_write(plugin, path, bytes);
    }
    // ---- ui.dialog -----------------------------------------------------------------------------------
    // One dialog at a time over the game; the same id again updates it (what was typed is kept).
    virtual std::string dialog_open(std::string_view /*plugin*/, const DialogSpec&) { return unavailable; }
    virtual void dialog_close(std::string_view /*plugin*/, std::string_view /*id, empty: any of the plugin's*/) {}
    // ---- skater.push / hold ----------------------------------------------------------------------------
    // The skater's velocity set to this (metres per second, world) once it is back on its board, and
    // the skater held where it is (no input, no physics) while asked. A pending push may wait two
    // seconds more (the skater is still getting back on its board) or be dropped.
    virtual std::string skater_push(std::string_view /*plugin*/, const Vec3&) { return unavailable; }
    virtual std::string skater_push_wait(std::string_view /*plugin*/) { return unavailable; }
    virtual std::string skater_push_reset(std::string_view /*plugin*/) { return unavailable; }
    virtual std::string skater_hold(std::string_view /*plugin*/, bool) { return unavailable; }
    // ---- the rest ----------------------------------------------------------------------------------
    // The music an outside player (Spotify and the like) is playing, and "play", "pause",
    // "next" or "previous" for it.
    virtual Track track() { return {}; }
    virtual std::string music(std::string_view) { return unavailable; }
    // The game's own soundtrack on the player's headphones (none while a Spotify station is on:
    // then the outside player's song is the one heard), and "next" for it.
    virtual std::optional<Track> game_track() { return std::nullopt; }
    virtual std::string game_music(std::string_view) { return unavailable; }
    virtual Progress progress() { return {}; }
    virtual std::optional<std::string> setting(std::string_view) { return std::nullopt; }
    virtual std::vector<std::string> setting_names() { return {}; }
    // A game setting held at `value` while the plugin keeps it, then put back as it was found
    // (reskate.settings.set); no value lets go. One plugin holds a setting at a time.
    virtual std::string set_setting(std::string_view /*plugin*/, std::string_view /*name*/, std::optional<std::string> /*value*/) { return unavailable; }
    // ---- camera ------------------------------------------------------------------------------------
    // The view from a pose (rows right, up, back and position, as follow_camera.h builds them) with
    // a vertical field of view in degrees (0 the camera's own). One plugin holds it at a time, and
    // Spectate takes it back while it runs; the game's own camera returns on release.
    virtual std::optional<CameraView> camera_view() { return std::nullopt; }
    virtual std::string set_camera(std::string_view /*plugin*/, const std::array<float, 16>&, float) { return unavailable; }
    // Spectate's camera on a session player (0 stops the one this plugin started).
    virtual std::string follow_player(std::string_view /*plugin*/, std::uint64_t) { return unavailable; }
    virtual void release_camera(std::string_view /*plugin*/) {}
    // "" while the plugin's view is the one shown, otherwise why not.
    virtual std::string camera_status(std::string_view /*plugin*/) { return unavailable; }
    // The plugin's camera as a step of the game's camera height button cycle (Engine/Game/Skater/
    // first_person_cycle.h): out of it, in it and off (ready to start), or in it and on. Held 300 ms,
    // so said every tick; one plugin at a time. The steps the button asks for come back from
    // take_camera_cycle, for the plugin in the cycle.
    enum class CycleCamera : std::uint8_t { out, ready, active };
    virtual std::string camera_cycle(std::string_view /*plugin*/, CycleCamera) { return unavailable; }
    struct CycleNews {
        std::string plugin;
        bool enter{};
    };
    virtual std::vector<CycleNews> take_camera_cycle() { return {}; }
    // Holds the skater where it is (noclip, with no input), while any plugin asks.
    virtual std::string freeze(std::string_view /*plugin*/, bool) { return unavailable; }
    // ---- the session's authority (plugins' server copies) --------------------------------------------
    // Drops a player from the session; a chat line to everyone; one to one player (a dedicated
    // server's only: a lobby has no private chat); moves a player's skater (their game does the move).
    virtual std::string kick(std::uint64_t, std::string_view /*why*/) { return unavailable; }
    virtual std::string announce(std::string_view /*plugin*/, std::string_view) { return unavailable; }
    virtual std::string tell(std::string_view /*plugin*/, std::uint64_t, std::string_view) { return unavailable; }
    virtual std::string move_player(std::uint64_t, const Vec3&) { return unavailable; }
    // Bans a player by SteamID64, connected or not (a dedicated server's only); lifts a ban.
    virtual std::string ban(std::uint64_t) { return unavailable; }
    virtual std::string unban(std::uint64_t) { return unavailable; }
    // A player's permissions, true where they may: what the server holds for them now, or nothing
    // for one it cannot hold any for. Applying makes them exactly these.
    struct Permissions {
        bool noclip = true, nobail = true, boosts = true, tuning = true, objects = true, ollies = true;
    };
    virtual std::optional<Permissions> permissions(std::uint64_t) { return std::nullopt; }
    virtual std::string apply_permissions(std::string_view /*plugin*/, std::uint64_t, const Permissions&) { return unavailable; }
    // A player's role on a dedicated server (data/roles.json): its name, its rank (0 to 1000), and
    // whether it gives any of the server's commands. Nothing for a player with no role, nor where
    // there are no roles (a lobby's host).
    struct Role {
        std::string name;
        int rank{};
        bool staff{};
    };
    virtual std::optional<Role> role(std::uint64_t) { return std::nullopt; }
    // A dedicated server's park mods (server.parks): those it has set up, whether each fits the map
    // running now and is spawned (its objects, and minutes left; 0 until removed). Spawning one (for
    // `minutes`, 0 until removed) or removing it goes as the server's own park-mod does.
    struct ServerPark {
        std::string name, map;
        bool here{}, spawned{};
        std::size_t objects{};
        unsigned minutes_left{};
    };
    virtual std::vector<ServerPark> server_parks() { return {}; }
    virtual std::string spawn_park(std::string_view /*plugin*/, std::string_view /*name*/, unsigned /*minutes*/) { return unavailable; }
    virtual std::string remove_park(std::string_view /*plugin*/, std::string_view /*name*/) { return unavailable; }
    // Cards and polls on players' screens (a dedicated server's only): each player's own, shown over
    // the server's announcement or vote until it ends, then the server's again. `players` empty is
    // everyone on the server now. A card is up for `seconds` (0: long enough to read). A poll has
    // 2 to max_vote_answers answers and runs `seconds` (0: the server's poll time); it is one card
    // with one `id` for all its players, and closes for each when they answer. A newer poll for a
    // player replaces theirs. What each player did comes back from take_poll_news.
    virtual std::string server_card(std::string_view /*plugin*/, const std::vector<std::uint64_t>& /*players*/, std::string_view /*text*/,
                                    unsigned /*seconds*/) {
        return unavailable;
    }
    virtual std::string server_poll(std::string_view /*plugin*/, const std::vector<std::uint64_t>& /*players*/, std::string_view /*question*/,
                                    const std::vector<std::string>& /*answers*/, unsigned /*seconds*/, std::uint32_t& /*id*/) {
        return unavailable;
    }
    // Closes a poll of the plugin's for everyone it is still open for.
    virtual std::string server_poll_close(std::string_view /*plugin*/, std::uint32_t /*id*/) { return unavailable; }
    // A player answered a plugin's poll (`answer` from 1, its text), or it closed for them unanswered
    // (`answer` 0, and why: "timeout", "left", "replaced" or "closed").
    struct PollNews {
        std::string plugin;
        std::uint32_t id{};
        std::uint64_t player{};
        std::size_t answer{};
        std::string text, why;
    };
    virtual std::vector<PollNews> take_poll_news() { return {}; }
    // One player hidden from others (a dedicated server's only): `skater` takes them out of those
    // players' games altogether (skater, name, objects, voice and sound, as if they had left);
    // without it, only their voice and/or sound is held back. `viewers` empty is everyone on now
    // but the target. Refused while the target shares a party or a throwdown with one of them.
    // Showing them again (`viewers` empty: everyone the plugin hid them from) sends their outfit,
    // poses and objects anew.
    struct Hiding {
        bool skater = true, voice = true, sound = true;
    };
    virtual std::string server_hide(std::string_view /*plugin*/, std::uint64_t /*target*/, const std::vector<std::uint64_t>& /*viewers*/,
                                    const Hiding&) {
        return unavailable;
    }
    virtual std::string server_unhide(std::string_view /*plugin*/, std::uint64_t /*target*/, const std::vector<std::uint64_t>& /*viewers*/) {
        return unavailable;
    }
    // ---- skater.animation: clips on the local skater's body (Extension/Skater/skater_animation.h) ------
    // One plugin plays at a time; none while a throwdown runs. A replacement plays when the game does
    // `trick` ("kickflip") with that pop ("" any, "low", "mid", "high"); a null clip takes it away.
    using ClipRef = std::shared_ptr<const anim_studio::Clip>;
    // `at`: seconds into the clip it starts from; `paused`: held there (seek and pause move it).
    // The game's own animations (Extension/AnimationStudio/game_clips.h): the names containing `containing`
    // (lower case), at most `most`; and one read as a clip. Empty, and why, while they are still read or
    // when that one is not a clip on the skater's rig.
    virtual std::vector<std::string> game_clip_names(std::string_view /*containing*/, std::size_t /*most*/, std::string& why) {
        why = unavailable;
        return {};
    }
    virtual ClipRef game_clip(std::string_view /*name*/, std::string& why) {
        why = unavailable;
        return nullptr;
    }
    // The stance a clip of the skater stands in at its first frame (true: goofy), from the skeleton the
    // game's clips are read with; empty when it cannot be told, or before the game's animations are read.
    virtual std::optional<bool> clip_goofy(const ClipRef&) { return std::nullopt; }
    // The clip mirrored for the other stance (game_clips.h Library::mirror), or empty and why.
    virtual ClipRef mirror_clip(const ClipRef&, std::string& why) {
        why = unavailable;
        return nullptr;
    }
    virtual std::string play_clip(std::string_view /*plugin*/, ClipRef, float /*speed*/, float /*blend*/, bool /*loop*/, float /*at*/, bool /*paused*/,
                                  bool /*floor*/) {
        return unavailable;
    }
    virtual void stop_clip(std::string_view /*plugin*/) {}
    virtual void seek_clip(std::string_view /*plugin*/, float /*seconds*/) {}
    virtual void pause_clip(std::string_view /*plugin*/, bool /*paused*/) {}
    virtual std::optional<float> clip_time(std::string_view /*plugin*/) { return std::nullopt; }
    virtual bool clip_playing(std::string_view /*plugin*/) { return false; }
    // Who sees a pose, a carry or a reach of the local skater (Extension/Skater/skater_animation.h
    // Shown): everyone, only the other players (sent, not drawn), or only this player (drawn, not sent).
    enum class Shown : std::uint8_t { everyone, others, me };
    // An arm of the local skater reaching (Extension/Skater/arm_reach.h): its hand joint to `at` in the
    // world, turned to `rotation` when given, `weight` of the way; held 0.3 s, set every tick. One
    // plugin at a time; none ends that plugin's. Empty when done, else why not.
    struct Reach {
        bool left{};
        Vec3 at{};
        std::optional<std::array<float, 4>> rotation;
        float weight{1};
        Shown shown{};
    };
    virtual std::string skater_reach(std::string_view /*plugin*/, std::optional<Reach>) { return unavailable; }
    // Something held out in the local skater's hands and aimed (Extension/Skater/carry.h): its front
    // to `front` in the world, pointing as `aim` says (its -Z the way it points); held 0.3 s, set every
    // tick. One plugin at a time; none ends that plugin's. Empty when done, else why not.
    struct Carry {
        bool left{}, both{};
        Vec3 front{};
        std::array<float, 4> aim{0, 0, 0, 1};
        Vec3 front_from_grip{};
        // Worn on the hand: the item's rotation in the hand joint's frame, and its grip from the hand joint.
        std::optional<std::pair<std::array<float, 4>, Vec3>> worn;
        float weight{1}, crouch{}, hunch{}, turn{}, step{}, step_phase{};
        bool still{}, push_straightens{};
        Shown shown{};
    };
    virtual std::string skater_carry(std::string_view /*plugin*/, std::optional<Carry>) { return unavailable; }
    // Joints of the local skater turned (in their own frame) and scaled on top of the animation, in
    // the pose drawn and sent (Extension/Skater/skater_animation.h pose) or one of them (`shown`); held
    // 0.3 s, so set every tick; empty ends it. One plugin at a time.
    struct JointOffset {
        std::uint16_t joint{};
        std::array<float, 4> rotation{0, 0, 0, 1};
        float scale{1};
        Vec3 offset{}; // added to its position, in its parent's frame
    };
    virtual std::string skater_pose(std::string_view /*plugin*/, const std::vector<JointOffset>&, float /*weight*/, Shown /*shown*/) {
        return unavailable;
    }
    // Where a hand joint of the local skater is in the world, as last evaluated (after any reach).
    struct HandPlace {
        Vec3 position{};
        std::array<float, 4> rotation{0, 0, 0, 1};
    };
    virtual std::optional<HandPlace> skater_hand(bool /*left*/) { return std::nullopt; }
    // Where a joint (0..394) of a skater is in the world: player 0 the local skater as its pose was
    // last evaluated, else another player's as the session draws it. Asking keeps it read for two
    // seconds, so the first ask comes back empty.
    struct JointPlace {
        Vec3 position{};
        std::array<float, 4> rotation{0, 0, 0, 1};
        float scale{1};
    };
    virtual std::optional<JointPlace> skater_joint(std::uint64_t /*player*/, std::uint16_t /*joint*/) { return std::nullopt; }
    // The physics tuning's values, as the trainer has them (Extension/Trainer/trainer.h): read,
    // found by words, and set over the player's own while the plugin keeps them (never saved).
    struct Tuning {
        std::string id, label, group, kind; // kind: number, integer, flag, multiplier
        double value{}, stock{};
        std::string tuner; // the plugin whose value it is, if any
    };
    virtual std::optional<Tuning> tuning(std::string_view /*id*/, std::string& why) {
        why = unavailable;
        return std::nullopt;
    }
    virtual std::vector<Tuning> find_tuning(std::string_view /*text*/, std::size_t /*most*/) { return {}; }
    virtual std::string tune(std::string_view /*plugin*/, std::string_view /*id*/, std::optional<double> /*value*/) { return unavailable; }
    virtual std::string replace_trick(std::string_view /*plugin*/, std::string /*trick*/, std::string /*pop*/, ClipRef, float /*speed*/) {
        return unavailable;
    }
    // Records the skater's pose for so many seconds; the clip comes from take_recording once done.
    virtual std::string record_clip(std::string_view /*plugin*/, float /*seconds*/) { return unavailable; }
    virtual ClipRef take_recording(std::string_view /*plugin*/) { return nullptr; }
    // The game's name for the trick the skater is doing now ("kickflip"), or empty.
    virtual std::string current_trick() { return {}; }
    // ---- outfit: items worn over the local skater's outfit (Extension/Skater/outfit_overlay.h) ----------
    struct OutfitSlot {
        std::string name, item;
        std::vector<float> parameters;
    };
    virtual std::string outfit_set(std::string_view /*plugin*/, std::string_view /*slot*/, std::string_view /*item*/, std::vector<float>, bool) {
        return unavailable;
    }
    virtual void outfit_clear(std::string_view /*plugin*/, std::string_view /*slot, empty: all*/) {}
    virtual std::string outfit_find(std::string_view) { return {}; }
    virtual std::vector<OutfitSlot> outfit_slots() { return {}; }
    virtual std::string outfit_status() { return unavailable; }
    // What the skater wears now (the selected preset): `slot` the kind of item ("Headwear", "Top", ...,
    // "Shoes", in the order worn from head to foot; clothes and accessories only), or the board's part
    // ("Deck", "Grip tape", "Grip cutout", "Trucks", "Wheels").
    struct WornItem {
        std::string slot, item, title;
    };
    virtual std::vector<WornItem> outfit_worn() { return {}; }
    // Items worn on top of the outfit (outfit_overlay::set_layers): the plugin's list replaced; empty
    // takes them off. Empty when taken, else why not.
    virtual std::string outfit_on_top(std::string_view /*plugin*/, std::vector<std::string> /*items*/) { return unavailable; }
    struct OnTop {
        std::vector<std::string> items, worn;
        std::string note;
    };
    virtual OnTop outfit_on_top_state(std::string_view /*plugin*/) { return {}; }
    virtual std::vector<WornItem> board_parts() { return {}; }
    // An item's title as the game shows it (its catalog's, the one an item grid showed, or its key
    // made readable).
    virtual std::string item_title(std::string_view key) { return std::string(key); }
    // ---- board: what is drawn of the local skateboard (Extension/Skater/board_pose.h) ------------------
    struct BoardBone {
        Vec3 position{};
        std::array<float, 4> rotation{0, 0, 0, 1};
        Vec3 scale{1, 1, 1};
    };
    virtual std::optional<std::array<BoardBone, 16>> board_bones() { return std::nullopt; }
    virtual std::string board_set(std::string_view /*plugin*/, int, const BoardBone&, bool /*world*/) { return unavailable; }
    // `everyone`: hidden from the other players too, not only on this screen.
    virtual std::string board_hide(std::string_view /*plugin*/, int, bool /*everyone*/) { return unavailable; }
    virtual void board_clear(std::string_view /*plugin*/, int /*bone, -1: all*/) {}
    virtual void board_park(std::string_view /*plugin*/, std::vector<int>) {}
    virtual std::string board_toss(std::string_view /*plugin*/, std::vector<int>, const Vec3&, const Vec3&, float) { return unavailable; }
    // How every board is fitted (board_pose.h fit); none: the plugin's fit ends.
    struct BoardFit {
        bool plates{};
        float axle = 1.0f;
    };
    virtual std::string board_fit(std::string_view /*plugin*/, std::optional<BoardFit>) { return unavailable; }
    // The shader parameters of a part of the local board (Extension/Customization/board_material.h):
    // part 0 the deck, 1 its grip, 2 the trucks, 3 the wheels. `type` is "colour", "number",
    // "integer", "switch" or "other"; `value` a colour's three channels or a number in [0], and
    // `integer` an integer's or a switch's. None without a board.
    struct BoardParameter {
        std::uint64_t key{};
        std::string type;
        std::array<float, 3> value{};
        std::int32_t integer{};
    };
    virtual std::optional<std::vector<BoardParameter>> board_parameters(int /*part*/) { return std::nullopt; }
    // A colour (`colour`, all three) or a number (value[0]) held there until cleared; empty when held.
    virtual std::string board_set_parameter(std::string_view /*plugin*/, int /*part*/, std::uint64_t, std::array<float, 3>, bool /*colour*/) {
        return unavailable;
    }
    // Part -1: every part; no key: every key.
    virtual void board_clear_parameter(std::string_view /*plugin*/, int /*part*/, std::optional<std::uint64_t>) {}
    // ---- variants: of the plugin's own mod (Engine/Vfs/mod_list.h Variants) -----------------------------
    struct VariantState {
        std::vector<std::string> installed; // its "order" first
        std::string chosen;                 // empty: none was
        std::string in_play;                // empty: none of them is in the game
    };
    // Empty when the mod declares no variants.
    virtual std::optional<VariantState> variants(std::string_view /*plugin*/, const std::filesystem::path& /*package*/) { return std::nullopt; }
    // The variant saved as the mod's choice, the mods applied again and the level loaded again with it.
    virtual std::string choose_variant(std::string_view /*plugin*/, const std::filesystem::path& /*package*/, std::string_view /*variant*/) {
        return unavailable;
    }
    // ---- the skater's physics -----------------------------------------------------------------------
    virtual SkaterNews take_skater_news() { return {}; }
    virtual std::optional<Vec3> board_velocity() { return std::nullopt; }
    // Its spin in that step: angular velocity in the world, radians a second.
    virtual std::optional<Vec3> board_spin() { return std::nullopt; }
    virtual std::optional<SlamNews> last_slam() { return std::nullopt; }
    // Every physics step since the last tick (kept once asked for: at most 512).
    virtual std::vector<PhysicsStep> take_physics_steps() { return {}; }
    // The skater's x-ray ready while any plugin asks (Extension/Skater/xray.h); d:skeleton draws it.
    virtual std::string xray(std::string_view /*plugin*/, bool) { return unavailable; }
    // The local skater not drawn on this screen while any plugin asks (Extension/Skater/xray.h).
    virtual std::string hide_skater(std::string_view /*plugin*/, bool) { return unavailable; }
    // skate.'s bottom left HUD hidden while any plugin asks; false keeps the plugin ready to hide it.
    virtual std::string hud_corner(std::string_view /*plugin*/, bool) { return unavailable; }
    // The game run at `speed` (0.05 to 1; 1 drops the ask) while it is the slowest asked; never in a
    // multiplayer session. And the speed it runs at for plugins now.
    virtual std::string game_speed(std::string_view /*plugin*/, float) { return unavailable; }
    virtual float game_speed_now() { return 1; }
    // The local skater's launches out of `from` ("grind", "ollie", "no_comply" or "boneless") from
    // now on (Extension/Skater/launches.h); empty: the game's own. One plugin a kind at a time. Every
    // launch comes back from take_launches.
    virtual std::string shape_launch(std::string_view /*plugin*/, std::string_view /*from*/, std::optional<LaunchShape>) { return unavailable; }
    virtual std::vector<LaunchNews> take_launches() { return {}; }
    // Pad controls kept from the game while any plugin asks: the triggers, and the left stick's
    // left/right while the skater grinds.
    // The skater carried up to a speed (m/s, 0 off) for the next 300 ms, eased down to it with may_slow.
    virtual std::string cruise(std::string_view /*plugin*/, float /*speed*/, bool /*may_slow*/) { return unavailable; }
    // The board's velocity set (or `add`ed to) by the next physics step with the skater on its board,
    // within 500 ms.
    virtual std::string set_velocity(std::string_view /*plugin*/, const Vec3& /*velocity*/, bool /*add*/) { return unavailable; }
    // What of the pad the game does not see (reskate.input.hide): the triggers, the left stick's
    // sideways while grinding, the right stick and the stick clicks.
    struct PadHidden {
        bool left_trigger{}, right_trigger{}, left_x_grinding{}, right_stick{}, left_stick_click{}, right_stick_click{};
    };
    virtual std::string hide_pad(std::string_view /*plugin*/, const PadHidden&) { return unavailable; }
    // A camcorder look over the game's picture (reskate.screen.look; Extension/UI/Overlay/screen_look.h
    // says what each is) for the next 300 ms; empty ends it. One plugin at a time.
    struct ScreenLook {
        float bend{}, fov{60}, age{};
        bool bars{}, hood{}, grade{}, soft{}, scanlines{}, grain{}, dropouts{}, head_switching{}, tracking{}, smear{}, iris{};
        double rolling{-1};
        std::array<std::string, 5> text;
    };
    virtual std::string screen_look(std::string_view /*plugin*/, std::optional<ScreenLook>) { return unavailable; }
    // A hand slaps the lens, while this plugin has the look.
    virtual std::string screen_slap(std::string_view /*plugin*/) { return unavailable; }
    // The skater bails as from a slam of `force` (0.5 to 3); not in a throwdown. While No Bail protects
    // the skater the wipeout is dropped, as any other is.
    virtual std::string wipeout(float /*force*/) { return unavailable; }
    // ---- audio: plugins' own sounds, in a level ---------------------------------------------------------
    // Its id, or 0 when the plugin plays 8 already (or there is no level).
    virtual std::uint64_t play_sound(std::string_view /*plugin*/, SoundStart) { return 0; }
    virtual bool change_sound(std::string_view /*plugin*/, std::uint64_t, const SoundChange&) { return false; }
    virtual void stop_sound(std::string_view /*plugin*/, std::uint64_t) {}
    virtual bool sound_playing(std::string_view /*plugin*/, std::uint64_t) { return false; }
    // ---- input -------------------------------------------------------------------------------------
    // The flight keys and mouse only while the plugin's camera is shown; pads while the game has the
    // focus. Capture keeps the game from the controller too, for 250 ms a call (renewed each tick),
    // and only while the plugin's camera is shown.
    virtual std::optional<FlightInput> flight_input(std::string_view /*plugin*/) { return std::nullopt; }
    virtual std::optional<PadInput> gamepad() { return std::nullopt; }
    virtual std::optional<StickInput> joystick() { return std::nullopt; }
    virtual std::string capture_input(std::string_view /*plugin*/, bool) { return unavailable; }
    // A named action on a key or pad combo (controller_bindings.h's encoding); presses come back
    // from take_presses as (plugin, action), only while the game itself has the input.
    virtual std::string bind(std::string_view /*plugin*/, std::string_view, std::uint32_t) { return unavailable; }
    virtual std::vector<std::pair<std::string, std::string>> take_presses() { return {}; }
    // Everything a plugin holds of the game (its HUD hide, nametags, binds), when it stops.
    virtual void release(std::string_view /*plugin*/) {}
};

// "F7", "Ctrl+K", "Shift+Alt+1", "Space"; or a pad combo, "Pad.LB+Pad.RB", "Pad.A", "Pad.Up"...
// As controller_bindings.h encodes binds; 0 for a name that is not one.
std::uint32_t combo_of(std::string_view name);
// A physics state as plugins hear it: "riding", "air", "wipeout", "grinding", "off_board",
// "planted", "other" or "none".
const char* skater_state_name(std::uint32_t state);
}
