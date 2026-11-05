#include "synchros_runtime.h"
#include "exchange.h"
#include "received.h"
#include "settings_file.h"
#include "wearer.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Vfs/mod_catalog.h"
#include "Engine/Vfs/synchros_closure.h"
#include "Engine/Vfs/synchros_outfit_mod.h"
#include "Extension/Assets/live_mods.h"
#include "Extension/Customization/item_origins.h"
#include "Extension/Multiplayer/Steam/steam_transport.h"

#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace dingosdk::multiplayer::synchros {
namespace fs = std::filesystem;
namespace {

void log(const std::string& line) {
    logging::log(logging::Level::info, logging::Channel::network, "{}", line);
}

fs::path game_root() { return mods::engine_data_root(); }
fs::path synchros_root() { return game_root() / "Mods" / dingosdk::synchros::synchros_folder; }
fs::path settings_path() { return synchros_root() / "settings.json"; }
fs::path outfit_mods_root() { return synchros_root() / "mods"; }

// The cutter over the enabled mods, read on a thread of its own the first time sharing needs it
// (about a second for fifty mods): until then every item is withheld as unreadable.
class LazyCloset final : public Closet {
public:
    std::optional<dingosdk::synchros::Cut> cut(std::string_view item) override {
        const auto index = ready();
        if (!index) return std::nullopt;
        std::lock_guard lock(use_);
        return index->cut(item);
    }
    std::vector<std::byte> read(const dingosdk::synchros::Piece& piece) override {
        const auto index = ready();
        if (!index) return {};
        std::lock_guard lock(use_);
        return index->read(piece);
    }
    void start() {
        if (started_.exchange(true)) return;
        std::thread([this] {
            try {
                std::vector<dingosdk::synchros::Source> sources;
                for (const auto& mod : mods::catalog().mods)
                    if (!mod.name.starts_with(dingosdk::synchros::outfit_mod_prefix) && mod.provides_layout)
                        sources.push_back({mod.directory, mod.name});
                auto index = std::make_shared<dingosdk::synchros::Index>(std::move(sources), game_root());
                log("Skate Synchros: " + std::to_string(index->items().size()) + " items of the enabled mods can be shared");
                std::lock_guard lock(hold_);
                index_ = std::move(index);
            } catch (const std::exception& failure) {
                log(std::string("Skate Synchros: the enabled mods could not be read for sharing: ") + failure.what());
            }
        }).detach();
    }

private:
    std::shared_ptr<dingosdk::synchros::Index> ready() {
        start();
        std::lock_guard lock(hold_);
        return index_;
    }
    std::atomic<bool> started_{};
    std::mutex hold_, use_;
    std::shared_ptr<dingosdk::synchros::Index> index_;
};

// The side connections of the session's transport.
class SideChannel final : public Channel {
public:
    SteamTransport* transport{};
    bool send(std::uint64_t peer, std::span<const std::uint8_t> bytes) override { return transport && transport->side_send(peer, bytes); }
    std::int64_t pending(std::uint64_t peer) const override { return transport ? transport->side_pending(peer) : -1; }
    void close(std::uint64_t peer) override { if (transport) transport->side_disconnect(peer); }
};

dingosdk::synchros::OutfitItem outfit_item(const ItemContents& item) {
    dingosdk::synchros::OutfitItem out{item.item, {}, {}};
    for (const auto& f : item.files) {
        dingosdk::synchros::OutfitFile o;
        o.kind = static_cast<dingosdk::synchros::OutfitFile::Kind>(f.kind);
        o.id = f.id;
        o.size = f.size;
        o.name = f.name;
        o.bundle = f.bundle;
        o.superbundle = f.superbundle;
        o.original_size = f.original_size;
        o.resource_type = f.resource_type;
        o.resource_id = f.resource_id;
        o.resource_meta = f.resource_meta;
        o.chunk_guid = f.chunk_guid;
        o.logical_offset = f.logical_offset;
        o.logical_size = f.logical_size;
        o.sha1 = f.sha1;
        o.chunk_record = f.chunk_record;
        out.files.push_back(std::move(o));
    }
    for (const auto& r : item.rows) out.rows.push_back({r.table, r.path, r.bundle, r.table_bundle, r.table_superbundle});
    return out;
}

struct State {
    std::mutex mutex;
    bool loaded{};
    Settings settings;
    std::unique_ptr<Cache> cache;
    LazyCloset closet;
    SideChannel channel;
    std::unique_ptr<Exchange> exchange;
    std::uint64_t session{};
    std::set<std::uint64_t> connected;
    std::map<std::uint64_t, std::uint64_t> revisions;   // each player's outfit revision, as last seen
    std::uint64_t own_revision{};
    std::map<std::uint64_t, std::uint64_t> dialled;      // when a side connection was last asked for
    std::set<std::string> wearing;
    std::map<std::uint64_t, std::set<std::string>> worn;
    // Outfits made into mods, waiting to be applied (live_mods::apply, without a reload: they show
    // from the next level load).
    std::atomic<int> writing{};
    std::atomic<bool> apply_wanted{};
    // An outfit arrived (not only one removed): with reload_on_arrival the map is loaded again.
    std::atomic<bool> arrived{};
    std::uint64_t last_ready{}, last_reload{};

