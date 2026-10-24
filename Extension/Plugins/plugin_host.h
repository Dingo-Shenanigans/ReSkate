#pragma once
#include "plugin_vm.h"
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Every plugin the game runs: which are loaded, reloaded when their files change, switched off
// with their mod, and stopped when they go over a limit. The host is what plugins share: the
// level being played and the console's command names. One thread drives it (the client tick).
namespace dingosdk::plugins {
// A mod folder that is a plugin, as discovery found it.
struct Candidate {
    std::string id;                  // the mod's folder name
    std::filesystem::path directory; // the mod folder
    std::string fingerprint;         // changes when any of its files does
    std::string refusal;             // why it may not load, from its files alone, or empty
    bool operator==(const Candidate&) const = default;
};

// What the host needs from where it runs: the game, or the test runner.
class Environment {
public:
    virtual ~Environment() = default;
    virtual void log(LogLevel level, std::string_view text) = 0;
    virtual void console(std::string_view text) = 0;
    virtual double now_ms() = 0;
    // <root>/<plugin id>/storage.json holds each plugin's storage.
    virtual std::filesystem::path storage_root() = 0;
    // Whether ReSkate has a console command starting with this word.
    virtual bool builtin_command(std::string_view word) = 0;
    // The game (Services, plugin_vm.h). Where there is none, there is no skater and nothing to hit.
    virtual std::optional<SkaterState> skater() { return std::nullopt; }
    virtual Teleport teleport(std::string_view /*plugin*/, const Vec3&, float) { return Teleport::unavailable; }
    virtual std::optional<Vec3> raycast(const Vec3&, const Vec3&) { return std::nullopt; }
    virtual std::optional<ScoreState> score() { return std::nullopt; }
    virtual void toast(std::string_view /*plugin*/, LogLevel, std::string_view) {}
    virtual bool ui_sound(std::string_view) { return false; }
    virtual std::vector<ModInfo> mods() { return {}; }
    // The session now; read each tick, so it should be cheap (the game caches it).
    virtual SessionState session() { return {}; }
    // Plugin messages out (plugin_wire.h's bytes, for every player) and in (who sent them).
    virtual bool send_message(std::vector<std::uint8_t>) { return false; }
    struct Received {
        std::uint64_t from{};
        std::vector<std::uint8_t> bytes;
    };
    virtual std::vector<Received> take_messages() { return {}; }
    virtual std::optional<Vec3> world_to_screen(const Vec3&) { return std::nullopt; }
    virtual std::array<float, 2> display() { return {1920, 1080}; }
    // What plugins drew this tick, and their panels; the panels are built only while shown.
    virtual void publish_layers(std::vector<plugin_ui::Layer>) {}
    virtual bool panels_shown() { return false; }
    // The plugin whose page the menu shows (empty: any, as the tests have it).
    virtual std::string shown_page() { return {}; }
    virtual void publish_panels(std::vector<plugin_ui::Panel>) {}
    virtual void publish_pages(std::vector<plugin_ui::PageEntry>) {}
    virtual std::vector<plugin_ui::Action> take_actions() { return {}; }
    // net.http, made off the client thread: false when it cannot start; answers come back later.
    virtual bool http_request(HttpRequest) { return false; }
    virtual std::vector<HttpResponse> take_http_responses() { return {}; }
    // The wider set's game (plugin_game.h); none by default.
    virtual Game& game() {
        static Game none;
        return none;
    }
};

struct Status {
    std::string id;
    std::string state;  // "running", "stopped", "switched off", "not loaded"
    std::string reason; // why it is not running
    std::size_t memory{};
    double tick_ms{};
    std::vector<CommandInfo> commands; // with the subcommands they declared
};

class Host final : private Services {
public:
    // A client host runs client plugins and those for both sides; a server host, server ones and both.
    // `suffix` follows each plugin's id where it is the game's owner of something, in its logs and
    // its storage: a lobby host's server copies ("@server") never share what the client copies hold.
    Host(Environment& environment, Side side, Limits limits = {}, std::string suffix = {});
    ~Host() override;
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    // The plugins discovery found now: new ones load, changed ones reload, missing ones unload.
    void apply(const std::vector<Candidate>& found);
    void tick(double dt_ms);
    void set_level(std::string level);
    // A trick of the local skater, to every plugin with events.tricks.
    void trick(const TrickNews& news);
    // Whether a running plugin has events.tricks: the score HUD is read only then.
    bool wants_tricks() const;
    // Whether a running plugin has chat.read: a dedicated server then hands it the /commands it
    // does not know itself.
    bool wants_chat() const;
    // Runs a plugin's console command when the text starts with one; false when none matched.
    bool run_console(std::string_view text);
    std::vector<Status> status() const;
    // The `plugin` console commands. Each returns what to print.
    std::string reload(std::string_view id);
    std::string switch_off(std::string_view id);
    std::string switch_on(std::string_view id);
    // Unloads every plugin (their unload handlers run).
    void shutdown();

private:
    struct Entry {
        Candidate candidate;
        std::unique_ptr<Plugin> plugin;
        std::string failure; // why it is not running
        bool held{};         // switched off with `plugin off` until `plugin on` or the game restarts
    };
    void load(Entry& entry);
    void unload(Entry& entry);
    // A plugin that stopped itself (a limit, its errors) is closed and its commands released.
    void collect(Entry& entry);
    void release_commands(std::string_view id);
    Entry* find(std::string_view id);

