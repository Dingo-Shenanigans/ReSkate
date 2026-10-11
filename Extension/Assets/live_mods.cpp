#include "live_mods.h"
#include "mod_layers.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Abi/native_data.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/World/custom_level_manifest.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Core/Platform/path_text.h"
#include "Engine/Game/Build/20260929/live_mods.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Vfs/mod_catalog.h"
#include "Engine/Vfs/mod_scoring.h"
#include "Extension/World/loading_screen.h"
#include "Extension/UI/Overlay/overlay.h"
#include <format>
#include <fstream>
#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <atomic>
#include <array>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace dingosdk::live_mods {
namespace {
namespace native = addr::live_mods;
using Register = void (*)(void *registry, const char *name);
using Find = void *(*)(void *registry, const char *name, char prefixed);
using Attach = void *(*)(void *superbundle, unsigned kind, void *chunk, int mode);
using PrepareToc = void (*)(void *data);

std::atomic<bool> merging{};
std::atomic<bool> reload_after{}, reload_ready{};
std::mutex state_mutex;
std::string last_status;
Progress merge_progress; // under state_mutex, while merging
std::optional<std::vector<std::filesystem::path>> pending_levels;
std::optional<std::vector<std::string>> applied; // set by a live apply; the launch's list before one
std::string current_level;

template <std::size_t N> bool matches(std::uintptr_t address, const std::array<std::uint8_t, N> &prefix) {
    std::array<std::uint8_t, N> actual{};
    return memory::read_bytes(address, actual.data(), N) && actual == prefix;
}
// A Frostbite string: up to 15 bytes inline, else a pointer; bit 0x80 of byte +0xf says which.
std::string engine_string(std::uintptr_t at) {
    std::uint8_t flag{};
    if (!memory::read_bytes(at + 0xf, &flag, 1)) return {};
    std::uintptr_t source = at;
    if ((flag & 0x80) && !memory::read_bytes(at, &source, sizeof(source))) return {};
    char text[256]{};
    if (!memory::read_bytes(source, text, sizeof(text) - 1)) return {};
    return text;
}
void *find_chunk(std::uintptr_t registry, const std::string &name) {
    std::uintptr_t manifest{}, begin{}, end{};
    if (!memory::read_bytes(registry + native::registry_manifest, &manifest, 8) || !manifest ||
        !memory::read_bytes(manifest + native::manifest_chunks, &begin, 8) ||
        !memory::read_bytes(manifest + native::manifest_chunks + 8, &end, 8))
        return nullptr;
    for (auto at = begin; at < end && at < begin + 8 * 1024; at += 8) {
        std::uintptr_t chunk{};
        if (memory::read_bytes(at, &chunk, 8) && chunk && _stricmp(engine_string(chunk + native::chunk_name).c_str(), name.c_str()) == 0)
            return reinterpret_cast<void *>(chunk);
    }
    return nullptr;
}

// Registers every superbundle the merged mods list that the game does not
// know yet, and lists it under its install chunk. Returns the names added.
std::vector<std::string> register_superbundles(const std::vector<std::pair<std::string, std::string>> &wanted,
                                               std::string &problem) {
    std::vector<std::string> added;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!matches(base + native::register_superbundle, native::register_superbundle_prefix) ||
        !matches(base + native::find_superbundle, native::find_superbundle_prefix) ||
        !matches(base + native::attach_superbundle, native::attach_superbundle_prefix)) {
        problem = "this game build differs, so new maps need a restart";
        return added;
    }
    const auto objects = layout_objects();
    std::uintptr_t registry{};
    if (!objects.manager || !objects.layers ||
        !memory::read_bytes(reinterpret_cast<std::uintptr_t>(objects.layers) + native::layers_registry, &registry, 8) ||
        !registry) {
        problem = "the game's superbundle registry is not available";
        return added;
    }
    const auto register_one = reinterpret_cast<Register>(base + native::register_superbundle);
    const auto find = reinterpret_cast<Find>(base + native::find_superbundle);
    const auto attach = reinterpret_cast<Attach>(base + native::attach_superbundle);
    // The engine keeps the name pointer; these live for the rest of the process.
    static std::deque<std::string> names;
    auto *lock = reinterpret_cast<CRITICAL_SECTION *>(static_cast<char *>(objects.manager) + native::manager_lock);
    EnterCriticalSection(lock);
    struct Leave { CRITICAL_SECTION *lock; ~Leave() { LeaveCriticalSection(lock); } } leave{lock};
    for (const auto &[name, chunk_name] : wanted) {
        auto *registry_object = reinterpret_cast<void *>(registry);
        if (find(registry_object, name.c_str(), 0)) continue;
        auto *chunk = find_chunk(registry, chunk_name);
        if (!chunk) {
            problem = "install chunk " + chunk_name + " not found for " + name;
            continue;
        }
        names.push_back(name);
        register_one(registry_object, names.back().c_str());
        if (auto *superbundle = find(registry_object, names.back().c_str(), 0)) {
            attach(superbundle, 0, chunk, 1);
            added.push_back(name);
        } else {
            problem = name + " could not be registered";
        }
    }
    return added;
}

