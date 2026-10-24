// The maps Settings offers to start in, and the "load" the launcher asks the game for.
#include "Launcher/start_maps.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace dingosdk;
namespace maps = launcher_start_maps;
namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (condition) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}

void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

} // namespace

int main(int argc, char** argv) {
    const fs::path root = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "reskate-start-maps-tests";
    std::error_code ignored;
    fs::remove_all(root, ignored);

    // No Mods folder: the game's own maps, San Vansterdam first.
    auto list = maps::start_maps(root);
    check(list.size() == 6, "The game's six maps are not all offered");
    check(!list.empty() && list.front().asset == maps::san_vansterdam && list.front().name == "San Vansterdam",
          "San Vansterdam is not first");
    check(maps::load_arguments(list.front()).empty(), "San Vansterdam asks for a load");
    const auto* grom = maps::find_start_map(list, "levels/game/dingolevel_isle_of_grom/dingolevel_isle_of_grom");
    check(grom && grom->name == "Isle of Grom" && grom->mod.empty(), "A retail map is not found whatever the case");
    check(grom && maps::load_arguments(*grom) == L"Levels/Game/DingoLevel_Isle_of_Grom/DingoLevel_Isle_of_Grom",
          "A retail map does not load by its asset");

    write(root / "Mods" / "Map Pack" / "reskate-levels.json", R"({"schema":1,"levels":[
        {"asset":"levels/game/dingolevel_reskate_one/dingolevel_reskate_one","displayName":"One","startPoints":["only"]},
        {"asset":"levels/game/dingolevel_reskate_two/dingolevel_reskate_two","displayName":"Two","startPoints":["Front Gate","back"]},
        {"asset":"levels/game/dingolevel_reskate_three/dingolevel_reskate_three","displayName":"Three","startPoints":["a","b"]}]})");
    write(root / "Mods" / "Switched Off" / "reskate-levels.json", R"({"schema":1,"levels":[
        {"asset":"levels/game/dingolevel_reskate_off/dingolevel_reskate_off","displayName":"Off","startPoints":[]}]})");
    write(root / "Mods" / "Broken" / "reskate-levels.json", R"({"schema":1,"levels":[{"asset":"../x"}]})");
    write(root / "Mods" / "mods.json", R"({"schema":1,"mods":[{"name":"Switched Off","enabled":false}]})");

    list = maps::start_maps(root);
    check(list.size() == 9, "The switched-on map mod's maps are not all offered, or others are");
    check(!maps::find_start_map(list, "levels/game/dingolevel_reskate_off/dingolevel_reskate_off"),
          "A switched-off mod's map is offered");
    const auto* one = maps::find_start_map(list, "levels/game/dingolevel_reskate_one/dingolevel_reskate_one");
    check(one && one->name == "One" && one->mod == "Map Pack", "A map mod's map is not named with its mod");
    check(one && maps::load_arguments(*one) == L"levels/game/dingolevel_reskate_one/dingolevel_reskate_one",
          "A map with one start point is given one");
    const auto* two = maps::find_start_map(list, "levels/game/dingolevel_reskate_two/dingolevel_reskate_two");
    check(two && maps::load_arguments(*two) == L"levels/game/dingolevel_reskate_two/dingolevel_reskate_two 'Front Gate'",
          "A map with several start points does not start at its first, quoted");
    const auto* three = maps::find_start_map(list, "levels/game/dingolevel_reskate_three/dingolevel_reskate_three");
    check(three && maps::load_arguments(*three) == L"levels/game/dingolevel_reskate_three/dingolevel_reskate_three a",
          "A plain start point is quoted");

    fs::remove_all(root, ignored);
    if (failures) return 1;
    std::cout << "start maps: ok\n";
    return 0;
}
