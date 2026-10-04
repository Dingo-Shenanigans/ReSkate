#include "mod_merge_internal.h"
#include "Engine/Resource/cas_codec.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Resource/bundle_ref_table.h"
#include "Engine/Resource/ebx_merge.h"
#include "Engine/Resource/ebx_writer.h"
#include "Engine/Resource/shader_lookup.h"
#include "Engine/Core/Platform/path_text.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace dingosdk::mods::detail {
namespace {
// The first bytes of a payload, for a log line: whether they look like a
// bundle manifest, a compressed block or nothing at all tells a reader what a
// mod really put where its manifest was expected.
std::string hex_preview(std::span<const std::byte> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text;
    for (const auto value : bytes) {
        const auto byte = static_cast<unsigned>(value);
        if (!text.empty()) text += ' ';
        text += digits[byte >> 4];
        text += digits[byte & 15];
    }
    return text.empty() ? std::string("(empty)") : text;
}

struct Asset {
    fb::BundleAsset asset;
    fb::BundleFileInfo file;
    bool base{};
    std::size_t regionIndex{};
};

// Region files run parallel to the manifest's ebx, then resources, then chunks.
std::vector<Asset> flatten(const fb::BundleRegion& region, const fb::BinaryBundle& manifest,
                           std::size_t first = 0) {
    std::vector<Asset> result;
    result.reserve(manifest.ebx.size() + manifest.resources.size() + manifest.chunks.size());
    const auto take = [&](const std::vector<fb::BundleAsset>& list) {
        for (const auto& asset : list) {
            const auto index = first + result.size();
            if (index >= region.files.size())
                throw std::runtime_error("Bundle region has fewer files than its manifest lists");
            result.push_back({asset, region.files[index], false, index});
        }
    };
    take(manifest.ebx);
    take(manifest.resources);
    take(manifest.chunks);
    return result;
}

std::string asset_key(const fb::BundleAsset& asset) {
    return std::to_string(static_cast<int>(asset.kind)) + ':' +
        (asset.kind == fb::AssetKind::chunk ? asset.guid.string() : lower(asset.name));
}

// Some bundles carry no inline manifest: their region only lists placements,
// so their assets cannot be named and the bundle can only be passed through
// whole, with the highest-priority copy winning.
struct BundleState {
    bool opaque{};
    // The mod whose copy is being carried whole because it could not be read,
    // and why; its additions vanish if a readable copy later takes over.
    std::string opaqueBy, opaqueWhy;
    // The manifest was read out of cas and has to be written back the same way.
    bool casBacked{};
    std::uint32_t manifestChunk{};
    std::vector<Asset> assets;
    std::vector<fb::BundleFileInfo> files;
    std::vector<std::byte> metadata;
    // Keyed position of each asset, so replacing one stays constant time: these
    // bundles hold tens of thousands of entries.
    std::map<std::string, std::size_t, std::less<>> seen;
    // Every copy of an asset more than one source supplies, base included.
    std::map<std::string, std::vector<Contribution>, std::less<>> history;
    // The base's own sha1 for each asset, so a mod that merely carries an
    // unchanged copy can be told apart from one that changed it.
    std::map<std::string, fb::Sha1, std::less<>> baseSha;
};

// Rebuilds the manifest and the matching placement list in the order the
// format requires: ebx, then resources, then chunks.
void build_manifest(const std::vector<Asset>& assets, std::span<const std::byte> chunkMetadata,
                    fb::BinaryBundle& manifest, std::vector<fb::BundleFileInfo>& files) {
    manifest.chunkMetadata.assign(chunkMetadata.begin(), chunkMetadata.end());
    files.reserve(assets.size());
    const auto emit = [&](fb::AssetKind kind, std::vector<fb::BundleAsset>& into) {
        for (const auto& entry : assets) {
            if (entry.asset.kind != kind) continue;
            into.push_back(entry.asset);
            files.push_back(entry.file);
        }
    };
    emit(fb::AssetKind::ebx, manifest.ebx);
    emit(fb::AssetKind::resource, manifest.resources);
    emit(fb::AssetKind::chunk, manifest.chunks);
}

} // namespace

