#include "Extension/Multiplayer/Remote/outfit_shuffle.h"
#include <iostream>
#include <set>

using namespace dingosdk::multiplayer;

namespace {
int failures = 0;
void check(bool ok, const char *what) {
    if (ok) return;
    ++failures;
    std::cerr << "FAIL: " << what << '\n';
}
constexpr std::uint32_t tops = 100, bottoms = 200, shoes = 300, hats = 400;
// Three slots (top, bottoms, hat) of a skater; the hat slot is empty.
CosmeticRecipe worn() {
    CosmeticRecipe r;
    r.key = skater_recipe_key;
    r.version = 1;
    r.items = {{11, "Own_TopShirt_A", {1, 2}}, {12, "Own_BottomPants_A", {3}}, {13, "", {4}}};
    return r;
}
const std::vector<std::uint32_t> categories{tops, bottoms, hats};
ShuffleCatalog catalog() {
    return ShuffleCatalog({{"Own_TopShirt_A", {tops}},
                           {"Own_TopShirt_B", {tops}},
                           {"Own_TopShirt_C", {tops, hats}}, // fits two categories
                           {"Own_BottomPants_A", {bottoms}},
                           {"Own_BottomPants_B", {bottoms}},
                           {"Own_Shoe_A", {shoes}},
                           {"Own_Hat_A", {hats}},
                           {"", {tops}}, // no name: never chosen
                           {"Own_TopShirt_B", {tops}}}); // listed twice: counted once
}
} // namespace

