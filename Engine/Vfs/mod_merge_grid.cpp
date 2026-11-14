#include "mod_merge_internal.h"
#include "Engine/Resource/cas_codec.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Resource/ebx_writer.h"
#include "Engine/Core/Platform/path_text.h"
#include "Engine/Core/Text/hex.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
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

// -- The combined grid, kept between merges ---------------------------------
//
// What combining gives is decided by the game's grid and the mods' own grids in their order,
// each as its mod stores it, and by the ReSkate that combines them: `key` is a hash of those.
// The file holds the key, what the combining said, each mod's renumbering and the grid itself.
constexpr std::string_view grid_cache_header = "ReSkate grid 1";

struct KeptGrid {
    std::vector<std::string> notes;
    std::map<std::string, std::vector<GridPlan::Remap>> remaps;
    std::vector<std::byte> payload;
};

class Writer {
public:
    void number(std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) bytes_.push_back(static_cast<char>((value >> shift) & 0xFF));
    }
    void text(std::string_view value) {
        number(value.size());
        bytes_.append(value);
    }
    [[nodiscard]] const std::string& bytes() const noexcept { return bytes_; }

private:
    std::string bytes_;
};

// Throws when the bytes end early or give a size they cannot hold.
class Reader {
public:
    explicit Reader(std::string_view bytes) : bytes_(bytes) {}
    std::uint64_t number() {
        if (bytes_.size() - at_ < 8) throw std::runtime_error("truncated");
        std::uint64_t value{};
        for (int shift = 0; shift < 64; shift += 8) value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes_[at_++])) << shift;
        return value;
    }
    std::size_t count(std::size_t each = 1) {
        const auto value = number();
        if (value > (bytes_.size() - at_) / each) throw std::runtime_error("truncated");
        return static_cast<std::size_t>(value);
    }
    std::string_view text() {
        const auto size = count();
        const auto value = bytes_.substr(at_, size);
        at_ += size;
        return value;
    }

private:
    std::string_view bytes_;
    std::size_t at_{};
};

// Nothing when the file is missing, cannot be read or was kept for other grids.
std::optional<KeptGrid> read_kept_grid(const fs::path& file, std::string_view key) noexcept {
    try {
        std::ifstream input(file, std::ios::binary);
        if (!input) return std::nullopt;
        const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        Reader reader(bytes);
        if (reader.text() != grid_cache_header || reader.text() != key) return std::nullopt;
        KeptGrid kept;
        for (auto notes = reader.count(8); notes--;) kept.notes.emplace_back(reader.text());
        for (auto mods = reader.count(8); mods--;) {
            auto& list = kept.remaps[std::string(reader.text())];
            for (auto remaps = reader.count(8); remaps--;) {
                GridPlan::Remap remap;
                remap.map = reader.text();
                remap.slots.slots.resize(reader.count(8));
                for (auto& slot : remap.slots.slots) slot = static_cast<std::uint32_t>(reader.number());
                list.push_back(std::move(remap));
            }
        }
        const auto payload = reader.text();
        kept.payload.resize(payload.size());
        if (!payload.empty()) std::memcpy(kept.payload.data(), payload.data(), payload.size());
        return kept;
    } catch (...) {
        return std::nullopt;
    }
}

