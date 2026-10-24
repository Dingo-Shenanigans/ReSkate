#pragma once
#include "plugin_manifest.h"
#include "plugin_ui.h"
#include "plugin_game.h"
#include "Engine/Core/Json/json.h"
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

// One plugin: its own Luau VM, the reskate API registered in it, and the limits that keep it from
// harming the game. Nothing a plugin does reaches the game except through the API, and every
// limit it goes over stops it, not the game.
//
// Not thread safe: a plugin is loaded, ticked and unloaded on one thread (the client tick).
namespace dingosdk::plugins {
enum class LogLevel { info, warn, error };

struct Limits {
    std::size_t memory = 64u << 20;     // the VM's heap
    double call_ms = 50;                // one call into the plugin (load, a handler, a command)
    double unload_ms = 20;              // its unload handlers, together
    double tick_ms = 2;                 // the average time a tick, over a window of 1 s...
    int over_windows = 3;               // ...over it this many windows in a row stops it
    int errors = 5;                     // more errors than this...
    double error_window_ms = 10'000;    // ...in this long stops it
    int log_lines_per_second = 50;
    std::size_t storage_bytes = 1u << 20;
    std::size_t file_bytes = 16u << 20; // a package file read
    std::size_t source_bytes = 1u << 20; // a source file
    std::size_t handlers = 256;         // per event
    std::size_t timers = 1024;
    std::size_t commands = 64;
    int raycasts_per_tick = 64;
    float raycast_metres = 1000;        // the longest ray
    int teleports_per_second = 2;
    std::size_t shapes = 4000;          // drawn each frame
    std::size_t shape_text_bytes = 64u << 10;
    std::size_t images = 32; // different pictures a plugin draws
    std::size_t clip_bytes = 48u << 20; // the clips a plugin holds, as poses (40 bytes a joint a frame)
    std::size_t sounds = 32;            // different waves a plugin plays, each at most 30 s
    int toasts = 3;                     // in 5 s
    int sounds_per_second = 10;
    std::size_t message_bytes = 4096 - 14; // a plugin message's payload (plugin_wire.h's header is 14)
    std::size_t message_bytes_per_second = 16u << 10;
    std::size_t panels = 8;
    std::size_t widgets = 64;           // a panel's
    std::size_t page_widgets = 160;     // its page's (one tab of it, as a page builds only the tab shown)
    std::size_t exposed = 64;           // functions a plugin offers others
    std::size_t listeners = 64;         // on_plugin handlers
    std::size_t event_bytes = 64u << 10; // an emitted event's data, as JSON
    std::size_t events_per_tick = 64;   // emitted by one plugin
    int http_per_minute = 10;
    int http_in_flight = 4;
    std::size_t http_body_bytes = 64u << 10;
    std::size_t http_url_bytes = 2048;
};

// The game, as plugins see it (Environment, plugin_host.h, gives the host these).
using Vec3 = std::array<float, 3>;
// A console command a plugin added, or a subcommand it declared for one (Plugin::commands).
struct CommandInfo {
    std::string name, args, help;
};
struct SkaterState {
    Vec3 position{};
    // Radians about world up, -pi to pi: 0 faces +Z, pi/2 faces +X, and a higher heading is turned
    // to the skater's left (checked in game). Teleports take it the same way.
    float heading{};
    float speed{};        // metres per second over the ground
    float vertical{};     // metres per second, up
    bool airborne{};
    float air_time{};     // seconds into the jump going on
    // The skater root's right axis, and whether it rides switch (Extension/Skater/launches.h), when known.
    std::optional<Vec3> right;
    bool switch_stance{};
};
enum class Teleport { sent, unavailable, forbidden };
// A trick of the local skater, read from the score HUD (Extension/Skater/trick_watch.h).
struct TrickNews {
    enum class Kind { started, landed, broken } kind{Kind::started};
    float points{}, multiplier{}; // landed
    Vec3 started_at{}, landed_at{};
    double ms{};                  // landed: from its start
    std::string reason;           // broken: "slam" or "wipeout"
    std::string name;             // landed: the trick the HUD named last (may be empty)
};
struct ScoreState {
    float line{}, multiplier{};
    bool in_trick{};
    std::string trick, detail; // the HUD's sequence header and descriptor ("Manual", "Kickflip"...)
    bool manual{};             // a manual is going on, as the HUD names it
};
// The multiplayer session as plugins see it: who is in it (the local player too).
struct SessionPlayer {
    std::uint64_t id{}; // SteamID64
    std::string name;
    bool me{}, host{};
    bool server{}; // the session's authority: a message from a plugin's server copy
    bool operator==(const SessionPlayer&) const = default;
};
struct SessionState {
    bool active{}, dedicated{};
    std::vector<SessionPlayer> players;
};
// net.http: a request the host makes for a plugin, and what came back.
struct HttpRequest {
    std::uint64_t id{};
    std::string plugin, method, host, path, body, content_type;
};
struct HttpResponse {
    std::uint64_t id{};
    std::string plugin;
    int status{};          // 0 when it failed
    std::string body, error;
};
// A mod in the Mods folder: its folder, the name it gives itself, whether it is on.
struct ModInfo {
    std::string folder, title;
    bool on{};
};

// What a plugin's VM can reach outside itself. The host implements it.
class Services {
public:
    virtual ~Services() = default;
    virtual void log(std::string_view plugin, LogLevel level, std::string_view text) = 0;
    virtual void console(std::string_view text) = 0;
    virtual double now_ms() = 0;           // a steady clock, for reskate.time and the windows
    virtual std::string level() = 0;       // the level being played, or empty
    virtual std::filesystem::path storage_file(std::string_view plugin) = 0;
    // Empty when the plugin may have the console command; otherwise why not.
    virtual std::string claim_command(std::string_view plugin, std::string_view name) = 0;
    // The local skater, or empty without one (the menu, a load).
    virtual std::optional<SkaterState> skater() = 0;
    // `plugin`: whose call it is (a throwdown match of its own may let it through, kit::allows_teleports).
    virtual Teleport teleport(std::string_view plugin, const Vec3& position, float heading) = 0;
    // Where the line first meets the world's collision, or empty when it is clear (or not loaded).
    virtual std::optional<Vec3> raycast(const Vec3& from, const Vec3& to) = 0;
    // The score HUD now, or empty without one.
    virtual std::optional<ScoreState> score() = 0;
    virtual void toast(std::string_view plugin, LogLevel level, std::string_view text) = 0;
    // One of the game's menu sounds by name; false for a name there is none of.
    virtual bool ui_sound(std::string_view name) = 0;
    virtual std::vector<ModInfo> mods() = 0;
    virtual const SessionState& session() = 0;
    // A plugin message to the same plugin on other players' games (to 0: everyone). False without a session.
    virtual bool send(std::uint32_t channel, std::uint64_t to, std::string_view payload) = 0;
    // A world position on screen (x, y in pixels, z its depth in front of the camera), or empty behind it.
    virtual std::optional<Vec3> world_to_screen(const Vec3& position) = 0;
    virtual std::array<float, 2> display() = 0;
    // Libraries and events between plugins: another running plugin's exposed function called with
    // plain data (empty, and the result set, when it worked; else why not), the names it exposes,
    // and an event for every plugin that listens (delivered at the next tick).
    virtual std::string call_plugin(std::string_view caller, std::string_view target, std::string_view function,
                                    const Json& arguments, Json& result) = 0;
    virtual std::string exposed(std::string_view target, std::vector<std::string>& names) = 0;
    virtual bool emit(std::string_view source, std::string_view event, Json data) = 0;
    // net.http: false when it cannot be sent now (the answer comes as Plugin::http_done).
    virtual bool http(HttpRequest request) = 0;
    // The wider set's game (plugin_game.h).
    virtual Game& game() = 0;
    // Whether this is the session's authority: plugins' server copies, on the lobby's host or the
    // dedicated server.
    virtual bool authority() = 0;
};

enum class State { loading, running, stopped };

class Plugin {
public:
    // `directory` is the mod folder; `id` its folder name.
    Plugin(std::string id, std::filesystem::path directory, Manifest manifest, Services& services, Limits limits);
    ~Plugin();
    Plugin(const Plugin&) = delete;
    Plugin& operator=(const Plugin&) = delete;