// Case-insensitive djb2, as the engine hashes bundle names.
std::uint32_t bundle_hash(std::string_view name) {
    std::uint32_t hash = 5381;
    for (auto c : name) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        hash = hash * 33U ^ static_cast<std::uint8_t>(c);
    }
    return hash;
}
// The root level description, from whichever asset domain holds it.
std::uintptr_t find_root_description(std::uintptr_t base) {
    const auto find = game::native_data().find_asset;
    if (!find) return 0;
    for (std::uint16_t domain = 0; domain < 0xbbf; ++domain) {
        std::uintptr_t owner{};
        if (!memory::read_bytes(base + addr::engine::domain_owners + domain * 8ULL, &owner, 8) || !owner) continue;
        if (const auto asset = find(domain, native::root_description)) return asset;
    }
    return 0;
}
// Adds every level to the root's on-demand list that it does not hold yet.
// Maps enabled at launch (or installed then) are there already, from globals.
std::vector<std::string> register_on_demand_levels(const std::vector<std::string> &levels, std::string &problem) {
    std::vector<std::string> added;
    if (levels.empty()) return added;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto description = find_root_description(base);
    if (!description) {
        problem = "the root level description is not loaded";
        return added;
    }
    const auto field = description + native::description_on_demand;
    std::uintptr_t entries{};
    std::uint32_t count{};
    if (!memory::read_bytes(field, &entries, 8) || !entries || !memory::read_bytes(entries - 4, &count, 4) || count > 4096) {
        problem = "the root level description's on-demand list is unreadable";
        return added;
    }
    std::vector<std::byte> rows(std::size_t{count} * native::on_demand_entry_size);
    if (!memory::read_bytes(entries, rows.data(), rows.size())) {
        problem = "the root level description's on-demand list is unreadable";
        return added;
    }
    const auto holds = [&](const std::string &level) {
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uintptr_t name{};
            std::memcpy(&name, rows.data() + i * native::on_demand_entry_size, 8);
            char text[260]{};
            if (name && memory::read_bytes(name, text, sizeof(text) - 1) && _stricmp(text, level.c_str()) == 0) return true;
        }
        return false;
    };
    static std::deque<std::string> names; // the engine keeps these pointers
    for (const auto &level : levels) {
        if (holds(level)) continue;
        names.push_back(level);
        std::array<std::byte, native::on_demand_entry_size> row{};
        const auto pointer = reinterpret_cast<std::uintptr_t>(names.back().c_str());
        const auto hash = bundle_hash(level);
        std::memcpy(row.data(), &pointer, 8);
        std::memcpy(row.data() + 8, &hash, 4);
        rows.insert(rows.end(), row.begin(), row.end());
        added.push_back(level);
    }
    if (added.empty()) return added;
    // A new array in the engine's layout: 16 header bytes ending in the count,
    // then the entries. The old array stays where it is, owned by globals.
    const auto total = static_cast<std::uint32_t>(rows.size() / native::on_demand_entry_size);
    auto *block = static_cast<std::byte *>(VirtualAlloc(nullptr, 16 + rows.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!block) {
        problem = "no memory for the on-demand list";
        return {};
    }
    std::memcpy(block + 12, &total, 4);
    std::memcpy(block + 16, rows.data(), rows.size());
    const auto replacement = reinterpret_cast<std::uintptr_t>(block + 16);
    SIZE_T written{};
    if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(field), &replacement, 8, &written) || written != 8) {
        problem = "the on-demand list could not be replaced";
        return {};
    }
    return added;
}

