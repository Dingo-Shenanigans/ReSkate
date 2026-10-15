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
// Keep the existing package size restriction; time is bounded by the session policy.
constexpr std::uint64_t largest_download = 8ull << 30;
namespace policy = multiplayer::map_fetch_policy;

std::mutex mutex;
std::atomic<Stage> stage{Stage::idle};
std::atomic<unsigned> workers{};
std::atomic<std::uint64_t> deadline{}, ask_until{}, transfer_until{};
multiplayer::MapNeed need;       // under mutex, while not idle
std::optional<multiplayer::MapNeed> pending; // a fresh attempt waiting for cancelled work to drain
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
std::string need_package() {
    std::lock_guard lock(mutex);
    return need.package;
}
bool need_server() {
    std::lock_guard lock(mutex);
    return need.server;
}

// The session that is waiting on the map is left: there is no map to wait for.
void leave_session(std::uint64_t expected = 0) {
    const auto owned = expected ? expected : multiplayer::map_fetch_deadline();
    if (multiplayer::fetching_map() && owned)
        multiplayer::queue_command("stop-map-fetch", std::to_string(owned), "");
}

// Back to idle with the reason said, as the plain "map not installed" notice is.
void fail(std::string reason, bool retry = false) {
    std::string text;
    bool update{};
    std::uint64_t failed_deadline{};
    {
        std::lock_guard lock(mutex);
        if (stage.exchange(Stage::idle) == Stage::idle) return;
        failed_deadline = deadline.load();
        watch.cancel.store(true);
        stop_install.store(true);
        apply_wanted.store(false);
        update = !need.have.empty();
        text = "The " + host_word(need.server) + (need.moved ? " changed to " : " is on ") + need.map +
               (update ? ", at another version (" + std::string(multiplayer::map_package_version(need.package)) + ") than the one installed on this PC (" + need.have + "). "
                       : ", which is not installed on this PC. ") + reason;
        if (!retry) last_failed = need.package;
    }
    if (multiplayer::map_fetch_deadline() == failed_deadline) leave_session(failed_deadline);
    logging::log(logging::Level::warning, logging::Channel::assets, "Map download: {}", text);
    overlay::notify(overlay::NoticeLevel::warning, update ? "Map version differs" : "Map not installed", std::move(text));
}

