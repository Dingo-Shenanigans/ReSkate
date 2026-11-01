#include "mod_store.h"
#include "map_download.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Vfs/https_download.h"
#include "Engine/Vfs/mod_catalog.h"
#include "Engine/Vfs/mod_conflicts.h"
#include "Engine/Vfs/mod_list.h"
#include "Engine/Vfs/thunderstore_package.h"
#include "Launcher/mod_manager.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <deque>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace dingosdk::mod_store {
namespace {
namespace fs = std::filesystem;
namespace package = thunderstore_package;

constexpr wchar_t agent[] = L"ReSkate";
constexpr std::uint64_t listing_most = 256ull << 20, index_most = 4ull << 20;
constexpr std::uint64_t stale_seconds = 10 * 60;
constexpr std::uint64_t largest_download = 8ull << 30;
constexpr std::uint32_t download_seconds = 4 * 60 * 60;

std::uint64_t unix_now() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
fs::path mods_root() { return mods::engine_data_root() / mods::mods_folder; }
fs::path local_folder() {
    std::array<wchar_t, 32768> local{};
    const auto size = GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), static_cast<DWORD>(local.size()));
    if (!size || size >= local.size()) return {};
    return fs::path(local.data()) / L"ReSkate" / L"thunderstore";
}
std::wstring wide(std::string_view ascii) { return std::wstring(ascii.begin(), ascii.end()); }
bool same(std::string_view a, std::string_view b) {
    return a.size() == b.size() && _strnicmp(a.data(), b.data(), a.size()) == 0;
}
void log(logging::Level level, const std::string& text) { logging::write(level, logging::Channel::assets, "Mod Manager: " + text); }

// ---------------------------------------------------------------- the listing

std::mutex listing_mutex;
std::shared_ptr<const Listing> current_listing = std::make_shared<Listing>(); // under the mutex
std::atomic<bool> fetching{};
std::atomic<std::uint64_t> next_fetch{};

