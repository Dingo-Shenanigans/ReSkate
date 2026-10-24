#include "plugin_host.h"
#include <cctype>
#include "plugin_api.h"
#include "plugin_wire.h"
#include "Engine/Core/Console/console_core.h"
#include "Extension/Throwdowns/throwdown_kit.h"
#include <algorithm>
#include <format>
#include <utility>

namespace dingosdk::plugins {
namespace {
std::string lowercase(std::string text) {
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return text;
}
}

Host::Host(Environment& environment, Side side, Limits limits, std::string suffix)
    : environment_(environment), side_(side), limits_(limits), suffix_(std::move(suffix)) {}

Host::~Host() { shutdown(); }

Host::Entry* Host::find(std::string_view id) {
    // An owner's id, as the game was given it, names its entry too.
    if (!suffix_.empty() && id.ends_with(suffix_)) id.remove_suffix(suffix_.size());
    const auto found = entries_.find(id);
    return found == entries_.end() ? nullptr : &found->second;
}

void Host::load(Entry& entry) {
    entry.failure.clear();
    entry.plugin.reset();
    const auto& id = entry.candidate.id;
    if (!entry.candidate.refusal.empty()) {
        entry.failure = entry.candidate.refusal;
        environment_.log(LogLevel::warn, std::format("Plugin {} was not loaded: {}", id, entry.failure));
        return;
    }
    Manifest manifest;
    try {
        std::string error;
        const auto text = read_package_file(entry.candidate.directory / manifest_file, maximum_manifest_bytes, error);
        if (!error.empty()) throw std::runtime_error(std::string(manifest_file) + " " + error);
        manifest = parse_manifest(text);
    } catch (const std::exception& error) {
        entry.failure = error.what();
        environment_.log(LogLevel::warn, std::format("Plugin {} was not loaded: {}", id, entry.failure));
        return;
    }
    const bool here = manifest.side == Side::both || manifest.side == side_;
    if (!here) {
        entry.failure = std::format("it is a {} plugin", side_name(manifest.side));
        environment_.log(LogLevel::info, std::format("Plugin {} was not loaded here: {}", id, entry.failure));
        return;
    }
    std::string permissions;
    for (const auto& permission : manifest.permissions) permissions += (permissions.empty() ? "" : ", ") + permission;
    Services& services = *this;
    entry.plugin = std::make_unique<Plugin>(id + suffix_, entry.candidate.directory, std::move(manifest), services, limits_);
    // Its messages go under the name its manifest.json gives it, the same on every player's game
    // whatever its folder is called; the folder's name when it has none.
    std::string channel = id;
    try {
        std::string error;
        const auto text = read_package_file(entry.candidate.directory / "manifest.json", maximum_manifest_bytes, error);
        if (error.empty()) {
            const auto json = Json::parse(text, {maximum_manifest_bytes, 8, 4096});
            if (json.is_object() && json.contains("name") && json.at("name").is_string() && !json.at("name").string().empty())
                channel = json.at("name").string();
        }
    } catch (const std::exception&) {}
    entry.plugin->set_channel(channel);
    if (!entry.plugin->load()) {
        entry.failure = entry.plugin->reason();
        entry.plugin.reset();
        release_commands(id); // its own log says why it stopped
        return;
    }
    environment_.log(LogLevel::info, std::format("Plugin {} loaded ({}).", id, permissions.empty() ? "no permissions" : permissions));
}

void Host::unload(Entry& entry) {
    if (entry.plugin) {
        entry.plugin->unload();
        entry.plugin.reset();
    }
    release_commands(entry.candidate.id + suffix_);
    environment_.game().release(entry.candidate.id + suffix_);
    throwdowns::kit::release(entry.candidate.id + suffix_);
}

void Host::collect(Entry& entry) {
    if (!entry.plugin || entry.plugin->state() != State::stopped) return;
    entry.failure = entry.plugin->reason();
    entry.plugin->close_if_stopped();
    entry.plugin.reset();
    release_commands(entry.candidate.id + suffix_);
    environment_.game().release(entry.candidate.id + suffix_);
    throwdowns::kit::release(entry.candidate.id + suffix_);
}

void Host::release_commands(std::string_view id) {
    std::erase_if(commands_, [&](const auto& item) { return item.second == id; });
}

void Host::apply(const std::vector<Candidate>& found) {
    std::map<std::string, const Candidate*, std::less<>> wanted;
    for (const auto& candidate : found) wanted.emplace(candidate.id, &candidate);
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (wanted.contains(it->first)) {
            ++it;
            continue;
        }
        if (it->second.plugin) environment_.log(LogLevel::info, std::format("Plugin {} unloaded: its mod was switched off or removed.", it->first));
        unload(it->second);
        it = entries_.erase(it);
    }
    for (const auto& [id, candidate] : wanted) {
        auto* entry = find(id);
        if (!entry) {
            entry = &entries_.emplace(id, Entry{*candidate, {}, {}, false}).first->second;
            load(*entry);
            continue;
        }
        if (entry->candidate == *candidate) continue;
        entry->candidate = *candidate;
        if (entry->held) continue;
        environment_.log(LogLevel::info, std::format("Plugin {} reloading: its files changed.", id));
        unload(*entry);
        load(*entry);
    }
}

