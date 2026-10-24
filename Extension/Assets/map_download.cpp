#include "map_download.h"
#include "live_mods.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Vfs/https_download.h"
#include "Engine/Vfs/mod_catalog.h"
#include "Engine/Vfs/mod_list.h"
#include "Engine/Vfs/thunderstore_package.h"
#include "Extension/Multiplayer/Net/protocol.h"
#include "Extension/Multiplayer/Session/monotonic_clock.h"
#include "Extension/UI/Overlay/overlay.h"
#include "Launcher/mod_manager.h"
#include <Windows.h>
#include <winhttp.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace dingosdk::map_download {
namespace {
namespace fs = std::filesystem;
namespace package = thunderstore_package;

constexpr wchar_t agent[] = L"ReSkate";
constexpr char community[] = "reskate";
// A map is at most a few gigabytes (an archive of one holds four); the download may take as
// long as a slow line needs for that, whether or not the session is still waiting on it.
constexpr std::uint64_t largest_download = 8ull << 30;
constexpr std::uint32_t download_seconds = 4 * 60 * 60;
namespace policy = multiplayer::map_fetch_policy;

// One package the session needs: the map, or one of the plugins.
struct Item {
    package::Name name;     // the map's with the host's version; a plugin's without one
    bool plugin{};
    std::string at_least;   // a plugin: the version the server named, the least it takes (empty: any)
    std::string have;       // the version here, when the one fetched replaces it
    std::string folder;     // its folder in Mods: an installed one's when it is here, else the package's
    std::string version, newest, description;
    std::uint64_t bytes{};  // the zip's Content-Length (0: not known)
    bool off{};             // here already, switched off: nothing is downloaded
    bool merge{};           // once it is in: it has game data, which the live merge applies
};

std::mutex mutex;
std::atomic<Stage> stage{Stage::idle};
std::atomic<unsigned> workers{};
std::atomic<std::uint64_t> deadline{}, ask_until{};
// The session stopped waiting while the map was coming (its deadline passed, the server gave up
// on the player, the connection went): the map is fetched and installed all the same, and the
// player is told to join again.
std::atomic<bool> detached{};
multiplayer::MapNeed need;       // under mutex, while not idle
std::optional<multiplayer::MapNeed> pending; // a fresh attempt waiting for cancelled work to drain
std::vector<Item> items;         // under mutex: the map first when it is needed, then the plugins
std::atomic<std::size_t> current{}; // the item being fetched
std::string last_failed;         // under mutex: a need (need_key) not to offer again this run
https::Watch watch;
std::atomic<bool> stop_install{};
std::atomic<bool> apply_wanted{}, apply_started{};
std::atomic<std::uint64_t> join_at{};
std::atomic<std::uint32_t> yes_bind{}, no_bind{};
std::atomic<bool> pick_download{true};

bool same(std::string_view a, std::string_view b) { return a.size() == b.size() && _strnicmp(a.data(), b.data(), a.size()) == 0; }
fs::path mods_root() { return mods::engine_data_root() / mods::mods_folder; }
std::string read_bytes(const fs::path& path, std::uintmax_t most) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size > most) return {};
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::string host_word(bool server) { return server ? "server" : "host"; }
// What a need is, to know it again: its map and its plugins.
std::string need_key(const multiplayer::MapNeed& of) {
    auto key = of.package;
    for (const auto& plugin : of.plugins) key += '|' + plugin;
    return key;
}
// A plugin's name for a player: its package's name with spaces ("FPV Drone").
std::string plugin_label(std::string_view package) {
    const auto name = package::parse_name(package);
    auto label = name ? name->name : std::string(package);
    std::replace(label.begin(), label.end(), '_', ' ');
    return label;
}
std::string plugin_list(const std::vector<std::string>& packages) {
    std::string out;
    for (std::size_t at = 0; at < packages.size(); ++at)
        out += (at == 0 ? "" : at + 1 == packages.size() ? " and " : ", ") + plugin_label(packages[at]);
    return out;
}
std::string need_package() {
    std::lock_guard lock(mutex);
    return need.package;
}
bool need_server() {
    std::lock_guard lock(mutex);
    return need.server;
}

// The session that is waiting is left: what it waits for will not come.
void leave_session(std::uint64_t expected = 0) {
    const auto owned = expected ? expected : multiplayer::map_fetch_deadline();
    if (multiplayer::fetching_map() && owned)
        multiplayer::queue_command("stop-map-fetch", std::to_string(owned), "");
}

