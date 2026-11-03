#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Cosmetics mods fetched because another player wore them, as opposed to mods the player installed:
// Mods/.reskate-peer-cosmetics.json. Such a mod is an ordinary mod folder at the bottom of the load
// order; this file only remembers where it came from and when its items were last seen, so the
// launch can delete the ones nobody wears any more (clean_up). Keeping one forgets it here, and from
// then on it is the player's own. Not in the generated folder, which every full merge deletes.
namespace dingosdk::mods::peer_cosmetics {

inline constexpr char store_file[] = ".reskate-peer-cosmetics.json";

// What the launch deletes.
enum class Cleanup : std::uint8_t {
    unused,       // a mod whose items nobody was seen wearing for `days`
    size,         // the least recently seen, until all of them fit in `max_mb`
    every_launch, // all of them
    never,
};
struct Policy {
    Cleanup cleanup = Cleanup::unused;
    unsigned days = 14;          // 1..365
    std::uint64_t max_mb = 2048; // 100..65536
    bool operator==(const Policy&) const = default;
};
inline constexpr std::array<std::string_view, 4> cleanup_names{"unused", "size", "every-launch", "never"};

struct Entry {
    std::string version;     // as installed
    std::string from;        // the player it was fetched for, for the menu
    std::int64_t fetched{};  // seconds since 1970
    std::int64_t seen{};     // last time anyone, this player included, wore one of its items
    bool remove{};           // deleted at the next launch, whatever the policy
    bool operator==(const Entry&) const = default;
};
struct Store {
    Policy policy;
    std::map<std::string, Entry, std::less<>> mods; // by folder name
    bool operator==(const Store&) const = default;
};

// The file's text and back. read() never throws: a missing or broken file is an empty store.
std::string to_json(const Store& store);
Store from_json(std::string_view text) noexcept;
Store read(const std::filesystem::path& mods_root) noexcept;
// Replaces the file. False when it could not be written.
bool write(const std::filesystem::path& mods_root, const Store& store) noexcept;

// Which fetched mods the policy takes away now, and why (for the log), oldest seen first. `sizes`
// gives each folder's bytes on disk; a folder it does not list counts as empty.
struct Removal {
    std::string folder;
    std::string why;
};
std::vector<Removal> expired(const Store& store, std::int64_t now, const std::map<std::string, std::uint64_t, std::less<>>& sizes);

// At launch, before the mods are merged (an archive the game has open cannot be deleted): deletes
// what expired() names, their mods.json rows and their entries, and forgets entries whose folder is
// gone. Returns a line per change, for the log. Never throws.
std::vector<std::string> clean_up(const std::filesystem::path& mods_root, std::int64_t now) noexcept;

// Whether a mod folder holds cosmetics and nothing else: the only TOCs are layout.toc and
// Win32/items.toc (and Win32/globals.toc, where an item pack may register its items), with no
// plugin, script, level, park or program in it. Only such a package is fetched for another player.
bool only_cosmetics(const std::filesystem::path& directory) noexcept;

// Bytes in a folder and everything under it; 0 when it cannot be read.
std::uint64_t folder_size(const std::filesystem::path& directory) noexcept;

// Seconds since 1970, now.
std::int64_t now_seconds() noexcept;

} // namespace dingosdk::mods::peer_cosmetics