void Host::tick(double dt_ms) {
    const double now = environment_.now_ms();
    const bool save = now >= next_save_;
    if (save) next_save_ = now + 2000;
    follow_session();
    deliver_messages();
    deliver_events();
    deliver_http();
    follow_game();
    for (auto& [id, entry] : entries_) {
        if (!entry.plugin) continue;
        entry.plugin->tick(dt_ms);
        collect(entry);
        if (save && entry.plugin) entry.plugin->save_storage();
    }
    draw_and_build();
}

void Host::follow_session() {
    auto now = environment_.session();
    if (!now.active) now.players.clear();
    std::vector<SessionPlayer> joined, left;
    const auto has = [](const std::vector<SessionPlayer>& list, std::uint64_t id) {
        return std::any_of(list.begin(), list.end(), [&](const auto& p) { return p.id == id; });
    };
    for (const auto& player : now.players)
        if (!has(session_.players, player.id)) joined.push_back(player);
    for (const auto& player : session_.players)
        if (!has(now.players, player.id)) left.push_back(player);
    session_ = std::move(now);
    if (joined.empty() && left.empty()) return;
    for (auto& [id, entry] : entries_) {
        if (!entry.plugin) continue;
        entry.plugin->session_changed(joined, left);
        collect(entry);
    }
}

void Host::deliver_messages() {
    std::uint64_t me = 0;
    for (const auto& player : session_.players)
        if (player.me) me = player.id;
    const bool server = side_ == Side::server;
    for (const auto& received : environment_.take_messages()) {
        const auto message = wire::decode(received.bytes);
        // A server copy hears what is for everyone or for the authority; a client copy, what is
        // for everyone or for it, and the authority's words only from the authority (the session
        // checks who sent those).
        if (!message || (server ? message->to > wire::authority && message->to != me : message->to && message->to != me) ||
            (server && message->authority))
            continue;
        SessionPlayer from{received.from};
        for (const auto& player : session_.players)
            if (player.id == received.from) from = player;
        if (message->authority) {
            from.server = true;
            if (from.name.empty()) from.name = "Server";
        }
        for (auto& [id, entry] : entries_) {
            if (!entry.plugin || entry.plugin->channel() != message->channel) continue;
            entry.plugin->receive(from, message->payload);
            collect(entry);
        }
    }
}

Host::Entry* Host::running(std::string_view name) {
    const auto wanted = lowercase(std::string(name));
    for (auto& [id, entry] : entries_)
        if (entry.plugin && entry.plugin->state() == State::running &&
            (lowercase(id) == wanted || lowercase(entry.plugin->channel_name()) == wanted))
            return &entry;
    return nullptr;
}

std::string Host::call_plugin(std::string_view caller, std::string_view target, std::string_view function, const Json& arguments,
                              Json& result) {
    auto* entry = running(target);
    if (!entry) return std::format("there is no running plugin {}", target);
    if (entry->candidate.id + suffix_ == caller) return "a plugin calls its own functions directly";
    auto error = entry->plugin->call_exposed(std::string(function), arguments, result);
    // (It is closed if that stopped it; the plugin that called carries on.)
    if (entry->plugin->state() == State::stopped) collect(*entry);
    return error;
}