int main() {
    const auto cat = catalog();
    check(cat.size() == 7, "The catalog drops nameless and repeated items");
    check(cat.in_category(tops) && cat.in_category(tops)->size() == 3, "A category lists the items that fit it");
    check(!cat.in_category(12345), "An unknown category lists nothing");

    // The same seed always gives the same outfit.
    auto a = worn(), b = worn();
    const auto seed = shuffle_seed(76561198000000001ULL, 7);
    check(seed != 0, "A seed is never 0");
    check(shuffle_recipe(a, categories, cat, seed) == 2, "Both worn items change");
    shuffle_recipe(b, categories, cat, seed);
    check(a == b, "The same seed gave two outfits");

    // Each item is another one that fits its slot; the slot and parameters are the sender's.
    check(a.items[0].asset != "Own_TopShirt_A" && a.items[0].asset.starts_with("Own_TopShirt_"), "A top became another top");
    check(a.items[1].asset == "Own_BottomPants_B", "Bottoms with one other choice took it");
    check(a.items[2].asset.empty(), "An empty slot stayed empty");
    check(a.items[0].slot == 11 && a.items[0].parameters == std::vector<std::uint32_t>({1, 2}), "A top kept its slot and parameters");
    check(a.items[1].parameters == std::vector<std::uint32_t>({3}), "Bottoms kept their parameters");

    // A different player, or a re-roll, looks different somewhere across a good number of tries.
    std::set<std::string> tops_seen;
    for (std::uint64_t epoch = 0; epoch < 64; ++epoch) {
        auto r = worn();
        shuffle_recipe(r, categories, cat, shuffle_seed(42, epoch));
        tops_seen.insert(r.items[0].asset);
    }
    check(tops_seen == std::set<std::string>({"Own_TopShirt_B", "Own_TopShirt_C"}), "Re-rolls reach every other top, and never the worn one");
    check(shuffle_seed(1, 1) != shuffle_seed(2, 1), "Two players get two seeds");
    check(shuffle_seed(1, 1) != shuffle_seed(1, 2), "A re-roll gets a new seed");

    // A slot's pick does not depend on the others.
    auto lone = worn();
    lone.items[1].asset.clear();
    shuffle_recipe(lone, categories, cat, seed);
    check(lone.items[0].asset == a.items[0].asset, "An empty neighbour moved another slot's pick");

    // Only the listed slots change, whatever they hold now: a stand-in for a top this PC lacks, and
    // a hat slot the stand-in step left empty. The bottoms are installed and stay as worn.
    {
        auto r = worn();
        r.items[0].asset = "Default_Top";
        const std::vector<bool> lacking{true, false, true};
        check(shuffle_recipe(r, categories, cat, seed, lacking) == 2, "Both listed slots change");
        check(r.items[0].asset.starts_with("Own_TopShirt_") && r.items[0].asset != "Default_Top",
              "A stand-in for a top was replaced by an installed top");
        check(r.items[1].asset == "Own_BottomPants_A" && r.items[1].parameters == std::vector<std::uint32_t>({3}),
              "A slot that is not listed stays as worn");
        check(r.items[2].asset == "Own_Hat_A" || r.items[2].asset == "Own_TopShirt_C", "An empty listed slot took an item that fits it");
        check(r.items[2].parameters == std::vector<std::uint32_t>({4}), "The listed empty slot kept its parameters");
        auto again = worn();
        again.items[0].asset = "Default_Top";
        shuffle_recipe(again, categories, cat, seed, lacking);
        check(again == r, "The same seed gave two outfits for the same lacking items");
        auto none = worn();
        check(shuffle_recipe(none, categories, cat, seed, {false, false, false}) == 0 && none == worn(),
              "A list with no slot in it changes nothing");
        check(shuffle_recipe(none, categories, cat, seed, {true}) == 0 && none == worn(),
              "A list of another size is ignored");
    }

    // The setting's three modes: their saved values, their order in the menu, and their words.
    check(static_cast<int>(ShuffleMode::off) == 0 && static_cast<int>(ShuffleMode::everything) == 1 &&
              static_cast<int>(ShuffleMode::not_installed) == 2,
          "The saved values of the modes changed");
    check(next_shuffle_mode(ShuffleMode::off) == ShuffleMode::everything &&
              next_shuffle_mode(ShuffleMode::everything) == ShuffleMode::not_installed &&
              next_shuffle_mode(ShuffleMode::not_installed) == ShuffleMode::off,
          "The menu steps nothing, everything, not installed, and round");
    check(shuffle_mode_from(0) == ShuffleMode::off && shuffle_mode_from(1) == ShuffleMode::everything &&
              shuffle_mode_from(2) == ShuffleMode::not_installed,
          "A saved mode did not read back");
    check(shuffle_mode_from(3) == ShuffleMode::off && shuffle_mode_from(-1) == ShuffleMode::off &&
              shuffle_mode_from(1LL << 40) == ShuffleMode::off,
          "A saved value that is no mode means off");
    check(parse_shuffle_mode("off") == ShuffleMode::off && parse_shuffle_mode("none") == ShuffleMode::off &&
              parse_shuffle_mode("all") == ShuffleMode::everything && parse_shuffle_mode("everything") == ShuffleMode::everything &&
              parse_shuffle_mode("missing") == ShuffleMode::not_installed &&
              parse_shuffle_mode("not-installed") == ShuffleMode::not_installed,
          "The console words did not parse");
    check(!parse_shuffle_mode("") && !parse_shuffle_mode("on") && !parse_shuffle_mode("2"), "Other words are not a mode");

    // A shuffle is active only with a mode and a seed; the board's keeps the mode and has a seed of its own.
    check(!Shuffle{}.active() && !Shuffle{ShuffleMode::everything, 0}.active() && !Shuffle{ShuffleMode::off, 7}.active() &&
              Shuffle{ShuffleMode::not_installed, 7}.active(),
          "A shuffle is active with a mode and a seed only");
    const Shuffle clothes{ShuffleMode::everything, seed};
    check(clothes.board().mode == ShuffleMode::everything && clothes.board().seed != 0 && clothes.board().seed != seed &&
              clothes.board().active() && clothes.board() == clothes.board(),
          "The board's shuffle has the mode and a seed of its own");
    check(Shuffle{ShuffleMode::off, 7}.board() == Shuffle{} && Shuffle{}.board() == Shuffle{},
          "An inactive shuffle has an inactive board");
    check(Shuffle{ShuffleMode::everything, 0xb0a4d5eedULL}.board().seed == 1, "A board seed is never 0");

    // Which slots each mode shuffles. Slots: a top this PC has, bottoms it lacks (the stand-in is
    // in), a hat it lacks with no stand-in (empty), and a slot the player left empty.
    constexpr std::uint32_t top_slot = 0xac4c6e13, bottoms_slot = 0xc06aa817, hat_slot = 0xac3d4305, shoes_slot = 0x35c5f429;
    check(shuffles_slot(top_slot) && shuffles_slot(0x0add98be) && !shuffles_slot(11) && !shuffles_slot(0),
          "Only the listed slots are shuffled");
    {
        CosmeticRecipe r;
        r.items = {{top_slot, "Own_TopShirt_A", {}}, {bottoms_slot, "Default_Pants", {}}, {hat_slot, "", {}}, {shoes_slot, "", {}}};
        const std::vector<bool> lacking{false, true, true, false};
        const auto missing = shuffle_slots(ShuffleMode::not_installed, r, lacking);
        check(missing == std::vector<bool>({false, true, true, false}), "Not installed shuffles the lacking slots, with or without a stand-in");
        const auto all = shuffle_slots(ShuffleMode::everything, r, lacking);
        check(all == std::vector<bool>({true, true, true, false}),
              "Everything shuffles each worn slot and each lacking slot, and leaves an empty one empty");
        check(shuffle_slots(ShuffleMode::off, r, lacking) == std::vector<bool>(4, false), "Off shuffles no slot");
        check(shuffle_slots(ShuffleMode::everything, r, {true}) == std::vector<bool>(4, false),
              "A lacking list of another size shuffles nothing");
        // A slot not in the list keeps its item, or its stand-in, in either mode.
        auto unlisted = r;
        unlisted.items[0].slot = 11;
        unlisted.items[1].slot = 12;
        check(shuffle_slots(ShuffleMode::everything, unlisted, lacking) == std::vector<bool>({false, false, true, false}),
              "Everything leaves a worn slot that is not listed alone");
        check(shuffle_slots(ShuffleMode::not_installed, unlisted, lacking) == std::vector<bool>({false, false, true, false}),
              "Not installed leaves a lacking slot that is not listed with its stand-in");
        // Fed to shuffle_recipe, "everything" moves a top this PC has; "not installed" keeps it.
        auto every = r, only_missing = r;
        const std::vector<std::uint32_t> kinds{tops, bottoms, hats, shoes};
        shuffle_recipe(every, kinds, cat, seed, all);
        shuffle_recipe(only_missing, kinds, cat, seed, missing);
        check(every.items[0].asset != "Own_TopShirt_A", "Everything changed an installed top");
        check(only_missing.items[0].asset == "Own_TopShirt_A", "Not installed kept an installed top");
        check(only_missing.items[1].asset.starts_with("Own_BottomPants_") && every.items[1].asset.starts_with("Own_BottomPants_"),
              "A stand-in was replaced by an item for its slot in both modes");
        check(!only_missing.items[2].asset.empty() && !every.items[2].asset.empty(), "A lacking hat with no stand-in shows something in both modes");
        check(only_missing.items[3].asset.empty() && every.items[3].asset.empty(), "A slot the player left empty stays empty in both modes");
        auto none = r;
        shuffle_recipe(none, kinds, cat, seed, shuffle_slots(ShuffleMode::off, r, lacking));
        check(none == r, "Off changes nothing");
    }

    // Cases that must leave the outfit alone.
    auto off = worn();
    check(shuffle_recipe(off, categories, cat, 0) == 0 && off == worn(), "Seed 0 means not shuffled");
    check(shuffle_recipe(off, {tops}, cat, seed) == 0 && off == worn(), "A recipe of another shape is left alone");
    const ShuffleCatalog sole({{"Own_TopShirt_A", {tops}}});
    check(shuffle_recipe(off, categories, sole, seed) == 0 && off == worn(), "Nothing else fits: nothing changes");
    check(shuffle_recipe(off, categories, ShuffleCatalog{}, seed) == 0 && off == worn(), "An empty catalog changes nothing");
    auto stray = worn();
    const std::vector<std::uint32_t> unlisted{9, 9, 9};
    check(shuffle_recipe(stray, unlisted, cat, seed) == 0 && stray == worn(), "A slot with no category in the catalog is left alone");

    // Near the texture limit the shuffle picks only from items already loaded.
    {
        const ShuffleCatalog both({{"Shirt", {tops}}, {"Pants", {bottoms}}, {"Overalls", {tops, bottoms}}, {"Cap", {hats}}});
        const auto loaded = both.only({"Cap", "Overalls", "Unknown_Mod_Item"});
        check(loaded.size() == 2, "Only the worn items that are in the catalog stay");
        const auto *fits_top = loaded.in_category(tops), *fits_bottom = loaded.in_category(bottoms);
        check(fits_top && fits_top->size() == 1 && loaded.name(fits_top->front()) == "Overalls" && fits_bottom &&
                  fits_bottom->size() == 1 && loaded.name(fits_bottom->front()) == "Overalls",
              "A kept item keeps every category it fits");
        check(!loaded.in_category(shoes) && loaded.in_category(hats), "Categories with nothing worn are empty");
        check(both.only({}).size() == 0, "Nothing worn: nothing to pick");
        auto r = worn();
        shuffle_recipe(r, categories, loaded, seed);
        check(r.items[0].asset == "Overalls" && r.items[1].asset == "Overalls" && r.items[2].asset.empty(),
              "Shuffling from what is worn picks only those items");
    }

    // Room for textures: past 80% of the limit only worn items are picked.
    check(shuffle_texture_room(0, 20480) && shuffle_texture_room(16383, 20480), "Below 80% there is room");
    check(!shuffle_texture_room(16384, 20480) && !shuffle_texture_room(20480, 20480) && !shuffle_texture_room(32764, 32764),
          "At 80% and past it there is none");
    check(shuffle_texture_room(16384, 32764), "The raised limit leaves more room");
    check(shuffle_texture_room(0, 0) && shuffle_texture_room(99999, 0), "An unknown limit leaves room");

    if (failures) return 1;
    std::cout << "outfit shuffle tests passed\n";
    return 0;
}
