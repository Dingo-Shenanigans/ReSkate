#pragma once

#include "mod_catalog.h"
#include "native_db.h"
#include "game_archives.h"
#include "Engine/Resource/material_grid.h"
#include "Engine/Resource/toc.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Shared by the mod merge's source files; nothing outside them uses it.
namespace dingosdk::mods::detail {
namespace fs = std::filesystem;
namespace fb = frostbite;

std::string lower(std::string_view text);

std::vector<std::byte> read_file(const fs::path& path);
void write_file(const fs::path& path, std::span<const std::byte> bytes);
using vfs::archive_file;
void unshare(const fs::path& path);
std::uint64_t append_file(const fs::path& from, const fs::path& to);
void link_or_copy(const fs::path& from, const fs::path& to);

struct RelativeFiles {
    std::vector<std::string> tocs;     // Win32/... .toc, generic separators
    std::vector<std::string> archives; // Win32/... cas_NN.cas
};

RelativeFiles scan(const fs::path& directory);

// Every mod is built against archive 1, so the merge moves each mod's archives
// to indices of their own and rewrites the mod's patch references to match. A
// placement addresses its payload with a 32-bit offset, so an archive holds at
// most 4 GB: one index per mod archive is what keeps the merged patch from
// having a size limit of its own.
struct ArchivePlacement {
    // Where a mod's archive ended up: an index of its own, which costs nothing
    // to place, or a byte offset inside an archive it shares. Sharing is for a
    // mod added while the game runs, which has no declared index to take, and
    // for a package directory with no index left.
    struct Spot {
        std::uint16_t archive{};
        std::uint64_t offset{};
    };
    std::map<std::pair<std::string, std::uint16_t>, Spot> at;
};

// Reverses CasStore::shift: turns a placement in the merged patch back into the
// mod's own archive number and offset there. Several of a mod's archives can
// share one patch archive (a mod added while the game runs has them all
// appended to archive 1), so the block is the one the offset falls in, not the
// first in that archive. False when the mod has no block there.
bool unshift(const ArchivePlacement& placement, std::string_view directory, std::uint16_t& archive,
             std::uint32_t& offset);

fb::Sha1 sha1_of(std::span<const std::byte> bytes);

// Where the launch's merge put every installed mod's archives, and which
// superbundle TOCs the patch holds: Mods/.reskate/reskate-placements.json.
inline constexpr wchar_t placements_file[] = L"reskate-placements.json";
struct PlacementRecord {
    std::map<std::string, ArchivePlacement, std::less<>> mods;
    std::vector<std::string> tocs;
    // The mods the root level's TOC was last merged from, in merge order.
    // A live merge keeps that order, so enabling or disabling a map leaves the
    // root exactly as the game has already read it.
    std::vector<std::string> root;
};
PlacementRecord read_placements(const fs::path& file);
void write_placements(const fs::path& file, const PlacementRecord& record);

// `storeKnown`: the content cache that says what the game's store sells is
// installed, so the mods were checked for copies of store items; a patch built
// without it is built again once it is there.
std::string merge_fingerprint(const Catalog& catalog, const std::vector<const Mod*>& mods,
                              const std::map<const Mod*, RelativeFiles>& modFiles, bool storeKnown);
inline constexpr wchar_t stamp_file[] = L"reskate-merge.stamp";
std::optional<MergeReport> previous_merge(const fs::path& output, const std::string& fingerprint);
void write_stamp(const fs::path& output, const std::string& fingerprint, const MergeReport& report);

using vfs::for_each_install_chunk_file;

struct Source {
    fs::path root;
    const ArchivePlacement* placement{};
    fb::TocDocument toc;
};

// No bundle in this game keeps its manifest inline: the asset list is the
// region's first file, stored in cas like any other payload. Merging inside a
// bundle therefore means decoding that payload, and writing one back.
class CasStore final : public vfs::GameArchives {
public:
    CasStore(fs::path baseRoot, fs::path output, const native_db::Node& layout);

    // Appends an encoded payload to the merged patch's own archive for that
    // install chunk, returning where it landed.
    [[nodiscard]] fb::BundleFileInfo write(std::uint32_t installChunk, std::uint16_t archive,
                                           std::span<const std::byte> encoded);

    void shift(fb::CasIdentifier& location, std::uint32_t& offset,
               const ArchivePlacement* placement) const;

private:
    fs::path output_;
    std::map<std::wstring, std::uint64_t> offsets_;
};

using ArchiveUse = std::set<std::pair<std::uint32_t, std::uint16_t>>;

// A bundle's asset list and where each asset's payload lives.
struct Listing {
    fb::BinaryBundle manifest;
    std::vector<fb::BundleFileInfo> files;
    std::size_t first{};  // region index of the manifest's first asset
};
// The asset list, found the way the merge reads it: inline, or as the
// cas-backed manifest among the region's small files. `root` is the layer the
// bundle came from; files it does not patch are read from `baseRoot`.
std::optional<Listing> list_bundle(const CasStore& store, const fs::path& root, const fs::path& baseRoot,
                                   const fb::TocBundle& bundle, const fs::path& gameRoot);
// The manifest's `ebxIndex`th EBX asset, decoded.
std::vector<std::byte> read_asset(const CasStore& store, const fs::path& root, const fs::path& baseRoot,
                                  const Listing& listing, std::size_t ebxIndex, const fs::path& gameRoot);

// The loading screens the mods add (the rows of the game's loading-screen
// configuration each one appends). Never throws; an unreadable mod is noted.
std::vector<MergeReport::LoadScreen> read_load_screens(const std::vector<const Mod*>& mods,
                                                       const std::map<const Mod*, RelativeFiles>& modFiles,
                                                       const CasStore& store, const fs::path& baseRoot,
                                                       const fs::path& gameRoot, MergeReport& report);

// The physics material grid every map's collision addresses (see
// Engine/Resource/material_grid.h). Maps that author their own surfaces each
// ship a copy of the game's grid with additions from the same first free slot;
// the merge combines those into the one grid the game reads and renumbers each
// map's collision to where its surfaces landed.
struct GridPlan {
    struct Remap {
        std::string map;                        // the level folder the grid came from; empty for the root's own name
        fb::material_grid::SlotMap slots;
    };
    std::string superbundle;                    // lower-case TOC path holding the grid's bundle
    std::string bundle;                         // lower-case bundle name
    std::string asset;                          // the live grid's asset name
    std::vector<std::byte> payload;             // the combined grid; empty when no mod adds surfaces
    std::map<std::string, std::vector<Remap>> remaps;  // by mod folder name