std::string Host::exposed(std::string_view target, std::vector<std::string>& names) {
    auto* entry = running(target);
    if (!entry) return std::format("there is no running plugin {}", target);
    names = entry->plugin->exposed_names();
    return {};
}

bool Host::emit(std::string_view source, std::string_view event, Json data) {
    if (events_.size() >= 1024) return false;
    events_.push_back({std::string(source), std::string(event), std::move(data)});
    return true;
}

void Host::follow_game() {
    auto& game = environment_.game();
    const auto wants = [&](std::string_view permission) {
        return std::any_of(entries_.begin(), entries_.end(),
            [&](const auto& item) { return item.second.plugin && item.second.plugin->has(permission); });
    };
    const auto each = [&](const auto& tell) {
        for (auto& [id, entry] : entries_) {
            if (!entry.plugin) continue;
            tell(*entry.plugin, id);
            collect(entry);
        }
    };
    if (wants("events.skater")) {
        const auto now = game.physics_state();
        const auto name = [](std::optional<std::uint32_t> s) { return s ? skater_state_name(*s) : "none"; };
        if (std::string_view(name(now)) != name(skater_state_)) {
            const auto previous = name(skater_state_);
            each([&](Plugin& plugin, const std::string&) { plugin.skater_changed(name(now), previous); });
        }
        skater_state_ = now;
        const auto wipeouts = game.wipeouts();
        if (wipeouts_known_ && wipeouts > wipeouts_) {
            const auto slam = game.last_slam();
            each([&](Plugin& plugin, const std::string&) { plugin.bailed(wipeouts, slam); });
        }
        wipeouts_ = wipeouts;
        wipeouts_known_ = true;
        auto news = game.take_skater_news();
        for (const auto& l : news.landings) {
            auto data = Json::object();
            data["down_speed"] = l.down_speed;
            data["speed"] = l.speed;
            data["onto"] = l.rail ? "rail" : "ground";
            each([&](Plugin& plugin, const std::string&) { plugin.game_event("board_landed", "events.skater", data); });
        }
        for (const auto& h : news.hits) {
            auto data = Json::object();
            data["bone"] = h.bone;
            data["impact"] = h.impact;
            data["vehicle"] = h.vehicle;
            data["board"] = h.board;
            each([&](Plugin& plugin, const std::string&) { plugin.game_event("body_hit", "events.skater", data); });
        }
        for (const auto& f : news.falls) {
            auto data = Json::object();
            data["seconds"] = f.seconds;
            data["left"] = f.left;
            data["right"] = f.right;
            auto bones = Json::object();
            for (const auto& b : f.bones) {
                auto bone = Json::object();
                bone["impact"] = b.impact;
                bone["slide"] = b.slide;
                bones[b.name] = std::move(bone);
            }
            data["bones"] = std::move(bones);
            each([&](Plugin& plugin, const std::string&) { plugin.game_event("bail_end", "events.skater", data); });
        }
    }
    if (wants("events.physics"))
        for (const auto& step : game.take_physics_steps()) {
            auto data = Json::object();
            data["at"] = step.at;
            data["seconds"] = step.seconds;
            data["wipeout"] = step.wipeout;
            data["airborne"] = step.airborne;
            if (step.ragdoll) data["ragdoll"] = *step.ragdoll;
            if (step.velocity) data["velocity"] = Json::array({(*step.velocity)[0], (*step.velocity)[1], (*step.velocity)[2]});
            if (step.spin) data["spin"] = *step.spin;
            auto bodies = Json::object();
            for (const auto& b : step.bodies) {
                auto body = Json::object();
                body["impact"] = b.impact;
                body["slide"] = b.slide;
                body["board"] = b.board;
                body["vehicle"] = b.vehicle;
                body["world"] = b.world;
                bodies[b.name] = std::move(body);
            }
            data["bodies"] = std::move(bodies);
            each([&](Plugin& plugin, const std::string&) { plugin.game_event("physics_step", "events.physics", data); });
        }
    if (wants("events.skater"))
        for (const auto& launch : game.take_launches()) each([&](Plugin& plugin, const std::string&) { plugin.launched(launch); });
    // The camera height button asking the plugin in its cycle to start or stop its camera.
    for (const auto& news : game.take_camera_cycle())
        each([&](Plugin& plugin, const std::string&) {
            if (plugin.id() == news.plugin) plugin.game_event("camera_cycle", "camera", Json(news.enter ? "enter" : "leave"));
        });
    // Touching (reskate.session.touch): the bodies read once for every plugin that has it on.
    {
        std::optional<std::vector<Body>> list;
        each([&](Plugin& plugin, const std::string&) {
            if (!plugin.touch_reach()) return;
            if (!list) list = game.bodies();
            plugin.touch_tick(*list);
        });
    }
    if (wants("chat.read")) {
        auto lines = game.chat_since(chat_seen_);
        // Lines from before the first plugin listened are not news.
        if (!chat_started_) {
            chat_started_ = true;
            for (const auto& line : lines) chat_seen_ = std::max(chat_seen_, line.sequence);
            lines.clear();
        }
        if (lines.size() > 20) lines.erase(lines.begin(), lines.end() - 20);
        for (const auto& line : lines) {
            chat_seen_ = std::max(chat_seen_, line.sequence);
            each([&](Plugin& plugin, const std::string&) { plugin.chat_line(line); });
        }
    }
    const double now = environment_.now_ms();
    if (wants("music") && now >= next_track_) {
        next_track_ = now + 1000;
        const auto track = game.track();
        if (track.available && track.revision != track_revision_) {
            track_revision_ = track.revision;
            each([&](Plugin& plugin, const std::string&) { plugin.track_changed(track); });
        }
    }
    // The trick feed (tricks.feed): what the trick list wrote, to plugins that read tricks.
    if (wants("events.tricks"))
        if (const auto notes = game.take_trick_notes(); !notes.empty()) {
            auto list = Json::array();
            for (const auto& n : notes) list.push_back(Json{{"entry", std::to_string(n.entry)}, {"kind", std::string(1, n.kind)}, {"text", n.text}});
            auto data = Json::object();
            data["notes"] = std::move(list);
            each([&](Plugin& plugin, const std::string&) { plugin.game_event("trick_notes", "events.tricks", data); });
        }
    if (wants("throwdowns")) {
        each([&](Plugin& plugin, const std::string&) {
            // What the native Throwdowns UI reports of the plugin's modes (throwdown_kit.h), each
            // mode by the plugin's own id.
            const auto prefix = plugin.id() + "/";
            for (auto& e : throwdowns::kit::take(plugin.id())) {
                auto data = Json::object();
                data["action"] = std::move(e.action);
                data["mode"] = e.mode.starts_with(prefix) ? e.mode.substr(prefix.size()) : std::move(e.mode);
                auto args = Json::array();
                for (auto& a : e.args) args.push_back(std::move(a));
                data["args"] = std::move(args);
                plugin.game_event("throwdown_ui", "throwdowns", data);
            }
        });
        const bool active = game.throwdowns().active;
        if (active != throwdown_active_) {
            throwdown_active_ = active;
            each([&](Plugin& plugin, const std::string&) { plugin.throwdown_phase(active ? "started" : "ended"); });
        }
    }
    // Answers to a server copy's polls (server.poll), and polls that closed unanswered, to the
    // plugin that asked.
    for (const auto& news : game.take_poll_news()) {
        auto data = Json::object();
        data["id"] = static_cast<double>(news.id);
        data["player"] = std::to_string(news.player);
        if (news.answer) {
            data["answer"] = static_cast<double>(news.answer);
            data["text"] = news.text;
        } else data["why"] = news.why;
        each([&](Plugin& plugin, const std::string&) {
            if (plugin.id() == news.plugin) plugin.game_event(news.answer ? "poll_answer" : "poll_closed", "server", data);
        });
    }
    for (const auto& [plugin, action] : game.take_presses())
        if (auto* entry = find(plugin); entry && entry->plugin) {
            entry->plugin->action(action);
            collect(*entry);
        }
}

