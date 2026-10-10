#include "mod_merge.h"

#include "content_cache.h"
#include "content_catalogs.h"
#include "mod_merge_internal.h"
#include "mod_store_copies.h"
#include "native_db.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>

namespace dingosdk::mods {
using namespace detail;
namespace {
using Node = native_db::Node;
// Where maps register themselves; read by the game only at launch.
constexpr std::string_view launch_level_registry = "win32/globals.toc";
// The root level: its sublevel manager lists every map, and it carries the
// shader-state tables (material rows) and the material grid maps add to. The
// renderer prepares the shader tables once, at launch, so the root must not
// change under it while the game runs: every installed map's root edits go in
// at launch, disabled ones included, and a live merge keeps them as they are.
constexpr std::string_view root_level = "win32/levels/game/dingolevel_root/dingolevel_root.toc";
// Superbundles the game mounts once, at launch. The merged patch always carries
// its own copy of each, even one no enabled mod changes, so a mod enabled or
// installed while the game runs has a mounted copy to replace in memory
// (Extension/Assets/live_mods.cpp) and one disabled can be swapped back out.
constexpr std::array<std::string_view, 2> launch_superbundles{"Win32/globals.toc", "Win32/items.toc"};

// Each enabled mod that adds copies of items the game's store sells
// (mod_store_copies.h) gets a problem; true when any does. The problem says no
// more than that the mod could not be merged: what was found is not for the
// mod's author to read. The catalogue is only read once a mod turns out to add
// an item at all.
bool store_copy_problems(const Catalog& catalog, MergeReport& report) {
    std::optional<content_cache::Catalogs> store;
    const auto found = check_store_copies(catalog, [&store](const std::string& key) {
        if (!store) store = content_cache::read_catalogs(content_cache::directory());
        return store->reserved(key);
    }, &report.notes);
    for (const auto& source : found.mods) report.problems[source.mod].emplace_back(store_copies_problem);
    return !found.mods.empty();
}
} // namespace

MergeReport merge_mods(const Catalog& catalog, const MergeObserver& observe, const MergeOptions& options) noexcept {
    MergeReport report;
    try {
        // How long each step took, for the log: a merge that takes many minutes
        // can then be traced to the step that takes them.
        using Clock = std::chrono::steady_clock;
        std::vector<std::pair<std::string, Clock::duration>> times;
        auto lapStart = Clock::now();
        const auto lap = [&](std::string step) {
            const auto now = Clock::now();
            times.emplace_back(std::move(step), now - lapStart);
            lapStart = now;
        };
        const auto output = catalog.root / generated_folder;
        std::error_code error;

        std::vector<const Mod*> mods;
        for (const auto& mod : catalog.mods) if (mod.provides_layout) mods.push_back(&mod);
        // With no mod enabled the patch stays, holding the game's own copy of
        // what it reads at launch (and any disabled mods' archives), so a mod
        // enabled or added later loads without a restart.
        std::map<const Mod*, RelativeFiles> modFiles;
        for (const auto* mod : mods) modFiles[mod] = scan(mod->directory);

        // What the store sells comes from the content cache, which the launcher installs.
        const bool storeKnown = content_cache::installed();
        // Disabled mods count too: their archives and map registration are placed at launch.
        auto fingerprint = merge_fingerprint(catalog, mods, modFiles, storeKnown);
        for (const auto& mod : catalog.inactive)
            fingerprint += " inactive " + mod.name + " " + mod_fingerprint(mod.directory);
        if (options.live) {
            fs::remove(output / stamp_file, error);
        } else if (auto previous = previous_merge(output, fingerprint)) {
            return std::move(*previous);
        }
        // A mod that adds copies of store items is not loaded at all. Found before
        // anything is built: the caller merges again without it, as it does for a
        // mod that cannot be merged, and the patch on disk stays for that merge to
        // reuse or replace.
        if (storeKnown && store_copy_problems(catalog, report)) return report;
        if (!options.live) fs::remove_all(output, error);
        lap("checking the mods");

        // Progress: each mod's archives, the material grid, a live merge's load
        // screens, the asset overrides, each superbundle, then the layout.
        std::set<std::string> distinctTocs;
        for (const auto& [mod, files] : modFiles)
            for (const auto& toc : files.tocs) distinctTocs.insert(lower(toc));
        for (const auto relative : launch_superbundles) distinctTocs.insert(lower(relative));
        MergeProgress progress{0, mods.size() + distinctTocs.size() + (options.live ? 4 : 3), mods.size(), {}};
        const auto advance = [&](std::string step) {
            if (!observe) return;
            progress.step = std::move(step);
            try { observe(progress); } catch (...) {}
            ++progress.done;
        };

        // Every superbundle any mod ships, and who ships it, in priority order.
        std::map<std::string, std::vector<const Mod*>, std::less<>> providers;
        std::map<const Mod*, ArchivePlacement> placements;
        std::vector<std::string> superbundles;
        // layeredInstallChunkFiles enumerates the archives the engine expects,
        // so every index a mod's payloads moved to has to be declared there.
        ArchiveUse used;

        // Every mod keeps the archive indices it was built for; where two mods
        // ship the same one, their archives are concatenated into a single file
        // and the later blocks are addressed by byte offset.
        // A mod's layout is the game's (as of the build it was made for) plus
        // its own entries. The merged layout starts from the installed game's
        // layout and takes only each mod's own entries, so mods made for an
        // earlier game build keep working after an update: a mod's full copy
        // would carry the old build's layout, and the game then waits on the
        // splash for bundles that layout places wrongly.
        auto parsedLayout = vfs::read_layout(catalog.data_root / L"Data" / L"layout.toc");
        std::vector<const Mod*> layoutSources = mods;
        if (layoutSources.empty() && !catalog.inactive.empty()) layoutSources.push_back(&catalog.inactive.front());
        auto& layout = parsedLayout.root;
        const auto baseRoot = catalog.data_root / L"Data";
        const auto gameRoot = catalog.data_root;
        CasStore store(baseRoot, output, layout);
        // Mods are built against archive 1, so that is where the rebuilt
        // manifests go too: an index the engine already knows in every package.
        constexpr std::uint16_t manifestArchive = 1;

        // What the layout declares in each package directory. A free index is
        // judged against its own directory rather than every directory at once
        // (judged against the union, three mods used up the gaps), lowest
        // first, and carries on past the highest index the game declares: the
        // engine keeps archives in a table keyed by layer, install chunk and a
        // 16-bit index, with no range to stay inside. What it cannot survive is
        // a reference to an archive the layout never declared.
        std::map<std::string, std::set<std::uint16_t>> declaredIn;
        for (const auto* field : {"layeredInstallChunkFiles", "unlayeredInstallChunkFiles"}) {
            const auto* node = layout.field(field);
            if (!node || node->type != 19) continue;
            for_each_install_chunk_file(node->payload(), [&](std::uint32_t id, std::uint16_t archive) {
                if (const auto* directory = store.find_directory(id)) declaredIn[*directory].insert(archive);
            });
        }
        std::map<std::string, std::set<std::uint16_t>> claimedArchives;
        const auto claim = [&](const std::string& directory) -> std::optional<std::uint16_t> {
            const auto& declared = declaredIn[directory];
            auto& claimed = claimedArchives[directory];
            for (std::uint32_t candidate = 1; candidate <= std::numeric_limits<std::uint16_t>::max(); ++candidate) {
                const auto index = static_cast<std::uint16_t>(candidate);
                if (!declared.contains(index) && claimed.insert(index).second) return index;
            }
            return std::nullopt;
        };

        // Where every mod's archives go. The launch's merge places the archives
        // of every installed mod, disabled ones too, and records it: a merge
        // while the game runs must not move an archive the game may hold open,
        // so it reuses that record, and a mod disabled at launch can then be
        // enabled without a restart.
        const auto placementsPath = output / placements_file;
        PlacementRecord record;
        if (options.live) record = read_placements(placementsPath);
        std::vector<const Mod*> placedMods = mods;
        for (const auto& mod : catalog.inactive) placedMods.push_back(&mod);
        for (const auto* mod : placedMods) {
            const bool active = std::find(mods.begin(), mods.end(), mod) != mods.end();
            if (options.live && !active && !record.mods.contains(mod->name)) continue; // installed since launch, still off
            const auto files = active ? modFiles[mod] : scan(mod->directory);
            if (!active) modFiles[mod] = files; // the material grid plan reads a disabled map's root edits too
            if (active) advance("Linking " + mod->name);
            for (const auto& relative : files.tocs) {
                // A map registers itself in globals (its level description and
                // the root's on-demand entry), which the game reads only at
                // launch. A disabled map's globals go in too, so enabling it
                // later only needs what is read at each load; while its level
                // and root TOCs are left out it cannot load.
                if (!active && !(mod->provides_levels &&
                                 (lower(relative) == launch_level_registry || lower(relative) == root_level)))
                    continue;
                auto& list = providers[lower(relative)];
                if (list.empty()) superbundles.push_back(relative);
                list.push_back(mod);
            }
            if (options.live) {
                if (const auto found = record.mods.find(mod->name); found != record.mods.end()) {
                    placements[mod] = found->second;
                    report.archives += found->second.at.size();
                    continue;
                }
                // Installed since launch: no free archive index was declared for
                // it then, so its archives go on the end of the patch's own
                // archive 1 in each package (the game reads appended data from an
                // archive it already has open) and are addressed by byte offset.
                // The record keeps the spots, so a later merge reuses them.
                for (const auto& relative : files.archives) {
                    const auto path = fs::path(relative);
                    const auto number = vfs::GameArchives::archive_index(path.stem().string());
                    if (!number) throw std::runtime_error("Unexpected archive name: " + relative);
                    auto directory = lower(path.parent_path().generic_string());
                    if (directory.starts_with("win32/")) directory.erase(0, 6);
                    const auto target = output / path.parent_path() / archive_file(manifestArchive);
                    if (!fs::exists(target, error))
                        throw std::runtime_error(mod->name + " needs a restart: the patch has no archive 1 in " + directory);
                    // Checked before anything is appended: past 4 GB the mod's
                    // payloads could not be addressed, and the next launch
                    // gives it an archive of its own anyway.
                    const auto held = fs::file_size(target, error);
                    const auto added = error ? std::uintmax_t{} : fs::file_size(mod->directory / path, error);
                    if (error || held + added > std::numeric_limits<std::uint32_t>::max())
                        throw std::runtime_error(mod->name + " needs a restart: the patch's archive 1 in " + directory +
                                                 " has no room left for it while the game runs");
                    ArchivePlacement::Spot spot{manifestArchive, append_file(mod->directory / path, target)};
                    placements[mod].at.emplace(std::pair{directory, *number}, spot);
                    report.notes.push_back(mod->name + ": " + directory + "/cas_" + std::to_string(*number) +
                                           " appended to cas_" + std::to_string(manifestArchive) + " at byte " +
                                           std::to_string(spot.offset) + " (added while the game runs)");
                    ++report.archives;
                }
                record.mods[mod->name] = placements[mod];
                continue;
            }
            for (const auto& relative : files.archives) {
                const auto path = fs::path(relative);
                const auto index = vfs::GameArchives::archive_index(path.stem().string());
                if (!index) { // the mod's problem: the catalog merges again without it
                    report.problems[mod->name].push_back("Unexpected archive name: " + relative);
                    continue;
                }
                const auto number = *index;
                // CasStore keys its directories relative to Win32, so the mod's
                // own Win32/ prefix comes off before the two are matched up.
                auto directory = lower(path.parent_path().generic_string());
                if (directory.starts_with("win32/")) directory.erase(0, 6);
                // The first mod to ship an index keeps it. A later one takes a
                // free index of its own, so the file is hard linked rather
                // than copied and no two mods share an archive's 4 GB of
                // addressable bytes; only with every index taken is it
                // concatenated onto the archive it collides with.
                auto& claimed = claimedArchives[directory];
                ArchivePlacement::Spot spot{number, 0};
                // Archive 1 stays the patch's own file, never a link to a mod's:
                // a live apply appends manifests and new mods to it while the
                // game has it open, and a linked file cannot be swapped for a
                // private copy then (Windows keeps it in use).
                claimed.insert(manifestArchive);
                if (number == manifestArchive) {
                    if (const auto free = claim(directory); free) {
                        spot.archive = *free;
                        link_or_copy(mod->directory / path, output / path.parent_path() / archive_file(*free));
                    } else {
                        spot.offset = append_file(mod->directory / path, output / path);
                    }
                } else if (claimed.insert(number).second) {
                    link_or_copy(mod->directory / path, output / path);
                } else if (const auto free = claim(directory); free) {
                    spot.archive = *free;
                    link_or_copy(mod->directory / path,
                                 output / path.parent_path() / archive_file(*free));
                } else {
                    spot.offset = append_file(mod->directory / path, output / path);
                }
                placements[mod].at.emplace(std::pair{directory, number}, spot);
                // A disabled mod's archives are read by no TOC yet, so nothing
                // else declares them; the engine only accepts archives
                // declared at launch.
                if (!active)
                    for (const auto chunk : store.chunks_in(directory)) used.emplace(chunk, spot.archive);
                if (spot.archive != number || spot.offset)
                    report.notes.push_back(mod->name + ": " + directory + "/cas_" +
                        std::to_string(number) + " placed as cas_" + std::to_string(spot.archive) +
                        (spot.offset ? " at byte " + std::to_string(spot.offset) : std::string{}) +
                        (active ? "" : " (disabled, ready to enable)"));
                ++report.archives;
            }
            record.mods[mod->name] = placements[mod];
        }

        for (const auto relative : launch_superbundles)
            if (providers.try_emplace(lower(relative)).second) superbundles.emplace_back(relative);
        // A mod added while the game runs has no archive index declared for it,
        // so its archives go on the end of the patch's own archive 1 (see the
        // live placement above). Every package gets one from launch, empty if
        // nothing needs it, so that works in any package.
        if (!options.live)
            for (const auto& [directory, declared] : declaredIn) {
                if (!declared.contains(manifestArchive)) continue;
                const auto path = output / L"Win32" / fs::path(directory) / archive_file(manifestArchive);
                if (fs::exists(path, error)) continue;
                fs::create_directories(path.parent_path(), error);
                write_file(path, {});
            }
        lap("linking");

        // Maps that author their own surfaces all number them from the same
        // first free slot of the game's material grid; they are combined into
        // the one grid the game reads before any bundle is merged, so each
        // map's collision can be renumbered as its bundles go by.
        // The root keeps the order it was first merged in: the launch's mods as
        // they were then, and any map installed since after them, so nothing
        // the game already holds from the root moves.
        auto& rootMods = providers[std::string(root_level)];
        if (options.live && !record.root.empty()) {
            std::vector<const Mod*> ordered;
            for (const auto& name : record.root)
                for (const auto* mod : rootMods)
                    if (mod->name == name) ordered.push_back(mod);
            for (const auto* mod : rootMods)
                if (std::find(ordered.begin(), ordered.end(), mod) == ordered.end()) ordered.push_back(mod);
            rootMods = std::move(ordered);
        }
        std::vector<std::string> rootNames;
        for (const auto* mod : rootMods) rootNames.push_back(mod->name);
        // Unchanged since the root was last written (only maps enabled or
        // disabled that were installed at launch): the file stays, byte for byte.
        const bool keepRoot = options.live && rootNames == record.root && !rootNames.empty() &&
                              fs::exists(output / fs::path(root_level), error);
        record.root = rootNames;
        // Without steps of their own, the material grid and the asset overrides showed
        // the last "Linking" for as long as they ran
        // (https://github.com/Dingo-Shenanigans/ReSkate/issues/138).
        advance("Planning the material grid");
        // rootMods belongs to providers; finish using it before erasing its node.
        const auto grid = plan_material_grid(rootMods, modFiles, store, baseRoot, gameRoot, report);
        if (rootMods.empty()) {
            providers.erase(std::string(root_level));
            std::erase_if(superbundles, [](const std::string& relative) { return lower(relative) == root_level; });
        }
        lap("material grid");
        if (options.live) {
            advance("Reading load screens");
            report.load_screens = read_load_screens(mods, modFiles, store, baseRoot, gameRoot, report);
            lap("load screens");
        }
        advance("Collecting asset overrides");
        auto overrides = collect_asset_overrides(mods, modFiles, store, baseRoot, gameRoot, report);
        // Carried chunks point into their mod's archives; move them to where those landed.
        for (auto& [name, chunks] : overrides.chunks)
            for (const auto* mod : mods)
                if (mod->name == name)
                    for (auto& chunk : chunks) store.shift(chunk.location, chunk.offset, &placements[mod]);
        lap("asset overrides");
        // The count above missed the root level when only a disabled map brings it, and
        // that is merged too: from here each superbundle is a step, then the layout.
        progress.total = progress.done + superbundles.size() + 1;

        // Each superbundle is merged by itself: it reads the game's and the mods' files and
        // what was worked out above, and what it writes waits in its own copy of the store
        // (CasStore::waiting). So they are merged on several threads, and settled here one
        // after another in the order they always were, which puts every byte where merging
        // them in turn would have. A merge while the game runs does them in turn, on this
        // thread: the game is using the others.
        struct Job {
            std::string relative;
            std::optional<CasStore> store;   // waiting: what the merge wrote
            MergeReport report;              // its notes, problems and counts
            ArchiveUse used;
            fb::TocDocument merged;
            // The bundles with a file in the waiting store, read back to be moved.
            std::vector<std::pair<std::size_t, fb::BundleRegion>> held;
            std::exception_ptr failure;
            bool done{};
        };
        std::vector<Job> jobs;
        jobs.reserve(superbundles.size());
        for (const auto& relative : superbundles) {
            if (keepRoot && lower(relative) == root_level) continue;
            auto& job = jobs.emplace_back();
            job.relative = relative;
            job.store.emplace(store.waiting());
        }
        // Made here: the jobs only read the map.
        for (const auto& [relative, shippedBy] : providers)
            for (const auto* mod : shippedBy) placements[mod];
        const auto run = [&](Job& job) noexcept {
            try {
                const auto& relative = job.relative;
                std::vector<Source> sources;
                for (const auto* mod : providers.at(lower(relative))) {
                    Source source;
                    source.root = mod->directory;
                    source.placement = &placements.at(mod);
                    try {
                        source.toc = fb::read_toc(read_file(mod->directory / fs::path(relative)));
                    } catch (const std::exception& failure) { // the mod's problem: the catalog merges again without it
                        job.report.problems[mod->name].push_back(relative + " could not be read: " + failure.what());
                        continue;
                    }
                    sources.push_back(std::move(source));
                }
                job.merged = combine(baseRoot / fs::path(relative), baseRoot, sources, job.report,
                                     relative, job.used, *job.store, manifestArchive, gameRoot, grid, overrides);
                if (job.store->holding())
                    for (std::size_t index = 0; index < job.merged.bundles.size(); ++index) {
                        auto region = fb::read_bundle_region(job.merged.bundles[index].region);
                        if (std::ranges::any_of(region.files, [](const fb::BundleFileInfo& file) {
                                return CasStore::waits(file.location); }))
                            job.held.emplace_back(index, std::move(region));
                    }
            } catch (...) {
                job.failure = std::current_exception();
            }
        };
        std::mutex jobsMutex;
        std::condition_variable jobDone;
        std::atomic<std::size_t> nextJob{0};
        std::atomic<bool> stopJobs{false};
        std::vector<std::jthread> workers;
        // Declared after the workers, so it goes first: a merge that fails stops them
        // taking more, and they are joined before anything they use goes.
        struct Stop {
            std::atomic<bool>& flag;
            ~Stop() { flag = true; }
        } stopOnExit{stopJobs};
        const std::size_t wantedWorkers = options.live || jobs.size() < 2 ? 0
            : std::min<std::size_t>({jobs.size(), std::max(1U, std::thread::hardware_concurrency()), 8});
        try {
            // Threads of their own, not the system's pool: the launcher holds the pool's
            // threads back while the game starts (see world_layer_scan.cpp).
            for (std::size_t index = 0; index < wantedWorkers; ++index)
                workers.emplace_back([&] {
                    for (;;) {
                        const auto mine = nextJob.fetch_add(1);
                        if (mine >= jobs.size() || stopJobs) return;
                        run(jobs[mine]);
                        {
                            std::lock_guard lock(jobsMutex);
                            jobs[mine].done = true;
                        }
                        jobDone.notify_all();
                    }
                });
        } catch (const std::system_error&) {} // fewer threads, or none: this one does the rest
        std::size_t settledJobs{};
        for (const auto& relative : superbundles) {
            if (keepRoot && lower(relative) == root_level) {
                advance("Keeping the root level as the game has it");
                ++report.superbundles;
                continue;
            }
            auto& job = jobs[settledJobs++];
            const auto& shippedBy = providers.at(lower(relative));
            advance("Merging " + fs::path(relative).stem().string() +
                    (shippedBy.empty() ? std::string(" (the game's own)")
                     : shippedBy.size() > 1 ? " from " + std::to_string(shippedBy.size()) + " mods"
                                            : " from " + shippedBy.front()->name));
            if (workers.empty()) {
                run(job);
            } else {
                std::unique_lock lock(jobsMutex);
                jobDone.wait(lock, [&] { return job.done; });
            }
            if (job.failure) std::rethrow_exception(job.failure);
            // What it wrote goes into the patch's archives now, and what points at it follows.
            store.settle(*job.store);
            for (auto& [index, region] : job.held) {
                for (auto& file : region.files) job.store->settled(file.location, file.offset);
                job.merged.bundles[index].region = fb::write_bundle_region(region.files, region.inlineManifest);
            }
            for (auto& chunk : job.merged.chunks) job.store->settled(chunk.location, chunk.offset);
            for (const auto& [installChunk, archive] : job.used)
                used.emplace(installChunk, archive == CasStore::waiting_archive ? manifestArchive : archive);
            report.mergedBundles += job.report.mergedBundles;
            report.mergedAssets += job.report.mergedAssets;
            report.notes.insert(report.notes.end(), std::make_move_iterator(job.report.notes.begin()),
                                std::make_move_iterator(job.report.notes.end()));
            for (auto& [mod, problems] : job.report.problems) {
                auto& list = report.problems[mod];
                list.insert(list.end(), std::make_move_iterator(problems.begin()), std::make_move_iterator(problems.end()));
            }
            auto bytes = fb::write_patch_toc(job.merged.bundles, job.merged.chunks, job.merged.flags);
            // Read the result back before publishing it: a TOC whose perfect
            // hash does not resolve would take the game down at load time.
            const auto check = fb::read_toc(bytes);
            fb::verify_toc(check);
            if (check.bundles.size() != job.merged.bundles.size() ||
                check.chunks.size() != job.merged.chunks.size())
                throw std::runtime_error("Merged TOC did not round-trip: " + relative);
            write_file(output / fs::path(relative), bytes);
            ++report.superbundles;
            // Done with: a big merge would otherwise hold every superbundle's until the end.
            job.merged = {};
            job.held = {};
            job.store.reset();
        }

        // A live merge takes out the TOCs of superbundles no enabled mod ships
        // any more, so the next load reads the game's own copy instead.
        {
            std::set<std::string> written;
            for (const auto& relative : superbundles) written.insert(lower(relative));
            if (options.live)
                for (const auto& previous : record.tocs)
                    if (!written.contains(lower(previous)) && fs::remove(output / fs::path(previous), error))
                        report.removed_tocs.push_back(previous);
            record.tocs = superbundles;
            write_placements(placementsPath, record);
        }
        lap("superbundles");

        // The layout lists every superbundle the merged layer now provides.
        advance("Writing the layout");
        auto* list = layout.field("superBundles");
        if (!list || list->type != 1) throw std::runtime_error("layout.toc has no superBundles list");
        std::set<std::string, std::less<>> known;
        for (const auto& row : list->children) {
            const auto* name = row.field("name");
            if (name && name->type == 7) known.emplace(lower(name->text));
        }
        // A mod's own layout.toc. A damaged one is that mod's problem: the merge runs again without it.
        const auto mod_layout = [&](const Mod& mod) -> std::optional<vfs::Layout> {
            try {
                return vfs::read_layout(mod.directory / L"layout.toc");
            } catch (const std::exception& error) {
                report.problems[mod.name].push_back(std::string("layout.toc could not be read: ") + error.what());
                return std::nullopt;
            }
        };
        for (const auto* mod : mods) {
            const auto ownLayout = mod_layout(*mod);
            if (!ownLayout) continue;
            const auto& own = ownLayout->root;
            const auto* ownManifest = own.field("installManifest");
            const auto* ownChunks = ownManifest ? ownManifest->field("installChunks") : nullptr;
            if (!ownChunks) continue;
            for (const auto& chunk : ownChunks->children) {
                const auto* chunkName = chunk.field("name");
                const auto* held = chunk.field("superbundles");
                if (!chunkName || !held) continue;
                for (const auto& entry : held->children)
                    if (entry.type == 7) report.superbundle_chunks.emplace_back(entry.text, chunkName->text);
            }
        }
        for (const auto* mod : layoutSources) {
            const auto otherLayout = mod_layout(*mod);
            if (!otherLayout) continue;
            const auto& other = otherLayout->root;
            const auto* otherList = other.field("superBundles");
            if (!otherList) continue;
            for (const auto& row : otherList->children) {
                const auto* name = row.field("name");
                if (!name || name->type != 7 || !known.emplace(lower(name->text)).second) continue;
                // The list is ordered by name and the engine searches it that
                // way, so an entry appended at the end is never found.
                const auto key = lower(name->text);
                auto at = list->children.begin();
                for (; at != list->children.end(); ++at) {
                    const auto* existing = at->field("name");
                    if (existing && existing->type == 7 && lower(existing->text) > key) break;
                }
                list->children.insert(at, native_db::make_named_record("name", name->text));
                report.notes.push_back("Merged layout adds " + name->text + " from " + mod->name);
            }

            // The superbundle list is only half of it: each install chunk in the
            // manifest names the superbundles it holds, and that is what tells
            // the engine where a bundle's data lives. Without it the level is
            // requested and then waits for content that is never located.
            auto* chunks = layout.field("installManifest")
                ? layout.field("installManifest")->field("installChunks") : nullptr;
            const auto* otherManifest = other.field("installManifest");
            const auto* otherChunks = otherManifest ? otherManifest->field("installChunks") : nullptr;
            if (!chunks || !otherChunks) continue;
            for (const auto& source : otherChunks->children) {
                const auto* sourceName = source.field("name");
                const auto* sourceList = source.field("superbundles");
                if (!sourceName || !sourceList) continue;
                Node* target{};
                for (auto& candidate : chunks->children) {
                    const auto* candidateName = candidate.field("name");
                    if (candidateName && candidateName->text == sourceName->text) { target = &candidate; break; }
                }
                auto* targetList = target ? target->field("superbundles") : nullptr;
                if (!targetList) continue;
                for (const auto& entry : sourceList->children) {
                    if (entry.type != 7) continue;
                    const auto key = lower(entry.text);
                    bool held{};
                    for (const auto& existing : targetList->children)
                        if (existing.type == 7 && lower(existing.text) == key) { held = true; break; }
                    if (held) continue;
                    auto position = targetList->children.begin();
                    for (; position != targetList->children.end(); ++position)
                        if (position->type == 7 && lower(position->text) > key) break;
                    Node added;
                    added.type = 7;
                    added.text = entry.text;
                    // A string node is written from its payload, terminator included.
                    added.owned.assign(entry.text.begin(), entry.text.end());
                    added.owned.push_back(0);
                    targetList->children.insert(position, std::move(added));
                    report.notes.push_back("Merged layout puts " + entry.text + " in " + sourceName->text);
                }
            }
        }
        // Declare the re-indexed archives, and only under the install chunks the
        // merged patch really reads them from: a record for an install chunk
        // that has no such file sends the engine looking for one that is absent.
        const auto declare = [&](const char* field) {
            auto* node = layout.field(field);
            if (!node || node->type != 19) return;
            const auto source = node->payload();
            if (source.size() % 8) throw std::runtime_error(std::string(field) + " is not a record table");
            std::vector<unsigned char> table(source.begin(), source.end());
            std::size_t added{};
            for (const auto& [installChunk, archive] : used) {
                const std::array<unsigned char, 4> id{
                    static_cast<unsigned char>(installChunk & 0xFF),
                    static_cast<unsigned char>((installChunk >> 8) & 0xFF),
                    static_cast<unsigned char>((installChunk >> 16) & 0xFF),
                    static_cast<unsigned char>((installChunk >> 24) & 0xFF)};
                // The table is grouped by install chunk and every group the game
                // ships is ordered by archive index, so a new archive goes inside
                // its own group and in that order, not after it and not last.
                std::size_t insertAt{};
                bool owns{}, already{}, placed{};
                for (std::size_t at = 0; at + 8 <= table.size(); at += 8) {
                    if (std::memcmp(table.data() + at + 2, id.data(), id.size())) continue;
                    owns = true;
                    const auto current = static_cast<std::uint16_t>(table[at] | (table[at + 1] << 8));
                    if (current == archive) already = true;
                    if (!placed && current > archive) { insertAt = at; placed = true; }
                    if (!placed) insertAt = at + 8;
                }
                if (!owns || already) continue;
                std::array<unsigned char, 8> record{};
                record[0] = static_cast<unsigned char>(archive & 0xFF);
                record[1] = static_cast<unsigned char>(archive >> 8);
                std::memcpy(record.data() + 2, id.data(), id.size());
                table.insert(table.begin() + static_cast<std::ptrdiff_t>(insertAt),
                             record.begin(), record.end());
                ++added;
            }
            if (!added) return;
            report.notes.push_back(std::string(field) + ": declared " + std::to_string(added) +
                " patch archive(s)");
            node->owned = std::move(table);
            node->bytes = {};
        };
        declare("layeredInstallChunkFiles");
        declare("unlayeredInstallChunkFiles");

        if (!options.live) {
            const auto rebuilt = native_db::write(layout);
            std::vector<unsigned char> file(native_db::envelope_size, 0);
            std::memcpy(file.data(), native_db::magic, sizeof(native_db::magic));
            file.insert(file.end(), rebuilt.begin(), rebuilt.end());
            write_file(output / L"layout.toc",
                       std::span<const std::byte>(reinterpret_cast<const std::byte*>(file.data()), file.size()));

            // The patch is complete without its stamp; lacking one only means the
            // next launch builds it again.
            try {
                write_stamp(output, fingerprint, report);
            } catch (const std::exception& failure) {
                report.notes.push_back(std::string("The merged patch will be rebuilt next launch: ") +
                                       failure.what());
            }
        }
        lap("layout");
        // After the stamp, which keeps the notes: a patch that is reused must not
        // report the times of the merge that built it.
        const auto seconds = [](Clock::duration elapsed) {
            const auto tenths = (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() + 50) / 100;
            return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " s";
        };
        std::string summary;
        Clock::duration all{};
        for (const auto& [step, elapsed] : times) {
            summary += (summary.empty() ? "Merge times: " : ", ") + step + " " + seconds(elapsed);
            all += elapsed;
        }
        report.notes.push_back(summary + " (" + seconds(all) + " for this merge)");
        report.built = true;
    } catch (const std::exception& failure) {
        report.issue = failure.what();
        // A live merge leaves the running patch alone: the game is reading it.
        std::error_code error;
        if (!options.live) fs::remove_all(catalog.root / generated_folder, error);
    } catch (...) {
        report.issue = "The mod merge failed";
        std::error_code error;
        if (!options.live) fs::remove_all(catalog.root / generated_folder, error);
    }
    return report;
}

} // namespace dingosdk::mods