    void load() {
        if (loaded) return;
        loaded = true;
        settings = load_settings(settings_path());
        cache = std::make_unique<Cache>(synchros_root() / "cache");
    }
    void save() {
        if (!save_settings(settings_path(), settings)) log("Skate Synchros: the settings could not be saved");
    }
};
State& state() {
    static State s;
    return s;
}

void write_outfit(std::uint64_t wearer) {
    auto& s = state();
    auto listing = s.cache->listing(wearer);
    ++s.writing;
    std::thread([wearer, listing = std::move(listing)] {
        auto& st = state();
        try {
            std::vector<dingosdk::synchros::OutfitItem> items;
            for (const auto& item : listing) items.push_back(outfit_item(item));
            const auto folder = dingosdk::synchros::next_outfit_folder(outfit_mods_root(), wearer);
            const auto made = dingosdk::synchros::write_outfit_mod(folder, wearer, game_root(), items,
                [&](const ContentId& id) { return st.cache->read(id); });
            if (made.written) {
                log("Skate Synchros: " + std::to_string(made.items.size()) + " of " + std::to_string(wearer) +
                    "'s items made into a mod; they show from the next level load");
                st.arrived = true;
                st.apply_wanted = true;
            } else {
                log("Skate Synchros: " + std::to_string(wearer) + "'s outfit could not be made into a mod: " + made.problem);
            }
        } catch (const std::exception& failure) {
            log(std::string("Skate Synchros: an outfit could not be made into a mod: ") + failure.what());
        }
        --st.writing;
    }).detach();
}

Game game_hooks() {
    Game g;
    g.worn_by = [](std::uint64_t peer) {
        auto& s = state();
        const auto found = s.worn.find(peer);
        return found == s.worn.end() ? std::set<std::string>{} : found->second;
    };
    g.can_show = [](std::string_view key) {
        std::string lowered(key);
        for (auto& c : lowered) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        const std::vector<std::string> keys{std::move(lowered)};
        const auto lookup = item_origins::lookup(keys);
        // Until the merge's item list is read, nothing is asked for.
        if (!lookup.ready || lookup.facts.empty()) return true;
        return lookup.facts.front().from_mod || lookup.facts.front().game;
    };
    g.wearing = [] { return state().wearing; };
    g.game_item = [](std::string_view key) { return item_origins::game_item(key); };
    g.outfit_ready = [](std::uint64_t wearer) { write_outfit(wearer); };
    g.note = [](const std::string& line) { log(line); };
    return g;
}

// With the state's lock held.
void stop_locked(State& s, SteamTransport& transport) {
    transport.close_side();
    s.exchange.reset();
    s.channel.transport = nullptr;
    s.session = 0;
    s.connected.clear();
    s.revisions.clear();
    s.worn.clear();
}

} // namespace