void Host::deliver_http() {
    for (const auto& response : environment_.take_http_responses()) {
        auto* entry = find(response.plugin);
        if (!entry || !entry->plugin) continue; // unloaded since: its answer goes nowhere
        entry->plugin->http_done(response);
        collect(*entry);
    }
}

void Host::deliver_events() {
    const auto events = std::exchange(events_, {});
    for (const auto& emitted : events) {
        std::string channel = emitted.source;
        if (const auto* source = find(emitted.source); source && source->plugin) channel = source->plugin->channel_name();
        for (auto& [id, entry] : entries_) {
            if (!entry.plugin || id + suffix_ == emitted.source) continue;
            entry.plugin->plugin_event(emitted.source, channel, emitted.event, emitted.data);
            collect(entry);
        }
    }
}

bool Host::send(std::uint32_t channel, std::uint64_t to, std::string_view payload) {
    if (!session_.active) return false;
    if (side_ == Side::server && to == wire::authority) return false; // it is the authority
    return environment_.send_message(wire::encode(channel, to, payload, side_ == Side::server));
}

void Host::draw_and_build() {
    std::vector<plugin_ui::Layer> layers;
    for (auto& [id, entry] : entries_) {
        if (!entry.plugin) continue;
        entry.plugin->draw();
        collect(entry);
        if (entry.plugin) {
            auto shapes = entry.plugin->take_shapes();
            if (!shapes.empty()) layers.push_back({id, std::move(shapes)});
        }
    }
    environment_.publish_layers(std::move(layers));
    for (auto& action : environment_.take_actions()) {
        // Esc on a full-screen screen closes it at once; the plugin hears screen_closed.
        if (action.widget == plugin_ui::close_widget) {
            if (auto* entry = find(action.plugin); entry && entry->plugin) {
                entry->plugin->close_screen();
                collect(*entry);
            }
            continue;
        }
        if (actions_.size() < 256) actions_.push_back(std::move(action));
    }
    // Pages and screens are built ten times a second: a plugin's page (and its panels) only while the
    // menu shows it, a screen while its plugin has it open.
    const double now = environment_.now_ms();
    if (now < next_panels_) return;
    next_panels_ = now + 100;
    const bool panels_shown = environment_.panels_shown();
    const auto shown = environment_.shown_page();
    std::vector<plugin_ui::Panel> panels;
    std::vector<plugin_ui::PageEntry> pages;
    for (auto& [id, entry] : entries_)
        if (entry.plugin)
            if (auto name = entry.plugin->page_name(); !name.empty()) pages.push_back({id, std::move(name)});
    // By name, as the rail lists them, whatever their case.
    std::sort(pages.begin(), pages.end(), [](const auto& a, const auto& b) {
        return std::lexicographical_compare(a.title.begin(), a.title.end(), b.title.begin(), b.title.end(),
                                            [](unsigned char x, unsigned char y) { return std::tolower(x) < std::tolower(y); });
    });
    if (pages != pages_) {
        pages_ = pages;
        environment_.publish_pages(std::move(pages));
    }
    for (auto& [id, entry] : entries_) {
        if (!entry.plugin) continue;
        std::vector<plugin_ui::Action> mine;
        for (const auto& action : actions_)
            if (action.plugin == id) mine.push_back(action);
        if (auto screen = entry.plugin->build_screen(mine)) panels.push_back(std::move(*screen));
        collect(entry);
        if (!panels_shown || !entry.plugin || (!shown.empty() && shown != id)) continue;
        auto built = entry.plugin->build_panels(mine);
        collect(entry);
        for (auto& panel : built) panels.push_back(std::move(panel));
    }
    actions_.clear();
    environment_.publish_panels(std::move(panels));
}

