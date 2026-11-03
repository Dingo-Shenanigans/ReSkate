#include "peer_cosmetics_fetch.h"
#include "live_mods.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Vfs/https_download.h"
#include "Engine/Vfs/mod_catalog.h"
#include "Engine/Vfs/mod_list.h"
#include "Engine/Vfs/thunderstore_package.h"
#include "Extension/Customization/item_origins.h"
#include "Extension/Customization/worn_items.h"
#include "Extension/Multiplayer/Remote/outfit_filter.h"
#include "Launcher/mod_manager.h"
#include <Windows.h>
#include <winhttp.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <tuple>

namespace dingosdk::peer_cosmetics_fetch {
namespace {
namespace fs = std::filesystem;
namespace package = thunderstore_package;
namespace store = mods::peer_cosmetics;

constexpr wchar_t agent[] = L"ReSkate";
constexpr char community[] = "reskate";
// Nine in ten cosmetics packages are under 60 MB and almost all under 350 MB (Thunderstore,
// 2026-10-25); one bigger than this is not fetched for somebody else's outfit.
constexpr std::uint64_t largest_download = 1ull << 30;
constexpr std::uint32_t download_seconds = 30 * 60;
// How often what players wore is written down, and how stale a mod's date may be before it is.
constexpr ULONGLONG seen_interval_ms = 60 * 1000;
constexpr std::int64_t seen_resolution = 60 * 60;

fs::path mods_root() { return mods::engine_data_root() / mods::mods_folder; }
std::string lower(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
// The store is read, changed and written under this, by the menu, the fetch and the tick.
std::mutex store_mutex;

std::mutex status_mutex;
Status current;         // under status_mutex, but for the counters the watch keeps
https::Watch watch;
std::atomic<bool> busy{}, apply_wanted{}, stop_install{};

void say(std::string line, logging::Level level = logging::Level::info) {
    logging::log(level, logging::Channel::assets, "Fetched cosmetics: {}", line);
    std::lock_guard lock(status_mutex);
    current.line = std::move(line);
}

void work(package::Name name, std::string from) {
    struct Done { ~Done() { busy.store(false); } } done;
    const auto folder = name.folder();
    const auto root = mods_root();
    try {
        say("looking up " + folder + " on Thunderstore.");
        https::Download result;
        const auto answer = https::get_text(package::details_url(name), 512 * 1024, 20, agent, &result, &watch);
        if (!answer)
            return say(result.http_status == 404 ? folder + " is not on Thunderstore." : "Thunderstore could not be reached.", logging::Level::warning);
        const auto choice = package::choose(*answer, name, community, package::Kind::cosmetics);
        if (!choice.ok) return say(folder + ": " + choice.reason, logging::Level::warning);
        const auto url = package::download_url(name, choice.version);
        if (const auto length = https::content_length(url, 8, agent, &watch); length && *length > largest_download)
            return say(folder + " is " + std::to_string(*length >> 20) + " MB, more than is fetched for another player's outfit.", logging::Level::warning);

        wchar_t temp[MAX_PATH + 1]{};
        const auto length = GetTempPathW(MAX_PATH, temp);
        const auto archive = fs::path(length ? std::wstring(temp, length) : L".") /
            (L"ReSkate-cosmetics-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::wstring(folder.begin(), folder.end()) + L".zip");
        std::error_code error;
        fs::remove(archive, error);
        watch.received.store(0);
        watch.total.store(0);
        say("downloading " + folder + " " + choice.version + ".");
        const auto download = https::get(url, archive, largest_download, download_seconds, agent, &watch);
        if (!download.ok) {
            fs::remove(archive, error);
            return say(download.http_status && download.http_status != 200
                ? folder + ": the download failed (Thunderstore answered " + std::to_string(download.http_status) + ")."
                : folder + ": the download failed (Windows error " + std::to_string(download.error) + ").", logging::Level::warning);
        }
        say("installing " + folder + ".");
        try {
            launcher_mods::install(root, archive, false, {}, stop_install, {folder, name.owner, true});
        } catch (const std::exception& failure) {
            fs::remove(archive, error);
            return say(folder + " could not be installed: " + failure.what(), logging::Level::warning);
        }
        fs::remove(archive, error);
        // Cosmetics and nothing else: no plugin, script, map or settings comes in this way.
        if (!store::only_cosmetics(root / folder)) {
            fs::remove_all(root / folder, error);
            return say(folder + " holds more than cosmetics, so it is not kept. Install it from the launcher if you trust it.", logging::Level::warning);
        }
        // At the bottom of the load order: the player's own mods win whatever it shares with them.
        if (!mods::set_mod_enabled(root, folder, true)) {
            fs::remove_all(root / folder, error);
            return say("mods.json could not be updated, so " + folder + " was taken out again.", logging::Level::warning);
        }
        {
            std::lock_guard lock(store_mutex);
            auto s = store::read(root);
            const auto now = store::now_seconds();
            s.mods[folder] = {choice.version, from.substr(0, 64), now, now, false};
            if (!store::write(root, s)) say(std::string(store::store_file) + " could not be written; " + folder + " is kept like your own mods.", logging::Level::warning);
        }
        apply_wanted.store(true);
        say("fetched " + folder + " " + choice.version + (from.empty() ? "" : " for " + from) + "; its items show from the next level load.");
    } catch (const std::exception& failure) {
        say(folder + ": " + failure.what(), logging::Level::warning);
    }
}

// The menu's copy of the store, and of each folder's size, which takes a walk over its files.
struct Cache {
    View view;
    ULONGLONG read_at{}, sized_at{};
    bool valid{};
};
std::mutex cache_mutex;
Cache cache;
void forget_view() {
    std::lock_guard lock(cache_mutex);
    cache.valid = false;
}

template <class Change> bool change_store(Change&& change) {
    bool changed{};
    {
        std::lock_guard lock(store_mutex);
        auto s = store::read(mods_root());
        changed = change(s) && store::write(mods_root(), s);
    }
    forget_view();
    return changed;
}
} // namespace

std::string fetch(const std::string& text, const std::string& from) {
    const auto name = package::parse_name(text);
    if (!name) return "\"" + text + "\" is not a Thunderstore package name (Owner-Name or Owner-Name-1.2.3).";
    std::error_code error;
    if (fs::is_directory(mods_root() / name->folder(), error)) {
        std::lock_guard lock(store_mutex);
        return store::read(mods_root()).mods.contains(name->folder()) ? name->folder() + " was fetched already."
                                                                       : name->folder() + " is installed already, as one of your own mods.";
    }
    if (busy.exchange(true)) return "Another package is being fetched; try again when it is done.";
    {
        std::lock_guard lock(status_mutex);
        current = {true, name->folder(), 0, 0, "starting."};
    }
    watch.cancel.store(false);
    try {
        std::thread(work, *name, from).detach();
    } catch (...) {
        busy.store(false);
        return "The fetch could not be started.";
    }
    return "Fetching " + name->folder() + "; `outfits status` says how it goes.";
}

Status status() {
    std::lock_guard lock(status_mutex);
    auto out = current;
    out.busy = busy.load();
    out.received = watch.received.load();
    out.total = watch.total.load();
    return out;
}

View view() {
    const auto now = GetTickCount64();
    std::lock_guard lock(cache_mutex);
    if (cache.valid && now - cache.read_at < 2000) return cache.view;
    store::Store s;
    {
        std::lock_guard store_lock(store_mutex);
        s = store::read(mods_root());
    }
    // Sizes and titles take a walk over the folders: every 30 s, or for a mod not seen before.
    const bool rescan = !cache.valid || now - cache.sized_at >= 30000;
    std::map<std::string, std::pair<std::uint64_t, std::string>, std::less<>> known;
    for (const auto& mod : cache.view.mods) known.emplace(mod.folder, std::pair{mod.bytes, mod.title});
    std::vector<mods::ModEntry> listed;
    const auto title_of = [&](const std::string& folder) {
        if (listed.empty()) listed = mods::scan_mods(mods::engine_data_root()).entries;
        const auto found = std::ranges::find_if(listed, [&](const auto& e) { return lower(e.mod.name) == lower(folder); });
        return found != listed.end() ? found->mod.title : folder;
    };
    View out;
    out.policy = s.policy;
    for (const auto& [folder, entry] : s.mods) {
        Fetched mod{folder, folder, entry.version, entry.from, entry.fetched, entry.seen, 0, entry.remove};
        if (const auto found = known.find(folder); !rescan && found != known.end())
            std::tie(mod.bytes, mod.title) = found->second;
        else {
            mod.bytes = store::folder_size(mods_root() / folder);
            mod.title = title_of(folder);
        }
        out.bytes += mod.bytes;
        out.mods.push_back(std::move(mod));
    }
    std::ranges::stable_sort(out.mods, [](const auto& a, const auto& b) { return a.seen > b.seen; });
    cache.view = out;
    cache.read_at = now;
    if (rescan) cache.sized_at = now;
    cache.valid = true;
    return out;
}

bool set_policy(const store::Policy& policy) {
    return change_store([&](store::Store& s) { s.policy = policy; return true; });
}

bool keep(const std::string& folder) {
    const bool kept = change_store([&](store::Store& s) { return s.mods.erase(folder) > 0; });
    if (kept) logging::log(logging::Level::info, logging::Channel::assets, "Fetched cosmetics: {} is kept as one of your own mods.", folder);
    return kept;
}

bool remove_at_next_launch(const std::string& folder, bool remove) {
    return change_store([&](store::Store& s) {
        const auto found = s.mods.find(folder);
        if (found == s.mods.end()) return false;
        found->second.remove = remove;
        return true;
    });
}

void tick(bool loading) {
    if (loading) return;
    if (apply_wanted.load() && !live_mods::busy()) {
        apply_wanted.store(false);
        // No reload: in a session that would end it. The items are in the game from the next load.
        const auto result = live_mods::apply(false);
        logging::log(logging::Level::info, logging::Channel::assets, "Fetched cosmetics: live apply: {}", result);
    }
    static ULONGLONG next_seen{};
    const auto now = GetTickCount64();
    if (now < next_seen) return;
    next_seen = now + seen_interval_ms;
    auto seen = multiplayer::outfit_filter::take_seen_mods();
    // What the player wears themselves counts too: an item they picked is one they use.
    try {
        auto worn = worn_items::worn_outfit();
        for (auto& part : worn_items::worn_board(false)) worn.push_back(std::move(part));
        for (const auto& item : worn)
            if (auto folder = multiplayer::outfit_filter::facts(item.item).mod; !folder.empty()) seen.insert(std::move(folder));
    } catch (...) {
    }
    if (seen.empty()) return;
    bool written{};
    {
        std::lock_guard lock(store_mutex);
        auto s = store::read(mods_root());
        const auto stamp = store::now_seconds();
        bool changed{};
        for (auto& [folder, entry] : s.mods)
            if (seen.contains(lower(folder)) && stamp - entry.seen >= seen_resolution) {
                entry.seen = stamp;
                changed = true;
            }
        written = changed && store::write(mods_root(), s);
    }
    if (written) forget_view();
}
} // namespace dingosdk::peer_cosmetics_fetch
