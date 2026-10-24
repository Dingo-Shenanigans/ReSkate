#include "Engine/Vfs/mod_catalog.h"
#include "Engine/Game/Build/supported_build.h"
#include <Windows.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using namespace dingosdk;
namespace {
void check(bool ok, const std::string &message) {
    if (ok) return;
    std::cerr << message << '\n';
    std::exit(1);
}
void write(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}
const mods::Mod *find(const mods::ModList &list, std::string_view name) {
    for (const auto &entry : list.entries)
        if (entry.mod.name == name) return &entry.mod;
    return nullptr;
}
bool listed(const std::vector<mods::Mod> &mods, std::string_view name) {
    return std::ranges::any_of(mods, [&](const mods::Mod &mod) { return mod.name == name; });
}
} // namespace

int main() {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const auto root = fs::path(temp) / ("reskate-mod-stamp-" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(root);
    const auto mods_root = root / "Mods";
    const std::string current(supported_build::game_sha256);
    const std::string other = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    // Built by ReSkate Studio for this game (CRLF and an upper-case hash are fine).
    write(mods_root / "Current" / "layout.toc", "x");
    std::string upper = current;
    for (auto &c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    write(mods_root / "Current" / ".reskate-studio-patch", "ReSkate Studio native Patch v1\r\nskate_sha256=" + upper + "\r\n");
    // An older Studio: the stamp without a game version.
    write(mods_root / "OldStudio" / "layout.toc", "x");
    write(mods_root / "OldStudio" / ".reskate-studio-patch", "ReSkate Studio native Patch v1\n");
    // Built for another Skate.exe.
    write(mods_root / "OtherBuild" / "layout.toc", "x");
    write(mods_root / "OtherBuild" / ".reskate-studio-patch", "ReSkate Studio native Patch v1\nskate_sha256=" + other + "\n");
    // Game data with no stamp at all.
    write(mods_root / "Unstamped" / "layout.toc", "x");
    // A park mod ships no game data: no stamp needed.
    write(mods_root / "Park" / "parks" / "sanvan.park.json", "{}");
    // Disabled and built for another game: still left out of the merge.
    write(mods_root / "OffOld" / "layout.toc", "x");
    write(mods_root / "OffOld" / ".reskate-studio-patch", "ReSkate Studio native Patch v1\nskate_sha256=" + other + "\n");
    write(mods_root / "mods.json", R"({"schema":1,"mods":[{"name":"OffOld","enabled":false}]})");

    const auto list = mods::scan_mods(root);
    check(list.issue.empty(), "The Mods folder was not read: " + list.issue);
    check(find(list, "Current") && find(list, "Current")->outdated.empty() && find(list, "Current")->skate_sha256 == current,
          "A mod built for this game was marked outdated");
    check(find(list, "OldStudio") && !find(list, "OldStudio")->outdated.empty(), "A stamp without a game version was accepted");
    check(find(list, "OtherBuild") && !find(list, "OtherBuild")->outdated.empty(), "A mod for another Skate.exe was accepted");
    check(find(list, "Unstamped") && !find(list, "Unstamped")->outdated.empty(), "Unstamped game data was accepted");
    check(find(list, "Park") && find(list, "Park")->outdated.empty(), "A park mod needed a game version stamp");

    const auto catalog = mods::load_catalog(root, {}, false);
    check(catalog.issue.empty(), "The catalog was rejected: " + catalog.issue);
    check(listed(catalog.mods, "Current") && listed(catalog.mods, "Park"), "A current mod was left out");
    for (const auto *name : {"OldStudio", "OtherBuild", "Unstamped"})
        check(!listed(catalog.mods, name) && listed(catalog.excluded, name) && listed(catalog.outdated, name),
              std::string("Outdated mod ") + name + " was not left out");
    check(listed(catalog.outdated, "OffOld") && !listed(catalog.inactive, "OffOld"), "A disabled outdated mod stayed in the merge");
    check(std::ranges::any_of(catalog.warnings, [](const std::string &w) { return w.find("OtherBuild") != std::string::npos && w.find("outdated") != std::string::npos; }),
          "No warning names the outdated mod");
    // Plugins' files from before their own folder survive the merge's generated folder: moved whole,
    // or merged into a folder the plugin has already, its own files kept.
    write(mods_root / ".reskate" / "data" / "racer" / "routes" / "bam" / "yes.json", "{\"name\":\"yes\"}");
    write(mods_root / ".reskate" / "data" / "both" / "old.txt", "old");
    write(mods_root / ".reskate" / "data" / "both" / "same.txt", "old");
    write(mods_root / ".reskate_plugin_data" / "both" / "same.txt", "new");
    const auto moved = mods::keep_plugin_data(mods_root);
    check(moved.size() == 2, "Both plugins' folders were not moved");
    check(fs::is_regular_file(mods_root / ".reskate_plugin_data" / "racer" / "routes" / "bam" / "yes.json") &&
              !fs::exists(mods_root / ".reskate" / "data" / "racer"),
          "A plugin's folder was not moved whole");
    std::ifstream same(mods_root / ".reskate_plugin_data" / "both" / "same.txt");
    std::string kept;
    std::getline(same, kept);
    same.close();
    check(fs::is_regular_file(mods_root / ".reskate_plugin_data" / "both" / "old.txt") && kept == "new",
          "A plugin with a folder already did not get the files it lacked, or lost its own");
    check(mods::keep_plugin_data(mods_root).size() <= 1, "Moving again moved more than the leftover copy");
    fs::remove_all(root);
    std::cout << "Mod build stamps: current, older Studio, other build, unstamped, park-only and disabled mods passed; plugin data kept.\n";
}