// Never throws: a grid that could not be kept is combined again next time.
void write_kept_grid(const fs::path& file, std::string_view key, const KeptGrid& kept) noexcept {
    try {
        Writer writer;
        writer.text(grid_cache_header);
        writer.text(key);
        writer.number(kept.notes.size());
        for (const auto& note : kept.notes) writer.text(note);
        writer.number(kept.remaps.size());
        for (const auto& [mod, list] : kept.remaps) {
            writer.text(mod);
            writer.number(list.size());
            for (const auto& remap : list) {
                writer.text(remap.map);
                writer.number(remap.slots.slots.size());
                for (const auto slot : remap.slots.slots) writer.number(slot);
            }
        }
        writer.text({reinterpret_cast<const char*>(kept.payload.data()), kept.payload.size()});
        write_file(file, std::as_bytes(std::span(writer.bytes())));
    } catch (...) {}
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
        const auto name = ascii_lower(resource);
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
                            const fs::path& baseRoot, const fs::path& gameRoot, MergeReport& report,
                            std::size_t threads, bool background, const fs::path& kept) {
    GridPlan plan;
    plan.bundle = std::string(gridBundle);
    plan.asset = std::string(liveGrid);

    // A grid one of a mod's bundles holds, as stored: found by going through the asset list of
    // the mod's root bundle and of each of its levels' own, which is the reading that takes time
    // (every map carries the root bundle) and is done for several mods at once.
    struct Copy {
        std::size_t toc{};   // which of the mod's TOCs it is in
        std::string map;
        std::string asset;
        fb::Sha1 sha1;
        std::vector<std::byte> encoded;
    };
    struct Read {
        std::vector<Copy> copies;
        std::string superbundle;                              // the first of its TOCs with the root bundle
        std::vector<std::pair<std::size_t, std::string>> failures;   // a TOC that could not be read to its end, and why
    };
    std::vector<Read> reads(mods.size());
    for_each_index(mods.size(), threads, background, [&](std::size_t at) {
        const auto* mod = mods[at];
        auto& read = reads[at];
        const auto& tocs = modFiles.at(mod).tocs;
        for (std::size_t tocAt = 0; tocAt < tocs.size(); ++tocAt) {
            const auto& relative = tocs[tocAt];
            try {
                const auto toc = fb::read_toc(read_file(mod->directory / fs::path(relative)));
                for (const auto& bundle : toc.bundles) {
                    const auto bundleName = ascii_lower(bundle.name);
                    const auto rootBundle = bundleName == gridBundle;
                    if (!rootBundle && !level_bundle(bundleName)) continue;
                    if (rootBundle && read.superbundle.empty()) read.superbundle = ascii_lower(relative);
                    const auto listing = list_bundle(store, mod->directory, baseRoot, bundle, gameRoot);
                    if (!listing) continue;
                    for (std::size_t index = 0; index < listing->manifest.ebx.size(); ++index) {
                        const auto& asset = listing->manifest.ebx[index];
                        auto name = ascii_lower(asset.name);
                        auto owner = grid_owner(name);
                        if (!owner) continue;
                        const auto& file = listing->files.at(listing->first + index);
                        read.copies.push_back({tocAt, std::move(*owner), std::move(name), asset.sha1,
                            store.read(file.location.patch ? mod->directory : baseRoot, file.location, file.offset, file.size)});
                    }
                }
            } catch (const std::exception& failure) {
                read.failures.emplace_back(tocAt, failure.what());
            } catch (...) {
                read.failures.emplace_back(tocAt, "it could not be read");
            }
        }
    });
    // Lowest priority first, the order the merge layers mods in.
    for (auto read = reads.rbegin(); read != reads.rend(); ++read)
        if (plan.superbundle.empty()) plan.superbundle = read->superbundle;

    // The game's own grid is what every copy was cloned from.
    std::optional<fb::ebx::Document> base;
    fb::Sha1 baseSha;
    const auto read_base = [&] {
        const auto baseToc = baseRoot / fs::path(plan.superbundle);
        const auto toc = fb::read_toc(read_file(baseToc));
        for (const auto& bundle : toc.bundles) {
            if (ascii_lower(bundle.name) != gridBundle) continue;
            const auto listing = list_bundle(store, baseRoot, baseRoot, bundle, gameRoot);
            if (!listing) break;
            for (std::size_t index = 0; index < listing->manifest.ebx.size(); ++index)
                if (ascii_lower(listing->manifest.ebx[index].name) == liveGrid) {
                    base = fb::ebx::read_document(read_asset(store, baseRoot, baseRoot, *listing, index, gameRoot));
                    baseSha = listing->manifest.ebx[index].sha1;
                }
            break;
        }
    };
    // Read before the mods' copies are, to tell which of them are the game's own: a failure
    // waits until it is known that a mod has a grid to combine at all.
    std::exception_ptr baseFailure;
    if (!plan.superbundle.empty()) try { read_base(); } catch (...) { baseFailure = std::current_exception(); }

    // The grid these copies combine into, when it was kept: only when every one of them was
    // read, and the game's own with them, so that what is kept is what combining them says.
    std::string key;
    if (!kept.empty() && base && !baseFailure &&
        std::ranges::all_of(reads, [](const Read& read) { return read.failures.empty(); })) {
        std::string inputs = "sdk " + sdk_stamp() + "\nbase " + hex_bytes(baseSha.bytes) + '\n';
        for (std::size_t at = mods.size(); at-- > 0;)
            for (const auto& copy : reads[at].copies) {
                if (copy.map.empty() && copy.sha1 == baseSha) continue;
                inputs += path_utf8(mods[at]->directory.filename()) + '|' + copy.map + '|' + copy.asset + '|' +
                          hex_bytes(copy.sha1.bytes) + '|' + hex_bytes(sha1_of(copy.encoded).bytes) + '\n';
            }
        key = hex_bytes(sha1_of(std::as_bytes(std::span(inputs))).bytes);
        if (auto found = read_kept_grid(kept, key)) {
            for (auto& note : found->notes) report.notes.push_back(std::move(note));
            plan.remaps = std::move(found->remaps);
            plan.payload = std::move(found->payload);
            return plan;
        }
    }
    const auto firstNote = report.notes.size();
    const auto keep = [&]() -> GridPlan& {
        if (!key.empty())
            write_kept_grid(kept, key, {{report.notes.begin() + static_cast<std::ptrdiff_t>(firstNote), report.notes.end()},
                                        plan.remaps, plan.payload});
        return plan;
    };

    struct Contribution {
        const Mod* mod{};
        std::string map;
        std::string asset;
        fb::Sha1 sha1;
        fb::ebx::Document document;
    };
    std::vector<Contribution> contributions;
    for (std::size_t at = mods.size(); at-- > 0;) {
        const auto* mod = mods[at];
        auto& read = reads[at];
        const auto modName = path_utf8(mod->directory.filename());
        const auto& tocs = modFiles.at(mod).tocs;
        auto copy = read.copies.begin();
        for (std::size_t tocAt = 0; tocAt < tocs.size(); ++tocAt) {
            try {
                for (; copy != read.copies.end() && copy->toc == tocAt; ++copy) {
                    // Every map mod carries the root bundle, and with it an untouched copy of the
                    // live grid; only a copy that differs adds anything, and only such a copy is
                    // decoded: a hundred maps' copies of the game's grid were ten seconds of a
                    // merge (2026-11-14).
                    if (base && copy->map.empty() && copy->sha1 == baseSha) continue;
                    auto document = fb::ebx::read_document(fb::decode_cas(copy->encoded, {gameRoot}));
                    if (document.rootType != "MaterialGridData") continue;
                    contributions.push_back({mod, std::move(copy->map), std::move(copy->asset), copy->sha1, std::move(document)});
                }
                for (const auto& [failed, why] : read.failures)
                    if (failed == tocAt) report.notes.push_back(modName + ": its material grid could not be read (" + why + ")");
            } catch (const std::exception& failure) {
                // (The rest of that TOC is not looked at, as when it could not be read.)
                while (copy != read.copies.end() && copy->toc == tocAt) ++copy;
                report.notes.push_back(modName + ": its material grid could not be read (" + failure.what() + ")");
            }
        }
    }
    if (contributions.empty() || plan.superbundle.empty()) return keep();
    if (baseFailure) std::rethrow_exception(baseFailure);
    if (!base) {
        report.notes.push_back("material grid: the game's own grid was not found; custom map surfaces are not combined");
        return plan;
    }
    std::erase_if(contributions, [&](const Contribution& contribution) {
        return contribution.map.empty() && contribution.sha1 == baseSha;
    });
    if (contributions.empty()) return keep();

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
    if (!combiner.added_slots()) return keep();

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
    return keep();
}

} // namespace dingosdk::mods::detail
