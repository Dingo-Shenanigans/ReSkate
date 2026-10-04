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

std::string merge_fingerprint(const Catalog& catalog, const std::vector<const Mod*>& mods,
                              const std::map<const Mod*, RelativeFiles>& modFiles);
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

    [[nodiscard]] const fs::path& output() const noexcept { return output_; }

private:
    fs::path output_;
    std::map<std::wstring, std::uint64_t> offsets_;
};

// Reverses CasStore::shift. Returns false when the placement has no block in that archive.
bool unshift(const ArchivePlacement& placement, std::string_view directory, std::uint16_t& archive,
             std::uint32_t& offset);

// Where one contributor's copy of an asset lives, so conflicting copies can be
// fetched and combined later.
struct Contribution {
    fs::path root;
    fb::BundleFileInfo file;   // where to read the copy: its own archive, or the patch
    fb::Sha1 sha1;
    std::vector<std::byte> resourceMeta;
    bool base{};
};

// The location to read a copy from: the patch if this merge wrote the copy, else the copy's own folder.
struct ReadBack {
    fs::path root;
    fb::BundleFileInfo file;
};
ReadBack read_back(const fs::path& contributor, const fs::path& output, fb::BundleFileInfo file, bool written,
                   const ArchivePlacement* placement, const std::string* directory);

// Returns the copies that differ from the base copy, lowest priority first. Identical copies
// count once, at the last one, so the highest-priority copy still wins a tie.
std::vector<const Contribution*> distinct_edits(const std::vector<Contribution>& history, const Contribution& base);

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
};
// By lower-case asset name, then the game's sha1 the change replaces.
using AssetOverrides = std::map<std::string, std::map<fb::Sha1, AssetOverride>, std::less<>>;

// The changes asset mods (mods that add no levels) make to the game's own EBX,
// the highest-priority mod's change winning. Never throws: an unreadable mod
// is noted and simply changes nothing elsewhere.
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