// Hands the game the TOC a live merge rewrote for a superbundle it mounted at
// launch: globals (loading-screen widgets of maps added since) and items (the
// cosmetics bundle the ownables system reloads at every level load, with its
// item collections and bundle ref table). Bundles are looked up in the TOC data
// when they load, so the top layer's entry is pointed at a copy of the new
// file; the old copy stays allocated for anything still holding it. The merge
// always writes these TOCs, so the top layer is the merged patch's.
bool refresh_toc(const std::string &superbundle, const std::filesystem::path &file, std::string &problem) {
    std::ifstream in(file, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() <= native::toc_header + 4) {
        problem = path_utf8(file.filename()) + " could not be read";
        return false;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!matches(base + native::find_superbundle, native::find_superbundle_prefix) ||
        !matches(base + native::prepare_toc, native::prepare_toc_prefix)) {
        problem = "this game build differs";
        return false;
    }
    const auto objects = layout_objects();
    std::uintptr_t registry{};
    if (!objects.manager || !objects.layers ||
        !memory::read_bytes(reinterpret_cast<std::uintptr_t>(objects.layers) + native::layers_registry, &registry, 8) || !registry) {
        problem = "the game's superbundle registry is not available";
        return false;
    }
    auto *lock = reinterpret_cast<CRITICAL_SECTION *>(static_cast<char *>(objects.manager) + native::manager_lock);
    EnterCriticalSection(lock);
    struct Leave { CRITICAL_SECTION *lock; ~Leave() { LeaveCriticalSection(lock); } } leave{lock};
    const auto find = reinterpret_cast<Find>(base + native::find_superbundle);
    const auto sb = reinterpret_cast<std::uintptr_t>(find(reinterpret_cast<void *>(registry), superbundle.c_str(), 0));
    std::uintptr_t mounts{}, mounts_end{};
    if (!sb || !memory::read_bytes(sb + native::superbundle_mounts, &mounts, 8) ||
        !memory::read_bytes(sb + native::superbundle_mounts + 8, &mounts_end, 8) || mounts == mounts_end) {
        problem = superbundle + " is not mounted";
        return false;
    }
    // The engine keeps TOC data in native byte order; convert a copy the same way.
    reinterpret_cast<PrepareToc>(base + native::prepare_toc)(bytes.data() + native::toc_header);
    std::uint32_t wanted_format{};
    std::memcpy(&wanted_format, bytes.data() + native::toc_header, 4);
    std::size_t swapped{};
    std::string seen;
    for (auto at = mounts; at < mounts_end && at < mounts + 8 * 16; at += 8) {
        std::uintptr_t mount{}, entry{}, entries_end{}, toc{}, data{}, owned{};
        std::uint32_t format{};
        if (!memory::read_bytes(at, &mount, 8) || !mount || !memory::read_bytes(mount + native::mount_tocs, &entry, 8) ||
            !memory::read_bytes(mount + native::mount_tocs + 8, &entries_end, 8) || entry >= entries_end ||
            !memory::read_bytes(entry + native::toc_entry_file, &toc, 8) || !toc ||
            !memory::read_bytes(entry + native::toc_entry_data, &data, 8) ||
            !memory::read_bytes(toc + native::toc_file_bytes, &owned, 8) || data != owned + native::toc_header ||
            !memory::read_bytes(data, &format, 4) || format != wanted_format) {
            seen += std::format(" [format {:#x}]", format);
            continue; // the top layer's TOC is the merged patch's, in its format
        }
        auto *copy = static_cast<char *>(VirtualAlloc(nullptr, bytes.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!copy) break;
        std::memcpy(copy, bytes.data(), bytes.size());
        const auto start = reinterpret_cast<std::uintptr_t>(copy);
        const auto start_data = start + native::toc_header;
        const std::uint64_t size = bytes.size();
        const auto write = [](std::uintptr_t to, const void *value) {
            SIZE_T written{};
            return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(to), value, 8, &written) && written == 8;
        };
        if (write(toc + native::toc_file_bytes, &start) && write(toc + native::toc_file_size, &size) &&
            write(toc + native::toc_file_size + 8, &size) && write(entry + native::toc_entry_data, &start_data) &&
            write(entry + native::toc_entry_data + 8, &start_data))
            ++swapped;
    }
    if (!swapped) problem = "no mounted copy of " + superbundle + " is the merged patch's (" + std::format("{:#x}", wanted_format) + "):" + seen;
    return swapped != 0;
}

