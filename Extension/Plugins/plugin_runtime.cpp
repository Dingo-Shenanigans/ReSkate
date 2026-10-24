#include "plugin_runtime.h"
#include "plugin_discovery.h"
#include "plugin_host.h"
#include "plugin_camera.h"
#include "plugin_http.h"
#include "plugin_game_runtime.h"
#include "plugin_wire.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include "Extension/Multiplayer/Session/session.h"
#include "Extension/UI/NativeMenu/ui_sound.h"
#include "Extension/UI/Overlay/overlay.h"
#include "Extension/Console/commands.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Skater/launches.h"
#include "Extension/Skater/score_hud.h"
#include "Extension/Skater/trick_watch.h"
#include "Extension/Trainer/trainer.h"
#include "Extension/Throwdowns/throwdown_kit.h"
#include <Windows.h>
#include <ShlObj.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <mutex>
#include <numbers>
#include <thread>

namespace dingosdk::plugins {
namespace {
std::uintptr_t base{};
// What the discovery thread last found of the Mods folder (reskate.mods).
std::mutex mods_mutex;
std::vector<ModInfo> found_mods; // under mods_mutex
// Plugin messages in and out of the session's throwdown packets: a few at a time. The server
// copies' (on a lobby's host) have their own.
std::mutex messages_mutex;
struct Mailbox {
    std::vector<Environment::Received> inbox;
    std::vector<std::vector<std::uint8_t>> outbox;
};
Mailbox client_mail, server_mail; // under messages_mutex

// The game for the plugins: a player's copies (client), or the server copies a lobby's host runs,
// which draw nothing and keep their storage apart.
class GameEnvironment final : public Environment {
public:
    GameEnvironment(std::filesystem::path storage, bool server) : storage_(std::move(storage)), server_(server) {}
    void log(LogLevel level, std::string_view text) override {
        logging::write(level == LogLevel::error ? logging::Level::error : level == LogLevel::warn ? logging::Level::warning : logging::Level::info,
            logging::Channel::plugins, text);
    }
    // As the console's own lines are written (console_line): an "error: " line is shown as one.
    void console(std::string_view text) override {
        const bool error = text.starts_with("error: ");
        logging::write(error ? logging::Level::error : logging::Level::info, logging::Channel::command, error ? text.substr(7) : text);
    }
    double now_ms() override {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    std::filesystem::path storage_root() override { return storage_; }
    bool builtin_command(std::string_view word) override {
        const auto& commands = console::game_commands();
        return commands.find(word) || !commands.under(word).empty();
    }
    // The trainer's telemetry of the local skater, published each tick. Its heading is 0 to 360
    // degrees the same way round; plugins get -pi to pi.
    std::optional<SkaterState> skater() override {
        const auto t = trainer::telemetry();
        if (!t.skater) return std::nullopt;
        const float heading = std::remainder(t.heading * std::numbers::pi_v<float> / 180, 2 * std::numbers::pi_v<float>);
        SkaterState state{t.position, heading, t.speed, t.vertical, t.airborne, t.air_time};
        if (const auto facing = launches::facing()) {
            state.right = facing->right;
            state.switch_stance = facing->switch_stance;
        }
        return state;
    }
    // As the console's tp: refused in a session whose host has turned teleporting (and noclip) off.
    Teleport teleport(std::string_view plugin, const Vec3& position, float heading) override {
        // A throwdown mode's own moves (line-ups, recoveries, returns to the start) are the game's:
        // allowed while that plugin's own match runs and asks for them (the kit's `match.teleports`),
        // never for another plugin's calls.
        if (!session_noclip_allowed()) {
            const auto flag = throwdowns::kit::flag::state();
            const bool mine = flag.owned && flag.mode.size() > plugin.size() && flag.mode.starts_with(plugin) && flag.mode[plugin.size()] == '/';
            if (!throwdowns::kit::allows_teleports(plugin, mine)) return Teleport::forbidden;
        }
        return teleport_local_skater_heading(position, heading) ? Teleport::sent : Teleport::unavailable;
    }
    std::optional<Vec3> raycast(const Vec3& from, const Vec3& to) override { return local_surface_hit(from, to); }
    std::optional<ScoreState> score() override { return score_now; }
    std::optional<ScoreState> score_now; // read each tick while a plugin wants tricks
    // ReSkate's notices in the top left, titled with the plugin's folder name.
    void toast(std::string_view plugin, LogLevel level, std::string_view text) override {
        overlay::notify(level == LogLevel::error ? overlay::NoticeLevel::error
                        : level == LogLevel::warn ? overlay::NoticeLevel::warning : overlay::NoticeLevel::info,
                        std::string(plugin), std::string(text));
    }
    bool ui_sound(std::string_view name) override {
        using multiplayer::UiSound;
        UiSound sound{};
        if (name == "select") sound = UiSound::select;
        else if (name == "cancel") sound = UiSound::cancel;
        else if (name == "navigate") sound = UiSound::navigate;
        else if (name == "toggle") sound = UiSound::toggle;
        else if (name == "tab") sound = UiSound::tab;
        else if (name == "countdown") sound = UiSound::countdown;
        else if (name == "go") sound = UiSound::go;
        else return false;
        multiplayer::queue_ui_sound(static_cast<std::uint32_t>(sound));
        return true;
    }
    std::vector<ModInfo> mods() override {
        std::lock_guard lock(mods_mutex);
        return found_mods;
    }
    // The session's model is large to copy: read four times a second.
    SessionState session() override {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_session_) return session_;
        next_session_ = now + std::chrono::milliseconds(250);
        const auto m = multiplayer::model();
        session_ = {};
        if (!m.active || !m.connected || !m.local_id || m.echo) return session_;
        session_.active = true;
        session_.dedicated = m.dedicated;
        session_.players.push_back({m.local_id, m.local_name, true, !m.dedicated && m.host_id == m.local_id});
        for (const auto& p : m.roster)
            if (p.connected && p.id && p.id != m.local_id)
                session_.players.push_back({p.id, p.name, false, !m.dedicated && p.id == m.host_id});
        return session_;
    }
    bool send_message(std::vector<std::uint8_t> bytes) override {
        std::lock_guard lock(messages_mutex);
        auto& outbox = mail().outbox;
        if (outbox.size() >= 64) return false;
        outbox.push_back(std::move(bytes));
        return true;
    }
    std::vector<Received> take_messages() override {
        std::lock_guard lock(messages_mutex);
        return std::exchange(mail().inbox, {});
    }
    std::optional<Vec3> world_to_screen(const Vec3& p) override {
        const auto display = plugin_ui::display();
        const auto projector = plugin_camera::projector(display[0], display[1]);
        return projector ? projector->project(p) : std::nullopt;
    }
    std::array<float, 2> display() override { return plugin_ui::display(); }
    void publish_layers(std::vector<plugin_ui::Layer> layers) override {
        if (!server_) plugin_ui::publish_layers(std::move(layers));
    }
    bool panels_shown() override { return !server_ && plugin_ui::panels_shown(); }
    std::string shown_page() override { return plugin_ui::shown_page(); }
    void publish_pages(std::vector<plugin_ui::PageEntry> pages) override {
        if (!server_) plugin_ui::publish_pages(std::move(pages));
    }
    void publish_panels(std::vector<plugin_ui::Panel> panels) override {
        if (!server_) plugin_ui::publish_panels(std::move(panels));
    }
    std::vector<plugin_ui::Action> take_actions() override { return server_ ? std::vector<plugin_ui::Action>{} : plugin_ui::take_actions(); }
    bool http_request(HttpRequest request) override { return http::start(std::move(request)); }
    Game& game() override { return real_game(base, storage_); }
    std::vector<HttpResponse> take_http_responses() override { return http::take(); }

private:
    std::chrono::steady_clock::time_point next_session_{};
    SessionState session_;

private:
    Mailbox& mail() { return server_ ? server_mail : client_mail; }
    std::filesystem::path storage_;
    bool server_{};
};

std::filesystem::path data_root;
// Left to the process's end on purpose: unloading plugins from a static destructor would run
// their scripts while the game shuts down.
GameEnvironment* environment{};
Host* host{};
// Plugins' server copies, while this game hosts a lobby: the session's authority.
GameEnvironment* server_environment{};
Host* server_host{};
bool serving{};
std::uint64_t local_player{};
std::mutex found_mutex;
std::vector<Candidate> found;      // under found_mutex
std::uint64_t found_generation{};  // under found_mutex
std::uint64_t applied_generation{};
// A change to the plugins waits for the game window to have the focus: a plugin being edited in
// another window loads once, when the player comes back to the game, not at every save. The first
// set loads at once, focused or not.
bool held_logged{};
bool game_focused() {
    DWORD process{};
    const auto window = GetForegroundWindow();
    return window && GetWindowThreadProcessId(window, &process) && process == GetCurrentProcessId();
}
std::chrono::steady_clock::time_point last_tick{};
// The local skater's tricks (Extension/Skater/trick_watch.h), and where and when the one going on began.
tricks::TrickWatch trick_watch;
Vec3 started_at{};
std::chrono::steady_clock::time_point started_time{};
std::string watched_level;
std::string last_trick_name; // the score HUD's last sequence header

// The score HUD into the trick watch, and what it saw to the plugins. Client thread.
void read_tricks() {
    const auto hud = tricks::read_score_hud(base);
    if (!hud) {
        environment->score_now.reset();
        return;
    }
    const auto skater = trainer::telemetry();
    tricks::TrickSignals signals;
    signals.action = hud->action;
    signals.line_score = hud->line_score;
    signals.multiplier = hud->multiplier;
    signals.slam = hud->slam;
    // A wipeout is physics states 300-399.
    signals.wipeout = skater.skater && skater.physics_state >= 300 && skater.physics_state < 400;
    const auto event = trick_watch.update(signals);
    environment->score_now = ScoreState{hud->line_score, hud->multiplier, trick_watch.in_trick(), hud->header, hud->descriptor,
                                        tricks::names_manual(*hud)};
    if (!hud->header.empty()) last_trick_name = hud->header;
    const auto now = std::chrono::steady_clock::now();
    TrickNews news;
    switch (event.kind) {
    case tricks::TrickEvent::Kind::none: return;
    case tricks::TrickEvent::Kind::started:
        started_at = skater.position;
        started_time = now;
        news.kind = TrickNews::Kind::started;
        news.started_at = started_at;
        break;
    case tricks::TrickEvent::Kind::landed:
        news.kind = TrickNews::Kind::landed;
        news.points = event.points;
        news.multiplier = event.multiplier;
        news.started_at = started_at;
        news.landed_at = skater.position;
        news.ms = std::chrono::duration<double, std::milli>(now - started_time).count();
        news.name = last_trick_name;
        break;
    case tricks::TrickEvent::Kind::broken:
        news.kind = TrickNews::Kind::broken;
        news.reason = signals.slam ? "slam" : "wipeout";
        break;
    }
    host->trick(news);
}

void watch() {
    FileChecks checks;
    for (;;) {
        try {
            std::vector<ModInfo> mods;
            auto plugins = discover(data_root, checks, &mods);
            {
                std::lock_guard lock(mods_mutex);
                found_mods = std::move(mods);
            }
            std::lock_guard lock(found_mutex);
            if (plugins != found || !found_generation) {
                found = std::move(plugins);
                ++found_generation;
            }
        } catch (const std::exception& error) {
            logging::log(logging::Level::warning, logging::Channel::plugins, "Looking for plugins failed: {}", error.what());
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

std::filesystem::path local_storage() {
    PWSTR raw{};
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) && raw)
        result = std::filesystem::path(raw) / L"ReSkate" / L"Plugins";
    CoTaskMemFree(raw);
    return result;
}
}

bool start(std::uintptr_t image_base) noexcept {
    try {
        if (host) return true;
        base = image_base;
        plugin_camera::image_base().store(image_base);
        std::array<wchar_t, 32768> path{};
        const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length >= path.size()) return false;
        data_root = std::filesystem::path(std::wstring(path.data(), length)).parent_path();
        auto storage = local_storage();
        if (storage.empty()) storage = data_root / L"ReSkate.Plugins";
        server_environment = new GameEnvironment(storage / L"server", true);
        environment = new GameEnvironment(std::move(storage), false);
        host = new Host(*environment, Side::client);
        server_host = new Host(*server_environment, Side::server, {}, "@server");
        std::thread(watch).detach();
        logging::write(logging::Level::info, logging::Channel::plugins, "Plugins: looking in Mods for reskate-plugin.json every 2 s.");
        return true;
    } catch (const std::exception& error) {
        logging::log(logging::Level::error, logging::Channel::plugins, "Plugins could not start: {}", error.what());
        return false;
    }
}

void on_client_tick() noexcept {
    if (!host) return;
    try {
        {
            std::vector<Candidate> latest;
            bool changed = false;
            {
                std::lock_guard lock(found_mutex);
                if (found_generation != applied_generation) {
                    if (applied_generation && !game_focused()) {
                        if (!std::exchange(held_logged, true))
                            logging::write(logging::Level::info, logging::Channel::plugins,
                                           "Plugins: a change waits for the game to have the focus.");
                    } else {
                        latest = found;
                        applied_generation = found_generation;
                        changed = true;
                        held_logged = false;
                    }
                }
            }
            if (changed) host->apply(latest);
            // The server copies run while this game hosts a lobby (a dedicated server runs its own).
            const auto session = environment->session();
            local_player = 0;
            bool hosting = false;
            for (const auto& player : session.players)
                if (player.me) {
                    local_player = player.id;
                    hosting = session.active && player.host && !session.dedicated;
                }
            if (hosting != serving) {
                serving = hosting;
                if (serving) {
                    std::lock_guard lock(found_mutex);
                    server_host->apply(found);
                    logging::write(logging::Level::info, logging::Channel::plugins, "Plugins: hosting a lobby, so server copies run here.");
                } else {
                    server_host->apply({});
                    std::lock_guard lock(messages_mutex);
                    server_mail = {};
                }
            } else if (serving && changed) server_host->apply(latest);
        }
        const auto now = std::chrono::steady_clock::now();
        const double dt = last_tick == std::chrono::steady_clock::time_point{} ? 0
            : std::chrono::duration<double, std::milli>(now - last_tick).count();
        last_tick = now;
        if (host->wants_tricks()) read_tricks();
        else environment->score_now.reset();
        host->tick(dt);
        if (serving) server_host->tick(dt);
    } catch (const std::exception& error) {
        logging::log(logging::Level::error, logging::Channel::plugins, "Plugin tick failed: {}", error.what());
    }
}

void set_level(std::string_view level) noexcept {
    if (!host) return;
    try {
        // A new level starts a new line: a trick going on before it scores nothing.
        if (level != watched_level) {
            watched_level = std::string(level);
            trick_watch = tricks::TrickWatch{};
        }
        host->set_level(std::string(level));
    }
    catch (const std::exception& error) {
        logging::log(logging::Level::error, logging::Channel::plugins, "Plugin level change failed: {}", error.what());
    }
}

bool run_console(const std::string& text) noexcept {
    if (!host) return false;
    try { return host->run_console(text) || (serving && server_host->run_console(text)); }
    catch (const std::exception& error) {
        logging::log(logging::Level::error, logging::Channel::plugins, "Plugin command failed: {}", error.what());
        return true;
    }
}

// Another player's plugin message. On a lobby's host the server copies hear what is for them and
// what is for everyone; the authority's words come only from the authority (session.cpp checks).
void receive_message(std::uint64_t sender, const std::vector<std::uint8_t>& bytes) noexcept {
    try {
        std::lock_guard lock(messages_mutex);
        const auto to = wire::to_of(bytes);
        const auto put = [&](Mailbox& mail) {
            if (mail.inbox.size() < 256) mail.inbox.push_back({sender, bytes});
        };
        if (serving) {
            if (wire::from_authority(bytes)) return;
            if (to == wire::authority || to == 0) put(server_mail);
            if (to != wire::authority) put(client_mail);
        } else if (to != wire::authority) put(client_mail);
    } catch (...) {}
}

// What goes out to the session. On a lobby's host, what its own copies say to the authority stays
// here, and what the authority says to everyone (or the host) is heard here too.
std::vector<std::vector<std::uint8_t>> take_outgoing() noexcept {
    std::lock_guard lock(messages_mutex);
    auto out = std::exchange(client_mail.outbox, {});
    if (!serving) return out;
    std::vector<std::vector<std::uint8_t>> sent;
    for (auto& bytes : out) {
        const auto to = wire::to_of(bytes);
        if ((to == wire::authority || to == 0) && server_mail.inbox.size() < 256) server_mail.inbox.push_back({local_player, bytes});
        if (to != wire::authority) sent.push_back(std::move(bytes));
    }
    for (auto& bytes : std::exchange(server_mail.outbox, {})) {
        const auto to = wire::to_of(bytes);
        if ((to == 0 || to == local_player) && client_mail.inbox.size() < 256) client_mail.inbox.push_back({local_player, bytes});
        if (to != local_player) sent.push_back(std::move(bytes));
    }
    return sent;
}

std::string describe() {
    if (!host) return "error: Plugins did not start.";
    auto plugins = host->status();
    // While this game hosts a lobby, its server copies too.
    if (serving)
        for (auto status : server_host->status()) {
            status.id += "@server";
            plugins.push_back(std::move(status));
        }
    if (plugins.empty()) return "No plugins. A plugin is a mod in Mods with a reskate-plugin.json.";
    std::string text;
    for (const auto& plugin : plugins) {
        if (!text.empty()) text += '\n';
        text += std::format("{}: {}", plugin.id, plugin.state);
        if (plugin.state == "running")
            text += std::format(", {:.1f} MB, {:.3f} ms a tick, {} command{}", plugin.memory / 1048576.0, plugin.tick_ms,
                plugin.commands.size(), plugin.commands.size() == 1 ? "" : "s");
        else if (!plugin.reason.empty()) text += " (" + plugin.reason + ")";
    }
    return text;
}

std::string list_commands() {
    if (!host) return "error: Plugins did not start.";
    std::string text;
    const auto add = [&](const std::vector<Status>& all, std::string_view suffix) {
        for (const auto& plugin : all)
            for (const auto& command : plugin.commands) {
                if (!text.empty()) text += '\n';
                text += std::format("{}{}{} - {} ({}{})", command.name, command.args.empty() ? "" : " ", command.args, command.help, plugin.id,
                                    suffix);
            }
    };
    add(host->status(), "");
    if (serving) add(server_host->status(), "@server"); // run when no player's copy has the command
    return text.empty() ? "No plugin has a console command." : text;
}

std::vector<CommandHint> command_hints() {
    static std::vector<CommandHint> hints;
    static std::uint64_t next{};
    const auto now = GetTickCount64();
    if (now < next) return hints;
    next = now + 500;
    hints.clear();
    if (!host) return hints;
    const auto add = [&](const std::vector<Status>& all, std::string_view suffix) {
        for (const auto& plugin : all)
            for (const auto& command : plugin.commands)
                if (std::none_of(hints.begin(), hints.end(), [&](const auto& hint) { return hint.name == command.name; }))
                    hints.push_back({command.name, command.args, command.help, plugin.id + std::string(suffix)});
    };
    add(host->status(), "");
    if (serving) add(server_host->status(), "@server");
    std::sort(hints.begin(), hints.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return hints;
}

// "<id>@server" names a server copy (on this game while it hosts a lobby); a plain id, the player's copy.
namespace {
template <class Do> std::string either(std::string_view id, Do&& act) {
    if (!host) return "error: Plugins did not start.";
    constexpr std::string_view server_suffix = "@server";
    if (id.ends_with(server_suffix)) {
        if (!serving) return "error: server copies run only while this game hosts a lobby.";
        return act(*server_host, id.substr(0, id.size() - server_suffix.size()));
    }
    return act(*host, id);
}
}
std::string reload(std::string_view id) { return either(id, [](Host& h, std::string_view i) { return h.reload(i); }); }
std::string switch_off(std::string_view id) { return either(id, [](Host& h, std::string_view i) { return h.switch_off(i); }); }
std::string switch_on(std::string_view id) { return either(id, [](Host& h, std::string_view i) { return h.switch_on(i); }); }
}
