// Writing a map mod's newest version into its manifest.json (server_map_update.h), without the
// network.
#include "Server/server_map_update.h"
#include "Engine/Core/Json/json.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using namespace dingosdk;
using namespace dingosdk::server;

namespace {
int failures{};
void check(bool ok, const std::string &what) {
    if (ok) return;
    ++failures;
    std::cerr << "FAIL: " << what << '\n';
}
std::string read(const fs::path &file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}
void write(const fs::path &file, const std::string &text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

// With a Thunderstore package's folder name as its argument: asks Thunderstore about it and
// says what it would do, changing nothing (the map counts as being played). By hand only.
int look_up(const std::string &folder) {
    const auto mods = fs::temp_directory_path() / "reskate-server-map-lookup" / "Mods";
    std::error_code ignored;
    fs::remove_all(mods.parent_path(), ignored);
    write(mods / folder / "manifest.json", "{\"version_number\": \"0.0.0\"}");
    write(mods / folder / "reskate-levels.json", "{\"levels\": []}");
    for (const auto &update : update_maps(mods, folder))
        std::cout << update.folder << ": installed " << update.from << ", newest " << (update.to.empty() ? "?" : update.to)
                  << (update.waiting ? " (would be written)" : "") << (update.problem.empty() ? "" : " - " + update.problem) << std::endl;
    fs::remove_all(mods.parent_path(), ignored);
    return 0;
}
} // namespace

int main(int argc, char **argv) {
    if (argc > 1) return look_up(argv[1]);
    const auto mods = fs::temp_directory_path() / "reskate-server-map-update-tests" / "Mods";
    std::error_code ignored;
    fs::remove_all(mods.parent_path(), ignored);
    try {
        // The version changes and the rest of the manifest is kept.
        const auto map = mods / "Team-Map";
        write(map / "manifest.json", "{\"name\": \"Map\", \"version_number\": \"1.0.0\", \"dependencies\": [\"A-B-1.0.0\"], \"website_url\": \"\"}");
        write(map / "reskate-levels.json", "{\"levels\": [1]}");
        check(map_version(map) == "1.0.0", "A manifest's version was not read");
        set_map_version(map, "1.2.0");
        const auto after = Json::parse(read(map / "manifest.json"));
        check(map_version(map) == "1.2.0", "The new version is not in the manifest");
        check(after.value("name", "") == "Map" && after.contains("dependencies") && after.at("dependencies").size() == 1 && after.contains("website_url"),
              "Writing the version lost something else of the manifest");
        check(read(map / "reskate-levels.json") == "{\"levels\": [1]}", "Writing the version touched the level list");
        check(!fs::exists(map / "manifest.json.new"), "The manifest's working file was left behind");

        // A folder with only a level list gets a manifest with the version alone.
        const auto lite = mods / "Team-Lite";
        write(lite / "reskate-levels.json", "{\"levels\": []}");
        check(map_version(lite).empty(), "A folder without a manifest has a version");
        set_map_version(lite, "3.0.0");
        check(map_version(lite) == "3.0.0", "A folder without a manifest did not get one with its version");

        // A manifest that is not what it should be is left as it is.
        const auto odd = mods / "Team-Odd";
        write(odd / "manifest.json", "[1, 2, 3]");
        bool threw{};
        try {
            set_map_version(odd, "9.9.9");
        } catch (const std::exception &) {
            threw = true;
        }
        check(threw && read(odd / "manifest.json") == "[1, 2, 3]", "A manifest that is not an object was written over");
        write(odd / "manifest.json", "{ not json");
        threw = false;
        try {
            set_map_version(odd, "9.9.9");
        } catch (const std::exception &) {
            threw = true;
        }
        check(threw && read(odd / "manifest.json") == "{ not json", "A manifest that cannot be read was written over");
    } catch (const std::exception &e) {
        check(false, std::string("The test stopped: ") + e.what());
    }
    fs::remove_all(mods.parent_path(), ignored);
    if (!failures) std::cout << "Server map update tests passed.\n";
    return failures ? 1 : 0;
}