void Host::set_level(std::string level) {
    if (level == level_) return;
    const auto previous = std::exchange(level_, std::move(level));
    for (auto& [id, entry] : entries_) {
        if (!entry.plugin) continue;
        entry.plugin->level_changed(previous, level_);
        collect(entry);
    }
}

void Host::trick(const TrickNews& news) {
    for (auto& [id, entry] : entries_) {
        if (!entry.plugin || !entry.plugin->has("events.tricks")) continue;
        entry.plugin->trick(news);
        collect(entry);
    }
}

bool Host::wants_tricks() const {
    return std::any_of(entries_.begin(), entries_.end(),
        [](const auto& item) { return item.second.plugin && item.second.plugin->has("events.tricks"); });
}

bool Host::wants_chat() const {
    return std::any_of(entries_.begin(), entries_.end(),
        [](const auto& item) { return item.second.plugin && item.second.plugin->has("chat.read"); });
}

bool Host::run_console(std::string_view text) {
    if (commands_.empty()) return false;
    const auto parsed = parse_console_command(text);
    if (!parsed || parsed.arguments.empty()) return false;
    const auto& words = parsed.arguments;
    // The longest command name the text starts with.
    for (std::size_t count = std::min<std::size_t>(3, words.size()); count > 0; --count) {
        std::string name;
        for (std::size_t i = 0; i < count; ++i) name += (i ? " " : "") + lowercase(words[i]);
        const auto found = commands_.find(name);
        if (found == commands_.end()) continue;
        auto* entry = find(found->second);
        if (!entry || !entry->plugin) return false;
        entry->plugin->run_command(name, {words.begin() + static_cast<std::ptrdiff_t>(count), words.end()});
        collect(*entry);
        return true;
    }
    return false;
}

