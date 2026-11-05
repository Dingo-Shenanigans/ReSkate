#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Skate Synchros: the outfits a viewer was sent, made into a mod the merge loads
// (contrib/outfit-sharing.md, "Showing a received item").
//
// Everything Skate Synchros keeps is under Mods/.synchros: not Mods/.reskate, which every merge
// empties, and a dot folder, which is never listed as a mod.
//   Mods/.synchros/cache/        the received files (Extension/Multiplayer/Synchros/received.h)
//   Mods/.synchros/mods/<id>.<n>/  one mod per wearer, a new folder for each version
//
// A version is never rewritten in place: a merge while the game runs keeps where it put a mod's
// archives by the mod's name (mod_merge.cpp), so a changed mod under the same name would be read
// at the old places. Each version is a mod of its own (synchros-<id>.<n>); the older ones go.
//
// One mod per wearer, so a wearer whose files
// cannot be merged (or that the store-copy check refuses) loses only their own outfit. The mods are
// added to the catalogue after every installed mod and ReSkate's own packs, so none of them ever
// wins over anything the player installed, and they go through the same merge and the same
// store-copy check (merge check 0x5343) as an installed mod.
//
// Each bundle a received file belongs to is written as a mod writes its copy of it: the game's own
// bundle with the received assets put in (in place of the game's where they replace one), its
// payloads pointing into the game's archives except for the received ones, which are in the mod's
// own. A bundle-reference table the received presets go into is the game's own with those rows
// added (bundle_ref_table.h, with_presets).
namespace dingosdk::synchros {

// The file and row descriptions as they arrived (Extension/Multiplayer/Synchros/synchros_wire.h),
// in the engine's own terms.
struct OutfitFile {
    enum Kind : std::uint8_t { ebx = 0, resource = 1, chunk = 2, toc_chunk = 3 } kind{};
    std::array<std::uint8_t, 20> id{};      // SHA-1 of the stored bytes: where the cache keeps them
    std::uint32_t size{};
    std::string name, bundle, superbundle;
    std::uint64_t original_size{};
    std::uint32_t resource_type{};
    std::uint64_t resource_id{};
    std::vector<std::uint8_t> resource_meta;
    std::array<std::uint8_t, 16> chunk_guid{};
    std::uint32_t logical_offset{}, logical_size{};
    std::array<std::uint8_t, 20> sha1{};
    std::vector<std::uint8_t> chunk_record;
};
struct OutfitRow {
    std::string table, path;
    std::uint32_t bundle{};
    std::string table_bundle, table_superbundle;
};
struct OutfitItem {
    std::string item;
    std::vector<OutfitFile> files;
    std::vector<OutfitRow> rows;
};

// The stored bytes of a received file, by its id; nothing when the cache does not have them.
using ReadReceived = std::function<std::optional<std::vector<std::uint8_t>>(const std::array<std::uint8_t, 20>&)>;

inline constexpr char synchros_folder[] = ".synchros";
// A received outfit mod's name in the catalogue: this, then the wearer's SteamID.
inline constexpr char outfit_mod_prefix[] = "synchros-";

// Marks a folder as a received outfit: never listed, never in the wardrobe.
inline constexpr char outfit_marker_file[] = "reskate-synchros.txt";

struct OutfitMod {
    bool written{};
    std::vector<std::string> items;   // the item keys it holds, lower case: kept out of the wardrobe
    std::size_t bundles{}, files{};
    std::uint64_t bytes{};
    std::string problem;              // why nothing was written
};

// Writes the mod for `wearer`'s items into `directory`, replacing what was there. Items with a
// file the cache does not have are left out (they are fetched again). `game` is the folder with
// Skate.exe. Written beside and renamed into place, so the merge never sees half a mod.
[[nodiscard]] OutfitMod write_outfit_mod(const std::filesystem::path& directory, std::uint64_t wearer,
                                         const std::filesystem::path& game, const std::vector<OutfitItem>& items,
                                         const ReadReceived& read);

// The received outfits' mod folders under `root` (Mods/.synchros/mods): the newest version of
// each wearer's, by wearer. Older versions are removed.
struct OutfitFolder {
    std::uint64_t wearer{};
    std::uint32_t version{};
    std::filesystem::path directory;
    std::vector<std::string> items;   // from its marker
    [[nodiscard]] std::string mod_name() const;
};
[[nodiscard]] std::vector<OutfitFolder> outfit_folders(const std::filesystem::path& root);
// Where the next version of `wearer`'s mod goes.
[[nodiscard]] std::filesystem::path next_outfit_folder(const std::filesystem::path& root, std::uint64_t wearer);
// Removes every version of `wearer`'s mod (blocked, or the cache emptied).
void remove_outfit_folders(const std::filesystem::path& root, std::uint64_t wearer);

// SHA-1, as the bundle manifests list a payload's (of its decoded bytes).
[[nodiscard]] std::array<std::uint8_t, 20> sha1(std::span<const std::byte> bytes);

} // namespace dingosdk::synchros
