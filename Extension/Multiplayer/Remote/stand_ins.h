#pragma once
#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What stands in for a peer's cosmetic this PC does not have (a player's cosmetics mod).
// Without a policy each missing item becomes the matching default_cosmetic_items entry, or an
// empty slot. A plugin with the "outfit.stand_ins" permission (reskate.outfit.stand_ins) can set
// one instead: which items may stand in, and whether the first that fits is taken or one picked
// at random. Only what this PC shows of other players changes; nothing is sent, saved or owned.
namespace dingosdk::multiplayer::stand_ins {
enum class Pick : std::uint8_t { first, random };
enum class Parts : std::uint8_t { both, skater, board };
inline constexpr std::size_t max_items = 4096;
struct Policy {
    std::string owner; // the plugin holding it
    Pick pick{Pick::first};
    Parts parts{Parts::both};
    // Empty: every installed item that fits the slot. Paid store items never stand in either way.
    std::vector<std::string> items;
    // Items never to stand in: names, in which "*" is any run of characters ("Own_Costume*",
    // "*GhostSkater*"). Any case (set() keeps them lower case). Only the candidates: an item a player
    // really wears still shows.
    std::vector<std::string> exclude;
    std::uint32_t seed{};
};

// The policy in force, or null for the game's defaults. Any thread.
std::shared_ptr<const Policy> current();
// Bumped with every change, so the players shown can be dressed again. Starts at 1.
std::uint64_t generation();
// Sets `policy` for its owner. Empty when set, else why not (another plugin holds it).
std::string set(Policy policy);
// Puts the defaults back, if `owner` holds the policy.
void clear(std::string_view owner);

inline std::string lower(std::string_view text) {
    std::string out(text);
    for (auto &c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return out;
}
// Whether `name` fits `rule`, in which "*" is any run of characters, none included. Both lower case.
inline bool fits(std::string_view name, std::string_view rule) {
    std::size_t n = 0, r = 0, star = std::string_view::npos, from = 0;
    while (n < name.size()) {
        if (r < rule.size() && rule[r] == '*') {
            star = r++; // the star takes nothing yet; it takes one more each time what follows fails
            from = n;
        } else if (r < rule.size() && rule[r] == name[n]) {
            ++r;
            ++n;
        } else if (star != std::string_view::npos) {
            r = star + 1;
            n = ++from;
        } else {
            return false;
        }
    }
    while (r < rule.size() && rule[r] == '*') ++r;
    return r == rule.size();
}
// Whether `asset` is in the policy's exclude list (which set() keeps lower case).
inline bool excluded(const Policy &policy, std::string_view asset) {
    if (policy.exclude.empty() || asset.empty()) return false;
    const auto name = lower(asset);
    for (const auto &rule : policy.exclude)
        if (fits(name, rule)) return true;
    return false;
}

// Whether the policy covers a recipe: the skater's (skater_recipe_key) or the board's.
inline bool covers(const Policy &policy, bool board) {
    return policy.parts == Parts::both || (policy.parts == Parts::board) == board;
}

inline std::uint64_t mix(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}
inline std::uint64_t text_hash(std::string_view text, std::uint64_t h = 1469598103934665603ULL) {
    for (unsigned char c : text) h = (h ^ c) * 1099511628211ULL;
    return h;
}
// The candidates for one missing item in the order they are tried. "first": as given. "random":
// shuffled by a seed made of the outfit (`outfit`, a hash of everything the peer wears), the slot,
// the missing item and the policy's seed, so a player keeps the same stand-ins for as long as they
// wear the same outfit (no flicker each time it is put on again), different players and slots get
// different ones, and a new seed gives everybody new ones.
inline std::vector<std::string> order(const Policy &policy, std::vector<std::string> candidates, std::uint64_t outfit,
                                      std::uint32_t slot, std::string_view missing) {
    if (policy.pick != Pick::random || candidates.size() < 2) return candidates;
    std::uint64_t state = mix(outfit ^ mix(text_hash(missing) ^ (std::uint64_t{slot} << 32 | policy.seed)));
    for (std::size_t i = candidates.size() - 1; i > 0; --i) {
        state = mix(state);
        std::swap(candidates[i], candidates[static_cast<std::size_t>(state % (i + 1))]);
    }
    return candidates;
}
} // namespace dingosdk::multiplayer::stand_ins
