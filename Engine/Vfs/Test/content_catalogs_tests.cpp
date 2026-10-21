// Reads the installed content cache (or the folder given as the first argument)
// and checks catalogue entries that are stable for build 24855063. Skips when no
// pack is available. A second argument writes the catalogues as JSON.
#include "Engine/Vfs/content_cache.h"
#include "Engine/Vfs/content_catalogs.h"
#include "Engine/Game/World/location_travel.h"
#include <fstream>
#include <iostream>

namespace {
int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
}

int main(int argc, char** argv) {
    using namespace dingosdk;
    const std::filesystem::path folder = argc > 1 ? std::filesystem::path(argv[1]) : content_cache::directory();
    if (!std::filesystem::exists(folder)) {
        std::cout << "No content cache at " << folder.string() << "; skipped.\n";
        return 0;
    }
    const auto catalogs = content_cache::read_catalogs(folder);
    check(catalogs.available, "catalogues read");
    check(catalogs.entitlements.size() == 381, "381 entitlements");
    check(catalogs.items.size() >= 3000, "owned items");
    check(catalogs.items.at("own_bkbanks_generic_volcanolarge_00001").value("title", "") == "Volcano, L Wood",
        "item keyed by owned asset id");
    check(catalogs.items.at("own_costume_gen_dembones_00001").value("rarity_id", "") == "legendary", "item rarity");
    // Unlocked by neighbourhood rank, though no rank record lists them as rewards.
    check(!catalogs.reserved("own_bkrails_generic_urbanpolejamsmall_00001") &&
        !catalogs.reserved("own_bkindustrial_generic_signstopsign_00001"), "rank unlocks are not store items");
    // Objects nothing offers are open; ones sold, bundled or from a collab are not.
    check(!catalogs.reserved("own_bkrails_generic_urbanpolejammedium_00001") &&
        !catalogs.reserved("own_bkprops_generic_signnoskateboarding_00001"), "objects nothing offers are open");
    check(catalogs.reserved("own_bkbounceable_generic_dartboard_00001"), "an object sold for hard currency");
    check(catalogs.reserved("own_bkramps_generic_shoeboxanglednike_00001"), "an object from a store bundle");
    check(catalogs.reserved("own_bkpads_generic_manualpadroundxgames_00001"), "a collab object");
    check(catalogs.reserved("own_deckgraphic_gen_popsicle_00123"), "cosmetics nothing offers stay reserved");
    const auto& line = catalogs.challenges.at("Base_CH02_M02_Line01");
    check(line.value("type", "") == "Line" && line.value("neighborhood", "") == "neighbourhood_rank_historic",
        "challenge type and neighbourhood");
    check(line.at("asset").string() == "activities/challenges/prefabs/line/base_ch02_m02_line01_ecsprefab",
        "challenge asset path");
    check(line.at("goals").size() == 4 && line.at("goals").at(0).at("id").string() == "Temp_CompleteLine" &&
        line.at("goals").at(1).at("feed_key").string() == "ID_CRITERIA_MANUALDIST_FEED", "challenge goals in order");
    check(!catalogs.challenges.contains("location_bam"), "records without goals are not challenges");
    const content_cache::ObjectCategory* ramp{};
    std::size_t browser{};
    for (const auto& category : catalogs.object_categories) {
        browser += category.quick_drop;
        if (category.id == "category_ramp") ramp = &category;
    }
    check(browser == 5, "five Object Browser categories");
    check(ramp && ramp->title == "Ramp" && ramp->quick_drop && !ramp->groups.empty() &&
        ramp->groups.front().id == "qdparent_aframe" && ramp->groups.front().title == "A Frame",
        "Ramp category with its object groups");
    check(catalogs.items.at("own_bkramps_generic_aframelong_00001").value("group", "") == "qdparent_aframe",
        "object names its group");
    const auto travel = location_travel_policy(Json::object(), catalogs);
    check(travel.enabled && travel.destinations.size() == 4, "four travel destinations");
    if (const auto* bam = find_travel_destination(travel, "location_bam"))
        check(bam->map == "bam" && bam->name == "San Van Downtown" && bam->medium == 1 &&
            bam->white_icon == "https://dingo-dev-assets.akamaized.net/cdn/production/c3257fa9304b1a916f4f580ef13de468",
            "San Van destination");
    else check(false, "San Van destination");
    if (const auto* mpr = find_travel_destination(travel, "location_mpr"))
        check(mpr->name == "SUMR Camp" && mpr->large_image.ends_with("-windows-default"), "SUMR Camp destination");
    else check(false, "SUMR Camp destination");
    const auto stadium = travel.access_points.find("accesspoint_bam_stadium");
    check(stadium != travel.access_points.end() && stadium->second == std::vector<std::string>{"location_stadium"},
        "stadium entrance offers only the first stadium");
    check(travel.access_points.size() == 5, "five access points");
    check(catalogs.music_playlists.size() == 54, "54 music playlists");
    if (const auto station = catalogs.music_playlists.find("0_Licensed_All"); station != catalogs.music_playlists.end())
        check(station->second.name == "San Van Soundtrack" && station->second.tracks.size() == 107 &&
            station->second.tracks.front() == "2hollis - flash" &&
            station->second.artwork == "cdn:/0e63e2111f9088448ae261ab0f930a4a", "San Van Soundtrack station");
    else check(false, "San Van Soundtrack station");
    if (const auto ballroom = catalogs.music_playlists.find("1920sBallroom"); ballroom != catalogs.music_playlists.end())
        check(ballroom->second.name == "Ballroom 237" && ballroom->second.tracks.size() == 3, "Ballroom station");
    else check(false, "Ballroom station");
    if (const auto art = catalogs.music_song_artwork.find("Freddie Gibbs, The Alchemist, Anderson .Paak - Ensalada");
        art != catalogs.music_song_artwork.end())
        check(art->second == "cdn:/ad89945108fd5d7d6fc94ccb33c0f500", "a song's cover art");
    else check(false, "a song's cover art");
    check(catalogs.music_song_artwork.size() >= 100, "the licensed songs have cover art");
    if (argc > 2) {
        Json dump{{"items", catalogs.items}, {"challenges", catalogs.challenges}, {"entitlements", Json::array()}};
        for (const auto& id : catalogs.entitlements) dump["entitlements"].push_back(id);
        std::ofstream(argv[2], std::ios::binary) << dump.dump(1);
    }
    // Build 24855063: 42 of 439 (28 sold for hard currency, 3 in the Nike bundle, 11 X Games).
    std::size_t objects{}, reserved_objects{};
    for (const auto& [key, item] : catalogs.items.items())
        if (key.starts_with("own_bk")) { ++objects; reserved_objects += catalogs.reserved(key); }
    std::cout << reserved_objects << " of " << objects << " objects reserved.\n";
    std::cout << catalogs.items.size() << " items, " << catalogs.challenges.size() << " challenges, "
              << catalogs.entitlements.size() << " entitlements.\n";
    return failures ? 1 : 0;
}