fs::path listing_file() {
    const auto folder = local_folder();
    return folder.empty() ? fs::path() : folder / wide("listing-" + thunderstore::community() + ".json");
}
// One listing chunk (a JSON array) without its brackets, for joining chunks into one array.
std::string_view inner(std::string_view json) {
    while (!json.empty() && std::isspace(static_cast<unsigned char>(json.front()))) json.remove_prefix(1);
    while (!json.empty() && std::isspace(static_cast<unsigned char>(json.back()))) json.remove_suffix(1);
    if (json.size() < 2 || json.front() != '[' || json.back() != ']') return {};
    json = json.substr(1, json.size() - 2);
    while (!json.empty() && std::isspace(static_cast<unsigned char>(json.front()))) json.remove_prefix(1);
    return json;
}
void publish(std::vector<ts::Package> packages, std::uint64_t fetched, std::string error) {
    std::lock_guard lock(listing_mutex);
    auto next = std::make_shared<Listing>();
    if (packages.empty() && !current_listing->packages.empty()) {
        // A failed fetch keeps what was there.
        next->packages = current_listing->packages;
        next->fetched = current_listing->fetched;
    } else {
        next->packages = std::move(packages);
        next->fetched = fetched;
    }
    next->error = std::move(error);
    next->fetching = fetching.load();
    next->serial = current_listing->serial + 1;
    current_listing = std::move(next);
}
// The whole listing as one JSON array: the index's chunks, else the plain listing. Empty on failure.
std::string download_listing(std::string& error) {
    const auto community = thunderstore::community();
    https::Download result;
    if (const auto index = https::get_text(thunderstore::listing_index_url(community), index_most, 30, agent, &result)) {
        try {
            const auto urls = thunderstore::parse_index(thunderstore::gunzip(*index, index_most));
            std::string joined = "[";
            bool all = !urls.empty();
            for (const auto& url : urls) {
                const auto chunk = https::get_text(url, listing_most, 120, agent, &result);
                if (!chunk) { all = false; break; }
                const auto text = thunderstore::gunzip(*chunk, listing_most);
                const auto body = inner(text);
                if (body.empty()) continue;
                if (joined.size() > 1) joined += ',';
                joined += body;
            }
            if (all) return joined + "]";
        } catch (const std::exception&) {}
    }
    const auto plain = https::get_text(thunderstore::listing_url(community), listing_most, 180, agent, &result);
    if (!plain) {
        error = result.http_status && result.http_status != 200
            ? "Thunderstore answered " + std::to_string(result.http_status) + "."
            : "Thunderstore could not be reached (Windows error " + std::to_string(result.error) + ").";
        return {};
    }
    try { return thunderstore::gunzip(*plain, listing_most); } catch (const std::exception& failure) { error = failure.what(); }
    return {};
}
void fetch_listing(bool from_disk) {
    try {
        const auto file = listing_file();
        if (from_disk && !file.empty()) {
            std::error_code error;
            if (fs::is_regular_file(file, error)) {
                std::ifstream in(file, std::ios::binary);
                std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                auto packages = thunderstore::parse_listing(text);
                const auto written = fs::last_write_time(file, error);
                const auto age = error ? stale_seconds : static_cast<std::uint64_t>(std::max<long long>(0,
                    std::chrono::duration_cast<std::chrono::seconds>(fs::file_time_type::clock::now() - written).count()));
                const auto when = unix_now() - std::min(age, unix_now());
                if (!packages.empty()) {
                    publish(std::move(packages), when, {});
                    if (age < stale_seconds) {
                        next_fetch.store(when + stale_seconds);
                        fetching.store(false);
                        publish({}, 0, {});
                        return;
                    }
                }
            }
        }
        std::string error;
        const auto text = download_listing(error);
        auto packages = text.empty() ? std::vector<ts::Package>() : thunderstore::parse_listing(text);
        if (!text.empty() && packages.empty()) error = "Thunderstore's listing could not be read.";
        if (!packages.empty() && !file.empty()) {
            std::error_code ignored;
            fs::create_directories(file.parent_path(), ignored);
            const auto temporary = fs::path(file).concat(L".tmp");
            {
                std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                out.write(text.data(), static_cast<std::streamsize>(text.size()));
            }
            MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        log(packages.empty() ? logging::Level::warning : logging::Level::info,
            packages.empty() ? "the listing could not be fetched: " + error : std::to_string(packages.size()) + " packages listed.");
        const auto now = unix_now();
        // A failure is tried again in a minute.
        next_fetch.store(now + (packages.empty() ? 60 : stale_seconds));
        fetching.store(false);
        publish(std::move(packages), now, std::move(error));
    } catch (const std::exception& failure) {
        next_fetch.store(unix_now() + 60);
        fetching.store(false);
        publish({}, 0, failure.what());
    } catch (...) {
        next_fetch.store(unix_now() + 60);
        fetching.store(false);
        publish({}, 0, "the listing could not be read");
    }
}
void start_fetch(bool from_disk) {
    if (fetching.exchange(true)) return;
    publish({}, 0, {});
    try { std::thread([from_disk] { fetch_listing(from_disk); }).detach(); }
    catch (...) { fetching.store(false); }
}

// ---------------------------------------------------------------- the installed mods

std::mutex mods_mutex;
std::shared_ptr<const Mods> current_mods; // under the mutex
std::atomic<bool> scanning{};
std::atomic<std::uint64_t> next_scan{};   // GetTickCount64
std::atomic<unsigned> mods_serial{};
// mods.json is written by one change at a time.
std::mutex write_mutex;
// Mods in the game whose new version went in here since the mods were last applied: the game still
// has the old one merged until they are.
std::mutex updated_mutex;
std::set<std::string, std::less<>> updated_since_apply; // under the mutex
bool updated_since(const std::string& folder) {
    std::lock_guard lock(updated_mutex);
    return std::any_of(updated_since_apply.begin(), updated_since_apply.end(), [&](const std::string& name) { return same(name, folder); });
}

std::atomic<std::uint64_t> applied_at{}; // GetTickCount64 when apply() last started a merge
Mods read_mods() {
    Mods result;
    result.settled = !live_mods::busy() && GetTickCount64() > applied_at.load() + 100;
    const auto list = mods::scan_mods(mods::engine_data_root());
    result.present = list.present;
    result.issue = list.issue;
    const auto& launch = mods::catalog();
    const auto applied = live_mods::applied_mods();
    const auto waiting = launcher_mods::pending(list.root);
    result.waiting = waiting.updates.size() + waiting.removals.size();
    const auto named = [](const std::vector<std::string>& names, const std::string& name) {
        return std::any_of(names.begin(), names.end(), [&](const std::string& other) { return same(other, name); });
    };
    // ReSkate's own packs live outside the Mods folder's list, always on.
    const auto kept = [&](const std::string& name) {
        const auto owner = mods::variant_owner(name);
        return (owner.empty() ? name : owner).starts_with("reskate-builtin-");
    };
    // A mod with variants is in effect when one of its variants is (mod_catalog.h).
    const auto in_effect = [&](const std::string& name) {
        return std::any_of(applied.begin(), applied.end(), [&](const std::string& other) {
            return same(other, name) || same(mods::variant_owner(other), name);
        });
    };
    const auto excluded = [&](const std::string& name) -> const mods::Mod* {
        for (const auto& mod : launch.excluded)
            if (same(mod.name, name)) return &mod;
        return nullptr;
    };
    const auto aside = mods::set_aside_for_variants();
    const auto title_of = [&](const std::string& name) {
        for (const auto& entry : list.entries)
            if (same(entry.mod.name, name)) return entry.mod.title.empty() ? name : entry.mod.title;
        return name;
    };
    bool reload_for_update{};
    for (const auto& entry : list.entries) {
        const auto& mod = entry.mod;
        Mod row{mod.name, mod.title.empty() ? mod.name : mod.title, mod.author, mod.version, mod.description,
                mods::mod_contents(mod), entry.enabled, in_effect(mod.name)};
        result.versions[mod.name] = mod.version;
        const auto* left_out = excluded(mod.name);
        const bool set_aside = entry.enabled && !row.in_effect && named(aside, mod.name);
        if (named(waiting.removals, mod.name)) {
            row.state = State::removal_waiting;
            row.why = "It is uninstalled the next time the game starts: the game has its files open now.";
        } else if (named(waiting.updates, mod.name)) {
            row.state = State::update_waiting;
            row.why = "Its update goes in the next time the game starts: the game has its files open now.";
        } else if (left_out && !mods::conflict_winner(*left_out).empty()) {
            row.state = State::left_out;
            row.why = "It changes the same parts of the game as " + title_of(mods::conflict_winner(*left_out)) +
                      ", which is higher in the load order, so only that one loads. Change the order in the launcher.";
        } else if (left_out) {
            row.state = State::not_loaded;
            row.why = !left_out->outdated.empty() ? left_out->outdated
                      : "It could not be merged cleanly, so none of it is used. Reinstall it, or ask its author for an update.";
        } else if (set_aside) {
            row.state = State::set_aside;
            row.why = "It changes what another mod's chosen variant replaces, and only one of them can be in the game.";
        } else if (entry.enabled && row.in_effect && updated_since(mod.name) && (mod.provides_layout || mod.provides_levels)) {
            row.state = State::apply_to_load;
            row.why = "Its new version is installed: it goes in the game when the mods are applied.";
            reload_for_update |= !live_mods::only_maps(mod.directory);
        } else if (mods::plugin_only(mod)) {
            // (Nothing of it is merged: the plugin host takes it from the Mods folder by itself.)
            row.in_effect = entry.enabled;
            row.state = entry.enabled ? State::loaded : State::disabled;
        } else if (entry.enabled && row.in_effect) row.state = State::loaded;
        else if (entry.enabled) row.state = State::apply_to_load;
        else if (row.in_effect) row.state = State::apply_to_unload;
        else row.state = State::disabled;
        if (row.state == State::apply_to_load || row.state == State::apply_to_unload) ++result.changes;
        result.mods.push_back(std::move(row));
    }
    // In effect, but its folder has been deleted: applying unloads it.
    for (const auto& name : applied)
        if (mods::variant_owner(name).empty() && !kept(name) &&
            std::none_of(list.entries.begin(), list.entries.end(), [&](const mods::ModEntry& entry) { return same(entry.mod.name, name); })) {
            Mod row{name, name};
            row.in_effect = row.gone = true;
            row.state = State::apply_to_unload;
            row.why = "Its folder is gone, but it stays in the game until the mods are applied.";
            ++result.changes;
            result.mods.push_back(std::move(row));
        }
    if (result.changes) result.effect = live_mods::preview(list);
    // (preview only sees mods going in or out; a new version of one in the game is neither.)
    if (reload_for_update && result.effect == live_mods::ApplyEffect::maps_only) result.effect = live_mods::ApplyEffect::reload_level;
    return result;
}
void start_scan() {
    if (scanning.exchange(true)) return;
    next_scan.store(GetTickCount64() + 1500);
    try {
        std::thread([] {
            std::shared_ptr<const Mods> fresh;
            try {
                auto read = read_mods();
                read.serial = ++mods_serial;
                fresh = std::make_shared<const Mods>(std::move(read));
            } catch (...) {}
            if (fresh) {
                std::lock_guard lock(mods_mutex);
                current_mods = std::move(fresh);
            }
            scanning.store(false);
        }).detach();
    } catch (...) { scanning.store(false); }
}
// After a change made here: read again at once (or as soon as the read under way ends).
void rescan() { next_scan.store(0); }

// ---------------------------------------------------------------- the queue

std::mutex progress_mutex;
Progress current_progress; // under the mutex
std::atomic<bool> busy{}, stop{};
https::Watch watch;

void set_activity(std::string text, float fraction = -1) {
    std::lock_guard lock(progress_mutex);
    current_progress.activity = std::move(text);
    current_progress.fraction = fraction;
}
// The package in hand: what it is, and where the job is with it.
void set_package(Progress::Step step, const ts::Package* wanted, std::size_t at, std::size_t of, std::vector<std::string> queued = {}) {
    std::lock_guard lock(progress_mutex);
    current_progress.step = step;
    current_progress.at = at;
    current_progress.of = of;
    current_progress.received = current_progress.total = 0;
    if (wanted) {
        current_progress.package = wanted->full_name;
        current_progress.title = wanted->title();
        current_progress.version = wanted->latest().number;
        current_progress.icon = wanted->latest().icon;
        current_progress.queued = std::move(queued);
    }
}
void set_bytes(std::uint64_t received, std::uint64_t total) {
    std::lock_guard lock(progress_mutex);
    current_progress.received = received;
    current_progress.total = total;
}
// A job ended: what it did, for the page. The queue may go on with the next.
void job_done(std::string result, bool failed) {
    log(failed ? logging::Level::warning : logging::Level::info, result);
    {
        std::lock_guard lock(progress_mutex);
        current_progress.activity.clear();
        current_progress.fraction = -1;
        current_progress.step = Progress::Step::starting;
        current_progress.package.clear();
        current_progress.title.clear();
        current_progress.version.clear();
        current_progress.icon.clear();
        current_progress.queued.clear();
        current_progress.received = current_progress.total = 0;
        current_progress.result = std::move(result);
        current_progress.failed = failed;
        ++current_progress.serial;
    }
    rescan();
}

// The jobs waiting their turn, and whether the worker runs. A job is an install of some packages or an
// uninstall of a folder.
struct Job {
    std::vector<std::string> install;
    std::string uninstall;
};
std::mutex jobs_mutex;
std::deque<Job> jobs;   // under jobs_mutex
bool working{};         // under jobs_mutex: the worker thread runs

// Downloads and installs one package (its newest version). Returns what happened ("" when it went
// in now, "later" when it waits for the next start) or throws why not.
std::string install_one(const ts::Package& wanted, std::size_t at, std::size_t of, std::vector<std::string> queued) {
    const auto& version = wanted.latest();
    const auto root = mods_root();
    const auto counter = of > 1 ? " (" + std::to_string(at + 1) + " of " + std::to_string(of) + ")" : std::string();
    const auto archive_folder = root / L".reskate-download";
    std::error_code error;
    fs::create_directories(archive_folder, error);
    const auto archive = archive_folder / (wide(wanted.full_name) + L"-ingame.zip");
    struct Cleanup {
        fs::path archive, folder;
        ~Cleanup() { std::error_code ignored; fs::remove(archive, ignored); fs::remove(folder, ignored); }
    } cleanup{archive, archive_folder};
    fs::remove(archive, error);
    // Always from thunderstore.io, whatever the listing names (as the map download does).
    const package::Name name{wanted.owner, wanted.name, version.number};
    set_package(Progress::Step::downloading, &wanted, at, of, std::move(queued));
    set_activity("Downloading " + wanted.title() + " v" + version.number + counter, 0);
    watch.received.store(0);
    watch.total.store(version.file_size);
    https::Download result;
    {
        // How far it is, said a few times a second while the download runs.
        struct Meter {
            std::atomic<bool> done{};
            std::thread thread;
            ~Meter() { done.store(true); if (thread.joinable()) thread.join(); }
        } meter;
        meter.thread = std::thread([&] {
            while (!meter.done.load()) {
                const auto total = watch.total.load(), received = watch.received.load();
                set_bytes(received, total);
                if (total) set_activity("Downloading " + wanted.title() + " v" + version.number + counter + "  " + size_text(received) + " of " + size_text(total),
                                        static_cast<float>(static_cast<double>(received) / static_cast<double>(total)));
                Sleep(100);
            }
        });
        result = https::get(package::download_url(name, version.number), archive, version.file_size ? version.file_size + (1 << 20) : largest_download,
                            download_seconds, agent, &watch);
    }
    if (stop.load()) throw std::runtime_error("Cancelled.");
    if (!result.ok)
        throw std::runtime_error(result.http_status && result.http_status != 200
            ? "the download failed (Thunderstore answered " + std::to_string(result.http_status) + ")"
            : "the download failed (Windows error " + std::to_string(result.error) + ")");
    set_package(Progress::Step::installing, nullptr, at, of);
    set_activity("Installing " + wanted.title() + " v" + version.number + counter, 0);
    launcher_mods::InstallOptions options;
    options.folder = ts::folder_for(wanted.full_name);
    options.author = wanted.owner;
    options.require_content = true;
    const auto meter_install = [&](float fraction) { set_activity("Installing " + wanted.title() + " v" + version.number + counter, fraction); };
    const bool update = fs::exists(root / wide(options.folder), error);
    try {
        launcher_mods::install(root, archive, true, meter_install, stop, options);
        if (update) {
            std::lock_guard lock(updated_mutex);
            updated_since_apply.insert(options.folder);
        }
        return {};
    } catch (const std::exception& failure) {
        if (!update || stop.load()) throw;
        // The game holds the files of the copy it replaces: it goes in at the next start.
        log(logging::Level::info, wanted.full_name + " is in use (" + failure.what() + "); its update waits for the next start.");
    }
    launcher_mods::install_later(root, archive, meter_install, stop, options);
    return "later";
}

// ---------------------------------------------------------------- READMEs

std::mutex readme_mutex;
std::map<std::string, std::shared_ptr<const Readme>, std::less<>> readmes; // under the mutex; null while fetching
} // namespace

