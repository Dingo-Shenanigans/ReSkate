// The item grids' order and filters, apart from the game: favorites first, the search, and the
// player's own collections.
#include "Extension/Customization/item_browser_order.h"
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace dingosdk::item_browser;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

// A grid as the game lists it: the worn item first, then the rest by name.
std::vector<Item> grid() {
    return {{"Own_TopShirt_Gen_TshirtRelaxed_00002", "Zebra Tee", "tops"},
            {"Own_TopShirt_Gen_CropVneck_00001", "Crop Top", "tops"},
            {"Own_TopShirt_Gen_HoodieRelaxed_00009", "Grey Hoodie", "tops"},
            {"Own_TopVest_Gen_ConstructionHiViz_00001", "Hi-Viz Vest", "tops"}};
}

void a_collection_filter_shows_only_its_items() {
    const auto items = grid();
    const Keys favorites{"Own_TopVest_Gen_ConstructionHiViz_00001"};
    const Keys jewelry{"Own_TopShirt_Gen_CropVneck_00001", "Own_HoodieNotInThisGrid"};
    const auto order = arrange(items, favorites, "", Filter::collection, &jewelry);
    check(order == std::vector<unsigned>{1}, "The collection's item in this grid, nothing else");
    check(arrange(items, favorites, "", Filter::collection, nullptr) == std::vector<unsigned>{0},
          "No collection: the grid keeps a tile to hold the focus");
    check(arrange(items, favorites, "", Filter::all, &jewelry) == std::vector<unsigned>({0, 3, 1, 2}),
          "Other filters leave the collection alone: worn item, favorite, the rest");
    check(arrange(items, favorites, "crop", Filter::collection, &jewelry) == std::vector<unsigned>{1}, "The search narrows a collection too");
}

void a_tab_takes_its_items_from_any_grid() {
    const auto tops = grid();
    const std::vector<Item> hats{{"", "None", "hats"}, {"Own_HeadwearHat_Gen_BeanieWatch_00001", "Watch Beanie", "hats"},
                                 {"Own_HeadwearHat_Gen_BallcapTrucker_00001", "Trucker Cap", "hats"}};
    const Keys tab{"Own_TopShirt_Gen_CropVneck_00001", "Own_HeadwearHat_Gen_BeanieWatch_00001"};
    check(collection_tiles(tops, tab, "") == std::vector<unsigned>{1} && collection_tiles(hats, tab, "") == std::vector<unsigned>{1},
          "Each grid gives the tab's items it holds, and no none tile");
    check(collection_tiles(hats, tab, "trucker").empty() && collection_tiles(hats, tab, "beanie") == std::vector<unsigned>{1},
          "The search narrows a tab too");
}

void a_collection_is_picked_by_name_or_number() {
    const std::vector<Collection> collections{{"Jewelry", {}}, {"Gloves", {}}, {"Random stuff", {}}};
    check(collection_named(collections, "jewelry") == 0 && collection_named(collections, " GLOVES ") == 1, "By its name, any case");
    check(collection_named(collections, "3") == 2 && collection_named(collections, "1") == 0, "By its number, from 1");
    check(collection_named(collections, "4") == -1 && collection_named(collections, "0") == -1, "A number past the list names none");
    check(collection_named(collections, "Necklaces") == -1 && collection_named(collections, "") == -1, "A new name names none");
}
} // namespace

int main() {
    try {
        a_collection_filter_shows_only_its_items();
        a_tab_takes_its_items_from_any_grid();
        a_collection_is_picked_by_name_or_number();
    } catch (const std::exception& error) {
        std::cerr << "item browser test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "item browser tests passed\n";
    return 0;
}