    // Creates the VM, runs the entry (or `entry` in its place, for the test runner), then the load
    // handlers. False when it stopped; reason() says why.
    bool load(std::string entry = {});
    // The unload handlers (while it runs), its storage saved, the VM closed. Safe to call twice.
    void unload();
    // Stops it at the next safe point: the VM is closed by close_if_stopped().
    void stop(std::string reason);
    void close_if_stopped();

    void tick(double dt_ms);
    void level_changed(const std::string& previous, const std::string& current);
    // trick_started, trick_landed or line_broken, to a plugin with events.tricks.
    void trick(const TrickNews& news);
    // Its draw handlers (overlay): what they drew is taken after.
    void draw();
    std::vector<plugin_ui::Shape> take_shapes() { return std::exchange(shapes_, {}); }
    // Its page and its menu panels, built with what the player did in them since the last build.
    std::vector<plugin_ui::Panel> build_panels(const std::vector<plugin_ui::Action>& actions);
    // Its name in the menu's PLUGINS section: its page's title, its id when it only has panels, or
    // empty for none.
    std::string page_name() const;
    // session.read: players who came and went.
    void session_changed(const std::vector<SessionPlayer>& joined, const std::vector<SessionPlayer>& left);
    // net.session: a message from the same plugin on another player's game.
    void receive(const SessionPlayer& from, const std::string& payload);
    // The name its messages go under: the same plugin on every game has it (its manifest.json name).
    void set_channel(std::string channel);
    const std::string& channel_name() const noexcept { return channel_; }
    // Libraries: one of its exposed functions, called by another plugin. Empty when it worked.
    std::string call_exposed(const std::string& function, const Json& arguments, Json& result);
    std::vector<std::string> exposed_names() const;
    // In a call now: it cannot be called into until that returns.
    bool busy() const noexcept { return calls_ > 0; }
    // An event another plugin emitted, to its on_plugin handlers for that plugin and event.
    void plugin_event(const std::string& source, const std::string& source_channel, const std::string& event, const Json& data);
    // net.http: the answer to one of its requests, to the function it gave.
    void http_done(const HttpResponse& response);
    // The wider set's events, each to a plugin with its permission and a handler for it.
    void skater_changed(const char* state, const char* previous);
    void bailed(std::uint64_t wipeouts, std::optional<SlamNews> slam);
    void launched(const LaunchNews& launch);
    // A game event with plain data (board_landed, body_hit, bail_end), to a plugin with `permission`.
    void game_event(std::string_view event, std::string_view permission, const Json& data);
    void chat_line(const ChatLine& line);
    // session.skaters: touching (reskate.session.touch). How near counts as touching while it is on,
    // and the players its skater touches now; touch_tick fires "touch" as one comes within reach and
    // "touch_end" once the two are 10 cm further apart than that (or the player is gone).
    struct Contact {
        std::uint8_t part{}, their_part{};
        float gap{};
        Vec3 at{};
    };
    std::optional<float> touch_reach() const noexcept { return touch_reach_; }
    const std::map<std::uint64_t, Contact>& contacts() const noexcept { return contacts_; }
    void touch_tick(const std::vector<Body>& bodies);
    void track_changed(const Track& track);
    void throwdown_phase(const char* phase);
    void action(const std::string& name);
    // ui.screen: the screen it has open, built; and closed (by Esc), which it hears.
    std::optional<plugin_ui::Panel> build_screen(const std::vector<plugin_ui::Action>& actions);
    void close_screen();
    std::uint32_t channel() const noexcept { return channel_hash_; }
    // A console command it registered: the words after the command's name.
    void run_command(const std::string& name, const std::vector<std::string>& arguments);
    void save_storage();