// True when everything a mod changes only matters once one of its maps is
// loaded: its TOCs are level superbundles (the root included, which every load
// re-reads) and globals, where maps register themselves. Such a mod needs no
// reload of the level being played; a player sees it by loading the map.
bool scan_maps_only(const std::filesystem::path &directory) {
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator at(directory, error), end; !error && at != end; at.increment(error)) {
        if (!at->is_regular_file(error) || _wcsicmp(at->path().extension().c_str(), L".toc") != 0) continue;
        auto relative = mods::ascii_path(std::filesystem::relative(at->path(), directory, error));
        std::ranges::transform(relative, relative.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (relative == "layout.toc" || relative == "win32/globals.toc" || relative.starts_with("win32/levels/")) continue;
        return false;
    }
    return !error;
}
// Cached per folder: the overlay asks every time it redraws its apply button.
bool maps_only(const std::filesystem::path &directory) {
    static std::mutex cache_mutex;
    static std::map<std::wstring, bool> cache;
    std::lock_guard lock(cache_mutex);
    const auto key = directory.wstring();
    if (const auto found = cache.find(key); found != cache.end()) return found->second;
    return cache[key] = scan_maps_only(directory);
}
// Whether the mods switched on or off since the last apply change anything the
// level being played shows (cosmetics, settings), rather than only maps.
bool changes_current_level(const mods::Catalog &catalog, const std::vector<std::string> &before) {
    const auto named = [](const std::vector<std::string> &names, const std::string &name) {
        return std::ranges::any_of(names, [&](const std::string &other) { return _stricmp(other.c_str(), name.c_str()) == 0; });
    };
    std::vector<std::string> now;
    for (const auto &mod : catalog.mods) now.push_back(mod.name);
    const auto directory = [&](const std::string &name) -> std::optional<std::filesystem::path> {
        for (const auto *list : {&catalog.mods, &catalog.inactive})
            for (const auto &mod : *list)
                if (_stricmp(mod.name.c_str(), name.c_str()) == 0) return mod.directory;
        return std::nullopt;
    };
    const auto changed = [&](const std::string &name) {
        const auto folder = directory(name);
        return !folder || !maps_only(*folder); // a folder since removed: assume the worst
    };
    for (const auto &name : now)
        if (!named(before, name) && changed(name)) return true;
    for (const auto &name : before)
        if (!named(now, name) && changed(name)) return true;
    return false;
}

void set_status(std::string text, logging::Level level = logging::Level::info) {
    logging::log(level, logging::Channel::assets, "Live mods: {}", text);
    std::lock_guard lock(state_mutex);
    last_status = std::move(text);
}