std::vector<const Contribution*> distinct_edits(const std::vector<Contribution>& history, const Contribution& base) {
    std::vector<const Contribution*> edits;
    std::set<std::pair<fb::Sha1, std::vector<std::byte>>> seen;
    for (auto contribution = history.rbegin(); contribution != history.rend(); ++contribution) {
        if (contribution->base || contribution->sha1 == base.sha1) continue;
        if (seen.emplace(contribution->sha1, contribution->resourceMeta).second) edits.push_back(&*contribution);
    }
    std::reverse(edits.begin(), edits.end());
    return edits;
}

// One superbundle, combined across every mod that ships it. `used` collects the
// patch archives the result actually references, keyed by install chunk, so the
// layout declares those and only those.
fb::TocDocument combine(const fs::path& baseToc, const fs::path& baseRoot,
                        std::vector<Source>& sources, MergeReport& report,
                        const std::string& relative, ArchiveUse& used, CasStore& store,
                        std::uint16_t manifestArchive, const fs::path& gameRoot, const GridPlan& grid,
                        const AssetOverrides& overrides) {
    fb::TocDocument merged;
    merged.flags = 3;

    std::map<std::string, fb::TocDocument, std::less<>> baseHolder;
    const fb::TocDocument* base{};
    std::error_code error;
    if (fs::is_regular_file(baseToc, error) && !error) {
        baseHolder.emplace(relative, fb::read_toc(read_file(baseToc)));
        base = &baseHolder.at(relative);
    }

    // Only bundles more than one mod ships need their contents merged; the rest
    // pass through as they arrived, which keeps the risky path narrow.
    std::map<std::string, std::size_t, std::less<>> providers;
    for (const auto& source : sources)
        for (const auto& bundle : source.toc.bundles) ++providers[lower(bundle.name)];

    // Bundle order follows the base, then anything a mod introduces.
    std::vector<std::string> order;
    std::map<std::string, BundleState, std::less<>> states;

    // Per mod: bundles whose payloads lie outside the archives the mod ships.
    struct ArchiveFault {
        std::size_t bundles{}, missing{}, overrun{};
        std::string example;
    };
    std::map<std::string, ArchiveFault> archiveFaults;

    const auto absorb = [&](const fb::TocBundle& bundle, const ArchivePlacement* placement,
                            bool isBase, const fs::path& root) {
        const auto key = lower(bundle.name);
        auto region = fb::read_bundle_region(bundle.region);
        const auto provider = providers.find(key);
        const auto shared = provider != providers.end() && provider->second > 1;
        const auto modName = isBase ? std::string("base") : path_utf8(root.filename());
        // The bundle holding the live material grid is rebuilt to carry the
        // combined one, and a mod whose surfaces moved has its collision
        // renumbered; both need the asset list even where one mod ships it.
        const auto gridBundle = !grid.payload.empty() && key == grid.bundle;
        const auto renumber = !isBase && grid.rewrites(modName);
        // A mod's copies of assets another mod changed take that change.
        const auto propagate = !isBase && !overrides.empty();
        // Reading the manifest only to look for such copies is optional: a
        // bundle that cannot be read that way just passes through as it is.
        const auto needed = shared || gridBundle || renumber;

        // A mod's payloads have to lie inside the archives it ships. A bundle
        // pointing past the end of one, or into one that is not there, means
        // the folder was truncated or copied from a different build, and the
        // level would wait for data that never arrives.
        if (!isBase) {
            std::size_t missing{}, overrun{};
            std::string example;
            for (const auto& file : region.files) {
                if (!file.location.patch) continue;
                const auto size = store.archive_size(root, file.location);
                if (!size) {
                    ++missing;
                    if (example.empty()) example = store.describe(file.location) + " is not in the mod folder";
                } else if (static_cast<std::uint64_t>(file.offset) + file.size > *size) {
                    ++overrun;
                    if (example.empty())
                        example = std::to_string(file.size) + " bytes at byte " + std::to_string(file.offset) +
                            " lie past the end of " + store.describe(file.location) + " (" +
                            std::to_string(*size) + " bytes long)";
                }
            }
            if (missing || overrun) {
                auto& fault = archiveFaults[modName];
                ++fault.bundles;
                fault.missing += missing;
                fault.overrun += overrun;
                if (fault.example.empty()) fault.example = bundle.name + ": " + example;
            }
        }

        // Read a cas-backed manifest before shifting, while the placement still
        // points at the archive it came from.
        fb::BinaryBundle casManifest;
        bool casBacked{};
        // Why the manifest could not be read, for the problem recorded once the
        // consequence for this copy is known.
        std::string manifestFailure;
        // Best effort: if the manifest cannot be recovered the bundle falls back
        // to being passed through whole, which is what it did before in-bundle
        // merging existed. Losing a merge is far better than losing every mod.
        // Studio writes the manifest as the region's first file, but a mod built
        // by another tool or an older build may keep it elsewhere, so every small
        // file is a candidate; a placement outside the patch lives in the base.
        if (region.inlineManifest.empty() && (needed || propagate) && !region.files.empty()) {
            std::string firstFailure;
            constexpr std::uint32_t largestManifest = 32u << 20;
            const auto expected = region.files.size() - 1;
            for (std::size_t index = 0; index < region.files.size() && !casBacked; ++index) {
                const auto& candidate = region.files[index];
                if (candidate.size > largestManifest) continue;
                try {
                    auto manifest = store.read_manifest(candidate.location.patch ? root : baseRoot,
                                                        candidate.location, candidate.offset,
                                                        candidate.size, gameRoot);
                    const auto assets = manifest.ebx.size() + manifest.resources.size() +
                        manifest.chunks.size();
                    if (assets != expected)
                        throw std::runtime_error("manifest lists " + std::to_string(assets) +
                            " assets but the region holds " + std::to_string(region.files.size()) +
                            " files");
                    casManifest = std::move(manifest);
                    casBacked = true;
                    if (index != 0)
                        report.notes.push_back(modName + ": " + bundle.name + ": manifest found at file " +
                            std::to_string(index) + " of the region");
                } catch (const std::exception& failure) {
                    if (firstFailure.empty()) firstFailure = failure.what();
                }
            }
            if (!casBacked && needed) {
                // Say where the manifest was looked for and what is really
                // there, so a broken mod can be diagnosed from the log alone.
                const auto& first = region.files.front();
                const auto& firstRoot = first.location.patch ? root : baseRoot;
                std::string where = "file 0 of " + std::to_string(region.files.size()) + " is " +
                    std::to_string(first.size) + " bytes at byte " + std::to_string(first.offset) + " of " +
                    store.describe(first.location) + (first.location.patch ? "" : " in the base game");
                if (const auto archive = store.archive_size(firstRoot, first.location); !archive) {
                    where += ", which is missing";
                } else if (static_cast<std::uint64_t>(first.offset) + first.size > *archive) {
                    where += ", which is only " + std::to_string(*archive) + " bytes long";
                } else {
                    try {
                        const auto head = store.read(firstRoot, first.location, first.offset,
                                                     std::min<std::uint32_t>(first.size, 8));
                        where += ", starting " + hex_preview(head);
                    } catch (const std::exception&) {}
                }
                manifestFailure = "its manifest could not be read (" + firstFailure + "); " + where;
                report.notes.push_back(modName + ": " + bundle.name + ": " + manifestFailure);
            }
        }

        // Collision renumbered for the combined material grid goes into the
        // merged patch now, while the placements still point into the mod's
        // own archives.
        std::map<std::size_t, fb::BundleFileInfo> renumbered;
        if (renumber && casBacked) {
            const auto first = 1 + casManifest.ebx.size();
            std::size_t failed{};
            std::string failure;
            for (std::size_t index = 0; index < casManifest.resources.size(); ++index) {
                auto& asset = casManifest.resources[index];
                if (asset.resourceType != fb::material_grid::physicsResourceType) continue;
                const auto* slots = grid.remap_for(modName, asset.name);
                if (!slots || first + index >= region.files.size()) continue;
                try {
                    const auto& file = region.files[first + index];
                    auto payload = fb::decode_cas(store.read(file.location.patch ? root : baseRoot, file.location,
                                                             file.offset, file.size), {gameRoot});
                    if (!fb::material_grid::remap_physics(payload, *slots)) continue;
                    renumbered[first + index] = store.write(file.location.installChunk, manifestArchive,
                                                            fb::encode_cas(payload, {gameRoot}));
                    asset.sha1 = sha1_of(payload);
                    asset.originalSize = payload.size();
                } catch (const std::exception& error) {
                    if (!failed++) failure = asset.name + ": " + error.what();
                }
            }
            if (!renumbered.empty())
                report.notes.push_back(modName + ": " + bundle.name + ": " + std::to_string(renumbered.size()) +
                    " collision resource(s) renumbered for the combined material grid");
            if (failed)
                report.notes.push_back(modName + ": " + bundle.name + ": " + std::to_string(failed) +
                    " collision resource(s) could not be renumbered, e.g. " + failure);
        }

        // The mod's unchanged copies of assets another mod changed: the change
        // goes into the merged patch, next to this bundle's other files.
        std::map<std::size_t, fb::BundleFileInfo> overridden;
        if (propagate && casBacked) {
            std::string example;
            for (std::size_t index = 0; index < casManifest.ebx.size(); ++index) {
                auto& asset = casManifest.ebx[index];
                const auto named = overrides.find(lower(asset.name));
                if (named == overrides.end()) continue;
                const auto change = named->second.find(asset.sha1);
                const auto at = 1 + index;
                if (change == named->second.end() || change->second.mod == modName || at >= region.files.size())
                    continue;
                try {
                    overridden[at] = store.write(region.files[at].location.installChunk, manifestArchive,
                                                 change->second.encoded);
                    asset.sha1 = change->second.sha1;
                    asset.originalSize = change->second.originalSize;
                    if (example.empty()) example = asset.name + " from " + change->second.mod;
                } catch (const std::exception& error) {
                    report.notes.push_back(modName + ": " + bundle.name + ": " + asset.name +
                        " kept the game's copy, the change could not be copied (" + error.what() + ")");
                }
            }
            if (!overridden.empty())
                report.notes.push_back(modName + ": " + bundle.name + ": " + std::to_string(overridden.size()) +
                    " asset(s) take another mod's change, e.g. " + example);
        }
        // Nothing moved in this bundle: it passes through whole, as before.
        if (casBacked && !shared && !gridBundle && renumbered.empty() && overridden.empty()) casBacked = false;

        for (auto& file : region.files) {
            store.shift(file.location, file.offset, placement);
            if (file.location.patch) used.emplace(file.location.installChunk, file.location.archive);
        }
        for (const auto* moved : {&renumbered, &overridden})
            for (const auto& [index, file] : *moved) {
                region.files[index] = file;
                used.emplace(file.location.installChunk, file.location.archive);
            }
        const auto fresh = !states.contains(key);
        if (fresh) { order.push_back(bundle.name); states.emplace(key, BundleState{}); }
        auto& state = states.at(key);

        if (region.inlineManifest.empty() && !casBacked) {
            // A copy whose contents cannot be read can only be passed through whole.
            // Once other providers have merged into this bundle, taking it whole
            // would throw their additions away (every map that shares the root
            // bundle would lose its level), so the unreadable copy is left out
            // instead: only its own additions to this bundle go missing.
            if (!fresh && !state.opaque && !isBase) {
                report.notes.push_back(modName + ": " + bundle.name +
                    ": left out of the merge, so this mod's additions to it are missing; "
                    "rebuild the mod with a current Studio");
                report.problems[modName].push_back(bundle.name + ": left out of the merge because " +
                    manifestFailure + "; this mod's additions to that bundle are missing");
                return;
            }
            if (fresh || !isBase) {
                // One unreadable copy replacing another: the earlier mod's
                // additions go with it.
                if (state.opaque && !state.opaqueBy.empty() && state.opaqueBy != modName)
                    report.problems[state.opaqueBy].push_back(bundle.name + ": replaced by " + modName +
                        "'s copy because " + state.opaqueWhy + "; this mod's additions to that bundle are missing");
                state.opaque = true;
                state.opaqueBy = isBase ? std::string{} : modName;
                state.opaqueWhy = manifestFailure;
                state.files = std::move(region.files);
                state.assets.clear(); state.seen.clear();
            }
            return;
        }
        const auto manifest = casBacked ? casManifest : fb::read_binary_bundle(region.inlineManifest);
        auto flat = flatten(region, manifest, casBacked ? 1 : 0);
        if (state.opaque) {
            // A named copy is more useful than an opaque one, so it takes over.
            if (isBase) return;
            if (!state.opaqueBy.empty() && state.opaqueBy != modName)
                report.problems[state.opaqueBy].push_back(bundle.name + ": replaced by " + modName +
                    "'s readable copy because " + state.opaqueWhy +
                    "; this mod's additions to that bundle are missing");
            state.opaque = false;
            state.opaqueBy.clear();
            state.opaqueWhy.clear();
            state.files.clear();
        }
        if (casBacked) state.casBacked = true;
        if (!region.files.empty()) state.manifestChunk = region.files.front().location.installChunk;
        if (fresh || state.metadata.empty()) state.metadata = manifest.chunkMetadata;
        for (auto& entry : flat) {
            entry.base = isBase;
            auto id = asset_key(entry.asset);
            if (entry.asset.kind == fb::AssetKind::ebx ||
                entry.asset.kind == fb::AssetKind::resource) {
                // If this merge wrote the copy, read it from the patch. Otherwise, read it
                // from its original offset in its own folder.
                const auto written = renumbered.contains(entry.regionIndex) || overridden.contains(entry.regionIndex);
                auto source = read_back(root, store.output(), entry.file, written, placement,
                                        store.find_directory(entry.file.location.installChunk));
                state.history[id].push_back(
                    {std::move(source.root), source.file, entry.asset.sha1, entry.asset.resourceMeta, isBase});
            }
            if (isBase) state.baseSha.insert_or_assign(id, entry.asset.sha1);
            const auto at = state.seen.find(id);
            if (at == state.seen.end()) {
                state.seen.emplace(std::move(id), state.assets.size());
                state.assets.push_back(std::move(entry));
                continue;
            }
            // A mod replacing an asset the base also ships wins over the base,
            // and the highest-priority mod wins over the mods below it.
            if (!isBase) {
                // Map mods rebuild shared bundles wholesale, so they carry the
                // base's copy of every asset they did not touch. That copy must
                // never win over a mod that actually changed the asset -- on
                // priority alone, a map mod's untouched shader tables reverted
                // a costume mod's, and its clothing rendered wrong.
                const auto& previous = state.assets[at->second];
                if (const auto shipped = state.baseSha.find(at->first); shipped != state.baseSha.end() &&
                    entry.asset.sha1 == shipped->second && previous.asset.sha1 != shipped->second)
                    continue;
                // Two mods rewriting one non-EBX asset cannot both be honoured;
                // say so, because the loser silently loses whatever it shipped.
                if (entry.asset.kind == fb::AssetKind::chunk && !previous.base &&
                    previous.asset.sha1 != entry.asset.sha1)
                    report.notes.push_back("contested chunk " + entry.asset.name +
                        ": kept the highest-priority copy");
                state.assets[at->second] = std::move(entry);
            }
        }
    };

    if (base) for (const auto& bundle : base->bundles) absorb(bundle, nullptr, true, baseRoot);
    const auto baseBundles = order.size();
    // Lowest priority first, so the highest-priority mod overwrites the rest.
    for (auto source = sources.rbegin(); source != sources.rend(); ++source)
        for (const auto& bundle : source->toc.bundles)
            absorb(bundle, source->placement, false, source->root);

    for (const auto& name : order) {
        auto& state = states.at(lower(name));
        std::vector<std::byte> region;
        if (state.opaque) {
            region = fb::write_bundle_region(state.files, {});
        } else {
            // Two mods rewriting one EBX asset is the case layering can never
            // answer: whichever copy wins, the other mod's additions are gone.
            // Combining the documents is the only shape that keeps both.
            for (auto& entry : state.assets) {
                const auto shaderTable = entry.asset.kind == fb::AssetKind::resource &&
                    (entry.asset.resourceType == fb::shader::programLookupType ||
                     entry.asset.resourceType == fb::shader::textureLookupType);
                // Cosmetic mods each register their presets in the character
                // bundle-reference table; the winning copy alone would leave the
                // other mods' items pointing at presets the game cannot find.
                const auto refTable = entry.asset.kind == fb::AssetKind::resource &&
                    fb::bundle_ref::is_table(entry.asset.name);
                if (entry.asset.kind != fb::AssetKind::ebx && !shaderTable && !refTable) continue;
                const auto found = state.history.find(asset_key(entry.asset));
                if (found == state.history.end()) continue;
                const Contribution* baseCopy{};
                for (const auto& contribution : found->second)
                    if (contribution.base) { baseCopy = &contribution; break; }
                if (!baseCopy) continue;
                // Both mods rebuild the whole bundle, so only the assets whose
                // content actually moved away from the base are contested.
                const auto edits = distinct_edits(found->second, *baseCopy);
                if (edits.size() < 2) continue;
                try {
                    // A placement that is not a patch one still lives in the
                    // base install, whichever mod's copy referenced it.
                    const auto decode = [&](const Contribution& from) {
                        return fb::decode_cas(
                            store.read(from.file.location.patch ? from.root : baseRoot,
                                       from.file.location, from.file.offset, from.file.size),
                            {gameRoot});
                    };
                    const auto baseBytes = decode(*baseCopy);
                    std::vector<std::vector<std::byte>> editBytes;
                    for (const auto* edit : edits) editBytes.push_back(decode(*edit));

                    std::vector<std::byte> rebuilt;
                    std::string what;
                    if (refTable) {
                        std::vector<fb::bundle_ref::Table> tables;
                        for (std::size_t index = 0; index < editBytes.size(); ++index)
                            tables.push_back({editBytes[index], edits[index]->resourceMeta});
                        const auto table = fb::bundle_ref::merge({baseBytes, baseCopy->resourceMeta}, tables);
                        if (!table.added) continue;
                        rebuilt = table.resource;
                        entry.asset.resourceMeta = table.resourceMeta;
                        what = std::to_string(table.added) + " preset(s)";
                        if (table.conflicts)
                            what += ", " + std::to_string(table.conflicts) + " disagreed";
                    } else if (shaderTable) {
                        // Each mod aliases new material keys onto shader programs
                        // and texture sets the base already ships, so the union of
                        // the rows they added is exactly right.
                        std::vector<fb::shader::Table> tables;
                        for (std::size_t index = 0; index < editBytes.size(); ++index)
                            tables.push_back({editBytes[index], edits[index]->resourceMeta});
                        const fb::shader::Table baseTable{baseBytes, baseCopy->resourceMeta};
                        const auto table = entry.asset.resourceType == fb::shader::programLookupType
                            ? fb::shader::merge_program_lookup(baseTable, tables)
                            : fb::shader::merge_texture_lookup(baseTable, tables);
                        if (!table.added) continue;
                        rebuilt = table.resource;
                        entry.asset.resourceMeta = table.resourceMeta;
                        what = std::to_string(table.added) + " material row(s)";
                        if (table.conflicts)
                            what += ", " + std::to_string(table.conflicts) + " disagreed";
                    } else {
                        const auto baseDocument = fb::ebx::read_document(baseBytes);
                        // A material grid is a square interaction matrix whose rows are
                        // addressed by slot numbers baked into each map's collision.
                        // Two mods that each appended surfaces both claim the same
                        // slots, so a plain merge would hand one map's triangles the
                        // other map's materials. The grid plan combines them instead
                        // and renumbers the collision (mod_merge_grid.cpp).
                        if (lower(baseDocument.rootType).find("materialgrid") != std::string::npos) {
                            if (grid.payload.empty())
                                report.notes.push_back(entry.asset.name + ": " + std::to_string(edits.size()) +
                                    " mods edit the shared material grid; kept the highest-priority copy");
                            continue;
                        }
                        std::vector<fb::ebx::Document> editDocuments;
                        for (const auto& bytes : editBytes)
                            editDocuments.push_back(fb::ebx::read_document(bytes));
                        std::vector<const fb::ebx::Document*> pointers;
                        for (const auto& document : editDocuments) pointers.push_back(&document);
                        fb::ebx::MergeSummary summary;
                        auto combined = fb::ebx::merge_documents(baseDocument, pointers, &summary);
                        if (!summary.instances && !summary.arrayEntries) continue;
                        rebuilt = fb::ebx::write_document(combined);
                        what = std::to_string(summary.instances) + " instance(s), " +
                               std::to_string(summary.arrayEntries) + " list entries";
                    }
                    const auto placement = store.write(state.manifestChunk, manifestArchive,
                                                       fb::encode_cas(rebuilt, {gameRoot}));
                    entry.file = placement;
                    entry.asset.sha1 = sha1_of(rebuilt);
                    entry.asset.originalSize = rebuilt.size();
                    used.emplace(placement.location.installChunk, placement.location.archive);
                    report.notes.push_back(entry.asset.name + ": combined " +
                        std::to_string(edits.size()) + " edits (" + what + ")");
                    ++report.mergedAssets;
                } catch (const std::exception& failure) {
                    report.notes.push_back(entry.asset.name +
                        ": kept the highest-priority copy, the edits could not be combined (" +
                        failure.what() + ")");
                }
            }

            if (!grid.payload.empty() && lower(name) == grid.bundle) {
                const auto found = state.seen.find(std::to_string(static_cast<int>(fb::AssetKind::ebx)) + ':' + grid.asset);
                if (found == state.seen.end()) {
                    report.notes.push_back("material grid: " + grid.asset + " is not in " + name +
                        "; the combined grid was not published");
                } else {
                    auto& entry = state.assets[found->second];
                    entry.file = store.write(state.manifestChunk, manifestArchive,
                                             fb::encode_cas(grid.payload, {gameRoot}));
                    entry.asset.sha1 = sha1_of(grid.payload);
                    entry.asset.originalSize = grid.payload.size();
                    used.emplace(entry.file.location.installChunk, entry.file.location.archive);
                    ++report.mergedAssets;
                }
            }

            fb::BinaryBundle manifest;
            std::vector<fb::BundleFileInfo> files;
            build_manifest(state.assets, state.metadata, manifest, files);
            if (!state.casBacked) {
                region = fb::write_bundle_region(files, fb::write_binary_bundle(manifest));
            } else {
                // The merged manifest goes back into the patch's own archive,
                // stored raw like the ones it replaces, and becomes the region's
                // first placement again.
                const auto placement = store.write(state.manifestChunk, manifestArchive,
                                                   fb::write_binary_bundle(manifest));
                used.emplace(placement.location.installChunk, placement.location.archive);
                files.insert(files.begin(), placement);
                region = fb::write_bundle_region(files, {});
                ++report.mergedBundles;
            }
        }
        merged.bundles.push_back({name, std::move(region), 1});
    }

    std::map<fb::Guid, std::size_t> chunkAt;
    // A chunk entry identical to the base's is a mod carrying it unchanged.
    std::map<fb::Guid, fb::TocChunk> baseChunk;
    if (base) for (const auto& chunk : base->chunks) {
        baseChunk.emplace(chunk.guid, chunk);
        if (chunkAt.emplace(chunk.guid, merged.chunks.size()).second) merged.chunks.push_back(chunk);
    }
    const auto same = [](const fb::TocChunk& left, const fb::TocChunk& right) {
        return left.location == right.location && left.offset == right.offset &&
               left.size == right.size && left.removed == right.removed;
    };
    for (auto source = sources.rbegin(); source != sources.rend(); ++source) {
        for (auto chunk : source->toc.chunks) {
            store.shift(chunk.location, chunk.offset, source->placement);
            if (chunk.location.patch && !chunk.removed)
                used.emplace(chunk.location.installChunk, chunk.location.archive);
            const auto at = chunkAt.find(chunk.guid);
            if (at != chunkAt.end()) {
                const auto shipped = baseChunk.find(chunk.guid);
                const auto carried = shipped != baseChunk.end() && same(chunk, shipped->second);
                const auto changed = shipped != baseChunk.end() && !same(merged.chunks[at->second], shipped->second);
                if (!(carried && changed)) merged.chunks[at->second] = chunk;
                continue;
            }
            chunkAt.emplace(chunk.guid, merged.chunks.size());
            merged.chunks.push_back(chunk);
        }
    }

    if (sources.size() > 1) {
        report.notes.push_back(relative + ": combined " + std::to_string(sources.size()) +
            " mods over " + std::to_string(baseBundles) + " base bundle(s)");
    }
    for (const auto& [modName, fault] : archiveFaults) {
        std::string text = relative + ": " + std::to_string(fault.bundles) + " bundle(s) point outside the "
            "archives this mod ships (" + std::to_string(fault.missing) + " file(s) in archives that are not "
            "there, " + std::to_string(fault.overrun) + " past the end of one); e.g. " + fault.example +
            ". The mod folder is incomplete or from a different build: reinstall it whole";
        report.notes.push_back(modName + ": " + text);
        report.problems[modName].push_back(std::move(text));
    }
    return merged;
}

} // namespace dingosdk::mods::detail
