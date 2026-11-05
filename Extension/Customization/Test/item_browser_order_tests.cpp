// The item grids' search and order (Extension/Customization/item_browser_order.h): a word finds an
// item by its name, its asset name, or its mod's author, name and folder; the clothing and board
// pickers' Author, Mod pack, Sort and Show.
#include "Extension/Customization/item_browser_order.h"
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace dingosdk::item_browser;

void check(bool ok, const std::string& message) {
    if (ok) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}
Item item(std::string key, std::string title, std::string mod = {}) {
    Item result;
    result.key = std::move(key);
    result.title = std::move(title);
    result.mod = std::move(mod);
    result.modded = !result.mod.empty();
    return result;
}
} // namespace

int main() {
    const auto pack = mod_words("Ada Lane", "Author Pack", "Ada-Author_Pack");
    check(pack == "ada lane\nauthor pack\nada-author_pack", "the mod's words are folded, one part a line: " + pack);
    check(mod_words("", "Bare", "Bare") == "bare\nbare", "a mod with no author");
    check(mod_words("", "", "").empty(), "no mod, no words");

    const auto hat = item("Own_HatBeanie_Ada", "Beanie", pack);
    const auto stock = item("Own_ShoeSneaker_Vans_Old", "Old Skool");
    // The item's own name and asset name, as before.
    check(matches(hat, "beanie") && matches(hat, "BEAN") && matches(hat, ""), "the name, without regard to case");
    check(matches(stock, "vans") && !matches(stock, "va"), "an asset name word from three letters on");
    check(!matches(stock, "own"), "the Own_ every asset name starts with");
    // The mod's author, name and folder.
    check(matches(hat, "ada") && matches(hat, "Lane") && matches(hat, "author pack") && matches(hat, "author_pack"),
          "the mod's author, name and folder");
    check(matches(hat, "beanie ada"), "every word, each in the name or the mod");
    check(!matches(hat, "ada vans"), "a word found nowhere hides the item");
    check(!matches(hat, "laneauthor") && !matches(hat, "packada"), "a word does not run from one part into the next");
    check(!matches(stock, "ada") && !matches(stock, "pack"), "an item with no known mod is found as before");

    // The search in the grid's order: a mod's name shows its items and leaves the rest out.
    const std::vector<Item> grid{item("Own_HatCap_Red", "Cap"), hat, item("Own_HatVisor_Ada", "Visor", pack), stock};
    const Keys none;
    const auto shown = arrange(grid, none, "author pack", Filter::all);
    check(shown == std::vector<unsigned>{1, 2}, "the pack's items, in the game's order");
    check(arrange(grid, none, "ada", Filter::game) == std::vector<unsigned>{0}, "nothing left: the first tile keeps the focus");

    // Show and X: the game's own items are "skate.".
    check(filter_names[static_cast<std::size_t>(Filter::game)] == "skate." && filter_names[static_cast<std::size_t>(Filter::mods)] == "Mods",
          "the filters' names");

    // The pickers: which grids have them, by their items' category.
    const auto in = [](std::string category) { Item i; i.key = "k"; i.category = std::move(category); return std::vector<Item>{Item{}, i}; };
    // The game's own clothing categories, "cust_" and the slot (a costume's too), and the body's.
    for (const auto category : {"cust_tops", "cust_bottoms", "cust_shoes", "cust_socks", "cust_hats", "cust_handwear",
                                "cust_eyewear", "cust_earwear", "cust_nosewear", "cust_face_accessory", "cust_outfits",
                                "cust_fullbodycostume", "cust_fullBodyCostume", "shirts", "pants"})
        check(picker_of(in(category)) == Picker::clothing, std::string(category) + " is the clothing picker");
    // The skater's appearance (the body's own parts), tattoos and gestures: each its own picker.
    for (const auto category : {"custhead", "custbodies", "custnoses", "custHair", "cust_makeup", "cust_freckles", "cust_acne",
                                "cust_moles", "cust_vitiligo", "cust_maturity", "hair", "beards", "skin_tone", "eyebrows"})
        check(picker_of(in(category)) == Picker::appearance, std::string(category) + " is the appearance picker");
    for (const auto category : {"tattoo_arml_1", "tattoo_chest_1", "Tattoos"})
        check(picker_of(in(category)) == Picker::tattoos, std::string(category) + " is the tattoos picker");
    for (const auto category : {"gestures", "Gestures", "emotes"})
        check(picker_of(in(category)) == Picker::gestures, std::string(category) + " is the gestures picker");
    // Clothing that names a part of the body stays clothing.
    for (const auto category : {"cust_eyebrowwear_left", "cust_nosewear", "cust_face_accessory"})
        check(picker_of(in(category)) == Picker::clothing, std::string(category) + " stays clothing");
    // What is none of them.
    for (const auto category : {"voice", "locomotion", "bkobject_x"})
        check(picker_of(in(category)) == Picker::none, std::string(category) + " has no picker");
    check(picker_names[static_cast<std::size_t>(Picker::appearance)] == "appearance" && picker_count == 6, "the pickers' names");
    // Most of the grid decides; a tie keeps the filters; unknown categories have no say.
    {
        const auto item = [](std::string category) { Item i; i.key = "k"; i.category = std::move(category); return i; };
        const std::vector<Item> tops{Item{}, item("cust_tops"), item("cust_tops"), item("tattoo_x"), item("misc_x")};
        check(picker_of(tops) == Picker::clothing && main_category(tops) == "cust_tops", "most of the grid decides");
        const std::vector<Item> body{item("custhead"), item("custhead"), item("cust_tops")};
        check(picker_of(body) == Picker::appearance, "a stray item does not make the body clothing");
        check(picker_of(std::vector<Item>{item("cust_tops"), item("custhead")}) == Picker::clothing, "a tie keeps the filters");
        check(picker_of(std::vector<Item>{item("misc_x")}) == Picker::none, "an unknown category alone has no picker");
    }
    check(picker_of(in("HeadwearHats")) == Picker::clothing && picker_of(in("Tops")) == Picker::clothing, "clothes");
    check(picker_of(in("BoardDeck")) == Picker::board && picker_of(in("Wheels")) == Picker::board &&
          picker_of(in("GripTape")) == Picker::board, "the board's parts");
    check(picker_of({}) == Picker::none && picker_of(std::vector<Item>{Item{}}) == Picker::none, "no picker without categories");

    // Author and Mod pack: the grid's mods, one each, by name; their authors, by name.
    const auto from = [](std::string key, std::string title, std::string author, std::string folder, std::string name) {
        auto result = item(std::move(key), std::move(title), mod_words(author, name, folder));
        result.author = std::move(author);
        result.pack = std::move(folder);
        result.pack_title = std::move(name);
        return result;
    };
    const std::vector<Item> shelf{Item{}, item("Own_Cap_Red", "Red Cap"), from("A1", "Zebra Tee", "bo", "bo_tees", "Tees"),
                                  from("A2", "Apple Tee", "Ada", "ada_basics", "basics"), from("A3", "Mango Tee", "bo", "bo_caps", "Caps"),
                                  from("A4", "Kiwi Tee", "", "anon", "Anon Pack"), from("A5", "Lime Tee", "bo", "bo_tees", "Tees")};
    const auto packs = packs_in(shelf);
    check(packs.size() == 4 && packs[0].title == "Anon Pack" && packs[1].title == "basics" && packs[2].title == "Caps" &&
          packs[3].title == "Tees", "one pack each, by name without regard to case");
    check(authors_of(packs) == std::vector<std::string>{"Ada", "bo"}, "one author each, none for a mod naming none");

    // They stack with each other, the search and Show, and the blank tile stays in front.
    Narrow by;
    by.author = "bo";
    check(arrange(shelf, none, "", Filter::all, by) == std::vector<unsigned>{0, 2, 4, 6}, "an author's items");
    by.pack = "bo_tees";
    check(arrange(shelf, none, "", Filter::all, by) == std::vector<unsigned>{0, 2, 6}, "an author's pack");
    check(arrange(shelf, none, "lime", Filter::all, by) == std::vector<unsigned>{0, 6}, "with the search");
    const Keys starred{"A1"};
    check(arrange(shelf, starred, "", Filter::favorites, by) == std::vector<unsigned>{0, 2}, "with Favorites");
    by = {};
    by.pack = "ada_basics";
    check(arrange(shelf, none, "", Filter::all, by) == std::vector<unsigned>{0, 3}, "a pack, any author");
    by.author = "bo";
    check(arrange(shelf, none, "", Filter::all, by) == std::vector<unsigned>{0}, "nothing left: the blank tile");

    // Sort: within the favorites and the rest, the game's order where two are alike.
    by = {};
    check(arrange(shelf, none, "", Filter::all, by) == std::vector<unsigned>{0, 1, 2, 3, 4, 5, 6}, "the game's order by default");
    by.sort = Sort::name;
    check(arrange(shelf, none, "", Filter::all, by) == std::vector<unsigned>{0, 3, 5, 6, 4, 1, 2}, "name A-Z");
    by.sort = Sort::name_descending;
    check(arrange(shelf, none, "", Filter::all, by) == std::vector<unsigned>{0, 2, 1, 4, 6, 5, 3}, "name Z-A");
    by.sort = Sort::pack;
    check(arrange(shelf, none, "", Filter::all, by) == std::vector<unsigned>{0, 5, 3, 4, 2, 6, 1}, "by pack, no mod last");
    by.sort = Sort::author;
    check(arrange(shelf, none, "", Filter::all, by) == std::vector<unsigned>{0, 3, 4, 2, 6, 1, 5}, "by author then pack, none last");
    check(arrange(shelf, Keys{"A5"}, "", Filter::all, by) == std::vector<unsigned>{0, 6, 3, 4, 2, 1, 5}, "favorites still first");
    std::cout << "item browser order tests passed\n";
}