const char* state_label(State state) {
    switch (state) {
    case State::loaded: return "LOADED";
    case State::disabled: return "DISABLED";
    case State::apply_to_load: return "APPLY TO LOAD";
    case State::apply_to_unload: return "APPLY TO UNLOAD";
    case State::left_out: return "LEFT OUT";
    case State::not_loaded: return "NOT LOADED";
    case State::set_aside: return "SET ASIDE";
    case State::update_waiting: return "RESTART TO UPDATE";
    case State::removal_waiting: return "RESTART TO REMOVE";
    }
    return "";
}

std::shared_ptr<const Listing> listing() {
    static std::atomic<bool> started{};
    if (!started.exchange(true)) start_fetch(true);
    else if (!fetching.load() && unix_now() >= next_fetch.load()) start_fetch(false);
    std::lock_guard lock(listing_mutex);
    return current_listing;
}
void refresh() {
    if (!fetching.load()) start_fetch(false);
}

std::shared_ptr<const Mods> installed() {
    if (!scanning.load() && GetTickCount64() >= next_scan.load()) start_scan();
    std::lock_guard lock(mods_mutex);
    return current_mods;
}

void set_enabled(const std::string& folder, bool on) {
    try {
        std::thread([folder, on] {
            try {
                std::lock_guard lock(write_mutex);
                auto list = mods::scan_mods(mods::engine_data_root());
                if (!list.issue.empty()) throw std::runtime_error("mods.json could not be read: " + list.issue);
                for (auto& entry : list.entries)
                    if (same(entry.mod.name, folder)) entry.enabled = on;
                mods::save_mod_order(list.root, list.entries);
                log(logging::Level::info, folder + (on ? " switched on." : " switched off."));
            } catch (const std::exception& failure) {
                std::lock_guard lock(progress_mutex);
                current_progress.result = std::string("Could not save mods.json: ") + failure.what();
                current_progress.failed = true;
                ++current_progress.serial;
            }
            rescan();
        }).detach();
    } catch (...) {}
}

