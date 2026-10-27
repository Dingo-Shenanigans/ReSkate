// The in-game Mod Manager's view of Thunderstore's listing: search, the front page's rows, what a
// package's button does, and what installing one brings with it.
#include "Extension/Assets/mod_store_view.h"

#include <algorithm>
#include <iostream>
#include <string>

using namespace dingosdk;
namespace ms = dingosdk::mod_store;
namespace ts = dingosdk::thunderstore;

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (condition) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}

// One package row of a listing, with one version.
std::string package(const std::string& owner, const std::string& name, const std::string& version, const std::string& categories,
                    const std::string& updated, unsigned downloads, const std::string& dependencies = {}, bool pinned = false,
                    bool deprecated = false, bool nsfw = false, const std::string& description = "A mod.") {
    const auto full = owner + '-' + name;
    return R"({"name": ")" + name + R"(", "full_name": ")" + full + R"(", "owner": ")" + owner + R"(",
      "package_url": "https://thunderstore.io/c/reskate/p/)" + owner + '/' + name + R"(/",
      "date_created": ")" + updated + R"(", "date_updated": ")" + updated + R"(",
      "rating_score": 1, "is_pinned": )" + (pinned ? "true" : "false") + R"(, "is_deprecated": )" + (deprecated ? "true" : "false") +
           R"(, "has_nsfw_content": )" + (nsfw ? "true" : "false") + R"(, "categories": [)" + categories + R"(],
      "versions": [{"name": ")" + name + R"(", "full_name": ")" + full + '-' + version + R"(", "description": ")" + description + R"(",
        "icon": "https://gcdn.thunderstore.io/live/repository/icons/)" + full + '-' + version + R"(.png",
        "version_number": ")" + version + R"(", "dependencies": [)" + dependencies + R"(], "downloads": )" + std::to_string(downloads) + R"(,
        "download_url": "https://thunderstore.io/package/download/)" + owner + '/' + name + '/' + version + R"(/",
        "date_created": ")" + updated + R"(", "website_url": "", "is_active": true, "file_size": 1048576}]})";
}

bool has(const std::vector<const ts::Package*>& list, std::string_view full_name) {
    return std::any_of(list.begin(), list.end(), [&](const ts::Package* p) { return p->full_name == full_name; });
}

} // namespace