// What the session needs and this PC does not have, said for a player: "The server is on
// Vancouver Plaza, which is not installed on this PC." Under mutex.
std::string needed_text(const multiplayer::MapNeed& of) {
    const auto who = host_word(of.server);
    std::string text;
    if (!of.package.empty())
        text = "The " + who + (of.moved ? " changed to " : " is on ") + of.map +
               (!of.have.empty() ? ", at another version (" + std::string(multiplayer::map_package_version(of.package)) +
                                       ") than the one installed on this PC (" + of.have + ")"
                                 : ", which is not installed on this PC");
    if (!of.plugins.empty())
        text += (text.empty() ? "The " + who + " needs " : ", and needs ") + std::string(of.plugins.size() == 1 ? "the plugin " : "the plugins ") +
                plugin_list(of.plugins) + ", which this PC does not have ready";
    return text + ". ";
}
std::string needed_title(const multiplayer::MapNeed& of) {
    if (of.package.empty()) return "Plugins needed";
    return of.have.empty() ? "Map not installed" : "Map version differs";
}

// Back to idle with the reason said, as the plain "map not installed" notice is.
void fail(std::string reason, bool retry = false) {
    std::string text, title;
    std::uint64_t failed_deadline{};
    {
        std::lock_guard lock(mutex);
        if (stage.exchange(Stage::idle) == Stage::idle) return;
        failed_deadline = deadline.load();
        watch.cancel.store(true);
        stop_install.store(true);
        apply_wanted.store(false);
        text = needed_text(need) + reason;
        title = needed_title(need);
        if (!retry) last_failed = need_key(need);
    }
    if (multiplayer::map_fetch_deadline() == failed_deadline) leave_session(failed_deadline);
    logging::log(logging::Level::warning, logging::Channel::assets, "Map download: {}", text);
    overlay::notify(overlay::NoticeLevel::warning, std::move(title), std::move(text));
}

// Whether the download has begun (or is done): from then on it goes to the end on its own time.
bool under_way(Stage now) {
    return now == Stage::downloading || now == Stage::installing || now == Stage::applying || now == Stage::joining;
}

bool continuing() {
    const auto now = stage.load();
    if (now == Stage::idle) return false;
    // The session's deadline ends a check or a question it outlived; a download under way is
    // not thrown away with it (a large map on a slow line would never come at all).
    if (!under_way(now) && policy::expired(deadline.load(), multiplayer::now_us())) {
        fail("The download timed out. Please try joining the server again.", true);
        return false;
    }
    return !watch.cancel.load();
}

bool advance(Stage from, Stage to) {
    if (!continuing()) return false;
    return stage.compare_exchange_strong(from, to);
}

void ask() {
    ask_until.store(std::min(deadline.load(), multiplayer::now_us() + policy::confirmation_us));
    advance(Stage::checking, Stage::asking);
}
// The packages of the maps installed here, by level (lower case), and every plugin here, read
// from the Mods folder in one pass (package_of, has_level, missing_plugins). Forgotten when
// anything is installed; read again in the background two minutes after it was read. It was
// read for each level asked, on the client thread, with every map offer a host sends (a guest
// joining, a map change): tens of milliseconds, a second and more with many mods (2026-10-15).
struct KnownPlugin {
    std::string folder, title, author, version;
    bool enabled{};
};
struct KnownTable {
    std::map<std::string, std::string, std::less<>> by_level;
    std::vector<KnownPlugin> plugins;
};
struct KnownPackages {
    KnownTable table;
    std::uint64_t read_at{};
    bool valid{}, reading{};
};
std::mutex known_mutex;
KnownPackages known_packages;