void apply_now() {
    const auto started = GetTickCount64();
    const auto before = applied_mods();
    auto catalog = mods::load_catalog(mods::engine_data_root(), {}, false);
    if (!catalog.issue.empty()) {
        set_status("mods.json could not be read: " + catalog.issue, logging::Level::warning);
        merging = false;
        return;
    }
    // As at launch (mods::load_catalog): a mod that cannot be merged cleanly is left out and the
    // others merged again without it, so applying mods now never loads one a restart would not.
    mods::MergeReport report;
    std::string left_out;
    for (int round = 0;; ++round) {
        // The mods in the game now were checked against the store when they went in.
        report = mods::merge_mods(catalog, [](const mods::MergeProgress &now) {
            std::lock_guard lock(state_mutex);
            merge_progress = {now.done, now.total, now.step};
        }, {.live = true, .checked = before});
        if (!report.issue.empty()) break;
        bool removed = false;
        for (std::size_t i = catalog.mods.size(); i-- > 0;) {
            const auto found = report.problems.find(catalog.mods[i].name);
            if (found == report.problems.end() || found->second.empty()) continue;
            if (round == 3) {
                report.issue = catalog.mods[i].name + " still could not be merged cleanly; restart the game to load the others";
                break;
            }
            logging::log(logging::Level::warning, logging::Channel::assets,
                         "Live mods: leaving out {}: it could not be merged cleanly ({} problem(s), first: {})",
                         catalog.mods[i].name, found->second.size(), found->second.front());
            left_out += (left_out.empty() ? "" : ", ") + catalog.mods[i].name;
            auto mod = std::move(catalog.mods[i]);
            catalog.mods.erase(catalog.mods.begin() + static_cast<std::ptrdiff_t>(i));
            mod.problems = found->second;
            catalog.excluded.push_back(std::move(mod));
            removed = true;
        }
        if (!report.issue.empty() || !removed) break;
    }
    if (!report.issue.empty()) {
        set_status("could not apply: " + report.issue, logging::Level::warning);
        merging = false;
        return;
    }
    if (!left_out.empty())
        overlay::notify(overlay::NoticeLevel::warning, "Left out " + left_out,
                        "It could not be merged cleanly. The other mods were applied without it.");
    // A mod switched on now can change how tricks score from the next level load on.
    if (const auto scoring = mods::check_scoring(catalog); scoring.fingerprint) {
        const bool already = mods::scoring_state().fingerprint != 0;
        mods::publish_scoring(scoring);
        std::string names;
        for (const auto &mod : scoring.mods) names += (names.empty() ? "" : ", ") + mod;
        logging::log(logging::Level::warning, logging::Channel::assets,
                     "Live mods: {} change(s) how tricks score or the skater handles (fingerprint {:016x}); in multiplayer you are kept out of "
                     "throwdowns and coop challenges until the game restarts without it.",
                     names, scoring.fingerprint);
        if (!already)
            overlay::notify(overlay::NoticeLevel::warning, names + " changes scoring or physics",
                            "In multiplayer you are kept out of throwdowns and coop challenges until you restart without it.");
    }
    std::string problem;
    const auto added = register_superbundles(report.superbundle_chunks, problem);
    for (const auto &name : added)
        logging::log(logging::Level::info, logging::Channel::assets, "Live mods: registered {}", name);
    // Maps the root has no on-demand entry for (installed since launch) get one.
    std::vector<std::string> levels;
    for (const auto &path : mods::level_manifest_paths(catalog)) {
        const auto manifest = read_custom_level_manifest(path);
        for (const auto &level : manifest.levels) levels.push_back(level.asset);
    }
    for (const auto &level : register_on_demand_levels(levels, problem))
        logging::log(logging::Level::info, logging::Channel::assets, "Live mods: added {} to the root level's on-demand list", level);
    for (const auto &name : report.removed_tocs)
        logging::log(logging::Level::info, logging::Channel::assets, "Live mods: removed {}", name);
    // Loading screens: mods installed since launch had no row in the
    // configuration, nor their widget in the globals TOC the game read then.
    std::vector<loading_screen::LiveScreen> screens;
    for (const auto &screen : report.load_screens) {
        // A mod's copy of the configuration keeps the game's own rows; only its maps' rows are its.
        if (std::ranges::none_of(levels, [&](const std::string &level) { return _stricmp(level.c_str(), screen.level.c_str()) == 0; }))
            continue;
        if (!placed_at_launch(screen.mod)) logging::log(logging::Level::info, logging::Channel::assets,
                                                        "Live mods: {} adds a loading screen for {}", screen.mod, screen.level);
        screens.push_back({screen.level, screen.bundle, screen.widget});
    }
    loading_screen::set_live_screens(std::move(screens));
    for (const auto *name : {"globals", "items"}) {
        std::string toc_problem;
        const auto file = mods::engine_data_root() / L"Mods" / L".reskate" / L"Win32" / (std::string(name) + ".toc");
        if (refresh_toc(std::string("Win32/") + name, file, toc_problem))
            logging::log(logging::Level::info, logging::Channel::assets, "Live mods: the game now reads the merged {} TOC", name);
        else if (problem.empty())
            problem = std::string(name) + " was not refreshed: " + toc_problem;
    }
    // Only maps added or taken away: nothing in the level being played changed,
    // so it is not reloaded even when asked; the maps are there to load.
    const bool reload = reload_after.exchange(false);
    const bool needed = reload && changes_current_level(catalog, before);
    {
        std::lock_guard lock(state_mutex);
        // Set with the manifests, so the client tick sees both in one update.
        if (needed) reload_ready = true;
        pending_levels = mods::level_manifest_paths(catalog);
        applied.emplace();
        for (const auto &mod : catalog.mods) applied->push_back(mod.name);
    }
    if (!left_out.empty()) problem += (problem.empty() ? "left out " : "; left out ") + left_out + ", which could not be merged cleanly";
    // Where the time went, step by step, as the launch's merge logs it.
    for (const auto &note : report.notes)
        if (note.starts_with("Merge times:")) logging::log(logging::Level::info, logging::Channel::assets, "Live mods: {}", note);
    const auto elapsed = static_cast<double>(GetTickCount64() - started) / 1000.0;
    set_status("applied " + std::to_string(catalog.mods.size()) + " mod(s) in " + std::to_string(elapsed).substr(0, 4) + "s; " +
                   (needed ? "reloading the level"
                    : reload ? "only maps changed, so the level was not reloaded"
                             : "the next level load uses them") +
                   (problem.empty() ? "" : " (" + problem + ")"),
               problem.empty() ? logging::Level::info : logging::Level::warning);
    merging = false;
}
// The worker thread: anything a mod's files make the merge throw is a status, not the end of the game.
void run() {
    try {
        apply_now();
    } catch (const std::exception &failure) {
        set_status(std::string("could not apply: ") + failure.what(), logging::Level::warning);
        merging = false;
    } catch (...) {
        set_status("could not apply the mods", logging::Level::warning);
        merging = false;
    }
}
} // namespace