    void log(std::string_view plugin, LogLevel level, std::string_view text) override;
    void console(std::string_view text) override;
    double now_ms() override;
    std::string level() override;
    std::filesystem::path storage_file(std::string_view plugin) override;
    std::string claim_command(std::string_view plugin, std::string_view name) override;
    std::optional<SkaterState> skater() override { return environment_.skater(); }
    Teleport teleport(std::string_view plugin, const Vec3& position, float heading) override {
        return environment_.teleport(plugin, position, heading);
    }
    std::optional<Vec3> raycast(const Vec3& from, const Vec3& to) override { return environment_.raycast(from, to); }
    std::optional<ScoreState> score() override { return environment_.score(); }
    void toast(std::string_view plugin, LogLevel level, std::string_view text) override { environment_.toast(plugin, level, text); }
    bool ui_sound(std::string_view name) override { return environment_.ui_sound(name); }
    std::vector<ModInfo> mods() override { return environment_.mods(); }
    const SessionState& session() override { return session_; }
    bool send(std::uint32_t channel, std::uint64_t to, std::string_view payload) override;
    std::optional<Vec3> world_to_screen(const Vec3& position) override { return environment_.world_to_screen(position); }
    std::array<float, 2> display() override { return environment_.display(); }
    std::string call_plugin(std::string_view caller, std::string_view target, std::string_view function, const Json& arguments,
                            Json& result) override;
    std::string exposed(std::string_view target, std::vector<std::string>& names) override;
    bool emit(std::string_view source, std::string_view event, Json data) override;
    bool http(HttpRequest request) override { return environment_.http_request(std::move(request)); }
    Game& game() override { return environment_.game(); }
    bool authority() override { return side_ == Side::server; }
    // What the game did since the last tick, to the plugins with the permission for it.
    void follow_game();
    void deliver_http();
    // A running plugin by its folder's name or its manifest.json name, whatever their case.
    Entry* running(std::string_view name);
    void deliver_events();
    // The session's players who came and went, and the messages that came in, to the plugins;
    // then what the plugins draw this tick, and their panels.
    void follow_session();
    void deliver_messages();
    void draw_and_build();

    Environment& environment_;
    Side side_;
    Limits limits_;
    std::string suffix_;
    std::map<std::string, Entry, std::less<>> entries_;
    std::map<std::string, std::string, std::less<>> commands_; // command name: plugin id
    std::string level_;
    double next_save_{};
    SessionState session_;
    double next_panels_{};
    std::vector<plugin_ui::PageEntry> pages_; // as last published
    std::vector<plugin_ui::Action> actions_; // waiting for the next panel build
    struct Emitted {
        std::string source, event;
        Json data;
    };
    std::vector<Emitted> events_; // emitted since the last tick
    std::optional<std::uint32_t> skater_state_;
    std::uint64_t wipeouts_{};
    bool wipeouts_known_{};
    std::uint64_t chat_seen_{};
    bool chat_started_{};
    std::uint64_t track_revision_{};
    double next_track_{};
    bool throwdown_active_{};
};
}
