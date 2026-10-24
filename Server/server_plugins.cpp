#include "server_plugins.h"
#include "server_host.h"
#include "Extension/Plugins/plugin_content.h"
#include "Extension/Plugins/plugin_host.h"
#include "Extension/Plugins/plugin_manifest.h"
#include "Extension/Plugins/plugin_wire.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <map>

namespace dingosdk::server {
namespace {
using namespace dingosdk::plugins;

constexpr std::size_t most_files = 20000;

std::string text_of(const std::filesystem::path &path) {
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

// Changes when any file of the package does: its paths, sizes and times.
std::string fingerprint_of(const std::filesystem::path &directory) {
    std::uint64_t hash = 1469598103934665603ull;
    const auto add = [&](std::string_view text) {
        for (const char c : text) {
            hash ^= static_cast<unsigned char>(c);
            hash *= 1099511628211ull;
        }
    };
    std::error_code code;
    std::size_t files = 0;
    for (std::filesystem::recursive_directory_iterator it(directory, std::filesystem::directory_options::skip_permission_denied, code), end;
         !code && it != end && ++files <= most_files; it.increment(code)) {
        std::error_code entry;
        if (!it->is_regular_file(entry)) continue;
        const auto relative = std::filesystem::relative(it->path(), directory, entry).generic_u8string();
        add(std::string_view(reinterpret_cast<const char *>(relative.data()), relative.size()));
        add(std::to_string(it->file_size(entry)));
        add(std::to_string(it->last_write_time(entry).time_since_epoch().count()));
    }
    return std::format("{:016x}", hash);
}

// The server's game, for plugins: its players and what the server does to them.
class ServerGame final : public Game {
  public:
    ServerGame(Host &host, ServerPlugins::Log log) : host_(host), log_(std::move(log)) {}
    std::string kick(std::uint64_t player, std::string_view why) override {
        if (!present(player)) return "that player is not on the server";
        host_.drop(player, why.empty() ? std::string("Removed from the server.") : "Removed from the server: " + std::string(why),
                   "a plugin removed them");
        return {};
    }
    std::string announce(std::string_view plugin, std::string_view text) override {
        host_.broadcast_player_chat("[" + std::string(plugin) + "] " + std::string(text));
        return {};
    }
    std::string tell(std::string_view plugin, std::uint64_t player, std::string_view text) override {
        if (!present(player)) return "that player is not on the server";
        host_.send_player_chat(player, "[" + std::string(plugin) + "] " + std::string(text));
        return {};
    }
    std::string move_player(std::uint64_t player, const Vec3 &at) override {
        return host_.teleport_player(player, at[0], at[1], at[2]) ? std::string{} : "that player is not on the server";
    }
    // Permissions are the player's locks turned around; any SteamID64, here or not, as `lock` takes.
    std::optional<Permissions> permissions(std::uint64_t player) override {
        if (!multiplayer::individual_steam_id(player)) return std::nullopt;
        const auto locks = host_.player_locks(player);
        return Permissions{!(locks & lock_noclip), !(locks & lock_nobail), !(locks & lock_boosts), !(locks & lock_tuning),
                           !(locks & lock_objects), !(locks & lock_ollies)};
    }
    std::string apply_permissions(std::string_view plugin, std::uint64_t player, const Permissions &p) override {
        if (!multiplayer::individual_steam_id(player)) return "not a player's SteamID64";
        const auto locks = static_cast<std::uint8_t>((p.noclip ? 0 : lock_noclip) | (p.nobail ? 0 : lock_nobail) | (p.boosts ? 0 : lock_boosts) |
                                                     (p.tuning ? 0 : lock_tuning) | (p.objects ? 0 : lock_objects) | (p.ollies ? 0 : lock_ollies));
        host_.set_locks(player, locks, "plugin " + std::string(plugin));
        return {};
    }
    // The role they have (the one given them, or the server's default role), here or not.
    std::optional<Role> role(std::uint64_t player) override {
        if (!multiplayer::individual_steam_id(player)) return std::nullopt; // the server itself has none
        const auto *held = host_.player_role(player);
        if (!held) return std::nullopt;
        return Role{held->name, held->rank, server::staff(held)};
    }
    std::vector<ServerPark> server_parks() override {
        std::vector<ServerPark> result;
        for (auto &p : host_.park_status()) result.push_back({std::move(p.name), std::move(p.map), p.here, p.spawned, p.objects, p.minutes_left});
        return result;
    }
    std::string spawn_park(std::string_view plugin, std::string_view name, unsigned minutes) override {
        return host_.plugin_park(plugin, true, name, minutes);
    }
    std::string remove_park(std::string_view plugin, std::string_view name) override { return host_.plugin_park(plugin, false, name, 0); }
    // Cards and polls on the players' screens, as the roster carries the server's own.
    std::string server_card(std::string_view plugin, const std::vector<std::uint64_t> &players, std::string_view text, unsigned seconds) override {
        return host_.plugin_card(plugin, players, text, seconds);
    }
    std::string server_poll(std::string_view plugin, const std::vector<std::uint64_t> &players, std::string_view question,
                            const std::vector<std::string> &answers, unsigned seconds, std::uint32_t &id) override {
        return host_.plugin_poll(plugin, players, question, answers, seconds, id);
    }
    std::string server_poll_close(std::string_view plugin, std::uint32_t id) override { return host_.plugin_poll_close(plugin, id); }
    std::vector<PollNews> take_poll_news() override {
        std::vector<PollNews> news;
        for (auto &n : host_.take_poll_news()) news.push_back({std::move(n.plugin), n.id, n.player, n.answer, std::move(n.text), std::move(n.why)});
        return news;
    }
    // A player hidden from others, wholly or only their voice and sound (the roster and relay).
    std::string server_hide(std::string_view plugin, std::uint64_t target, const std::vector<std::uint64_t> &viewers, const Hiding &what) override {
        return host_.plugin_hide(plugin, target, viewers, what.skater, what.voice, what.sound);
    }
    std::string server_unhide(std::string_view plugin, std::uint64_t target, const std::vector<std::uint64_t> &viewers) override {
        return host_.plugin_unhide(plugin, target, viewers);
    }
    void release(std::string_view plugin) override {
        host_.plugin_screens_release(plugin);
        host_.plugin_hides_release(plugin);
    }
    // The skaters of the players in the world, as their latest poses put them. The server has
    // no skater of its own, so this is every player's.
    std::vector<PeerSkater> peer_skaters() override {
        std::vector<PeerSkater> result;
        for (const auto &s : host_.skater_poses()) {
            const auto &q = s.root.rotation;
            // The rotation's +Z about world up, as a client's plugin_game_runtime.cpp reads it.
            const float heading = std::atan2(2 * (q[0] * q[2] + q[3] * q[1]), 1 - 2 * (q[0] * q[0] + q[1] * q[1]));
            result.push_back({s.id, s.root.position, heading, static_cast<double>(s.age_us) / 1000.0});
        }
        return result;
    }
    // mods.content: every mod folder in the server's Mods, as a game reads its enabled mods. The
    // server has no load order or mods.json of its own; the folders come by name, as for its maps.
    std::string content_list(std::string_view folder, std::vector<ContentFile> &files) override {
        content::list(mod_folders(), folder, files);
        return {};
    }
    std::string content_read(std::string_view mod, std::string_view folder, std::string_view path, std::string &text) override {
        return content::read(mod_folders(), mod, folder, path, text);
    }
    // Through the console's own ban and unban, so they keep its checks and save the bans file.
    std::string ban(std::uint64_t player) override {
        const auto answer = host_.command("ban " + std::to_string(player));
        if (!host_.is_banned(player)) return answer;
        log_("[Plugins] " + answer); // the console's ban leaves logging to the console
        return {};
    }
    // Chat as the server passes it on: players' lines, the server's own to everyone, and the
    // /commands it does not know itself (Host::chat_since).
    std::vector<ChatLine> chat_since(std::uint64_t sequence) override {
        std::vector<ChatLine> out;
        for (auto &line : host_.chat_since(sequence)) out.push_back({line.sequence, line.sender, std::move(line.name), std::move(line.text), false, line.server});
        return out;
    }
    std::string world_map() override { return host_.map_name(); }
    std::uint64_t world_id() override { return host_.world_id(); }
    // The maps the server knows, which travel.load switches to (as the console's map does).
    std::vector<LevelInfo> levels() override {
        std::vector<LevelInfo> out;
        for (const auto &level : server::levels()) out.push_back({level.asset, level.name, true});
        return out;
    }
    std::string travel(std::string_view kind, std::string_view target) override {
        if (kind != "level") return "a dedicated server has no fast travel: load one of its maps (travel.load)";
        return host_.plugin_map(target);
    }
    // The time of day for everyone, as the console's tod sets it; the server has no other world.
    std::string set_environment(std::string_view kind, std::string_view key, std::string_view) override {
        if (kind != "tod") return unavailable;
        return host_.plugin_time_of_day(key);
    }
    std::string unban(std::uint64_t player) override {
        const auto answer = host_.command("unban " + std::to_string(player));
        if (host_.is_banned(player)) return answer;
        log_("[Plugins] " + answer);
        return {};
    }

  private:
    // Read again at most every 2 s: a plugin may list them every tick.
    const std::vector<content::Mod> &mod_folders() {
        const auto now = std::chrono::steady_clock::now();
        if (!mods_read_ || now - *mods_read_ >= std::chrono::seconds(2)) {
            mods_read_ = now;
            mods_ = content::mod_folders(host_.config().mods);
        }
        return mods_;
    }
    bool present(std::uint64_t player) const {
        for (const auto &p : host_.player_list())
            if (p.id == player) return true;
        return false;
    }
    Host &host_;
    ServerPlugins::Log log_;
    std::vector<content::Mod> mods_;
    std::optional<std::chrono::steady_clock::time_point> mods_read_;
};

class ServerEnvironment final : public Environment {
  public:
    ServerEnvironment(Host &host, std::filesystem::path storage, ServerPlugins::Log log)
        : host_(host), storage_(std::move(storage)), log_(log), game_(host, log) {}
    void log(LogLevel level, std::string_view text) override {
        log_(std::string(level == LogLevel::error ? "[Plugins] error: " : level == LogLevel::warn ? "[Plugins] warning: " : "[Plugins] ") +
             std::string(text));
    }
    void console(std::string_view text) override { log_(std::string(text)); }
    double now_ms() override {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    std::filesystem::path storage_root() override { return storage_; }
    // A plugin's command runs only for a line the server does not know, so none can hide one.
    bool builtin_command(std::string_view word) override { return word == "plugin"; }
    // The players on the server; the server is none of them (it is the authority).
    SessionState session() override {
        SessionState state{true, true, {}};
        for (const auto &p : host_.player_list()) state.players.push_back({p.id, p.name, false, false});
        return state;
    }
    bool send_message(std::vector<std::uint8_t> bytes) override {
        if (outbox.size() >= 256) return false;
        outbox.push_back(std::move(bytes));
        return true;
    }
    std::vector<Received> take_messages() override { return std::exchange(inbox, {}); }
    Game &game() override { return game_; }

    std::vector<Received> inbox;
    std::vector<std::vector<std::uint8_t>> outbox;

  private:
    Host &host_;
    std::filesystem::path storage_;
    ServerPlugins::Log log_;
    ServerGame game_;
};
} // namespace

struct ServerPlugins::State {
    State(Host &host, std::filesystem::path folder, std::filesystem::path storage, Log log)
        : folder(std::move(folder)), environment(host, std::move(storage), log), plugins(environment, Side::server), log(std::move(log)),
          server(host) {}
    std::filesystem::path folder;
    ServerEnvironment environment;
    plugins::Host plugins;
    Log log;
    Host &server;
    std::uint64_t next_scan{}, last_tick{};
    std::map<std::string, std::string, std::less<>> checked; // a fingerprint's file check
    std::vector<Candidate> found;
    bool announced{};

    void scan() {
        std::vector<Candidate> latest;
        std::error_code code;
        if (std::filesystem::is_directory(folder, code))
            for (std::filesystem::directory_iterator it(folder, std::filesystem::directory_options::skip_permission_denied, code), end;
                 !code && it != end; it.increment(code)) {
                std::error_code entry;
                if (!it->is_directory(entry) || !std::filesystem::is_regular_file(it->path() / manifest_file, entry)) continue;
                Candidate candidate{text_of(it->path().filename()), it->path(), fingerprint_of(it->path()), {}};
                auto known = checked.find(candidate.fingerprint);
                if (known == checked.end()) known = checked.emplace(candidate.fingerprint, check_package_files(it->path())).first;
                candidate.refusal = known->second;
                latest.push_back(std::move(candidate));
            }
        std::sort(latest.begin(), latest.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
        if (latest == found) return;
        found = std::move(latest);
        if (!announced && !found.empty()) {
            announced = true;
            log("[Plugins] " + std::to_string(found.size()) + " server plugin folder(s) in " + text_of(folder) + ".");
        }
        plugins.apply(found);
    }
};

ServerPlugins::ServerPlugins(Host &host, std::filesystem::path folder, std::filesystem::path storage, Log log)
    : state_(std::make_unique<State>(host, std::move(folder), std::move(storage), std::move(log))) {}
ServerPlugins::~ServerPlugins() {
    try { state_->plugins.shutdown(); } catch (...) {}
}

void ServerPlugins::tick(std::uint64_t now_us) {
    auto &s = *state_;
    try {
        if (now_us >= s.next_scan) {
            s.next_scan = now_us + 2'000'000;
            s.scan();
        }
        const double dt = s.last_tick ? static_cast<double>(now_us - s.last_tick) / 1000.0 : 0;
        s.last_tick = now_us;
        // The map the server runs, so plugins hear level_unloading and level_loaded when it
        // changes, as a game's plugins do (when the server changes it; players load it after).
        s.plugins.set_level(s.server.map_level());
        s.plugins.tick(dt);
        for (auto &bytes : std::exchange(s.environment.outbox, {})) s.server.send_plugin_message(wire::to_of(bytes), bytes);
    } catch (const std::exception &error) {
        s.log(std::string("[Plugins] error: the plugin tick failed: ") + error.what());
    }
}

bool ServerPlugins::hears_chat() const { return state_->plugins.wants_chat(); }

void ServerPlugins::receive(std::uint64_t from, const std::vector<std::uint8_t> &bytes) {
    auto &inbox = state_->environment.inbox;
    const auto to = wire::to_of(bytes);
    if ((to == 0 || to == wire::authority) && inbox.size() < 256) inbox.push_back({from, bytes});
}

std::optional<std::string> ServerPlugins::command(std::string_view line) {
    auto &s = *state_;
    std::string_view rest = line;
    const auto word = [&] {
        while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
        const auto end = rest.find(' ');
        const auto taken = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end);
        return taken;
    };
    if (word() != "plugin") {
        if (s.plugins.run_console(line)) return std::string("Ran by a plugin.");
        return std::nullopt;
    }
    const auto verb = word();
    const auto id = word();
    if (verb == "reload") return s.plugins.reload(id);
    if (verb == "off") return s.plugins.switch_off(id);
    if (verb == "on") return s.plugins.switch_on(id);
    if (verb == "commands") {
        std::string text;
        for (const auto &plugin : s.plugins.status())
            for (const auto &command : plugin.commands)
                text += (text.empty() ? "" : "\n") + std::format("{}{}{} - {} ({})", command.name, command.args.empty() ? "" : " ", command.args,
                                                                  command.help, plugin.id);
        return text.empty() ? std::string("No plugin has a console command.") : text;
    }
    if (!verb.empty() && verb != "list") return std::string("plugin list|commands|reload <id>|off <id>|on <id>");
    const auto all = s.plugins.status();
    if (all.empty())
        return "No server plugins. A plugin is a folder in " + text_of(s.folder) + " with a reskate-plugin.json (\"side\" \"server\" or \"both\").";
    std::string text;
    for (const auto &plugin : all) {
        if (!text.empty()) text += '\n';
        text += std::format("{}: {}", plugin.id, plugin.state);
        if (plugin.state == "running") text += std::format(", {:.1f} MB, {:.3f} ms a tick", plugin.memory / 1048576.0, plugin.tick_ms);
        else if (!plugin.reason.empty()) text += " (" + plugin.reason + ")";
    }
    return text;
}
} // namespace dingosdk::server