KnownTable read_known_packages() {
    KnownTable table;
    const auto list = mods::scan_mods(mods::engine_data_root());
    for (const auto& entry : list.entries) {
        if (entry.mod.provides_plugin)
            table.plugins.push_back({entry.mod.name, entry.mod.title, entry.mod.author, entry.mod.version, entry.enabled});
        if (!entry.enabled || !entry.mod.provides_levels) continue;
        const auto package = multiplayer::map_package_name(entry.mod.name, entry.mod.version);
        for (auto level : entry.mod.levels) {
            std::transform(level.begin(), level.end(), level.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            table.by_level.emplace(std::move(level), package); // (the first mod that has a level is the one)
        }
    }
    return table;
}
// Reads the table on a thread of its own when there is none, or it is two minutes old. Under
// known_mutex.
void refresh_known_packages(std::uint64_t now) {
    if (known_packages.reading || (known_packages.valid && now - known_packages.read_at < 120000)) return;
    known_packages.reading = true;
    try {
        std::thread([] {
            KnownTable table;
            bool ok{};
            try { table = read_known_packages(); ok = true; } catch (...) {}
            std::lock_guard lock(known_mutex);
            if (ok) {
                known_packages.table = std::move(table);
                known_packages.read_at = GetTickCount64();
                known_packages.valid = true;
            }
            known_packages.reading = false;
        }).detach();
    } catch (...) { known_packages.reading = false; }
}
// The table, read here when there is none (only just after something was installed). Takes
// known_mutex; `use` runs under it.
template<class Use>
auto with_known(Use use) {
    const auto now = GetTickCount64();
    std::unique_lock lock(known_mutex);
    if (!known_packages.valid) {
        lock.unlock();
        auto table = read_known_packages();
        lock.lock();
        if (!known_packages.valid) {
            known_packages.table = std::move(table);
            known_packages.read_at = now;
            known_packages.valid = true;
        }
    }
    refresh_known_packages(now);
    return use(known_packages.table);
}
void forget_known() {
    std::lock_guard known(known_mutex);
    known_packages.valid = false;
}
// Whether a plugin here is the package: in the package's folder, or with its name and author.
bool is_package(const package::Name& name, std::string_view folder, std::string_view title, std::string_view author) {
    return same(folder, name.folder()) || (same(title, name.name) && same(author, name.owner));
}
// Whether `have` is at least `at_least` (any version is, when none is asked).
bool new_enough(std::string_view have, std::string_view at_least) {
    return at_least.empty() || (!have.empty() && !multiplayer::map_version_newer(at_least, have));
}

// The mod's row in mods.json when it is here: the map's by its package folder, a plugin's by
// is_package (an enabled one first). Its entry, and whether it was found.
std::optional<mods::ModEntry> installed(const Item& item) {
    const auto list = mods::scan_mods(mods::engine_data_root());
    std::optional<mods::ModEntry> found;
    for (const auto& entry : list.entries) {
        const bool match = item.plugin ? is_package(item.name, entry.mod.name, entry.mod.title, entry.mod.author)
                                       : same(entry.mod.name, item.name.folder());
        if (match && (!found || (entry.enabled && !found->enabled))) found = entry;
    }
    return found;
}

// Stage::checking for the map: is it here but off, or what does Thunderstore say of it. False
// when it failed (and fail() has said why).
bool check_map(Item& item, bool first) {
    const bool update = !item.have.empty();
    // (At another version than the host's, it is fetched whether it is on or off.)
    if (const auto here = update ? std::nullopt : installed(item)) {
        if (here->enabled) {
            fail("Its mod is installed, but the map did not load: update or reinstall it from the launcher.");
            return false;
        }
        item.off = true;
        item.description = here->mod.description.substr(0, 240);
        // Its own icon, from its folder.
        if (first)
            try {
                overlay::set_map_download_icon(read_bytes(mods_root() / item.folder / L"icon.png", 4 * 1024 * 1024));
            } catch (const std::exception&) {}
        return true;
    }
    https::Download result;
    const auto answer = https::get_text(package::details_url(item.name), 512 * 1024, 20, agent, &result, &watch);
    if (!continuing()) return false;
    if (!answer) {
        fail(result.http_status == 404 ? "Its mod is not on Thunderstore. Install its map mod yourself and join again."
                                       : "Thunderstore could not be reached to look for it.");
        return false;
    }
    const auto choice = package::choose(*answer, item.name, community, package::Kind::map);
    if (!choice.ok) {
        fail(choice.reason + " Install its map mod yourself and join again.");
        return false;
    }
    // Another version is only worth fetching when it is the host's: any other leaves the two apart still.
    if (update && choice.version != multiplayer::map_package_version(need_package())) {
        fail("That version is not on Thunderstore any more. Ask the " + host_word(need_server()) + " to update the map, or install its version yourself.");
        return false;
    }
    // The package page names the version and not its size. The download's Content-Length is
    // what the card shows; the body is not fetched until the player says yes.
    const auto length = https::content_length(package::download_url(item.name, choice.version), 8, agent, &watch);
    item.version = choice.version;
    item.newest = choice.newest;
    item.description = choice.description;
    item.bytes = length.value_or(0);
    // Its icon for the card: small, and not worth holding the question up for long.
    if (first && !choice.icon.empty())
        if (const auto png = https::get_text(std::wstring(choice.icon.begin(), choice.icon.end()), 2 * 1024 * 1024, 8, agent, nullptr, &watch))
            overlay::set_map_download_icon(*png);
    return true;
}

// Stage::checking for a plugin. False when it failed; `ready` when it turns out to be here
// after all (switched on since the session looked).
bool check_plugin(Item& item, bool first, bool& ready) {
    const auto label = plugin_label(item.name.folder());
    if (const auto here = installed(item)) {
        item.folder = here->mod.name;
        if (here->mod.provides_plugin && new_enough(here->mod.version, item.at_least)) {
            if (here->enabled) {
                ready = true;
                return true;
            }
            item.off = true;
            item.version = here->mod.version;
            item.description = here->mod.description.substr(0, 240);
            if (first)
                try {
                    overlay::set_map_download_icon(read_bytes(mods_root() / item.folder / L"icon.png", 4 * 1024 * 1024));
                } catch (const std::exception&) {}
            return true;
        }
        // Here at an older version (or not as a plugin): the new one goes in its place.
        item.have = here->mod.version.empty() ? std::string("an older version") : here->mod.version;
    }
    https::Download result;
    const auto answer = https::get_text(package::details_url(item.name), 512 * 1024, 20, agent, &result, &watch);
    if (!continuing()) return false;
    if (!answer) {
        fail(result.http_status == 404 ? label + " is not on Thunderstore. Install it yourself and join again."
                                       : "Thunderstore could not be reached to look for " + label + ".");
        return false;
    }
    const auto choice = package::choose(*answer, item.name, community, package::Kind::plugin);
    if (!choice.ok) {
        fail(label + ": " + choice.reason + " It is not installed from here.");
        return false;
    }
    // (The server names the least it takes; the newest is fetched, which is at least that.)
    if (!new_enough(choice.newest, item.at_least)) {
        fail("Thunderstore has version " + choice.newest + " of " + label + ", older than the " + item.at_least + " the " +
             host_word(need_server()) + " needs.");
        return false;
    }
    const auto length = https::content_length(package::download_url(item.name, choice.newest), 8, agent, &watch);
    item.version = choice.newest;
    item.newest = choice.newest;
    item.description = choice.description;
    item.bytes = length.value_or(0);
    if (first && !choice.icon.empty())
        if (const auto png = https::get_text(std::wstring(choice.icon.begin(), choice.icon.end()), 2 * 1024 * 1024, 8, agent, nullptr, &watch))
            overlay::set_map_download_icon(*png);
    return true;
}

// Stage::checking: what each package is and where it comes from.
void check() {
    std::vector<Item> list;
    {
        std::lock_guard lock(mutex);
        list = items;
    }
    try {
        std::vector<Item> wanted;
        for (auto& item : list) {
            if (!continuing()) return;
            const bool first = wanted.empty();
            bool ready{};
            if (!(item.plugin ? check_plugin(item, first, ready) : check_map(item, first))) return;
            if (!ready) wanted.push_back(std::move(item));
        }
        if (wanted.empty()) {
            // (Everything turned out to be here: the session sees so itself and goes on.)
            auto from = Stage::checking;
            stage.compare_exchange_strong(from, Stage::idle);
            return;
        }
        {
            std::lock_guard lock(mutex);
            items = std::move(wanted);
        }
        ask();
    } catch (const std::exception& failure) {
        fail(std::string("Looking for it failed: ") + failure.what());
    }
}

// Downloads one package into its folder in Mods (Stage::downloading, then Stage::installing).
// False when it failed or was stopped.
bool fetch_one(const Item& item) {
    const auto root = mods_root();
    const bool update = !item.have.empty();
    wchar_t temp[MAX_PATH + 1]{};
    const auto length = GetTempPathW(MAX_PATH, temp);
    const auto archive = fs::path(length ? std::wstring(temp, length) : L".") /
        (L"ReSkate-map-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::wstring(item.folder.begin(), item.folder.end()) + L".zip");
    std::error_code error;
    fs::remove(archive, error);
    watch.received.store(0);
    watch.total.store(0);
    // (The session's deadline is not the download's: only its own time limit stops it.)
    const auto download = [&](const std::string& selected) {
        watch.deadline_us.store(0);
        return https::get(package::download_url(item.name, selected), archive, largest_download, download_seconds, agent, &watch);
    };
    auto result = download(item.version);
    // The host's version of a map can be gone from Thunderstore: the newest is the same map.
    if (!item.plugin && !update && !result.ok && result.http_status == 404 && item.version != item.newest && !watch.cancel.load()) {
        fs::remove(archive, error);
        logging::log(logging::Level::info, logging::Channel::assets,
            "Map download: {} {} is not on Thunderstore any more; getting {}.", item.folder, item.version, item.newest);
        result = download(item.newest);
    }
    if (!result.ok) {
        fs::remove(archive, error);
        if (!continuing()) return false;
        if (result.error == ERROR_TIMEOUT || result.error == ERROR_WINHTTP_TIMEOUT) {
            fail("The download timed out. Please try joining the server again.", true);
            return false;
        }
        fail(result.http_status && result.http_status != 200
            ? "The download failed (Thunderstore answered " + std::to_string(result.http_status) + ")."
            : "The download failed (Windows error " + std::to_string(result.error) + ").");
        return false;
    }
    if (!advance(Stage::downloading, Stage::installing)) {
        fs::remove(archive, error);
        return false;
    }
    try {
        // (In the installed one's place when it is the same package at another version. The
        // game may hold the old one's files open when it was used this run: then it cannot be
        // replaced until the game is closed.)
        launcher_mods::install(root, archive, update, [](float) { continuing(); }, stop_install, {item.folder, item.name.owner, true});
    } catch (const std::exception& failure) {
        fs::remove(archive, error);
        if (!continuing()) return false;
        fail(update ? std::string("It could not be replaced while the game runs (") + failure.what() +
                          "). Close the game, update it from the launcher and join again."
                    : std::string("It could not be installed: ") + failure.what());
        return false;
    }
    fs::remove(archive, error);
    return continuing();
}

// Whether what is in the folder is what it was let in as: a map and nothing else, or a plugin
// (with what game data it brings, which is then merged). False when it is not, and it is taken
// out again unless it was here before.
bool vetted(Item& item) {
    const auto root = mods_root();
    const auto folder = root / item.folder;
    std::error_code error;
    if (!item.plugin) {
        if (fs::exists(folder / L"reskate-levels.json", error) && live_mods::only_maps(folder)) {
            item.merge = true;
            return true;
        }
        if (!item.off)
            try { launcher_mods::remove(root, item.folder); } catch (const std::exception&) {}
        fail("Its package holds more than a map, so it is not installed from here. Install it from the launcher if you trust it.");
        return false;
    }
    std::vector<std::string> notes;
    mods::Mod mod;
    try { mod = mods::describe_mod(folder, item.folder, notes); } catch (const std::exception&) {}
    if (mod.provides_plugin) {
        item.merge = mod.provides_layout || mod.provides_levels;
        return true;
    }
    if (!item.off && item.have.empty())
        try { launcher_mods::remove(root, item.folder); } catch (const std::exception&) {}
    fail(plugin_label(item.name.folder()) + " is not a plugin, so it is not installed from here. Install it from the launcher if you trust it.");
    return false;
}

// Stage::downloading through Stage::installing for every package in turn, then Stage::applying
// (the client thread's) when any has game data, else Stage::joining.
void fetch() {
    std::vector<Item> list;
    {
        std::lock_guard lock(mutex);
        list = items;
    }
    try {
        bool any_merge{}, any_switch{};
        for (std::size_t at = 0; at < list.size(); ++at) {
            auto& item = list[at];
            current.store(at);
            if (!continuing()) return;
            // (Each after the first is downloaded as the first was.)
            if (at && !advance(Stage::installing, Stage::downloading)) return;
            if (item.off) {
                if (!advance(Stage::downloading, Stage::installing)) return;
            } else if (!fetch_one(item)) {
                return;
            }
            if (!continuing()) return;
            if (!vetted(item)) return;
            any_merge |= item.merge;
            any_switch |= item.off || !item.have.empty();
        }
        forget_known();
        {
            std::lock_guard lock(mutex);
            items = list;
        }
        // One that was here switched off, or at another version, is wanted on now.
        if (any_switch) {
            if (!continuing()) return;
            auto mods_list = mods::scan_mods(mods::engine_data_root());
            if (!mods_list.issue.empty()) return fail("mods.json could not be read: " + mods_list.issue);
            for (auto& entry : mods_list.entries)
                for (const auto& item : list)
                    if ((item.off || !item.have.empty()) && same(entry.mod.name, item.folder)) entry.enabled = true;
            mods::save_mod_order(mods_list.root, mods_list.entries);
            forget_known();
        }
        if (!continuing()) return;
        if (!any_merge) {
            // Plugins alone: the plugin host takes them from the Mods folder itself.
            logging::log(logging::Level::info, logging::Channel::assets, "Map download: the plugins are in Mods.");
            join_at.store(multiplayer::now_us());
            advance(Stage::installing, Stage::joining);
            return;
        }
        logging::log(logging::Level::info, logging::Channel::assets, "Map download: {} is in Mods; applying it.", list.front().folder);
        apply_started.store(false);
        apply_wanted.store(true);
        if (!advance(Stage::installing, Stage::applying)) apply_wanted.store(false);
    } catch (const std::exception& failure) {
        fail(std::string("It could not be installed: ") + failure.what());
    }
}

// One piece of work at a time, which the stage sees to: each is started by the move into its
// stage. Detached, as the live merge's thread is: the game may close while a map downloads.
void start(void (*work)()) {
    ++workers;
    try {
        std::thread([work] {
            struct Finished { ~Finished() { --workers; } } finished;
            work();
        }).detach();
    } catch (...) {
        --workers;
        throw;
    }
}
} // namespace

void offer(const multiplayer::MapNeed& asked) {
    std::unique_lock lock(mutex);
    // (Another session's need cancels this attempt, unless its own session has stopped waiting
    // on it already: then it finishes, and the new need is turned away below.)
    if (stage.load() != Stage::idle && !detached.load() && asked.deadline_us && asked.deadline_us != deadline.load()) {
        lock.unlock();
        fail("Download cancelled.", true);
        lock.lock();
    }
    // A cancelled worker/atomic live merge must drain before its globals or files are reused.
    if (stage.load() == Stage::idle && (workers.load() || (apply_started.load() && live_mods::busy()))) {
        pending = asked;
        overlay::notify(overlay::NoticeLevel::warning, "Map download",
            "The previous map operation is still finishing. Waiting before starting this map.");
        return;
    }
    pending.reset();
    auto idle = Stage::idle;
    // (Taken before anything is set up, so a second need while this one is in hand is dropped.)
    if (!stage.compare_exchange_strong(idle, Stage::checking)) {
        // Another need is in hand (the session moved on while it came). One at a time: the one
        // being fetched is finished, and the session, which now waits on this other one, is left.
        if (need_key(asked) != need_key(need)) {
            overlay::notify(overlay::NoticeLevel::warning, "Map changed again",
                "The " + host_word(asked.server) + " moved on to " + asked.map + " while another map was downloading. Join again when it is done.");
            leave_session();
        }
        return;
    }
    {
        need = asked;
        std::vector<Item> list;
        bool fetchable = !asked.package.empty() || !asked.plugins.empty();
        if (!asked.package.empty()) {
            if (const auto name = package::parse_name(asked.package)) {
                Item map;
                map.name = *name;
                map.have = asked.have;
                map.folder = name->folder();
                list.push_back(std::move(map));
            } else {
                fetchable = false;
            }
        }
        for (const auto& plugin : asked.plugins) {
            auto name = package::parse_name(plugin);
            if (!name) {
                fetchable = false;
                continue;
            }
            Item item;
            item.plugin = true;
            item.at_least = std::exchange(name->version, {});
            item.name = std::move(*name);
            item.folder = item.name.folder();
            list.push_back(std::move(item));
        }
        if (!fetchable || need_key(asked) == last_failed) {
            // (Said once already, or nothing this can fetch: the plain notice.)
            overlay::notify(overlay::NoticeLevel::warning, needed_title(asked),
                needed_text(asked) + (asked.package.empty() ? "Install them and join again." : "Install its map mod and join again."));
            stage.store(Stage::idle);
            leave_session();
            return;
        }
        items = std::move(list);
        current.store(0);
    }
    overlay::set_map_download_icon({});
    watch.cancel.store(false);
    const auto now = multiplayer::now_us();
    deadline.store(asked.deadline_us ? asked.deadline_us : now + policy::session_us);
    ask_until.store(0);
    detached.store(false);
    watch.deadline_us.store(deadline.load());
    stop_install.store(false);
    apply_wanted.store(false);
    apply_started.store(false);
    pick_download.store(true);
    try {
        start(check);
    } catch (...) {
        lock.unlock();
        fail("The download could not be started. Please try joining again.", true);
    }
}

void answer(bool download) noexcept {
    try {
        // (A click and a key can answer in the same moment: only the first does.)
        if (!continuing()) return;
        const auto now = multiplayer::now_us();
        if (stage.load() == Stage::asking && policy::expired(ask_until.load(), now)) {
            fail("The download was not confirmed within two minutes. Please try joining again.", true);
            return;
        }
        if (!download) {
            if (stage.load() == Stage::asking || stage.load() == Stage::downloading)
                fail("Download cancelled.", true);
        } else {
            // Publish the stage and claim its worker together. Cancellation cannot allow
            // a fresh offer to reuse the globals before this thread starts its worker.
            std::lock_guard lock(mutex);
            auto asked = Stage::asking;
            if (!watch.cancel.load() && stage.compare_exchange_strong(asked, Stage::downloading)) start(fetch);
        }
    } catch (...) {
        fail("The download could not be started. Please try joining again.", true);
    }
}

bool asking() noexcept { return stage.load(std::memory_order_relaxed) == Stage::asking; }
void pick(bool download) noexcept { pick_download.store(download, std::memory_order_relaxed); }
bool picked() noexcept { return pick_download.load(std::memory_order_relaxed); }

void set_binds(std::uint32_t yes, std::uint32_t no) noexcept {
    yes_bind.store(yes, std::memory_order_relaxed);
    no_bind.store(no, std::memory_order_relaxed);
}

View view() {
    View out;
    out.stage = stage.load();
    if (out.stage == Stage::idle || out.stage == Stage::checking) return out;
    std::lock_guard lock(mutex);
    for (const auto& item : items) {
        View::Item shown;
        shown.name = item.plugin ? plugin_label(item.name.folder()) : need.map;
        shown.author = item.name.owner;
        shown.version = item.version;
        shown.description = item.description;
        shown.have = item.have;
        shown.plugin = item.plugin;
        shown.installed = item.off;
        shown.bytes = item.off ? 0 : item.bytes;
        out.bytes += shown.bytes;
        out.items.push_back(std::move(shown));
    }
    out.current = std::min(current.load(), out.items.empty() ? std::size_t{} : out.items.size() - 1);
    // The map's own facts, for the card that asks for a map alone.
    if (!items.empty() && !items.front().plugin) {
        const auto& map = items.front();
        out.map = need.map;
        out.package = map.name.folder() + (map.version.empty() ? std::string() : " " + map.version);
        out.author = map.name.owner;
        out.version = map.version;
        out.description = map.description;
        out.installed = map.off;
        out.have = need.have;
    }
    if (out.stage == Stage::applying) {
        const auto now = live_mods::progress();
        out.step = now.done;
        out.steps = now.total;
        out.step_name = now.step;
    }
    out.server = need.server;
    out.moved = need.moved;
    out.choice = pick_download.load(std::memory_order_relaxed);
    out.received = watch.received.load();
    out.total = watch.total.load();
    out.yes_bind = yes_bind.load(std::memory_order_relaxed);
    out.no_bind = no_bind.load(std::memory_order_relaxed);
    return out;
}

void tick() {
    warm_packages();
    std::optional<multiplayer::MapNeed> next;
    {
        std::lock_guard lock(mutex);
        if (pending && pending->deadline_us != multiplayer::map_fetch_deadline()) pending.reset();
        if (pending && stage.load() == Stage::idle && !workers.load() &&
            !(apply_started.load() && live_mods::busy()))
            next = std::exchange(pending, std::nullopt);
    }
    if (next) offer(*next);
    auto now = stage.load();
    if (now != Stage::idle) {
        const auto session = multiplayer::model();
        // Admission (or completed travel) retires the session deadline. Do not turn an
        // already successful join into a timeout while waiting out the UI's settling delay.
        if (now == Stage::joining && session.active && session.connected &&
            !multiplayer::fetching_map() && !multiplayer::map_fetch_deadline()) {
            std::lock_guard lock(mutex);
            if (stage.compare_exchange_strong(now, Stage::idle)) last_failed.clear();
            now = stage.load();
        }
        const bool gave_up = policy::expired(deadline.load(), multiplayer::now_us()) ||
            session.status.find("stopped waiting for the map download") != std::string::npos;
        // The session stopped waiting while the download was under way: it finishes all the same.
        if (!session.active && under_way(now) && !detached.exchange(true) && gave_up)
            overlay::notify(overlay::NoticeLevel::warning, need_package().empty() ? "Plugins still downloading" : "Map still downloading",
                "The " + [] { std::lock_guard lock(mutex); return host_word(need.server); }() +
                " stopped waiting for the download. It goes on and installs when it is done; join again then.");
        if (session.active && !detached.load() && now != Stage::joining && multiplayer::map_fetch_deadline() != deadline.load())
            fail("Download cancelled.", true);
        if (!session.active && !under_way(now) && gave_up)
            fail("The server stopped waiting for the download.", true);
        if (continuing() && now == Stage::asking &&
            policy::expired(ask_until.load(), multiplayer::now_us()))
            fail("The download was not confirmed within two minutes. Please try joining again.", true);
        now = stage.load();
    }
    // The session leaves the map alone until it has been applied (an update's level is one the
    // game already has, and would load from files that are being replaced).
    multiplayer::hold_map_load((now != Stage::idle && now != Stage::joining) ||
        workers.load() || (apply_started.load() && live_mods::busy()), need_package());
    if (now == Stage::applying) {
        if (apply_wanted.load()) {
            // (A merge of the player's own may be running: wait for it and start this one.)
            if (live_mods::busy()) return;
            apply_wanted.store(false);
            const auto said = live_mods::apply(false);
            if (!live_mods::busy()) return fail("It is installed, but could not be applied now: " + said + " Restart the game to load it.");
            apply_started.store(true);
            return;
        }
        if (!apply_started.load() || live_mods::busy()) return;
        apply_started.store(false);
        std::vector<std::string> merged;
        {
            std::lock_guard lock(mutex);
            for (const auto& item : items)
                if (item.merge) merged.push_back(item.folder);
        }
        const auto applied = live_mods::applied_mods();
        for (const auto& folder : merged)
            if (std::none_of(applied.begin(), applied.end(), [&](const std::string& name) { return same(name, folder); }))
                return fail(folder + " is installed, but was left out when the mods were applied (" + live_mods::status() + ").");
        // The level list takes a new map on the next ticks, and the session, which has been
        // trying the map all along, loads it then.
        join_at.store(multiplayer::now_us());
        advance(Stage::applying, Stage::joining);
        return;
    }
    if (now == Stage::joining) {
        const auto waited = multiplayer::now_us() - join_at.load();
        if (waited < 2500000) return;
        // Still waiting on it: the game has the mods and the session still cannot go on.
        const auto session = multiplayer::model();
        if (multiplayer::fetching_map() || (session.active && !session.connected)) {
            if (waited >= policy::installed_load_us)
                fail(need_package().empty() ? "They are installed, but the session still does not see them. Restart the game and join again."
                                            : "It is installed, but the game still cannot load the map. Restart the game and join again.");
            return;
        }
        {
            // Should it not load after all, it is said and not offered round and round.
            std::lock_guard lock(mutex);
            last_failed.clear();
        }
        if (!continuing() || !stage.compare_exchange_strong(now, Stage::idle)) return;
        // The session ended while it came (the host left, the connection dropped): back to it,
        // while it would still have the player. Past its deadline it would not.
        if (!multiplayer::model().active &&
            (policy::expired(deadline.load(), multiplayer::now_us()) || !multiplayer::queue_command("rejoin", "", "")))
            overlay::notify(overlay::NoticeLevel::info, need_package().empty() ? "Plugins installed" : "Map installed",
                            "Join the session again from the server browser.");
    }
}

void warm_packages() noexcept {
    try {
        std::lock_guard lock(known_mutex);
        if (!known_packages.valid) refresh_known_packages(GetTickCount64());
    } catch (...) {}
}

std::string package_of(std::string_view asset) {
    if (asset.empty()) return {};
    std::string key(asset);
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return with_known([&](const KnownTable& table) {
        const auto found = table.by_level.find(key);
        return found != table.by_level.end() ? found->second : std::string{};
    });
}

bool has_level(std::string_view asset) {
    if (asset.empty()) return false;
    std::string key(asset);
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return with_known([&](const KnownTable& table) { return table.by_level.contains(key); });
}

std::vector<std::string> missing_plugins(const std::vector<std::string>& required) {
    return with_known([&](const KnownTable& table) {
        std::vector<std::string> missing;
        for (const auto& wanted : required) {
            const auto name = package::parse_name(wanted);
            const bool ready = name && std::any_of(table.plugins.begin(), table.plugins.end(), [&](const KnownPlugin& plugin) {
                return plugin.enabled && is_package(*name, plugin.folder, plugin.title, plugin.author) && new_enough(plugin.version, name->version);
            });
            if (!ready) missing.push_back(wanted);
        }
        return missing;
    });
}
}