void tick(SteamTransport& transport, const SessionView& view) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.load();
    const bool wanted = view.playing && (s.settings.share || s.settings.show);
    if (!wanted) {
        if (s.exchange) stop_locked(s, transport);
        return;
    }
    if (!transport.open_side(steam_virtual_port)) return;
    s.channel.transport = &transport;
    if (s.settings.share) s.closet.start();
    if (!s.exchange || s.session != view.session) {
        s.exchange = std::make_unique<Exchange>(s.settings, *s.cache, s.closet, s.channel, game_hooks(), view.me);
        s.exchange->begin(view.session);
        s.session = view.session;
        s.connected.clear();
        s.revisions.clear();
        s.dialled.clear();
    }
    s.wearing = view.wearing;
    if (view.own_revision != s.own_revision) {
        s.own_revision = view.own_revision;
        s.exchange->own_outfit_changed();
    }
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    std::set<std::uint64_t> present;
    for (const auto& player : view.players) {
        present.insert(player.id);
        s.worn[player.id] = player.worn;
        // One side connection a pair: the lower SteamID dials, the other waits (both answer).
        if (s.settings.show && !s.connected.contains(player.id) && view.me < player.id &&
            now - s.dialled[player.id] > 15000) {
            s.dialled[player.id] = now;
            transport.side_connect(player.id);
        }
        const auto revision = s.revisions.find(player.id);
        if (revision == s.revisions.end() || revision->second != player.outfit_revision) {
            s.revisions[player.id] = player.outfit_revision;
            if (revision != s.revisions.end()) s.exchange->outfit_changed(player.id);
        }
    }
    std::erase_if(s.worn, [&](const auto& entry) { return !present.contains(entry.first); });
    for (const auto peer : transport.take_side_closed()) {
        s.connected.erase(peer);
        s.exchange->closed(peer);
    }
    for (const auto peer : transport.side_peers())
        if (s.connected.insert(peer).second) s.exchange->connected(peer);
    for (const auto& message : transport.side_receive()) s.exchange->received(message.peer, message.bytes);
    s.exchange->tick();
    if (s.exchange->take_settings_changed()) s.save();
    // Outfits are applied together: a few seconds after the last was made into a mod, and with a
    // reload at most once a minute, so a lobby full of arrivals is not a string of loading screens.
    if (s.arrived && !s.last_ready) s.last_ready = now;
    if (s.apply_wanted && !s.writing && !live_mods::busy() && (!s.last_ready || now - s.last_ready > 3000)) {
        const bool reload = s.arrived && s.settings.reload_on_arrival && (!s.last_reload || now - s.last_reload > 60000);
        s.apply_wanted = false;
        s.arrived = false;
        s.last_ready = 0;
        if (reload) s.last_reload = now;
        log("Skate Synchros: applying received outfits" + std::string(reload ? ", and loading the map again to show them: " : ": ") +
            live_mods::apply(reload));
    }
}

void stop(SteamTransport& transport) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    stop_locked(s, transport);
}

Settings settings() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.load();
    return s.settings;
}

std::string change(const std::function<std::string(Settings&)>& change) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.load();
    auto said = change(s.settings);
    if (s.settings.share && s.settings.own_code.empty()) renew_code(s.settings);
    s.save();
    // Players already connected hear of it now, not at the next session.
    if (s.exchange) s.exchange->settings_changed();
    return said;
}