    const std::string& id() const noexcept { return id_; }
    const std::filesystem::path& directory() const noexcept { return directory_; }
    const Manifest& manifest() const noexcept { return manifest_; }
    State state() const noexcept { return state_; }
    const std::string& reason() const noexcept { return reason_; }
    std::size_t memory() const noexcept { return memory_; }
    double tick_ms() const noexcept { return last_window_ms_; }
    // Its console commands and their help.
    // Its console commands, and the subcommands each declared ("prop random", run by "prop"'s
    // function): the name, its arguments as help shows them, and its help.
    std::vector<CommandInfo> commands() const;
    bool has(std::string_view permission) const { return manifest_.permissions.contains(permission); }
    // Over a limit or stopped: the API refuses every call until the VM is closed.
    bool halted() const noexcept { return halt_ != Halt::none || memory_exceeded_; }

private:
    friend struct Api;
    friend struct ApiFunctions;
    friend struct UiApi;
    friend struct MoreApi;
    friend struct WorldApi;
    friend struct ServicesApi;
    friend struct KitApi;
    friend struct CaptureApi;
    struct Subcommand { std::string name, args, help; };
    struct Command { std::string help; int function{}; std::vector<Subcommand> subcommands; };
    struct Timer { double due{}, every{}; int function{}; };
    using Clock = std::chrono::steady_clock;
    enum class Halt { none, time, memory, stopped };