std::string apply(bool reload_level) {
    if (!layout_objects().manager) return "No merged mod patch was mounted at launch; restart the game to load mods.";
    if (merging.exchange(true)) return "Mods are already being applied.";
    {
        std::lock_guard lock(state_mutex);
        merge_progress = {};
    }
    reload_after = reload_level;
    std::thread(run).detach();
    return reload_level ? "Applying mods; the level reloads when they are ready, unless only maps changed."
                        : "Applying mods; they take effect at the next level load.";
}
bool take_reload() { return reload_ready.exchange(false); }
void set_current_level(std::string level) {
    std::lock_guard lock(state_mutex);
    if (current_level != level) current_level = std::move(level);
}
ApplyEffect preview(const mods::ModList &list) {
    std::string level;
    {
        std::lock_guard lock(state_mutex);
        level = current_level;
    }
    const auto before = applied_mods();
    const auto named = [](const std::vector<std::string> &names, const std::string &name) {
        return std::ranges::any_of(names, [&](const std::string &other) { return _stricmp(other.c_str(), name.c_str()) == 0; });
    };
    std::vector<std::string> now;
    for (const auto &entry : list.entries)
        if (entry.enabled) now.push_back(entry.mod.name);
    const auto entry_of = [&](const std::string &name) -> const mods::ModEntry * {
        for (const auto &entry : list.entries)
            if (_stricmp(entry.mod.name.c_str(), name.c_str()) == 0) return &entry;
        return nullptr;
    };
    bool reload{};
    for (const auto &name : before) {
        if (named(now, name)) continue;
        const auto *entry = entry_of(name);
        // A folder deleted since: what it held is unknown, so assume the worst.
        if (!entry) {
            reload = true;
            continue;
        }
        if (!level.empty() && std::ranges::any_of(entry->mod.levels, [&](const std::string &asset) {
                return _stricmp(asset.c_str(), level.c_str()) == 0;
            }))
            return ApplyEffect::leave_level;
        reload |= !maps_only(entry->mod.directory);
    }
    for (const auto &name : now)
        if (!named(before, name))
            if (const auto *entry = entry_of(name)) reload |= !maps_only(entry->mod.directory);
    return reload ? ApplyEffect::reload_level : ApplyEffect::maps_only;
}
bool busy() noexcept { return merging.load(); }
Progress progress() {
    std::lock_guard lock(state_mutex);
    return merge_progress;
}
bool only_maps(const std::filesystem::path& directory) { return scan_maps_only(directory); }
std::string status() {
    std::lock_guard lock(state_mutex);
    return last_status;
}
std::vector<std::string> applied_mods() {
    {
        std::lock_guard lock(state_mutex);
        if (applied) return *applied;
    }
    std::vector<std::string> names;
    for (const auto &mod : mods::catalog().mods) names.push_back(mod.name);
    return names;
}
bool placed_at_launch(const std::string &name) {
    const auto &launch = mods::catalog();
    const auto named = [&](const mods::Mod &mod) { return _stricmp(mod.name.c_str(), name.c_str()) == 0; };
    return std::ranges::any_of(launch.mods, named) || std::ranges::any_of(launch.inactive, named);
}
std::optional<std::vector<std::filesystem::path>> take_level_manifests() {
    std::lock_guard lock(state_mutex);
    return std::exchange(pending_levels, std::nullopt);
}
} // namespace dingosdk::live_mods