std::vector<std::string> report() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.load();
    std::vector<std::string> lines;
    lines.push_back(std::string("Share my outfit: ") + (s.settings.share ? "on" : "off") + ", show shared outfits: " +
                    (s.settings.show ? "on" : "off"));
    if (!s.exchange) return lines;
    for (const auto peer : s.connected) {
        const auto st = s.exchange->status(peer);
        lines.push_back(std::to_string(peer) + ": " + (st.sharing ? "you share with them" : "you do not share with them") + "; " +
                        (st.welcomed ? "they share with you, " + std::to_string(st.items) + " items, " +
                                           std::to_string(st.files_missing) + " files to come"
                                     : "they do not share with you") +
                        (st.refused.empty() ? "" : " (stopped: " + st.refused + ")"));
    }
    return lines;
}

std::vector<PlayerStatus> players() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    std::vector<PlayerStatus> out;
    if (!s.exchange) return out;
    std::set<std::uint64_t> ids(s.connected.begin(), s.connected.end());
    for (const auto& [id, worn] : s.worn) ids.insert(id);
    for (const auto id : ids) {
        const auto st = s.exchange->status(id);
        out.push_back({id, s.connected.contains(id), st.sharing, st.welcomed, st.items, st.files_missing, st.bytes_received, st.refused});
    }
    return out;
}

void block_player(std::uint64_t player) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.load();
    if (s.exchange) s.exchange->blocked(player);
    else {
        SessionGrants none;
        block(s.settings, none, player);
        s.cache->forget(player);
    }
    dingosdk::synchros::remove_outfit_folders(outfit_mods_root(), player);
    s.save();
    s.apply_wanted = true;
}

void empty_cache() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.load();
    s.cache->empty();
    std::error_code ec;
    fs::remove_all(outfit_mods_root(), ec);
    s.apply_wanted = true;
}

std::uint64_t cache_bytes() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.load();
    return s.cache->size();
}

namespace {
std::vector<std::string> words(std::string_view text) {
    std::vector<std::string> out;
    std::string word;
    for (const char c : text) {
        if (c == ' ' || c == '\t') {
            if (!word.empty()) out.push_back(std::move(word));
            word.clear();
        } else {
            word += c;
        }
    }
    if (!word.empty()) out.push_back(std::move(word));
    return out;
}
std::optional<std::uint64_t> steam_id(const std::string& text) {
    try {
        std::size_t used = 0;
        const auto id = std::stoull(text, &used);
        if (used == text.size() && id) return id;
    } catch (...) {}
    return std::nullopt;
}
std::string join_lines(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& line : lines) out += (out.empty() ? "" : "\n") + line;
    return out;
}
} // namespace