    static void* allocate(void* ud, void* ptr, std::size_t old_size, std::size_t new_size);
    static void interrupt(lua_State* L, int gc);
    // Calls the function under the `count` arguments on top of the main stack; false (and the
    // error reported) when it raised one. Stops the plugin past its limits.
    // With `results`, those are left on the main stack when it returns true.
    bool call(int count, std::string_view what, double limit_ms, int results = 0);
    void report(std::string_view what, std::string_view message);
    // reskate.log and console.print: at most limits_.log_lines_per_second lines between them.
    bool count_line();
    void log(LogLevel level, std::string_view text);
    void print(std::string_view text);
    // The compiled chunk of a source file on the main stack, or false and the error on it.
    bool load_chunk(lua_State* L, const std::string& relative, const std::filesystem::path& file);
    void fire(std::string_view event, const std::vector<std::string>& arguments);
    // Each handler of `event` with what `push` puts on the stack (it returns how many values).
    void fire_with(std::string_view event, const std::function<int(lua_State*)>& push);
    void close();

    std::string id_;
    std::filesystem::path directory_;
    std::string code_root_; // the entry's folder in the package: where require looks
    Manifest manifest_;
    Services& services_;
    Limits limits_;
    lua_State* L_{};
    int script_thread_{}, modules_{};
    State state_{State::loading};
    std::string reason_;

    std::size_t memory_{};
    Halt halt_{Halt::none};
    bool memory_exceeded_{};
    Clock::time_point deadline_{};
    bool timed_{};
    unsigned interrupts_{};
    double spent_ms_{};

    bool window_started_{};
    double window_start_{};
    double window_spent_{};
    int window_ticks_{}, over_windows_{};
    double last_window_ms_{};
    std::vector<double> errors_;
    double log_second_{};
    int log_lines_{};
    int raycasts_{};                    // since the last tick
    std::optional<float> touch_reach_;
    std::map<std::uint64_t, Contact> contacts_;
    double teleport_second_{};
    int teleports_{};
    std::vector<plugin_ui::Shape> shapes_;
    std::size_t shape_text_{};
    bool drawing_{};
    double shapes_warned_at_{-1e9}; // the drawing limit is said at most every 10 s
    std::map<std::string, std::string, std::less<>> image_files_; // a picture's path in the package, and its file
    // skater.animation: clips by their path in the package, or "rec:<n>" for a recording, and the
    // bytes they hold (at most limits_.clip_bytes).
    std::map<std::string, std::shared_ptr<const anim_studio::Clip>, std::less<>> clips_;
    std::size_t clip_bytes_{};
    int recordings_{};
    // reskate.audio: waves by their path in the package, and how many sounds began this second.
    std::map<std::string, std::shared_ptr<const Wave>, std::less<>> waves_;
    double wave_second_{};
    int wave_starts_{};
    int draw_table_{}, ui_table_{};
    struct PanelEntry { std::string title; int function{}; int dock{}; float width{}; };
    std::vector<PanelEntry> panels_;
    std::optional<PanelEntry> page_;        // reskate.menu.page
    plugin_ui::Panel* building_{};          // the panel a build is filling
    std::vector<plugin_ui::Action> pending_; // what the player did, for the build going on
    std::vector<double> toasts_;
    double sound_second_{};
    int sounds_{};
    double message_second_{};
    std::size_t message_bytes_{};
    std::string channel_;
    std::uint32_t channel_hash_{};
    std::map<std::string, int, std::less<>> exposed_;
    struct Listener {
        std::string source, event;
        int function{};
    };
    std::vector<Listener> listeners_;
    int calls_{};
    std::uint64_t emit_tick_{}, tick_number_{}; // the tick events were emitted in; ticks so far
    std::size_t emitted_{};
    std::map<std::uint64_t, int> http_pending_; // request id: its callback
    double control_second_{}, edit_second_{}, environment_second_{}, voice_second_{}, music_second_{};
    int controls_{}, edits_{}, environments_{}, voices_{}, musics_{};
    double travel_at_{-1e9}, chat_at_{-1e9}, local_chat_second_{}, server_second_{}, host_at_{-1e9}, wipeout_at_{-1e9};
    int local_chats_{}, server_calls_{};
    double files_second_{}, dialog_second_{}, reveal_second_{};
    int file_calls_{}, dialog_calls_{}, reveals_{};
    std::optional<std::pair<Vec3, float>> trainer_point_;
    std::set<std::string, std::less<>> binds_;
    std::vector<PanelEntry> screens_;
    std::string open_screen_;
    std::vector<double> http_sent_;
    std::uint64_t next_http_{1};

    std::map<std::string, std::vector<int>, std::less<>> handlers_;
    std::map<int, Timer> timers_;
    int next_timer_{1};
    std::map<std::string, Command, std::less<>> commands_;
    std::set<std::string, std::less<>> loading_modules_;

    Json storage_ = Json::object();
    bool storage_loaded_{}, storage_dirty_{};
};
}
