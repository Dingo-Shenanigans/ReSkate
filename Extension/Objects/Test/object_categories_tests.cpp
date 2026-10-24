// Checks the Object Browser layout built from category records, first on a small
// hand-made catalogue, then on the installed content cache when there is one.
#include "Extension/Objects/object_categories.h"
#include "Engine/Vfs/content_cache.h"
#include <iostream>

namespace {
int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
const dingosdk::objects::BrowserCategory* find(const std::vector<dingosdk::objects::BrowserCategory>& layout,
    std::string_view id) {
    for (const auto& category : layout)
        if (category.id == id) return &category;
    return nullptr;
}
}

int main() {
    using namespace dingosdk;
    content_cache::Catalogs catalogs;
    catalogs.available = true;
    catalogs.items["own_bkramps_aframe_long_00001"] = Json{{"title", "A Frame, Long"}, {"group", "qdparent_aframe"}};
    catalogs.items["own_bkramps_aframe_short_00001"] = Json{{"title", "A Frame, Short"}, {"group", "qdparent_aframe"}};
    catalogs.items["own_bkrails_flat_00001"] = Json{{"group", "qdparent_flatrail"}};
    catalogs.items["own_bkvertbasesmall_00001"] = Json{{"group", "bkvertbasesmall"}};
    catalogs.object_categories = {
        {"category_bk", "Build Kit", "cdn:/1", 1, false, {{"bkvertbasesmall", "Bowl Small", 0}}},
        {"category_rail", "Rail", "cdn:/2", 2, true, {{"qdparent_flatrail", "Flat Rail", 0}}},
        {"category_ramp", "Ramp", "cdn:/3", 1, true, {{"qdparent_aframe", "A Frame", 0}}},
    };
    const std::vector<objects::InstalledObject> owned{
        {"own_bkramps_aframe_long_00001", "bkramps", "A Frame, Long"},
        {"own_bkramps_aframe_short_00001", "bkramps", "A Frame, Short"},
        {"own_bkrails_flat_00001", "bkrails", "Flat"},
        {"own_bkvertbasesmall_00001", "bkvertbasesmall", "Bowl"},
        {"own_bkprops_unknown_00001", "bkprops", "Unknown prop"},
    };
    const auto layout = objects::browser_layout(owned, catalogs);
    check(layout.size() == 4, "four categories");
    check(layout.size() == 4 && layout[0].id == "category_ramp" && layout[1].id == "category_rail" &&
        layout[2].id == "category_bk" && layout[3].id == "bkprops",
        "browser categories by priority, then other build-kit ones, then unplaced families");
    if (const auto* ramp = find(layout, "category_ramp")) {
        check(ramp->title == "Ramp" && ramp->tiles.size() == 1, "variants share one tile");
        check(ramp->tiles[0].id == "qdparent_aframe" && ramp->tiles[0].title == "A Frame" &&
            ramp->tiles[0].items.size() == 2, "tile named after its group");
    }
    if (const auto* other = find(layout, "bkprops"))
        check(other->tiles.size() == 1 && other->tiles[0].id == "own_bkprops_unknown_00001" &&
            other->tiles[0].title == "Unknown prop", "unplaced object gets its own tile");
    const auto groups = objects::object_groups(catalogs);
    check(objects::browser_tile("OWN_BKRAMPS_AFRAME_LONG_00001", catalogs, groups) == "qdparent_aframe",
        "tile lookup ignores case");
    check(objects::browser_tile("own_bkprops_unknown_00001", catalogs, groups) == "own_bkprops_unknown_00001",
        "unplaced tile is the object itself");

    // A mod's reskate-objects.json: a category and families of its own, a family in the game's Rail
    // category and an object in the game's A Frame family. It cannot rename or regroup the game's items.
    {
        auto modded = catalogs;
        const auto refused = content_cache::add_mod_objects(modded, R"({"schema": 1,
            "categories": [{"id": "category_shoebox", "title": "Shoebox Pads"}, {"id": "category_ramp"}],
            "families": [
                {"id": "qdparent_shoebox_dc", "title": "DC Shoebox", "category": "category_shoebox"},
                {"id": "qdparent_shoebox_etnies", "title": "Etnies Shoebox", "category": "category_shoebox", "priority": 0},
                {"id": "qdparent_mod_rail", "title": "Mod Rail", "category": "category_rail"},
                {"id": "qdparent_aframe", "category": "category_shoebox"},
                {"id": "qdparent_orphan", "category": "category_missing"}],
            "objects": [
                {"key": "own_bkrails_shoebox_dc_12m", "title": "DC Shoebox 12m", "family": "qdparent_shoebox_dc"},
                {"key": "OWN_BKRAILS_SHOEBOX_DC_9M_RED", "title": "DC Shoebox 9m, Red", "family": "qdparent_shoebox_dc"},
                {"key": "own_bkrails_shoebox_dc_6m_blue", "family": "qdparent_shoebox_dc"},
                {"key": "own_bkrails_shoebox_etnies_12m", "title": "Etnies Shoebox 12m", "family": "qdparent_shoebox_etnies"},
                {"key": "own_bkramps_mod_aframe_00001", "title": "A Frame, Mod", "family": "qdparent_aframe"},
                {"key": "own_bkprops_named_00001", "title": "Named Prop"},
                {"key": "own_bkramps_aframe_long_00001", "title": "Renamed", "family": "qdparent_shoebox_dc"},
                {"key": "own_bkrails_shoebox_dc_12m", "title": "Twice"},
                {"key": "own_bkprops_lost_00001", "family": "qdparent_missing"},
                {"key": "bkprops_no_prefix", "title": "No prefix"}]})");
        check(refused.size() == 7, "taken ids, the game's item, a repeat, a missing family and a bad key refused");
        const auto* shoebox = &modded.object_categories.back();
        check(shoebox->id == "category_shoebox" && shoebox->title == "Shoebox Pads" && shoebox->quick_drop &&
            shoebox->priority == 3 && shoebox->groups.size() == 2, "mod category after the game's, in the browser");
        check(shoebox->groups[0].priority == 1 && shoebox->groups[1].priority == 0, "family priorities");
        const auto& rail = modded.object_categories[1];
        check(rail.groups.size() == 2 && rail.groups[1].id == "qdparent_mod_rail" && rail.groups[1].priority == 1,
            "mod family in the game's category");
        check(modded.items.at("own_bkramps_aframe_long_00001").value("title", "") == "A Frame, Long" &&
            !modded.mod_objects.contains("own_bkramps_aframe_long_00001"), "the game's item keeps its name and family");
        check(modded.mod_objects.at("own_bkrails_shoebox_dc_12m").value("title", "") == "DC Shoebox 12m" &&
            modded.mod_objects.at("own_bkrails_shoebox_dc_9m_red").value("group", "") == "qdparent_shoebox_dc" &&
            modded.mod_objects.at("own_bkrails_shoebox_dc_9m_red").value("object_type", "") == "QuickDrop",
            "first name wins, keys are lower case, family objects are Quick Drop objects");
        check(!modded.mod_objects.at("own_bkprops_named_00001").contains("group"), "a title alone places nothing");
        check(!modded.reserved("own_bkrails_shoebox_dc_12m"), "mod objects are never store items");

        const std::vector<objects::InstalledObject> installed{
            {"own_bkramps_aframe_long_00001", "bkramps", "A Frame, Long"},
            {"own_bkramps_aframe_short_00001", "bkramps", "A Frame, Short"},
            {"own_bkrails_flat_00001", "bkrails", "Flat"},
            {"own_bkvertbasesmall_00001", "bkvertbasesmall", "Bowl"},
            {"own_bkrails_shoebox_dc_12m", "bkrails", "DC Shoebox 12m"},
            {"own_bkrails_shoebox_dc_6m_blue", "bkrails", "own bkrails shoebox dc 6m blue"},
            {"own_bkrails_shoebox_dc_9m_red", "bkrails", "DC Shoebox 9m, Red"},
            {"own_bkrails_shoebox_etnies_12m", "bkrails", "Etnies Shoebox 12m"},
            {"own_bkramps_mod_aframe_00001", "bkramps", "A Frame, Mod"},
            {"own_bkprops_named_00001", "bkprops", "Named Prop"},
        };
        const auto mod_layout = objects::browser_layout(installed, modded);
        check(mod_layout.size() == 5 && mod_layout[2].id == "category_shoebox" && mod_layout[4].id == "bkprops",
            "mod category among the browser categories, by priority");
        if (const auto* pads = find(mod_layout, "category_shoebox")) {
            check(pads->tiles.size() == 2 && pads->tiles[0].id == "qdparent_shoebox_etnies" &&
                pads->tiles[1].id == "qdparent_shoebox_dc", "one tile per mod family");
            check(pads->tiles.size() == 2 && pads->tiles[1].items.size() == 3 &&
                pads->tiles[1].items[0] == "own_bkrails_shoebox_dc_12m", "a family's objects share its tile");
        }
        if (const auto* ramp = find(mod_layout, "category_ramp"))
            check(ramp->tiles.size() == 1 && ramp->tiles[0].items.size() == 3, "mod object in the game's family");
        const auto mod_groups = objects::object_groups(modded);
        check(objects::browser_tile("OWN_BKRAILS_SHOEBOX_DC_9M_RED", modded, mod_groups) == "qdparent_shoebox_dc",
            "mod object tile lookup ignores case");

        const auto unchanged = [&](const char* json) {
            auto copy = catalogs;
            try { content_cache::add_mod_objects(copy, json); } catch (const std::exception&) {
                return copy.mod_objects.size() == 0 && copy.object_categories.size() == catalogs.object_categories.size();
            }
            return false;
        };
        check(unchanged(R"({"schema": 2, "objects": []})"), "another schema is refused");
        check(unchanged(R"({"schema": 1, "categories": [{"id": "category_new"}], "objects": {}})"),
            "a file that is not this shape adds nothing");
        check(unchanged("not json"), "a file that is not JSON is refused");
    }

    if (content_cache::installed()) {
        const auto& cache = content_cache::catalogs();
        std::vector<std::string> browser;
        for (const auto& category : cache.object_categories)
            if (category.quick_drop) browser.push_back(category.title);
        check(browser.size() == 5, "five Object Browser categories in the cache");
        const auto cached = objects::object_groups(cache);
        const auto placement = objects::object_placement("own_bkramps_generic_aframelong_00001", cache, cached);
        check(placement.category && placement.category->title == "Ramp" && placement.group &&
            placement.group->title == "A Frame", "cached object placed under Ramp / A Frame");
        check(cache.items.at("own_bkramps_generic_aframelong_00001").value("object_type", "") == "QuickDrop",
            "cached object type");
        std::cout << browser.size() << " browser categories, " << cached.size() << " object groups in the cache.\n";
    } else {
        std::cout << "No content cache installed; cache checks skipped.\n";
    }
    return failures ? 1 : 0;
}