bool continuing() {
    if (stage.load() == Stage::idle) return false;
    if (policy::expired(deadline.load(), multiplayer::now_us())) {
        const auto now = stage.load();
        fail(now == Stage::installing || now == Stage::applying
            ? "The map could not be installed before the session timeout."
            : "Map download timed out. Please try joining the server again.", true);
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
// The packages of the maps installed here, by level (package_of): forgotten when one is installed.
struct Known {
    std::string package;
    std::uint64_t until{};
};
std::mutex known_mutex;
std::map<std::string, Known, std::less<>> known_packages;

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
    bool update{};
    {
        std::lock_guard lock(mutex);
        name = wanted;
        update = !need.have.empty();
    }
    try {
        std::string about;
        // (At another version than the host's, it is fetched whether it is on or off.)
        if (const auto on = update ? std::optional<bool>{} : installed(name.folder(), about)) {
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
            ask();
            return;
        }
        https::Download result;
        const auto answer = https::get_text(package::details_url(name), 512 * 1024, 20, agent, &result, &watch);
        if (!continuing()) return;
        if (!answer)
            return fail(result.http_status == 404 ? "Its mod is not on Thunderstore. Install its map mod yourself and join again."
                                                  : "Thunderstore could not be reached to look for it.");
        const auto choice = package::choose(*answer, name, community);
        if (!choice.ok) return fail(choice.reason + " Install its map mod yourself and join again.");
        // Another version is only worth fetching when it is the host's: any other leaves the two apart still.
        if (update && choice.version != multiplayer::map_package_version(need_package()))
            return fail("That version is not on Thunderstore any more. Ask the " + host_word(need_server()) + " to update the map, or install its version yourself.");
        {
            std::lock_guard lock(mutex);
            version = choice.version;
            newest = choice.newest;
            description = choice.description;
        }
        // Its icon for the card: small, and not worth holding the question up for long.
        if (!choice.icon.empty())
            if (const auto png = https::get_text(std::wstring(choice.icon.begin(), choice.icon.end()), 2 * 1024 * 1024, 8, agent, nullptr, &watch))
                overlay::set_map_download_icon(*png);
        ask();
    } catch (const std::exception& failure) {
        fail(std::string("Looking for it failed: ") + failure.what());
    }
}

// Stage::downloading and Stage::installing, then Stage::applying is the client thread's.
void fetch() {
    package::Name name;
    std::string get, fallback;
    bool off{}, update{};
    {
        std::lock_guard lock(mutex);
        name = wanted;
        get = version;
        fallback = newest;
        off = installed_off;
        update = !need.have.empty();
    }
    const auto root = mods_root();
    const auto folder = name.folder();
    try {
        if (!continuing()) return;
        if (!off) {
            wchar_t temp[MAX_PATH + 1]{};
            const auto length = GetTempPathW(MAX_PATH, temp);
            const auto archive = fs::path(length ? std::wstring(temp, length) : L".") /
                (L"ReSkate-map-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::wstring(folder.begin(), folder.end()) + L".zip");
            std::error_code error;
            fs::remove(archive, error);
            watch.received.store(0);
            watch.total.store(0);
            // One transfer budget across fallback requests; leave time for installation and joining.
            const auto transfer = transfer_until.load();
            const auto download = [&](const std::string& selected) {
                watch.deadline_us.store(transfer);
                return https::get(package::download_url(name, selected), archive, largest_download,
                    static_cast<std::uint32_t>(policy::transfer_us / policy::second_us), agent, &watch);
            };
            auto result = download(get);
            // The host's version can be gone from Thunderstore: the newest is the same map.
            if (!update && !result.ok && result.http_status == 404 && get != fallback && !watch.cancel.load()) {
                fs::remove(archive, error);
                logging::log(logging::Level::info, logging::Channel::assets,
                    "Map download: {} {} is not on Thunderstore any more; getting {}.", folder, get, fallback);
                result = download(fallback);
            }
            if (!result.ok) {
                fs::remove(archive, error);
                if (!continuing()) return;
                if (result.error == ERROR_TIMEOUT || result.error == ERROR_WINHTTP_TIMEOUT ||
                    policy::expired(transfer, multiplayer::now_us()))
                    return fail("Map download timed out. Please try joining the server again.", true);
                return fail(result.http_status && result.http_status != 200
                    ? "The download failed (Thunderstore answered " + std::to_string(result.http_status) + ")."
                    : "The download failed (Windows error " + std::to_string(result.error) + ").");
            }
            if (policy::expired(transfer, multiplayer::now_us())) {
                fs::remove(archive, error);
                return fail("Map download timed out. Please try joining the server again.", true);
            }
            if (!advance(Stage::downloading, Stage::installing)) {
                fs::remove(archive, error);
                return;
            }
            try {
                // (In the installed one's place when it is the same map at another version. The
                // game may hold the old one's files open when it was skated this run: then it
                // cannot be replaced until the game is closed.)
                launcher_mods::install(root, archive, update, [](float) { continuing(); }, stop_install, {folder, name.owner, true});
            } catch (const std::exception& failure) {
                fs::remove(archive, error);
                if (!continuing()) return;
                return fail(update ? std::string("It could not be replaced while the game runs (") + failure.what() +
                                         "). Close the game, update it from the launcher and join again."
                                   : std::string("It could not be installed: ") + failure.what());
            }
            fs::remove(archive, error);
            if (!continuing()) return;
            {
                std::lock_guard known(known_mutex);
                known_packages.clear();
            }
        } else {
            if (!advance(Stage::downloading, Stage::installing)) return;
        }
        if (!continuing()) return;
        // A map and nothing else: this way in is for maps.
        std::error_code error;
        if (!fs::exists(root / folder / L"reskate-levels.json", error) || !live_mods::only_maps(root / folder)) {
            if (!off) {
                try { launcher_mods::remove(root, folder); } catch (const std::exception&) {}
            }
            return fail("Its package holds more than a map, so it is not installed from here. Install it from the launcher if you trust it.");
        }
        if (off || update) { // (a map at another version may have been switched off: it is wanted now)
            if (!continuing()) return;
            auto list = mods::scan_mods(mods::engine_data_root());
            if (!list.issue.empty()) return fail("mods.json could not be read: " + list.issue);
            for (auto& entry : list.entries)
                if (same(entry.mod.name, folder)) entry.enabled = true;
            mods::save_mod_order(list.root, list.entries);
        }
        if (!continuing()) return;
        logging::log(logging::Level::info, logging::Channel::assets, "Map download: {} is in Mods; applying it.", folder);
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
    if (stage.load() != Stage::idle && asked.deadline_us && asked.deadline_us != deadline.load()) {
        lock.unlock();
        fail("Map download cancelled.", true);
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
    const auto name = package::parse_name(asked.package);
    // (Taken before anything is set up, so a second need while this one is in hand is dropped.)
    if (!stage.compare_exchange_strong(idle, Stage::checking)) {
        // Another map is in hand (the session moved on while it came). One at a time: the one
        // being fetched is finished, and the session, which now waits on this other one, is left.
        bool other{};
        other = asked.package != need.package;
        if (other) {
            overlay::notify(overlay::NoticeLevel::warning, "Map changed again",
                "The " + host_word(asked.server) + " moved on to " + asked.map + " while another map was downloading. Join again when it is done.");
            leave_session();
        }
        return;
    }
    {
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
    const auto now = multiplayer::now_us();
    deadline.store(asked.deadline_us ? asked.deadline_us : now + policy::session_us);
    ask_until.store(0);
    transfer_until.store(0);
    watch.deadline_us.store(deadline.load());
    stop_install.store(false);
    apply_wanted.store(false);
    apply_started.store(false);
    pick_download.store(true);
    try {
        start(check);
    } catch (...) {
        lock.unlock();
        fail("The map download could not be started. Please try joining again.", true);
    }
}

void answer(bool download) noexcept {
    try {
        // (A click and a key can answer in the same moment: only the first does.)
        if (!continuing()) return;
        const auto now = multiplayer::now_us();
        if (stage.load() == Stage::asking && policy::expired(ask_until.load(), now)) {
            fail("The map download was not confirmed within two minutes. Please try joining again.", true);
            return;
        }
        if (!download) {
            if (stage.load() == Stage::asking || stage.load() == Stage::downloading)
                fail("Map download cancelled.", true);
        } else {
            bool no_time{};
            {
                // Publish the stage and claim its worker together. Cancellation cannot allow
                // a fresh offer to reuse the globals before this thread starts its worker.
                std::lock_guard lock(mutex);
                auto asked = Stage::asking;
                if (!watch.cancel.load() && stage.compare_exchange_strong(asked, Stage::downloading)) {
                    transfer_until.store(policy::transfer_deadline(deadline.load(), now));
                    no_time = policy::expired(transfer_until.load(), now) && !installed_off;
                    if (!no_time) start(fetch);
                }
            }
            if (no_time) fail("Map download timed out. Please try joining the server again.", true);
        }
    } catch (...) {
        fail("The map download could not be started. Please try joining again.", true);
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
    out.have = need.have;
    out.choice = pick_download.load(std::memory_order_relaxed);
    out.received = watch.received.load();
    out.total = watch.total.load();
    out.yes_bind = yes_bind.load(std::memory_order_relaxed);
    out.no_bind = no_bind.load(std::memory_order_relaxed);
    return out;
}

void tick() {
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
        if (session.active && now != Stage::joining && multiplayer::map_fetch_deadline() != deadline.load())
            fail("Map download cancelled.", true);
        if (!session.active && session.status.find("stopped waiting for the map download") != std::string::npos)
            fail("The server stopped waiting for the map download.", true);
        if (continuing() && now == Stage::asking &&
            policy::expired(ask_until.load(), multiplayer::now_us()))
            fail("The map download was not confirmed within two minutes. Please try joining again.", true);
        if (continuing() && now == Stage::downloading &&
            policy::expired(transfer_until.load(), multiplayer::now_us()))
            fail("Map download timed out. Please try joining the server again.", true);
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
        join_at.store(multiplayer::now_us());
        advance(Stage::applying, Stage::joining);
        return;
    }
    if (now == Stage::joining) {
        const auto waited = multiplayer::now_us() - join_at.load();
        if (waited < 2500000) return;
        // Still waiting on it: the game has the mod and cannot find the map in it.
        const auto session = multiplayer::model();
        if (multiplayer::fetching_map() || (session.active && !session.connected)) {
            if (waited >= policy::installed_load_us) fail("It is installed, but the game still cannot load the map. Restart the game and join again.");
            return;
        }
        {
            // Should it not load after all, it is said and not offered round and round.
            std::lock_guard lock(mutex);
            last_failed.clear();
        }
        if (!continuing() || !stage.compare_exchange_strong(now, Stage::idle)) return;
        // The session ended while the map came (the host left, the connection dropped): back to it.
        if (!multiplayer::model().active && !multiplayer::queue_command("rejoin", "", ""))
            overlay::notify(overlay::NoticeLevel::warning, "Map installed", "Join the session again from the server browser.");
    }
}

std::string package_of(std::string_view asset) {
    auto& cache_mutex = known_mutex;
    auto& cache = known_packages;
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
