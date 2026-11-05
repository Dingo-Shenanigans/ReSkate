#pragma once
#include "Engine/Core/Text/ascii.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
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
// Every candidate is matched against every rule with a "*", so those are few. A plain name is one
// lookup in a set however many there are, so a plugin can leave out as many as it lists.
inline constexpr std::size_t max_exclude = 64;
inline constexpr std::size_t max_exclude_names = max_items;
struct Policy {
    std::string owner; // the plugin holding it
    Pick pick{Pick::first};
    Parts parts{Parts::both};
    // Empty: every installed item that fits the slot. Paid store items never stand in either way.
    std::vector<std::string> items;
    // Items never to stand in: names, in which "*" is any run of characters ("Own_Costume*",
    // "*GhostSkater*"). Any case (set() keeps them lower case). Only the candidates: an item a player
    // really wears still shows. set() keeps the rules with a "*" here and moves plain names to
    // `exclude_names`.
    std::vector<std::string> exclude;
    std::unordered_set<std::string> exclude_names;
    std::uint32_t seed{};
    // Whether each of `items` is left after `exclude`. set() works it out on the plugin's thread, so
    // the engine thread matches none of them when it dresses a player.
    std::vector<bool> kept;
};

// The policy in force, or null for the game's defaults. Any thread.
std::shared_ptr<const Policy> current();
// Bumped with every change, so the players shown can be dressed again. Starts at 1.
std::uint64_t generation();
// Sets `policy` for its owner. Empty when set, else why not (another plugin holds it).
std::string set(Policy policy);
// Puts the defaults back, if `owner` holds the policy.
void clear(std::string_view owner);

// Where `piece` (not empty) first starts in `text`, or npos. Knuth-Morris-Pratt, so the work is
// linear in both whatever a plugin writes; a piece longer than a rule can be is plainly searched.
inline std::size_t find_piece(std::string_view text, std::string_view piece) {
    std::array<std::uint8_t, 256> back{}; // the longest start of `piece` that also ends piece[0..i]
    if (piece.size() > back.size()) return text.find(piece);
    for (std::size_t i = 1, k = 0; i < piece.size(); ++i) {
        while (k && piece[i] != piece[k]) k = back[k - 1];
        if (piece[i] == piece[k]) ++k;
        back[i] = static_cast<std::uint8_t>(k);
    }
    for (std::size_t i = 0, k = 0; i < text.size(); ++i) {
        while (k && text[i] != piece[k]) k = back[k - 1];
        if (text[i] == piece[k]) ++k;
        if (k == piece.size()) return i + 1 - k;
    }
    return std::string_view::npos;
}
// Whether `name` fits `rule`, in which "*" is any run of characters, none included. Both lower case.
// What comes before the first star starts the name and what follows the last ends it; each piece
// between is taken where it first appears after the one before, which never loses a match. A piece
// longer than what is left of the name fails before it is searched, so the work stays about the
// length of the name and the rule however many stars or rules a plugin sends.
inline bool fits(std::string_view name, std::string_view rule) {
    const auto first = rule.find('*');
    if (first == std::string_view::npos) return name == rule;
    const auto last = rule.rfind('*');
    const auto head = rule.substr(0, first), tail = rule.substr(last + 1);
    if (head.size() + tail.size() > name.size() || !name.starts_with(head) || !name.ends_with(tail)) return false;
    auto rest = name.substr(head.size(), name.size() - head.size() - tail.size());
    auto middle = rule.substr(first + 1, last - first); // keeps the last star, so every piece ends in one
    while (!middle.empty()) {
        const auto star = middle.find('*');
        const auto piece = middle.substr(0, star);
        middle.remove_prefix(star + 1);
        if (piece.empty()) continue;
        if (piece.size() > rest.size()) return false;
        const auto at = find_piece(rest, piece);
        if (at == std::string_view::npos) return false;
        rest.remove_prefix(at + piece.size());
    }
    return true;
}
// Whether `asset` is in the policy's exclude list (which set() keeps lower case).
inline bool excluded(const Policy &policy, std::string_view asset) {
    if ((policy.exclude.empty() && policy.exclude_names.empty()) || asset.empty()) return false;
    const auto name = ascii_lower(asset);
    if (policy.exclude_names.contains(name)) return true;
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
// The same order for any list of that length: the engine thread orders positions in its lists.
template <class T>
std::vector<T> order(const Policy &policy, std::vector<T> candidates, std::uint64_t outfit, std::uint32_t slot,
                     std::string_view missing) {
    if (policy.pick != Pick::random || candidates.size() < 2) return candidates;
    std::uint64_t state = mix(outfit ^ mix(text_hash(missing) ^ (std::uint64_t{slot} << 32 | policy.seed)));
    for (std::size_t i = candidates.size() - 1; i > 0; --i) {
        state = mix(state);
        std::swap(candidates[i], candidates[static_cast<std::size_t>(state % (i + 1))]);
    }
    return candidates;
}
} // namespace dingosdk::multiplayer::stand_ins
