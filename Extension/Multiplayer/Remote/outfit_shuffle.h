#pragma once
#include "cosmetics.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Shuffled outfits: what the local player sees other players wearing, in place of either every
// item or just the items this PC does not have installed (a player's cosmetics mod, which would
// otherwise show a fixed stand-in), as the ShuffleMode setting says. A player's real outfit is
// never touched, sent or saved; the shuffle is applied to a copy just before it is put on their
// skater (native_cosmetics.cpp), so other people still see you as you really are. Nothing here
// reads game memory, so the rules can be tested on their own (Test/outfit_shuffle_tests.cpp).
namespace dingosdk::multiplayer {
// What the setting shuffles. The values are the ones the menu's view carries
// (SessionView::outfit_shuffle) and the profile saves, so they never change.
enum class ShuffleMode : std::uint8_t {
    off = 0,           // everything shows as worn (an item this PC lacks gets the usual stand-in)
    everything = 1,    // every item a player wears or rides takes a random item that fits
    not_installed = 2, // only the items this PC lacks do
};
inline constexpr std::uint8_t shuffle_modes = 3;
inline constexpr ShuffleMode shuffle_mode_from(long long value) noexcept {
    return value >= 0 && value < shuffle_modes ? static_cast<ShuffleMode>(value) : ShuffleMode::off;
}
// The next mode in the order the menu steps through them: nothing, everything, not installed.
inline constexpr ShuffleMode next_shuffle_mode(ShuffleMode mode) noexcept {
    return static_cast<ShuffleMode>((static_cast<std::uint8_t>(mode) + 1) % shuffle_modes);
}
// The words the console takes, or nothing for any other text.
inline std::optional<ShuffleMode> parse_shuffle_mode(std::string_view word) noexcept {
    if (word == "off" || word == "none" || word == "nothing") return ShuffleMode::off;
    if (word == "all" || word == "everything") return ShuffleMode::everything;
    if (word == "missing" || word == "not-installed" || word == "uninstalled") return ShuffleMode::not_installed;
    return std::nullopt;
}

// What to shuffle for one player: the mode, and the player's seed (shuffle_seed). Both must
// say so; a default one shuffles nothing. `mods_only` picks only from items a mod installed
// (ShufflePools::mods), whichever slots the mode covers.
struct Shuffle {
    ShuffleMode mode{};
    std::uint64_t seed{};
    bool mods_only{};
    bool active() const noexcept { return mode != ShuffleMode::off && seed != 0; }
    // The same for the player's board, with a seed of its own so its picks do not follow the
    // clothes'. An inactive shuffle stays inactive (and compares equal to a default one).
    Shuffle board() const noexcept {
        if (!active()) return {};
        const auto derived = seed ^ 0xb0a4d5eedULL;
        return {mode, derived ? derived : 1, mods_only};
    }
    bool operator==(const Shuffle &) const = default;
};

struct ShuffleCandidate {
    std::string name;
    std::vector<std::uint32_t> categories;
    bool modded{}; // a mod installed it: it is in no retail catalogue
};

// Sorts `items` by name and drops nameless and repeated ones (the first of a name stays).
inline void tidy_shuffle_candidates(std::vector<ShuffleCandidate> &items) {
    std::stable_sort(items.begin(), items.end(),
                     [](const ShuffleCandidate &a, const ShuffleCandidate &b) { return a.name < b.name; });
    std::erase_if(items, [](const ShuffleCandidate &item) { return item.name.empty(); });
    items.erase(std::unique(items.begin(), items.end(),
                            [](const ShuffleCandidate &a, const ShuffleCandidate &b) { return a.name == b.name; }),
                items.end());
}

// Installed cosmetics, indexed by the categories they fit. A slot takes an item whose
// categories include the slot's own (the check CosmeticMemory::validate makes).
class ShuffleCatalog {
public:
    ShuffleCatalog() = default;
    explicit ShuffleCatalog(std::vector<ShuffleCandidate> items) {
        // In name order, so a seed always picks the same item from the same install.
        tidy_shuffle_candidates(items);
        for (auto &item : items) {
            const auto index = names_.size();
            names_.push_back(std::move(item.name));
            std::sort(item.categories.begin(), item.categories.end());
            item.categories.erase(std::unique(item.categories.begin(), item.categories.end()), item.categories.end());
            for (const auto category : item.categories) by_category_[category].push_back(index);
        }
    }
    std::size_t size() const noexcept { return names_.size(); }
    const std::string &name(std::size_t index) const { return names_[index]; }
    // Indices of the items that fit `category`, in name order, or nothing.
    const std::vector<std::size_t> *in_category(std::uint32_t category) const {
        const auto found = by_category_.find(category);
        return found == by_category_.end() ? nullptr : &found->second;
    }
    // How many items fit `category`.
    std::size_t fitting(std::uint32_t category) const {
        const auto *fits = in_category(category);
        return fits ? fits->size() : 0;
    }

private:
    std::vector<std::string> names_;
    std::map<std::uint32_t, std::vector<std::size_t>> by_category_;
};

// The installed cosmetics as two pools that share no item: the game's (in a retail catalogue) and
// the ones a mod installed. Mod items only draws from `mods`; otherwise a pick is one draw over
// both. `mods` is empty when there are no mod items, or when nothing could be judged modded (no
// content pack installed).
struct ShufflePools {
    ShuffleCatalog game, mods;
    ShufflePools() = default;
    explicit ShufflePools(std::vector<ShuffleCandidate> items) {
        // Repeats go first, so a name listed both ways lands in one pool only.
        tidy_shuffle_candidates(items);
        std::vector<ShuffleCandidate> retail, modded;
        for (auto &item : items) (item.modded ? modded : retail).push_back(std::move(item));
        game = ShuffleCatalog(std::move(retail));
        mods = ShuffleCatalog(std::move(modded));
    }
    std::size_t size() const noexcept { return game.size() + mods.size(); }
};

// The slots the shuffle may touch (template slot ids, as `mp cosmetic-slots` prints them): the
// skater's clothes, eyewear and tattoos, and the whole board. Every other slot, including the
// skater's body, face and voice and any slot a mod adds, always shows what the player wears, or the
// usual stand-in, as with the shuffle off.
inline constexpr std::array<std::uint32_t, 21> shuffled_slot_ids{
    0x9b15af3e, // cust_fullBodyCostume
    0xac3d4305, // cust_hats
    0xac4c6e13, // cust_tops
    0xc06aa817, // cust_bottoms
    0x35c5f429, // cust_shoes
    0xc6ca18f3, // cust_eyewear
    0x35caa14c, // cust_socks
    0xf8984dad, // cust_outfits
    0xb4ec0b53, // tattoo_armL_1
    0xb4ebb3cd, // tattoo_armR_1
    0xa70e1f88, // tattoo_chest_1
    0xa7ecb563, // tattoo_legL_1
    0xa7ecddfd, // tattoo_legR_1
    0x755d5ee0, // board_gripcolor1
    0xc83bb94e, // board_wheelcolor
    0x66071628, // board_bottomart
    0x976f18c4, // board_grippattern
    0x0add98be, // truck
    0x355284de, // boardsticker0
    0x355284df, // boardsticker1
    0x355284dc, // boardsticker2
};

inline constexpr bool shuffles_slot(std::uint32_t slot) noexcept {
    return std::find(shuffled_slot_ids.begin(), shuffled_slot_ids.end(), slot) != shuffled_slot_ids.end();
}

// Which slots of a recipe `mode` shuffles, to hand to shuffle_recipe as `only`. `lacking[i]` says
// item `i` is not installed on this PC; `recipe` is the outfit after the usual stand-ins went in
// for those, so a slot with an item in it is one the player wears (or a stand-in for one).
//   not_installed: just the lacking slots, whatever their stand-in is (or none).
//   everything:    every slot that holds an item, and the lacking slots too, so an item this
//                  PC lacks never ends up showing as nothing. A slot left empty stays empty.
//   off:           nothing.
// Either way only slots in shuffled_slot_ids; the rest keep the item or its stand-in.
// Always one entry per item, so shuffle_recipe takes it as written; a `lacking` of another size
// shuffles nothing.
inline std::vector<bool> shuffle_slots(ShuffleMode mode, const CosmeticRecipe &recipe, const std::vector<bool> &lacking) {
    std::vector<bool> slots(recipe.items.size(), false);
    if (mode == ShuffleMode::off || lacking.size() != slots.size()) return slots;
    for (std::size_t i = 0; i < slots.size(); ++i)
        slots[i] = shuffles_slot(recipe.items[i].slot) &&
                   (lacking[i] || (mode == ShuffleMode::everything && !recipe.items[i].asset.empty()));
    return slots;
}

// SplitMix64: small, and the same on every platform.
inline std::uint64_t shuffle_next(std::uint64_t &state) noexcept {
    std::uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

// A player's shuffle for one `epoch` (a new epoch is a re-roll). Never 0: 0 means "not shuffled".
inline std::uint64_t shuffle_seed(std::uint64_t player, std::uint64_t epoch) noexcept {
    std::uint64_t state = player ^ (epoch * 0x9e3779b97f4a7c15ULL);
    const auto seed = shuffle_next(state);
    return seed ? seed : 1;
}

// Gives items of `recipe` another item that fits their slot, keeping the slot and the parameters
// the sender chose (the slot, not the item, decides their layout). `slot_categories[i]` is the
// category recipe item `i` must fit; a recipe of another shape is left alone. A slot's pick
// depends on the seed and the slot only, so one slot changing does not move the others.
//
// `only` says which slots to shuffle: `only[i]` for recipe item `i`, and then a slot is shuffled
// whatever it holds now (the stand-in for an item this PC lacks, or nothing). Without it, every
// worn item is shuffled and empty slots stay empty, so nobody is handed a hat or a tattoo they
// did not wear. Returns how many items changed.
//
// The draw is over every item of `pools` that fits, in name order, as if they were one catalog.
inline std::size_t shuffle_recipe(CosmeticRecipe &recipe, const std::vector<std::uint32_t> &slot_categories,
                                  std::initializer_list<const ShuffleCatalog *> pools, std::uint64_t seed,
                                  const std::vector<bool> &only = {}) {
    if (!seed || slot_categories.size() != recipe.items.size() || (!only.empty() && only.size() != recipe.items.size()))
        return 0;
    std::size_t changed{};
    std::vector<const std::string *> fits;
    for (std::size_t i = 0; i < recipe.items.size(); ++i) {
        auto &item = recipe.items[i];
        if (only.empty() ? item.asset.empty() : !only[i]) continue;
        // Anything that fits except what they already wear.
        fits.clear();
        for (const auto *pool : pools)
            if (const auto *indices = pool->in_category(slot_categories[i]))
                for (const auto index : *indices)
                    if (pool->name(index) != item.asset) fits.push_back(&pool->name(index));
        if (fits.empty()) continue;
        std::sort(fits.begin(), fits.end(), [](const std::string *a, const std::string *b) { return *a < *b; });
        std::uint64_t state = seed ^ (static_cast<std::uint64_t>(item.slot) * 0xd1b54a32d192ed03ULL);
        item.asset = *fits[static_cast<std::size_t>(shuffle_next(state) % fits.size())];
        ++changed;
    }
    return changed;
}
inline std::size_t shuffle_recipe(CosmeticRecipe &recipe, const std::vector<std::uint32_t> &slot_categories,
                                  const ShuffleCatalog &catalog, std::uint64_t seed,
                                  const std::vector<bool> &only = {}) {
    return shuffle_recipe(recipe, slot_categories, {&catalog}, seed, only);
}
// With `mods_only` just the mod pool; otherwise both, in one draw.
inline std::size_t shuffle_recipe(CosmeticRecipe &recipe, const std::vector<std::uint32_t> &slot_categories,
                                  const ShufflePools &pools, bool mods_only, std::uint64_t seed,
                                  const std::vector<bool> &only = {}) {
    return mods_only ? shuffle_recipe(recipe, slot_categories, {&pools.mods}, seed, only)
                     : shuffle_recipe(recipe, slot_categories, {&pools.game, &pools.mods}, seed, only);
}
} // namespace dingosdk::multiplayer