std::string apply() {
    if (busy.load()) return "Wait for the install under way to finish.";
    std::shared_ptr<const Mods> now;
    {
        std::lock_guard lock(mods_mutex);
        now = current_mods;
    }
    const bool reload = now && now->effect == live_mods::ApplyEffect::reload_level;
    const auto said = live_mods::apply(reload);
    applied_at.store(GetTickCount64());
    if (live_mods::busy()) {
        std::lock_guard lock(updated_mutex);
        updated_since_apply.clear();
    }
    rescan();
    return said;
}

Progress progress() {
    Progress result;
    {
        std::lock_guard lock(progress_mutex);
        result = current_progress;
    }
    // And what waits after the job in hand.
    std::lock_guard lock(jobs_mutex);
    result.waiting = jobs.size();
    for (const auto& job : jobs) {
        result.queued.insert(result.queued.end(), job.install.begin(), job.install.end());
        if (!job.uninstall.empty()) result.queued.push_back(job.uninstall);
    }
    return result;
}

namespace {
// An install job: the packages asked for at their newest, and what they need that is not here at the
// version asked for (read from the Mods folder now: a job before this one may have installed it).
std::pair<std::string, bool> install_job(const std::vector<std::string>& wanted) {
    const auto shown = listing();
    std::vector<ts::Package> packages;
    for (const auto& name : wanted)
        if (const auto* found = ts::find_package(shown->packages, name)) packages.push_back(*found);
    if (packages.empty()) return {"That mod is not in Thunderstore's listing any more.", true};
    ts::Installed have;
    for (const auto& entry : mods::scan_mods(mods::engine_data_root()).entries) have[entry.mod.name] = entry.mod.version;
    for (const auto& package : packages) have.erase(ts::folder_for(package.full_name));
    const auto plan = mod_store::plan(packages, shown->packages, have);
    if (plan.packages.empty()) return {"Nothing to install.", false};
    std::size_t now_in{}, later{};
    std::vector<std::string> failed;
    for (std::size_t i = 0; i < plan.packages.size(); ++i) {
        if (stop.load()) break;
        const auto& package = plan.packages[i];
        try {
            std::vector<std::string> rest;
            for (std::size_t j = i + 1; j < plan.packages.size(); ++j) rest.push_back(plan.packages[j].full_name);
            const auto how = install_one(package, i, plan.packages.size(), std::move(rest));
            (how.empty() ? now_in : later) += 1;
            log(logging::Level::info, package.full_name + " v" + package.latest().number + (how.empty() ? " installed." : " waits for the next start."));
        } catch (const std::exception& failure) {
            if (stop.load()) break;
            failed.push_back(package.title() + ": " + failure.what());
        }
    }
    std::string result;
    const auto title = plan.packages.back().title();
    if (stop.load()) result = "Cancelled. " + std::to_string(now_in) + " of " + std::to_string(plan.packages.size()) + " installed.";
    else if (failed.empty() && later == 0)
        result = plan.packages.size() == 1 ? title + " is installed." : std::to_string(now_in) + " mods installed (" + title + " and what it needs).";
    else if (failed.empty())
        result = later == plan.packages.size() ? "The game has the old files open: the update goes in the next time it starts."
                 : std::to_string(now_in) + " installed; " + std::to_string(later) + " go in the next time the game starts.";
    else {
        result = failed.size() == 1 ? failed.front() : std::to_string(failed.size()) + " could not be installed: " + failed.front();
        if (now_in) result += " (" + std::to_string(now_in) + " installed)";
    }
    if (!plan.unknown.empty()) result += " Not on Thunderstore: " + plan.unknown.front() + (plan.unknown.size() > 1 ? " and more." : ".");
    return {result, stop.load() || !failed.empty() || !plan.unknown.empty()};
}
std::pair<std::string, bool> uninstall_job(const std::string& folder) {
    set_package(Progress::Step::uninstalling, nullptr, 0, 1);
    {
        std::lock_guard lock(progress_mutex);
        current_progress.package = current_progress.title = folder;
    }
    set_activity("Uninstalling " + folder);
    const auto root = mods_root();
    try {
        launcher_mods::remove(root, folder);
        return {folder + " is uninstalled (it is in the Recycle Bin).", false};
    } catch (const std::exception& failure) {
        log(logging::Level::info, folder + " could not be removed now (" + failure.what() + "); it goes at the next start.");
    }
    try {
        {
            // Off now, so the next apply takes it out of the game; gone at the next start.
            std::lock_guard lock(write_mutex);
            auto list = mods::scan_mods(mods::engine_data_root());
            if (list.issue.empty()) {
                for (auto& entry : list.entries)
                    if (same(entry.mod.name, folder)) entry.enabled = false;
                mods::save_mod_order(list.root, list.entries);
            }
        }
        launcher_mods::remove_later(root, folder);
        return {folder + " is switched off, and uninstalled the next time the game starts.", false};
    } catch (const std::exception& failure) {
        return {std::string("Could not uninstall it: ") + failure.what(), true};
    }
}
// The worker: the jobs one after another, each once nothing else is changing the Mods folder (a merge,
// a session's map download); then it ends, until a job is queued again.
void work() {
    for (;;) {
        Job job;
        {
            std::lock_guard lock(jobs_mutex);
            if (jobs.empty()) {
                working = false;
                busy.store(false);
                std::lock_guard progress_lock(progress_mutex);
                current_progress.busy = false;
                return;
            }
            job = std::move(jobs.front());
            jobs.pop_front();
        }
        stop.store(false);
        watch.cancel.store(false);
        set_package(Progress::Step::starting, nullptr, 0, 0);
        while (!stop.load() && (live_mods::busy() || map_download::view().stage != map_download::Stage::idle)) {
            set_activity(live_mods::busy() ? "Waiting for the mods being applied..." : "Waiting for the session's map download...");
            Sleep(250);
        }
        set_activity("Starting...");
        std::pair<std::string, bool> outcome;
        try {
            outcome = stop.load() ? std::pair<std::string, bool>{"Cancelled.", true}
                      : job.uninstall.empty() ? install_job(job.install) : uninstall_job(job.uninstall);
        } catch (const std::exception& failure) {
            outcome = {std::string("It could not be done: ") + failure.what(), true};
        } catch (...) {
            outcome = {"It could not be done.", true};
        }
        job_done(std::move(outcome.first), outcome.second);
    }
}
// Adds a job, starting the worker when it is not running.
std::string enqueue(Job job) {
    std::lock_guard lock(jobs_mutex);
    jobs.push_back(std::move(job));
    {
        std::lock_guard progress_lock(progress_mutex);
        if (!current_progress.busy) {
            current_progress.busy = true;
            current_progress.result.clear();
            current_progress.failed = false;
            current_progress.activity = "Starting...";
            current_progress.fraction = -1;
        }
    }
    if (!working) {
        try {
            std::thread(work).detach();
            working = true;
            busy.store(true);
        } catch (...) {
            jobs.pop_back();
            std::lock_guard progress_lock(progress_mutex);
            current_progress.busy = false;
            return "The install could not start.";
        }
    }
    return {};
}
} // namespace

