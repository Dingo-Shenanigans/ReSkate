#include "map_download.h"
#include "live_mods.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Vfs/https_download.h"
#include "Engine/Vfs/mod_catalog.h"
#include "Engine/Vfs/mod_list.h"
#include "Engine/Vfs/thunderstore_package.h"
#include "Extension/Multiplayer/Net/protocol.h"
#include "Extension/UI/Overlay/overlay.h"
#include "Launcher/mod_manager.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

namespace dingosdk::map_download {
namespace {
namespace fs = std::filesystem;
namespace package = thunderstore_package;

constexpr wchar_t agent[] = L"ReSkate";
constexpr char community[] = "reskate";
// A map is at most a few gigabytes (an archive of one holds four); the download may take as
// long as a slow line needs for that.
constexpr std::uint64_t largest_download = 8ull << 30;
constexpr std::uint32_t download_seconds = 4 * 60 * 60;

std::mutex mutex;
std::atomic<Stage> stage{Stage::idle};
multiplayer::MapNeed need;       // under mutex, while not idle
package::Name wanted;            // under mutex
std::string version;             // under mutex: the one to download
std::string newest;              // under mutex
std::string description;         // under mutex: Thunderstore's, or the installed mod's own
bool installed_off{};            // under mutex: here already, switched off
std::string last_failed;         // under mutex: a package not to offer again this run
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

// The session that is waiting on the map is left: there is no map to wait for.
void leave_session() {
    if (multiplayer::fetching_map()) multiplayer::queue_command("stop", "", "");
}

// Back to idle with the reason said, as the plain "map not installed" notice is.
void fail(std::string reason) {
    std::string text;
    {
        std::lock_guard lock(mutex);
        text = "The " + host_word(need.server) + (need.moved ? " changed to " : " is on ") + need.map +
               ", which is not installed on this PC. " + reason;
        last_failed = need.package;
    }
    stage.store(Stage::idle);
    leave_session();
    logging::log(logging::Level::warning, logging::Channel::assets, "Map download: {}", text);
    overlay::notify(overlay::NoticeLevel::warning, "Map not installed", std::move(text));
}

// The mod's row in mods.json when its folder is here: whether it is switched on, and what it
// says of itself.
std::optional<bool> installed(const std::string& folder, std::string& about) {
    const auto list = mods::scan_mods(mods::engine_data_root());
    for (const auto& entry : list.entries)
        if (same(entry.mod.name, folder)) {
            about = entry.mod.description;
            return entry.enabled;
        }
    return std::nullopt;
}

// Stage::checking: is it here but off, or what does Thunderstore say of it.
void check() {
    package::Name name;
    {
        std::lock_guard lock(mutex);
        name = wanted;
    }
    try {
        std::string about;
        if (const auto on = installed(name.folder(), about)) {
            if (*on) return fail("Its mod is installed, but the map did not load: update or reinstall it from the launcher.");
            {
                std::lock_guard lock(mutex);
                installed_off = true;
                description = about.substr(0, 240);
            }
            // Its own icon, from its folder.
            try {
                const auto bytes = read_bytes(mods_root() / name.folder() / L"icon.png", 4 * 1024 * 1024);
                overlay::set_map_download_icon(bytes);
            } catch (const std::exception&) {}
            stage.store(Stage::asking);
            return;
        }
        https::Download result;
        const auto answer = https::get_text(package::details_url(name), 512 * 1024, 20, agent, &result);
        if (!answer)
            return fail(result.http_status == 404 ? "Its mod is not on Thunderstore. Install its map mod yourself and join again."
                                                  : "Thunderstore could not be reached to look for it.");
        const auto choice = package::choose(*answer, name, community);
        if (!choice.ok) return fail(choice.reason + " Install its map mod yourself and join again.");
        {
            std::lock_guard lock(mutex);
            version = choice.version;
            newest = choice.newest;
            description = choice.description;
        }
        // Its icon for the card: small, and not worth holding the question up for long.
        if (!choice.icon.empty())
            if (const auto png = https::get_text(std::wstring(choice.icon.begin(), choice.icon.end()), 2 * 1024 * 1024, 8, agent))
                overlay::set_map_download_icon(*png);
        stage.store(Stage::asking);
    } catch (const std::exception& failure) {
        fail(std::string("Looking for it failed: ") + failure.what());
    }
}

// Stage::downloading and Stage::installing, then Stage::applying is the client thread's.
void fetch() {
    package::Name name;
    std::string get, fallback;
    bool off{};
    {
        std::lock_guard lock(mutex);
        name = wanted;
        get = version;
        fallback = newest;
        off = installed_off;
    }
    const auto root = mods_root();
    const auto folder = name.folder();
    try {
        if (!off) {
            wchar_t temp[MAX_PATH + 1]{};
            const auto length = GetTempPathW(MAX_PATH, temp);
            const auto archive = fs::path(length ? std::wstring(temp, length) : L".") /
                (L"ReSkate-map-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::wstring(folder.begin(), folder.end()) + L".zip");
            std::error_code error;
            fs::remove(archive, error);
            watch.received.store(0);
            watch.total.store(0);
            auto result = https::get(package::download_url(name, get), archive, largest_download, download_seconds, agent, &watch);
            // The host's version can be gone from Thunderstore: the newest is the same map.
            if (!result.ok && result.http_status == 404 && get != fallback && !watch.cancel.load()) {
                fs::remove(archive, error);
                logging::log(logging::Level::info, logging::Channel::assets,
                    "Map download: {} {} is not on Thunderstore any more; getting {}.", folder, get, fallback);
                result = https::get(package::download_url(name, fallback), archive, largest_download, download_seconds, agent, &watch);
            }
            if (!result.ok) {
                fs::remove(archive, error);
                if (watch.cancel.load()) { stage.store(Stage::idle); leave_session(); return; }
                return fail(result.http_status && result.http_status != 200
                    ? "The download failed (Thunderstore answered " + std::to_string(result.http_status) + ")."
                    : "The download failed (Windows error " + std::to_string(result.error) + ").");
            }
            stage.store(Stage::installing);
            try {
                launcher_mods::install(root, archive, false, [](float) {}, stop_install, {folder, name.owner, true});
            } catch (const std::exception& failure) {
                fs::remove(archive, error);
                return fail(std::string("It could not be installed: ") + failure.what());
            }
            fs::remove(archive, error);
        } else {
            stage.store(Stage::installing);
        }
        // A map and nothing else: this way in is for maps.
        std::error_code error;
        if (!fs::exists(root / folder / L"reskate-levels.json", error) || !live_mods::only_maps(root / folder)) {
            if (!off) {
                try { launcher_mods::remove(root, folder); } catch (const std::exception&) {}
            }
            return fail("Its package holds more than a map, so it is not installed from here. Install it from the launcher if you trust it.");
        }
        if (off) {
            auto list = mods::scan_mods(mods::engine_data_root());
            if (!list.issue.empty()) return fail("mods.json could not be read: " + list.issue);
            for (auto& entry : list.entries)
                if (same(entry.mod.name, folder)) entry.enabled = true;
            mods::save_mod_order(list.root, list.entries);
        }
        logging::log(logging::Level::info, logging::Channel::assets, "Map download: {} is in Mods; applying it.", folder);
        apply_started.store(false);
        apply_wanted.store(true);
        stage.store(Stage::applying);
    } catch (const std::exception& failure) {
        fail(std::string("It could not be installed: ") + failure.what());
    }
}

// One piece of work at a time, which the stage sees to: each is started by the move into its
// stage. Detached, as the live merge's thread is: the game may close while a map downloads.
void start(void (*work)()) { std::thread(work).detach(); }
} // namespace

void offer(const multiplayer::MapNeed& asked) {
    auto idle = Stage::idle;
    const auto name = package::parse_name(asked.package);
    // (Taken before anything is set up, so a second need while this one is in hand is dropped.)
    if (!stage.compare_exchange_strong(idle, Stage::checking)) {
        // Another map is in hand (the session moved on while it came). One at a time: the one
        // being fetched is finished, and the session, which now waits on this other one, is left.
        bool other{};
        {
            std::lock_guard lock(mutex);
            other = asked.package != need.package;
        }
        if (other) {
            overlay::notify(overlay::NoticeLevel::warning, "Map changed again",
                "The " + host_word(asked.server) + " moved on to " + asked.map + " while another map was downloading. Join again when it is done.");
            leave_session();
        }
        return;
    }
    {
        std::lock_guard lock(mutex);
        need = asked;
        if (!name || asked.package == last_failed) {
            // (Said once already, or nothing this can fetch: the plain notice.)
            overlay::notify(overlay::NoticeLevel::warning, "Map not installed",
                "The " + host_word(asked.server) + (asked.moved ? " changed to " : " is on ") + asked.map +
                ", which is not installed on this PC. Install its map mod and join again.");
            stage.store(Stage::idle);
            leave_session();
            return;
        }
        wanted = *name;
        version.clear();
        newest.clear();
        description.clear();
        installed_off = false;
    }
    overlay::set_map_download_icon({});
    watch.cancel.store(false);
    stop_install.store(false);
    pick_download.store(true);
    start(check);
}

void answer(bool download) noexcept {
    try {
        // (A click and a key can answer in the same moment: only the first does.)
        auto asked = Stage::asking;
        if (stage.compare_exchange_strong(asked, download ? Stage::downloading : Stage::idle)) {
            if (download) start(fetch);
            else leave_session();
        } else if (asked == Stage::downloading && !download) {
            watch.cancel.store(true);
        }
    } catch (...) {
        stage.store(Stage::idle);
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
    out.map = need.map;
    out.package = wanted.folder() + (version.empty() ? std::string() : " " + version);
    out.author = wanted.owner;
    out.version = version;
    out.description = description;
    if (out.stage == Stage::applying) {
        const auto now = live_mods::progress();
        out.step = now.done;
        out.steps = now.total;
        out.step_name = now.step;
    }
    out.server = need.server;
    out.moved = need.moved;
    out.installed = installed_off;
    out.choice = pick_download.load(std::memory_order_relaxed);
    out.received = watch.received.load();
    out.total = watch.total.load();
    out.yes_bind = yes_bind.load(std::memory_order_relaxed);
    out.no_bind = no_bind.load(std::memory_order_relaxed);
    return out;
}

void tick() {
    const auto now = stage.load();
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
        std::string folder;
        {
            std::lock_guard lock(mutex);
            folder = wanted.folder();
        }
        const auto applied = live_mods::applied_mods();
        if (std::none_of(applied.begin(), applied.end(), [&](const std::string& name) { return same(name, folder); }))
            return fail("It is installed, but was left out when the mods were applied (" + live_mods::status() + ").");
        // The level list takes the new map on the next ticks, and the session, which has been
        // trying the map all along, loads it then.
        join_at.store(GetTickCount64());
        stage.store(Stage::joining);
        return;
    }
    if (now == Stage::joining) {
        const auto waited = GetTickCount64() - join_at.load();
        if (waited < 2500) return;
        // Still waiting on it: the game has the mod and cannot find the map in it.
        if (multiplayer::fetching_map()) {
            if (waited > 30000) fail("It is installed, but the game still cannot load the map. Restart the game and join again.");
            return;
        }
        {
            // Should it not load after all, it is said and not offered round and round.
            std::lock_guard lock(mutex);
            last_failed = need.package;
        }
        stage.store(Stage::idle);
        // The session ended while the map came (the host left, the connection dropped): back to it.
        if (!multiplayer::model().active && !multiplayer::queue_command("rejoin", "", ""))
            overlay::notify(overlay::NoticeLevel::warning, "Map installed", "Join the session again from the server browser.");
    }
}

std::string package_of(std::string_view asset) {
    struct Known {
        std::string package;
        std::uint64_t until{};
    };
    static std::mutex cache_mutex;
    static std::map<std::string, Known, std::less<>> cache;
    if (asset.empty()) return {};
    std::string key(asset);
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto now = GetTickCount64();
    std::lock_guard lock(cache_mutex);
    if (const auto found = cache.find(key); found != cache.end() && now < found->second.until) return found->second.package;
    // Asked with every map offer a host sends: the Mods folder is read at most every two
    // minutes for a level.
    std::string result;
    const auto list = mods::scan_mods(mods::engine_data_root());
    for (const auto& entry : list.entries) {
        if (!entry.enabled || !entry.mod.provides_levels) continue;
        if (std::none_of(entry.mod.levels.begin(), entry.mod.levels.end(), [&](const std::string& level) { return same(level, key); })) continue;
        result = multiplayer::map_package_name(entry.mod.name, entry.mod.version);
        break;
    }
    cache[key] = {result, now + 120000};
    return result;
}
}