int main() {
    const auto text = "[" +
        package("Zee", "Desert_Springs", "1.2.0", R"("Maps")", "2026-10-20T10:00:00Z", 5000) + "," +
        package("Sandos", "Vancouver_Plaza", "2.0.0", R"("Maps")", "2026-06-01T10:00:00Z", 9000) + "," +
        package("Fourth", "Board_Pack", "1.0.0", R"("Skateboards", "Cosmetics")", "2026-10-22T10:00:00Z", 300,
                R"("Fourth-Board_Lib-1.0.0", "Missing-Thing-1.0.0")") + "," +
        package("Fourth", "Board_Lib", "1.0.0", R"("Libraries")", "2026-10-01T10:00:00Z", 100) + "," +
        package("Team", "Pinned_Pack", "1.0.0", R"("Modpacks")", "2025-01-01T10:00:00Z", 10, {}, true) + "," +
        package("Old", "Deprecated_Map", "1.0.0", R"("Maps")", "2025-01-01T10:00:00Z", 80000, {}, false, true) + "," +
        package("Bad", "Not_Safe", "1.0.0", R"("Cosmetics")", "2026-10-23T10:00:00Z", 90000, {}, false, false, true) + "," +
        package("Bad", "Also_Not_Safe", "1.0.0", R"("NSFW")", "2026-10-23T10:00:00Z", 90000) + "," +
        package("Zee", "Hoodie", "1.0.0", R"("Cosmetics")", "2026-10-10T10:00:00Z", 700, {}, false, false, false, "A warm desert hoodie") + "," +
        package("Zee", "Map_Tool", "1.0.0", R"("Tools", "AI Generated", "Misc")", "2026-10-11T10:00:00Z", 50) + "]";
    const auto packages = ts::parse_listing(text);
    check(packages.size() == 10, "the fixture listing parses");
    ts::Installed none;

    // ---- search
    {
        const auto all = ms::search(packages, {}, none);
        check(!has(all, "Bad-Not_Safe") && !has(all, "Bad-Also_Not_Safe"), "NSFW packages are never listed, by mark or by category");
        check(!has(all, "Old-Deprecated_Map"), "a deprecated package is not listed when it is not installed");
        ts::Installed have{{"Old-Deprecated_Map", "1.0.0"}};
        check(has(ms::search(packages, {}, have), "Old-Deprecated_Map"), "a deprecated package is listed once it is installed");
        check(!all.empty() && all.front()->full_name == "Team-Pinned_Pack", "pinned packages come first");
        check(all.size() > 2 && all[1]->full_name == "Sandos-Vancouver_Plaza", "then the most downloaded");
        const auto desert = ms::search(packages, {"DESERT"}, none);
        check(desert.size() == 2 && has(desert, "Zee-Desert_Springs") && has(desert, "Zee-Hoodie"),
              "search finds the title and the description, in any case");
        check(ms::search(packages, {"sandos"}, none).size() == 1, "search finds the owner");
        const auto maps = ms::search(packages, {{}, {"Maps"}}, none);
        check(maps.size() == 2 && has(maps, "Zee-Desert_Springs") && has(maps, "Sandos-Vancouver_Plaza"), "a category shown only keeps only its packages");
        const auto several = ms::search(packages, {{}, {"Maps", "Skateboards"}}, none);
        check(several.size() == 3 && has(several, "Fourth-Board_Pack"), "several categories shown only keep the packages in any of them");
        const auto without = ms::search(packages, {{}, {}, {"Cosmetics"}}, none);
        check(!has(without, "Fourth-Board_Pack") && !has(without, "Zee-Hoodie") && has(without, "Zee-Desert_Springs"),
              "a hidden category leaves out every package in it, whatever else it is in");
        const auto both = ms::search(packages, {{}, {"Skateboards", "Maps"}, {"Cosmetics"}}, none);
        check(both.size() == 2 && !has(both, "Fourth-Board_Pack"), "hidden wins over shown only");
        const auto updated = ms::search(packages, {{}, {"Maps"}, {}, ms::Sort::updated}, none);
        check(updated.size() == 2 && updated.front()->full_name == "Zee-Desert_Springs", "LAST UPDATED puts the newest update first");
        const auto by_name = ms::search(packages, {{}, {}, {}, ms::Sort::name}, none);
        check(by_name.size() > 2 && by_name[1]->full_name == "Fourth-Board_Lib", "NAME sorts A to Z after the pinned ones");
    }

    // ---- categories
    {
        const auto categories = ms::categories(packages);
        const auto count = [&](std::string_view name) {
            for (const auto& [category, n] : categories)
                if (category == name) return n;
            return -1;
        };
        check(count("Maps") == 3 && count("Cosmetics") == 2, "categories count their packages, NSFW ones left out");
        check(count("NSFW") == -1, "a category only NSFW packages have is not offered");
        check(!categories.empty() && categories.front().first == "Maps", "the biggest category comes first");
    }

    // ---- the front page
    {
        const auto front = ms::front(packages, none, "2026-10-24", 3, 10);
        check(front.featured.size() == 3, "featured holds at most what was asked for");
        check(!front.featured.empty() && front.featured.front()->full_name == "Team-Pinned_Pack", "a pinned package is featured first");
        check(front.featured.size() == 3 && front.featured[1]->full_name == "Zee-Desert_Springs",
              "then the most downloaded of those updated in the last 60 days (not the older, more downloaded one)");
        check(std::none_of(front.featured.begin(), front.featured.end(), [](const ts::Package* p) { return ms::nsfw(*p) || p->deprecated; }),
              "nothing NSFW or deprecated is featured");
        const auto shelf = [&](std::string_view title) -> const ms::Shelf* {
            for (const auto& row : front.shelves)
                if (row.title == title) return &row;
            return nullptr;
        };
        const auto* popular = shelf("MOST POPULAR");
        check(popular && !popular->packages.empty() && popular->packages.front()->full_name == "Sandos-Vancouver_Plaza", "MOST POPULAR is by downloads");
        const auto* fresh = shelf("NEW");
        check(fresh && !fresh->packages.empty() && fresh->packages.front()->full_name == "Fourth-Board_Pack", "NEW is by when it was made");
        check(shelf("MAPS") && shelf("COSMETICS") && shelf("SKATEBOARDS") && shelf("MISC"), "every category gets a row of its own");
        check(!shelf("LIBRARIES") && !shelf("MODPACKS") && !shelf("TOOLS") && !shelf("AI GENERATED"),
              "but mod packs, libraries, tools and AI generated");
        check(!shelf("NSFW"), "a category only NSFW packages have gets no row");
        std::size_t maps_at{}, misc_at{};
        for (std::size_t i = 0; i < front.shelves.size(); ++i) {
            if (front.shelves[i].title == "MAPS") maps_at = i;
            if (front.shelves[i].title == "MISC") misc_at = i;
        }
        check(maps_at < misc_at, "the biggest categories come first");
        check(ms::front(packages, none, "2026-10-24", 8, 2).shelves.front().packages.size() == 2, "a row holds at most what was asked for");
    }

    // ---- what a package's button does
    {
        const auto* desert = ts::find_package(packages, "Zee-Desert_Springs");
        check(desert && ms::action_for(*desert, none) == ms::Action::install, "not installed: INSTALL");
        check(desert && ms::action_for(*desert, {{"Zee-Desert_Springs", "1.1.0"}}) == ms::Action::update, "older: UPDATE");
        check(desert && ms::action_for(*desert, {{"Zee-Desert_Springs", "1.2.0"}}) == ms::Action::installed, "the newest: INSTALLED");
    }

    // ---- what installing brings
    {
        const auto* pack = ts::find_package(packages, "Fourth-Board_Pack");
        check(pack != nullptr, "the pack is listed");
        if (pack) {
            const auto plan = ms::plan(std::vector<ts::Package>{*pack}, packages, none);
            check(plan.packages.size() == 2 && plan.packages.front().full_name == "Fourth-Board_Lib" && plan.packages.back().full_name == "Fourth-Board_Pack",
                  "what a package needs comes first");
            check(plan.unknown.size() == 1 && plan.unknown.front().starts_with("Missing-Thing"), "a dependency the listing lacks is named");
            check(plan.bytes == 2 * 1048576, "the download size adds up");
            const auto again = ms::plan(std::vector<ts::Package>{*pack}, packages, {{"Fourth-Board_Lib", "1.0.0"}});
            check(again.packages.size() == 1, "a dependency already installed at its version is not fetched again");
            const auto twice = ms::plan(std::vector<ts::Package>{*pack, *ts::find_package(packages, "Fourth-Board_Lib")}, packages, none);
            check(twice.packages.size() == 2, "each package is fetched once");
        }
    }

    // ---- text
    check(ms::size_text(1536 * 1024) == "1.5 MB" && ms::size_text(2048) == "2 KB", "sizes read as KB and MB");
    check(ms::count_text(950) == "950" && ms::count_text(12300) == "12.3K" && ms::count_text(2500000) == "2.5M", "counts read short");
    check(ms::date_text("2026-10-24T12:00:00Z") == "Oct 24, 2026", "dates read as a player would write them");

    if (failures) {
        std::cerr << failures << " mod store view check(s) failed\n";
        return 1;
    }
    std::cout << "mod store view: all checks passed\n";
    return 0;
}