bool queued(std::string_view package) {
    {
        std::lock_guard lock(progress_mutex);
        if (current_progress.package == package ||
            std::find(current_progress.queued.begin(), current_progress.queued.end(), package) != current_progress.queued.end())
            return true;
    }
    std::lock_guard lock(jobs_mutex);
    return std::any_of(jobs.begin(), jobs.end(), [&](const Job& job) {
        return std::find(job.install.begin(), job.install.end(), package) != job.install.end();
    });
}

std::string install(const std::vector<std::string>& wanted) {
    const auto shown = listing();
    std::vector<std::string> names;
    for (const auto& name : wanted)
        if (ts::find_package(shown->packages, name) && !queued(name) &&
            std::find(names.begin(), names.end(), name) == names.end())
            names.push_back(name);
    if (names.empty()) {
        const bool listed = std::any_of(wanted.begin(), wanted.end(), [&](const std::string& name) { return ts::find_package(shown->packages, name); });
        return listed ? "Already in the download queue." : "That mod is not in Thunderstore's listing.";
    }
    return enqueue({std::move(names), {}});
}

std::string uninstall(const std::string& folder) {
    if (!mods::valid_mod_name(folder)) return "That is not a mod folder.";
    {
        std::lock_guard lock(jobs_mutex);
        if (std::any_of(jobs.begin(), jobs.end(), [&](const Job& job) { return same(job.uninstall, folder); }))
            return "Already in the queue.";
    }
    return enqueue({{}, folder});
}