std::string console_command(std::string_view text) {
    const auto w = words(text);
    const auto verb = w.empty() ? std::string("status") : w[0];
    const auto arg = [&](std::size_t i) { return i < w.size() ? w[i] : std::string(); };
    const auto rest = [&](std::size_t from) {
        std::string out;
        for (auto i = from; i < w.size(); ++i) out += (out.empty() ? "" : " ") + w[i];
        return out;
    };
    const auto on_off = [&](bool Settings::*flag, const char* what) {
        const auto value = arg(1);
        if (value != "on" && value != "off") return std::string("Say on or off.");
        return change([&](Settings& s) {
            s.*flag = value == "on";
            return std::string(what) + " " + value + ".";
        });
    };
    if (verb == "status") return join_lines(report());
    if (verb == "share") return on_off(&Settings::share, "Share my outfit");
    if (verb == "show") return on_off(&Settings::show, "Show shared outfits");
    if (verb == "code") {
        const auto s = settings();
        if (s.own_code.empty()) return "No code yet: turn sharing on (synchros share on) to get one.";
        return "Your code: " + show_code(s.own_code, CodeKind::personal) + " (give it to whoever may see your outfit).";
    }
    if (verb == "new-code")
        return change([](Settings& s) {
            return "Your new code: " + renew_code(s) + ". Players already paired stay until you remove them.";
        });
    if (verb == "add")
        return change([&](Settings& s) {
            const auto why = enter_code(s, rest(1));
            return why.empty() ? std::string("Code added: you will see that player's outfit when you are in a session together.") : why;
        });
    if (verb == "remove")
        return change([&](Settings& s) {
            const auto symbols = read_code(rest(1), CodeKind::personal);
            const auto before = s.entered.size();
            if (symbols) std::erase(s.entered, *symbols);
            return before != s.entered.size() ? std::string("Code removed.") : std::string("You have no such code.");
        });
    if (verb == "codes") {
        const auto s = settings();
        std::vector<std::string> lines{std::to_string(s.entered.size()) + " codes entered:"};
        for (const auto& code : s.entered) lines.push_back("  " + show_code(code, CodeKind::personal));
        return join_lines(lines);
    }
    if (verb == "group") {
        const auto what = arg(1);
        if (what == "create")
            return change([&](Settings& s) {
                const auto shown = create_group(s, rest(2));
                return shown.empty() ? std::string("You are in as many groups as there can be.")
                                     : "Group made. Its code: " + shown + " (everyone who enters it sees everyone else's outfit).";
            });
        if (what == "join")
            return change([&](Settings& s) {
                const auto why = join_group(s, arg(2), rest(3));
                return why.empty() ? std::string("Joined the group.") : why;
            });
        const auto index = [&]() -> std::optional<std::size_t> {
            try {
                const auto n = std::stoul(arg(2));
                if (n >= 1) return n - 1;
            } catch (...) {}
            return std::nullopt;
        }();
        if (what == "leave" || what == "new-code") {
            if (!index) return "Say which group, by its number in synchros groups.";
            return change([&](Settings& s) {
                if (*index >= s.groups.size()) return std::string("There is no such group.");
                if (what == "leave") {
                    leave_group(s, *index);
                    return std::string("Left the group.");
                }
                const auto shown = renew_group_code(s, *index);
                return shown.empty() ? std::string("Only the PC that made a group can give it a new code.")
                                     : "The group's new code: " + shown + ". Give it to everyone still meant to be in it.";
            });
        }
        return "synchros group create <name> | join <code> [name] | leave <n> | new-code <n>";
    }
    if (verb == "groups") {
        const auto s = settings();
        std::vector<std::string> lines{std::to_string(s.groups.size()) + " groups:"};
        for (std::size_t i = 0; i < s.groups.size(); ++i)
            lines.push_back("  " + std::to_string(i + 1) + ". " + s.groups[i].name + "  " + show_code(s.groups[i].code, CodeKind::group) +
                            (s.groups[i].created ? "  (made here)" : ""));
        return join_lines(lines);
    }
    if (verb == "paired") {
        const auto s = settings();
        std::vector<std::string> lines{std::to_string(s.paired.size()) + " players have your code:"};
        for (const auto id : s.paired) lines.push_back("  " + std::to_string(id));
        return join_lines(lines);
    }
    if (verb == "unpair" || verb == "unblock") {
        const auto id = steam_id(arg(1));
        if (!id) return "Say the player's SteamID64.";
        return change([&](Settings& s) {
            auto& list = verb == "unpair" ? s.paired : s.blocked;
            return list.erase(*id) ? std::string("Done.") : std::string("That player is not on the list.");
        });
    }
    if (verb == "block") {
        const auto id = steam_id(arg(1));
        if (!id) return "Say the player's SteamID64.";
        block_player(*id);
        return "Blocked: they neither see your outfit nor show theirs, and what you had of theirs is gone.";
    }
    if (verb == "cache") return "Received outfits take " + std::to_string(cache_bytes() / 1000000) + " MB.";
    if (verb == "empty-cache") {
        empty_cache();
        return "Emptied: shared outfits show as stand-ins again from the next level load.";
    }
    return "synchros status | share on|off | show on|off | code | new-code | add <code> | remove <code> | codes | "
           "group ... | groups | paired | unpair <id> | block <id> | unblock <id> | cache | empty-cache";
}

} // namespace dingosdk::multiplayer::synchros
