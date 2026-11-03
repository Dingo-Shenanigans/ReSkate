// What of other players' outfits shows (Extension/Multiplayer/Remote/outfit_filter.h): which items
// each rule hides, and the rules' file read back.
#include "Extension/Multiplayer/Remote/outfit_filter.h"

#include <iostream>

using namespace dingosdk::multiplayer::outfit_filter;

namespace {
int failures = 0;
void check(bool condition, const char *what) {
    if (condition) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}
const ItemFacts game{true, {}};                    // the game's own
const ItemFacts changed{true, "retex-pack"};       // the game's, which a mod here changes
const ItemFacts modded{false, "nolan-pack"};       // a mod's, installed here
const ItemFacts missing{false, {}};                // not installed here
constexpr std::uint64_t ann = 76561198000000001ULL, bob = 76561198000000002ULL;
} // namespace

int main() {
    {
        const Rules none;
        check(!hides(none, ann, "own_topshirt_gen_tshirtrelaxed_00007", game), "no rules: the game's items show");
        check(!hides(none, ann, "Own_Nolan_Tee", modded), "no rules: mod items show");
        check(!hides(none, ann, "", modded), "an empty slot is never hidden");
    }
    {
        Rules everyone;
        everyone.mod_items = false;
        check(hides(everyone, ann, "Own_Nolan_Tee", modded), "mod items off: a mod's item is hidden");
        check(hides(everyone, ann, "Own_Somebody_Else", missing), "and one not installed here");
        check(!hides(everyone, ann, "Own_TopShirt_Gen_X", game), "but never the game's own");
        check(!hides(everyone, ann, "Own_TopShirt_Gen_Y", changed), "nor a game item a mod only changes");
    }
    {
        Rules one;
        one.players[ann] = "Ann";
        check(hides(one, ann, "Own_Nolan_Tee", modded), "a player in game items only: their mod item is hidden");
        check(!hides(one, bob, "Own_Nolan_Tee", modded), "nobody else's");
        check(!hides(one, ann, "Own_TopShirt_Gen_X", game), "their game items show");
    }
    {
        Rules by_name;
        by_name.items.insert("own_nolan_tee");
        by_name.items.insert("own_topshirt_gen_x");
        check(hides(by_name, bob, "Own_Nolan_Tee", modded), "an item hidden by name, whatever its case");
        check(hides(by_name, bob, "OWN_TOPSHIRT_GEN_X", game), "a game item can be hidden by name");
        check(!hides(by_name, bob, "Own_Nolan_Hat", modded), "the mod's other items show");
    }
    {
        Rules by_mod;
        by_mod.mods.insert("nolan-pack");
        by_mod.mods.insert("retex-pack");
        check(hides(by_mod, bob, "Own_Nolan_Hat", modded), "every item of a hidden mod");
        check(hides(by_mod, bob, "Own_TopShirt_Gen_Y", changed), "a game item the hidden mod changes");
        check(!hides(by_mod, bob, "Own_Other", ItemFacts{false, "other-pack"}), "another mod's items show");
        check(!hides(by_mod, bob, "Own_Unknown", missing), "an item of no mod here is not the mod's");
    }
    {
        Rules rules;
        rules.mod_items = false;
        rules.items = {"own_nolan_tee"};
        rules.mods = {"nolan-pack"};
        rules.players = {{ann, "Ann"}, {bob, ""}};
        const auto back = from_json(to_json(rules));
        check(back == rules, "the rules' file reads back as written");
        check(back.players.contains(76561198000000001ULL), "a Steam ID survives whole");
    }
    {
        check(from_json("") == Rules{}, "no file: nothing hidden");
        check(from_json("[1, 2]") == Rules{}, "not an object: nothing hidden");
        const auto loose = from_json(R"({"mod_items": "no", "items": ["Own_A", 5, ""], "mods": "x",
                                        "players": {"0": "zero", "abc": "x", "12": 7}})");
        check(loose.mod_items, "a mod_items that is not true or false is left on");
        check(loose.items == std::set<std::string, std::less<>>{"own_a"}, "only names are taken, lower case");
        check(loose.mods.empty(), "mods that are not a list are none");
        check(loose.players.size() == 1 && loose.players.contains(12) && loose.players.at(12).empty(),
              "only numbered players, a name that is not text left empty");
    }
    {
        check(readable_key("Own_NikeSB_Olive") == "NikeSB Olive", "a key reads as words");
        check(readable_key("Own_Nike_SB_x_Chunky_Dunky_Dunk_Low_-_Clean") == "Nike SB x Chunky Dunky Dunk Low - Clean",
              "a dash between words stays one dash");
        check(readable_key("Own_Own_TopShirt_SSK_JERSEY_BLK") == "TopShirt SSK JERSEY BLK", "a doubled Own_ goes too");
        check(readable_key("own_carhartt__cap_") == "carhartt cap", "runs of underscores are one space, none at the end");
        check(readable_key("Own_") == "Own" && readable_key("Hellboy") == "Hellboy", "a bare Own_ keeps its word, a plain name is as it was");
    }
    {
        check(readable_mod("nolanishigh-nolanishighs_cosmetics_pack_1") == "nolanishighs cosmetics pack 1", "a mod folder reads as its name");
        check(readable_mod("altdoug-south_florida__") == "south florida", "runs of underscores are one space");
        check(readable_mod("plain") == "plain" && readable_mod("owner-") == "owner-", "a folder with no name after the owner is as it was");
    }
    if (failures) std::cerr << failures << " outfit filter test(s) failed\n";
    else std::cout << "outfit filter tests passed\n";
    return failures ? 1 : 0;
}