void cancel() {
    {
        std::lock_guard lock(jobs_mutex);
        jobs.clear();
    }
    stop.store(true);
    watch.cancel.store(true);
}

std::shared_ptr<const Readme> readme(const ts::Package& wanted) {
    const auto key = wanted.full_name + '-' + wanted.latest().number;
    {
        std::lock_guard lock(readme_mutex);
        if (const auto found = readmes.find(key); found != readmes.end()) return found->second;
        if (readmes.size() >= 64) readmes.clear();
        readmes.emplace(key, nullptr);
    }
    try {
        std::thread([key, url = ts::readme_url(wanted)] {
            auto result = std::make_shared<Readme>();
            if (const auto json = https::get_text(url, 1 << 20, 20, agent)) {
                try { result->text = ts::readme_lines(ts::parse_readme(*json), 300); }
                catch (...) { result->failed = true; }
            } else result->failed = true;
            std::lock_guard lock(readme_mutex);
            readmes[key] = std::move(result);
        }).detach();
    } catch (...) {}
    return nullptr;
}

std::string today() {
    SYSTEMTIME now{};
    GetSystemTime(&now);
    char text[16];
    std::snprintf(text, sizeof(text), "%04u-%02u-%02u", now.wYear, now.wMonth, now.wDay);
    return text;
}
}