    [[nodiscard]] bool rewrites(const std::string& mod) const;
    // How a physics resource of `mod` has to be renumbered; null when it keeps its slots.
    [[nodiscard]] const fb::material_grid::SlotMap* remap_for(const std::string& mod, std::string_view resource) const;
};

// Reads every mod's copy of the grid and the game's own, and combines them.
// Never throws for one bad mod: its surfaces fall back to the default one.
GridPlan plan_material_grid(const std::vector<const Mod*>& mods,
                            const std::map<const Mod*, RelativeFiles>& modFiles, const CasStore& store,
                            const fs::path& baseRoot, const fs::path& gameRoot, MergeReport& report);

// An EBX asset a mod changed from the game's own copy. Maps carry the game's
// copies of what their levels need (a level's camera framing, for one), and
// those copies have to take the change too, or the mod works on the game's
// levels and not on the maps.
struct AssetOverride {
    std::string mod;
    fb::Sha1 sha1;                    // the mod's version
    std::uint64_t originalSize{};
    std::vector<std::byte> encoded;   // its payload, as stored in cas
    // Present for a Lua resource replacement: its identity and metadata must
    // travel with the script bytes, including source and bytecode sizes.
    std::optional<fb::BundleAsset> resource;
    // The documents the changed EBX refers to that its mod added to the bundle it
    // made the change in, by their guids. They go wherever the change goes: the
    // game keeps a list in more bundles than one, each with what the list names.
    std::set<fb::Guid> names;
};
inline constexpr std::uint32_t luaScriptResourceType = 0xEC383B87;
// An EBX asset a mod added to one of the game's bundles, or a resource it added
// under the same name as such an asset (a wave's sound-bank). Maps carry their own
// copy of a bundle like bam_coregameassets, and that copy loads instead of the
// game's on their levels; an edit to a list there (the master music playlist)
// propagates as a change, so the asset it now names has to come along too. The
// game also keeps such a list in more bundles than the one the mod put its
// assets in, and a copy of any of those takes the change the same way.
struct AssetAddition {
    std::string mod;
    fb::BundleAsset asset;
    std::vector<std::byte> encoded;   // its payload, as stored in cas
    // The lower-case relative path of the TOC the mod ships this bundle in. A copy of the
    // bundle in that same TOC merges with the mod's own bundle and already has the asset;
    // only copies in other superbundles (a map's level TOC) need it carried.
    std::string toc;
    // For an EBX: its document's guid, which is what a list names it by, and the
    // guids of the documents it refers to in turn (a song names its wave).
    fb::Guid file;
    std::vector<fb::Guid> names;
};
struct AssetOverrides {
    // By lower-case asset name, then the game's sha1 the change replaces.
    std::map<std::string, std::map<fb::Sha1, AssetOverride>, std::less<>> changed;
    // Separate from EBX: a script's EBX wrapper can have the same asset name.
    std::map<std::string, std::map<fb::Sha1, AssetOverride>, std::less<>> scripts;
    // By lower-case bundle name, in priority order.
    std::map<std::string, std::vector<AssetAddition>, std::less<>> added;
    // By mod folder name: TOC chunks the mod adds that its carried additions name
    // (a new wave's audio). A map's superbundle resolves chunks from its own TOC,
    // so wherever the additions go these entries go too. Collected pointing into
    // the mod's own archives; the merge shifts them to where those archives landed.
    std::map<std::string, std::vector<fb::TocChunk>, std::less<>> chunks;

    [[nodiscard]] bool empty() const noexcept { return changed.empty() && scripts.empty() && added.empty(); }
};

// The changes and additions asset mods (mods that add no levels) make to the
// game's own EBX and Lua scripts, the highest-priority mod's version winning. Never throws: an
// unreadable mod is noted and simply changes nothing elsewhere.
AssetOverrides collect_asset_overrides(const std::vector<const Mod*>& mods,
                                       const std::map<const Mod*, RelativeFiles>& modFiles,
                                       const CasStore& store, const fs::path& baseRoot,
                                       const fs::path& gameRoot, MergeReport& report);

fb::TocDocument combine(const fs::path& baseToc, const fs::path& baseRoot,
                        std::vector<Source>& sources, MergeReport& report,
                        const std::string& relative, ArchiveUse& used, CasStore& store,
                        std::uint16_t manifestArchive, const fs::path& gameRoot, const GridPlan& grid,
                        const AssetOverrides& overrides);

} // namespace dingosdk::mods::detail