std::vector<Status> Host::status() const {
    std::vector<Status> result;
    for (const auto& [id, entry] : entries_) {
        Status status{id};
        if (entry.plugin) {
            status.state = "running";
            status.memory = entry.plugin->memory();
            status.tick_ms = entry.plugin->tick_ms();
            status.commands = entry.plugin->commands();
        } else if (entry.held) {
            status.state = "switched off";
            status.reason = "plugin on " + id + suffix_ + " starts it again";
        } else {
            status.state = entry.failure.starts_with("it is a ") ? "not loaded" : "stopped";
            status.reason = entry.failure;
        }
        result.push_back(std::move(status));
    }
    return result;
}

std::string Host::reload(std::string_view id) {
    auto* entry = find(id);
    if (!entry) return std::format("error: There is no plugin {}.", id);
    entry->held = false;
    unload(*entry);
    load(*entry);
    return entry->plugin ? std::format("Plugin {} reloaded.", id) : std::format("error: Plugin {} did not start: {}", id, entry->failure);
}

std::string Host::switch_off(std::string_view id) {
    auto* entry = find(id);
    if (!entry) return std::format("error: There is no plugin {}.", id);
    entry->held = true;
    unload(*entry);
    return std::format("Plugin {} switched off until plugin on {}, or the next launch.", std::string(id) + suffix_, std::string(id) + suffix_);
}

std::string Host::switch_on(std::string_view id) {
    auto* entry = find(id);
    if (!entry) return std::format("error: There is no plugin {}.", id);
    if (entry->plugin) return std::format("Plugin {} is running.", id);
    entry->held = false;
    load(*entry);
    return entry->plugin ? std::format("Plugin {} started.", id) : std::format("error: Plugin {} did not start: {}", id, entry->failure);
}

void Host::shutdown() {
    for (auto& [id, entry] : entries_) unload(entry);
    entries_.clear();
}

void Host::log(std::string_view plugin, LogLevel level, std::string_view text) {
    environment_.log(level, std::format("[{}] {}", plugin, text));
}
void Host::console(std::string_view text) { environment_.console(text); }
double Host::now_ms() { return environment_.now_ms(); }
std::string Host::level() { return level_; }
std::filesystem::path Host::storage_file(std::string_view plugin) {
    return environment_.storage_root() / std::filesystem::path(std::string(plugin)) / "storage.json";
}

std::string Host::claim_command(std::string_view plugin, std::string_view name) {
    const auto first = name.substr(0, name.find(' '));
    if (first == "plugin" || first == "help" || environment_.builtin_command(first))
        return std::format("ReSkate has a console command \"{}\": pick another name", first);
    for (const auto& [taken, owner] : commands_) {
        if (owner == plugin) continue;
        const bool overlaps = taken == name || name.starts_with(taken + " ") || taken.starts_with(std::string(name) + " ");
        if (overlaps) return std::format("the plugin {} has the console command \"{}\"", owner, taken);
    }
    commands_.emplace(std::string(name), std::string(plugin));
    return {};
}
}
