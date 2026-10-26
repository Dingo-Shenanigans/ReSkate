#include "mod_merge_internal.h"
#include "Engine/Resource/cas_codec.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Resource/ebx_writer.h"
#include "Engine/Core/Platform/path_text.h"

#include <algorithm>
#include <optional>
#include <stdexcept>

namespace dingosdk::mods::detail {
namespace {
namespace grid = fb::material_grid;

// The only grid the game reads: the root level's, through its LevelData.
constexpr std::string_view liveGrid = "levels/game/dingolevel_root/dingolevel_root/materialgrid_win32";
constexpr std::string_view gridBundle = "win32/levels/game/dingolevel_root/dingolevel_root";

}

// A bundle's asset list, found the way the merge reads it: inline, or as the
// cas-backed manifest among the region's small files.
std::optional<Listing> list_bundle(const CasStore& store, const fs::path& root, const fs::path& baseRoot,
                                   const fb::TocBundle& bundle, const fs::path& gameRoot) {
    auto region = fb::read_bundle_region(bundle.region);
    Listing listing;
    if (!region.inlineManifest.empty()) {
        listing.manifest = fb::read_binary_bundle(region.inlineManifest);
        listing.files = std::move(region.files);
        return listing;
    }
    if (region.files.empty()) return std::nullopt;
    const auto expected = region.files.size() - 1;
    for (std::size_t candidate_at = 0; candidate_at < region.files.size(); ++candidate_at) {
        const auto& candidate = region.files[candidate_at];
        if (candidate.size > (32u << 20)) continue;
        try {
            auto manifest = store.read_manifest(candidate.location.patch ? root : baseRoot, candidate.location,
                                                candidate.offset, candidate.size, gameRoot);
            if (manifest.ebx.size() + manifest.resources.size() + manifest.chunks.size() != expected) continue;
            listing.manifest = std::move(manifest);
            listing.files = std::move(region.files);
            // Callers index assets after slot zero. Finding a manifest later in
            // the region must not leave its bytes in an asset's payload slot.
            const auto at = listing.files.begin() + static_cast<std::ptrdiff_t>(candidate_at);
            std::rotate(listing.files.begin(), at, at + 1);
            listing.first = 1;
            return listing;
        } catch (const std::exception&) {}
    }
    return std::nullopt;
}

std::vector<std::byte> read_asset(const CasStore& store, const fs::path& root, const fs::path& baseRoot,
                                  const Listing& listing, std::size_t ebxIndex, const fs::path& gameRoot) {
    const auto& file = listing.files.at(listing.first + ebxIndex);
    return fb::decode_cas(store.read(file.location.patch ? root : baseRoot, file.location, file.offset, file.size),
                          {gameRoot});
}

namespace {

// "levels/game/<map>/materialgrid_win32" (a Studio map's own surfaces) gives
// "<map>"; the live grid's own name, which older tools edited in place, gives "".
std::optional<std::string> grid_owner(const std::string& name) {
    if (name == liveGrid) return std::string{};
    constexpr std::string_view prefix = "levels/game/";
    constexpr std::string_view suffix = "/materialgrid_win32";
    if (!name.starts_with(prefix) || !name.ends_with(suffix)) return std::nullopt;
    const auto map = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
    if (map.empty() || map.find('/') != std::string::npos) return std::nullopt;
    return map;
}

// A level's own bundle, win32/levels/game/<level>/<level>, where a Studio map
// keeps the grid carrying its surfaces.
bool level_bundle(std::string_view name) {
    constexpr std::string_view prefix = "win32/levels/game/";
    if (!name.starts_with(prefix)) return false;
    const auto rest = name.substr(prefix.size());
    const auto slash = rest.find('/');
    if (slash == std::string_view::npos) return false;
    const auto level = rest.substr(0, slash);
    return rest.substr(slash + 1) == level && name != gridBundle;
}

std::string slot_range(std::size_t first, std::size_t count) {
    return count == 1 ? std::to_string(first) : std::to_string(first) + "-" + std::to_string(first + count - 1);
}

} // namespace

bool GridPlan::rewrites(const std::string& mod) const {
    const auto found = remaps.find(mod);
    if (found == remaps.end()) return false;
    for (const auto& remap : found->second)
        if (!remap.slots.identity()) return true;
    return false;
}

const grid::SlotMap* GridPlan::remap_for(const std::string& mod, std::string_view resource) const {
    const auto found = remaps.find(mod);
    if (found == remaps.end() || found->second.empty()) return nullptr;
    const Remap* chosen{};
    if (found->second.size() == 1) {
        chosen = &found->second.front();
    } else {
        // A mod carrying several maps: each map's collision lives under its
        // own folder, world/custom/<map>/...
        const auto name = lower(resource);
        for (const auto& remap : found->second)
            if (!remap.map.empty() && name.find('/' + remap.map + '/') != std::string::npos) { chosen = &remap; break; }
        if (!chosen)
            for (const auto& remap : found->second)
                if (remap.map.empty()) { chosen = &remap; break; }
    }
    return chosen && !chosen->slots.identity() ? &chosen->slots : nullptr;
}

GridPlan plan_material_grid(const std::vector<const Mod*>& mods,
                            const std::map<const Mod*, RelativeFiles>& modFiles, const CasStore& store,
                            const fs::path& baseRoot, const fs::path& gameRoot, MergeReport& report) {
    GridPlan plan;
    plan.bundle = std::string(gridBundle);
    plan.asset = std::string(liveGrid);

    struct Contribution {
        const Mod* mod{};
        std::string map;
        std::string asset;
        fb::Sha1 sha1;
        fb::ebx::Document document;
    };
    std::vector<Contribution> contributions;
    // Lowest priority first, the order the merge layers mods in.
    for (auto at = mods.rbegin(); at != mods.rend(); ++at) {
        const auto* mod = *at;
        const auto modName = path_utf8(mod->directory.filename());
        for (const auto& relative : modFiles.at(mod).tocs) {
            try {
                const auto toc = fb::read_toc(read_file(mod->directory / fs::path(relative)));
                for (const auto& bundle : toc.bundles) {
                    const auto bundleName = lower(bundle.name);
                    const auto rootBundle = bundleName == gridBundle;
                    if (!rootBundle && !level_bundle(bundleName)) continue;
                    if (rootBundle && plan.superbundle.empty()) plan.superbundle = lower(relative);
                    const auto listing = list_bundle(store, mod->directory, baseRoot, bundle, gameRoot);
                    if (!listing) continue;
                    for (std::size_t index = 0; index < listing->manifest.ebx.size(); ++index) {
                        const auto& asset = listing->manifest.ebx[index];
                        const auto name = lower(asset.name);
                        auto owner = grid_owner(name);
                        if (!owner) continue;
                        auto document = fb::ebx::read_document(
                            read_asset(store, mod->directory, baseRoot, *listing, index, gameRoot));
                        if (document.rootType != "MaterialGridData") continue;
                        contributions.push_back({mod, std::move(*owner), name, asset.sha1, std::move(document)});
                    }
                }
            } catch (const std::exception& failure) {
                report.notes.push_back(modName + ": its material grid could not be read (" + failure.what() + ")");
            }
        }
    }
    if (contributions.empty() || plan.superbundle.empty()) return plan;

    // The game's own grid is what every copy was cloned from.
    std::optional<fb::ebx::Document> base;
    fb::Sha1 baseSha;
    {
        const auto baseToc = baseRoot / fs::path(plan.superbundle);
        const auto toc = fb::read_toc(read_file(baseToc));
        for (const auto& bundle : toc.bundles) {
            if (lower(bundle.name) != gridBundle) continue;
            const auto listing = list_bundle(store, baseRoot, baseRoot, bundle, gameRoot);
            if (!listing) break;
            for (std::size_t index = 0; index < listing->manifest.ebx.size(); ++index)
                if (lower(listing->manifest.ebx[index].name) == liveGrid) {
                    base = fb::ebx::read_document(read_asset(store, baseRoot, baseRoot, *listing, index, gameRoot));
                    baseSha = listing->manifest.ebx[index].sha1;
                }
            break;
        }
    }
    if (!base) {
        report.notes.push_back("material grid: the game's own grid was not found; custom map surfaces are not combined");
        return plan;
    }

    // Every map mod carries the root bundle, and with it an untouched copy of
    // the live grid; only a copy that differs adds anything.
    std::erase_if(contributions, [&](const Contribution& contribution) {
        return contribution.map.empty() && contribution.sha1 == baseSha;
    });
    if (contributions.empty()) return plan;

    grid::Combiner combiner(*base);
    std::size_t combined{};
    for (auto& contribution : contributions) {
        const auto modName = path_utf8(contribution.mod->directory.filename());
        const auto before = combiner.slots();
        grid::SlotMap slots;
        try {
            slots = combiner.add(contribution.document);
        } catch (const std::exception& failure) {
            // Its collision still names slots past the live grid, which crash
            // the game on contact; the default surface is the safe reading. Not
            // a merge problem: those leave the whole mod out, and the map
            // itself is fine.
            slots = grid::SlotMap::to_default(combiner.base_slots(), 0x2000);
            report.notes.push_back(modName + ": " + contribution.asset + ": its custom surfaces could not be combined "
                "into the game's material grid (" + failure.what() + "); they behave as the default surface");
            plan.remaps[modName].push_back({contribution.map, std::move(slots)});
            continue;
        }
        const auto added = combiner.slots() - before;
        if (added) {
            ++combined;
            const auto own = combiner.base_slots();
            report.notes.push_back("material grid: " + modName + " adds surface slot(s) " + slot_range(own, added) +
                (before == own ? std::string{} : ", renumbered to " + slot_range(before, added)));
        }
        plan.remaps[modName].push_back({contribution.map, std::move(slots)});
    }
    if (!combiner.added_slots()) return plan;

    const auto document = combiner.result();
    auto payload = fb::ebx::write_document(document);
    const auto check = fb::ebx::read_document(payload);
    if (check.fileGuid != base->fileGuid || !check.root() || check.root()->instanceGuid != base->root()->instanceGuid)
        throw std::runtime_error("the combined material grid changed identity");
    plan.payload = std::move(payload);
    report.notes.push_back("material grid: " + std::to_string(combiner.added_slots()) + " surface slot(s) from " +
        std::to_string(combined) + " mod(s) combined into the game's grid (" +
        std::to_string(combiner.carried_instances()) + " relation(s) carried, " +
        std::to_string(combiner.reused_instances()) + " matched to the game's)");
    return plan;
}

} // namespace dingosdk::mods::detail
