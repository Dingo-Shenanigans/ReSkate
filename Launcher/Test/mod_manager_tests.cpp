// Launcher mod manager: install from .zip and folders, the Mods list and mods.json.
#include "Launcher/mod_manager.h"

#include "Engine/Vfs/mod_list.h"

#include <Windows.h>

#include <miniz.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace dingosdk;

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (condition) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}

void write(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

// A stored (uncompressed) .zip, enough to exercise the reader.
void make_zip(const fs::path& path, const std::vector<std::pair<std::string, std::string>>& files) {
    std::string out, directory;
    const auto u16 = [](std::string& s, unsigned v) { s += char(v & 0xff); s += char(v >> 8 & 0xff); };
    const auto u32 = [](std::string& s, unsigned long v) { for (int i = 0; i < 4; ++i) s += char(v >> (8 * i) & 0xff); };
    for (const auto& [name, data] : files) {
        const auto crc = mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(data.data()), data.size());
        const auto offset = out.size();
        u32(out, 0x04034b50); u16(out, 20); u16(out, 0); u16(out, 0); u16(out, 0); u16(out, 0);
        u32(out, crc); u32(out, static_cast<unsigned long>(data.size())); u32(out, static_cast<unsigned long>(data.size()));
        u16(out, static_cast<unsigned>(name.size())); u16(out, 0);
        out += name; out += data;
        u32(directory, 0x02014b50); u16(directory, 20); u16(directory, 20); u16(directory, 0); u16(directory, 0);
        u16(directory, 0); u16(directory, 0); u32(directory, crc);
        u32(directory, static_cast<unsigned long>(data.size())); u32(directory, static_cast<unsigned long>(data.size()));
        u16(directory, static_cast<unsigned>(name.size())); u16(directory, 0); u16(directory, 0); u16(directory, 0);
        u16(directory, 0); u32(directory, name.ends_with('/') ? 0x10 : 0); u32(directory, static_cast<unsigned long>(offset));
        directory += name;
    }
    const auto start = out.size();
    out += directory;
    u32(out, 0x06054b50); u16(out, 0); u16(out, 0);
    u16(out, static_cast<unsigned>(files.size())); u16(out, static_cast<unsigned>(files.size()));
    u32(out, static_cast<unsigned long>(directory.size())); u32(out, static_cast<unsigned long>(start)); u16(out, 0);
    write(path, out);
}

std::string install(const fs::path& mods, const fs::path& source, bool replace = false) {
    std::atomic<bool> cancel{};
    return launcher_mods::install(mods, source, replace, {}, cancel);
}

template<class F> std::string error_of(F&& run) {
    try { run(); } catch (const std::exception& failure) { return failure.what(); }
    return {};
}

} // namespace

int main() {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const auto root = fs::path(temp) / (L"reskate-mod-manager-tests-" + std::to_wstring(GetCurrentProcessId()));
    fs::remove_all(root);
    const auto game = root / L"game";
    const auto sources = root / L"sources";
    fs::create_directories(game);
    const auto mods = launcher_mods::mods_root(game);
    check(mods == game / L"Mods", "Without ModData the Mods folder sits beside the game");

    // A mod inside a top-level folder, with its own description.
    make_zip(sources / L"download (1).zip", {
        {"Cool Park/", ""},
        {"Cool Park/layout.toc", "toc"},
        {"Cool Park/Win32/levels/cool.sb", "bundle"},
        {"Cool Park/reskate-mod.json", R"({"schema":1,"name":"Cool Park Deluxe","author":"Zee","version":"1.2","description":"A park."})"},
        {"Cool Park/reskate-levels.json", R"({"schema":1,"levels":[{"asset":"levels/game/cool/cool"}]})"}});
    check(install(mods, sources / L"download (1).zip") == "Cool Park", "A zip's top folder names the mod");
    check(read(mods / L"Cool Park" / L"Win32" / L"levels" / L"cool.sb") == "bundle", "Nested files are extracted");
    check(!fs::exists(mods / L".reskate-install"), "Staging is cleaned up");

    // Files at the top of the zip: the archive's name becomes the folder name.
    make_zip(sources / L"Flat Mod!.zip", {{"reskate-levels.json", R"({"schema":1,"levels":[]})"}});
    check(install(mods, sources / L"Flat Mod!.zip") == "Flat Mod_", "Unsafe characters in names are replaced");

    // Mods packed deeper, as release zips often are.
    make_zip(sources / L"pack.zip", {{"readme.txt", "hi"}, {"pack/Mods/Deep_Mod/layout.toc", "toc"}});
    check(install(mods, sources / L"pack.zip") == "Deep_Mod", "The folder holding the marker is the mod");
    check(!fs::exists(mods / L"Deep_Mod" / L"readme.txt"), "Files outside the mod are left behind");

    // Same name again: refused, then replaced on request.
    bool conflict = false;
    try { install(mods, sources / L"download (1).zip"); } catch (const launcher_mods::AlreadyInstalled& existing) {
        conflict = existing.name == "Cool Park";
    }
    check(conflict, "Installing over an installed mod asks first");
    make_zip(sources / L"v2.zip", {{"Cool Park/layout.toc", "toc v2"}});
    check(install(mods, sources / L"v2.zip", true) == "Cool Park" && read(mods / L"Cool Park" / L"layout.toc") == "toc v2",
          "Replacing installs the new copy");

    // Hostile or unrelated archives.
    make_zip(sources / L"evil.zip", {{"../evil.txt", "x"}, {"layout.toc", "toc"}});
    check(error_of([&] { install(mods, sources / L"evil.zip"); }).find("unsafe path") != std::string::npos &&
              !fs::exists(game / L"evil.txt") && !fs::exists(mods / L"evil"),
          "Paths leaving the mod folder are refused");
    make_zip(sources / L"photos.zip", {{"a.jpg", "x"}});
    check(error_of([&] { install(mods, sources / L"photos.zip"); }).find("does not contain a ReSkate mod") != std::string::npos,
          "Archives without a mod are refused");
    write(sources / L"notes.txt", "x");
    check(!error_of([&] { install(mods, sources / L"notes.txt"); }).empty(), "Only .zip files and folders install");

    // A folder.
    write(sources / L"Loose" / L"My Folder Mod" / L"layout.toc", "toc");
    check(install(mods, sources / L"Loose") == "My Folder Mod", "Folders install like zips");
    check(fs::exists(sources / L"Loose" / L"My Folder Mod" / L"layout.toc"), "Installing a folder copies it");

    // The list: unlisted folders load after listed ones, by name.
    auto list = mods::scan_mods(game);
    check(list.present && list.issue.empty() && list.entries.size() == 4, "Every installed mod is listed");
    check(list.entries[0].mod.name == "Cool Park" && list.entries[0].enabled, "Unlisted mods are enabled");
    check(list.entries[0].mod.title == "Cool Park" && list.entries[0].mod.provides_layout,
          "The replaced mod shows its new contents");
    const auto deep = std::find_if(list.entries.begin(), list.entries.end(), [](const auto& e) { return e.mod.name == "Deep_Mod"; });
    check(deep != list.entries.end() && deep->mod.title == "Deep_Mod", "Titles fall back to the folder name");

    // Save an order with one mod disabled, then read it back.
    std::reverse(list.entries.begin(), list.entries.end());
    list.entries[1].enabled = false;
    mods::save_mod_order(mods, list.entries);
    const auto saved = mods::scan_mods(game);
    check(saved.issue.empty() && saved.entries.size() == 4, "The saved mods.json reads back");
    for (std::size_t i = 0; i < 4; ++i)
        check(saved.entries[i].mod.name == list.entries[i].mod.name && saved.entries[i].enabled == list.entries[i].enabled,
              "Order and enabled state round-trip");
    check(read(mods / L"mods.json").find(R"("enabled": false)") != std::string::npos, "mods.json is plain readable JSON");

    // Metadata from reskate-mod.json.
    write(mods / L"Deep_Mod" / L"reskate-mod.json",
          "\xef\xbb\xbf{\"name\":\"Deep\",\"author\":\"Someone\",\"version\":\"3\",\"description\":\"Line\\u0001\"}");
    for (const auto& entry : mods::scan_mods(game).entries)
        if (entry.mod.name == "Deep_Mod")
            check(entry.mod.title == "Deep" && entry.mod.author == "Someone" && entry.mod.version == "3" &&
                  entry.mod.description == "Line?", "reskate-mod.json fills the details, control characters removed");

    // manifest.json is preferred, with version_number.
    write(mods / L"Deep_Mod" / L"manifest.json",
          R"({"name":"Deep Manifest","author":"Park Maker","version_number":"2.1.0","description":"From the manifest"})");
    for (const auto& entry : mods::scan_mods(game).entries)
        if (entry.mod.name == "Deep_Mod")
            check(entry.mod.title == "Deep Manifest" && entry.mod.author == "Park Maker" && entry.mod.version == "2.1.0" &&
                  entry.mod.description == "From the manifest", "manifest.json fills the details ahead of reskate-mod.json");

    // Park mods: parks/<map>.park.json is listed per map.
    write(mods / L"Deep_Mod" / L"parks" / L"bam.park.json", "{}");
    write(mods / L"Deep_Mod" / L"parks" / L"grom.park.json", "{}");
    for (const auto& entry : mods::scan_mods(game).entries)
        if (entry.mod.name == "Deep_Mod")
            check(entry.mod.park_maps == std::vector<std::string>{"bam", "grom"}, "Park maps are listed");
    make_zip(sources / L"parkmod.zip", {{"My Park/manifest.json", R"({"name":"My Park"})"},
                                        {"My Park/parks/bam.park.json", "{}"}});
    check(install(mods, sources / L"parkmod.zip") == "My Park", "A park mod (manifest.json only) installs");

    // Left-out mods: reported while their files are unchanged, forgotten once they change.
    {
        std::map<std::string, mods::Exclusion, std::less<>> exclusions;
        exclusions["Deep_Mod"] = {mods::mod_fingerprint(mods / L"Deep_Mod"), "sdk", {"levels/game/x: damaged"}};
        mods::write_exclusions(mods, exclusions);
        const auto read_back = mods::read_exclusions(mods);
        check(read_back.size() == 1 && read_back.at("Deep_Mod").sdk == "sdk" &&
                  read_back.at("Deep_Mod").problems == std::vector<std::string>{"levels/game/x: damaged"},
              "Exclusions round-trip");
        const auto listed = mods::scan_mods(game);
        check(listed.excluded.contains("Deep_Mod") && listed.excluded.size() == 1, "An unchanged left-out mod is reported");
        write(mods / L"Deep_Mod" / L"new file.txt", "reinstalled");
        check(mods::scan_mods(game).excluded.empty(), "A changed mod is no longer reported as left out");
        mods::write_exclusions(mods, {});
        check(!fs::exists(mods / mods::exclusions_file), "An empty exclusion list removes the file");
    }

    // A broken mods.json still lists the folders, so the manager can repair it.
    write(mods / L"mods.json", R"({"schema":1,"mods":[{"name":"Cool Park"}]})");
    const auto broken = mods::scan_mods(game);
    check(!broken.issue.empty() && broken.entries.size() == 5, "A malformed mods.json is reported, folders still listed");
    mods::save_mod_order(mods, broken.entries);
    check(mods::scan_mods(game).issue.empty(), "Saving repairs a malformed mods.json");

    // Thunderstore packages: manifest.json, icon.png and README.md at the top of the zip.
    {
        const auto store_game = root / L"store";
        fs::create_directories(store_game);
        const auto store = launcher_mods::mods_root(store_game);
        const std::string manifest =
            R"({"name":"Desert_Springs","version_number":"1.2.3","website_url":"","description":"Sand.","dependencies":[]})";
        const auto installed = [&](std::string_view name) -> std::optional<mods::Mod> {
            for (const auto& entry : mods::scan_mods(store_game).entries)
                if (entry.mod.name == name) return entry.mod;
            return std::nullopt;
        };

        // Downloaded from the site: files at the top, the archive named Namespace-Name-Version.
        make_zip(sources / L"Zee-Desert_Springs-1.2.3.zip", {{"manifest.json", manifest}, {"icon.png", "png"},
            {"README.md", "# Desert"}, {"layout.toc", "toc"}, {"Win32/levels/desert.sb", "bundle"}});
        check(install(store, sources / L"Zee-Desert_Springs-1.2.3.zip") == "Zee-Desert_Springs",
              "A downloaded package installs without the version in its folder name");
        check(read(store / L"Zee-Desert_Springs" / L"icon.png") == "png" &&
                  read(store / L"Zee-Desert_Springs" / L"Win32" / L"levels" / L"desert.sb") == "bundle",
              "Package files install with the mod");
        make_zip(sources / L"Map-2.zip", {{"layout.toc", "toc"}});
        check(install(store, sources / L"Map-2.zip") == "Map-2", "Names that merely end in a number keep it");

        // The usual packaging mistake: the mod folder zipped inside the package.
        make_zip(sources / L"nested.zip", {{"manifest.json", manifest}, {"icon.png", "png"}, {"README.md", "readme"},
            {"CHANGELOG.md", "log"}, {"DesertSprings/layout.toc", "toc"},
            {"DesertSprings/manifest.json", R"({"name":"Old"})"}, {"DesertSprings/Win32/levels/desert.sb", "bundle"}});
        check(install(store, sources / L"nested.zip") == "DesertSprings",
              "Content in a folder is found past the package's own manifest.json");
        check(read(store / L"DesertSprings" / L"manifest.json") == manifest &&
                  read(store / L"DesertSprings" / L"icon.png") == "png" &&
                  read(store / L"DesertSprings" / L"CHANGELOG.md") == "log" &&
                  read(store / L"DesertSprings" / L"Win32" / L"levels" / L"desert.sb") == "bundle",
              "Package files at the top follow the mod in and replace its own copies");
        check(!fs::exists(store / L"DesertSprings" / L"DesertSprings"), "The wrapper folder is not kept");

        // A package only replaces the nested metadata files it actually supplies.
        const std::vector<std::pair<std::string, std::string>> partial_package{
            {"manifest.json", manifest}, {"rEaDmE.md", "package readme"},
            {"Partial/layout.toc", "toc"}, {"Partial/manifest.json", R"({"name":"Old"})"},
            {"Partial/README.md", "old readme"}, {"Partial/icon.png", "nested icon"},
            {"Partial/CHANGELOG.md", "nested changelog"}};
        make_zip(sources / L"partial.zip", partial_package);
        const auto folder_package = sources / L"partial-folder";
        for (const auto& [name, data] : partial_package) write(folder_package / name, data);
        for (const auto& source : {sources / L"partial.zip", folder_package}) {
            check(install(store, source, true) == "Partial", "Partial packages install from zips and folders");
            check(read(store / L"Partial" / L"manifest.json") == manifest &&
                      read(store / L"Partial" / L"README.md") == "package readme",
                  "Present package files replace nested copies regardless of filename case");
            check(read(store / L"Partial" / L"icon.png") == "nested icon" &&
                      read(store / L"Partial" / L"CHANGELOG.md") == "nested changelog",
                  "Nested metadata survives when the package has no replacement");
        }

        // What the launcher's Thunderstore installs ask for.
        launcher_mods::InstallOptions options;
        options.folder = "Zee-Desert_Springs";
        options.author = "Zee";
        options.require_content = true;
        std::atomic<bool> cancel{};
        check(launcher_mods::install(store, sources / L"nested.zip", true, {}, cancel, options) == "Zee-Desert_Springs",
              "The package's own folder name wins");
        const auto desert = installed("Zee-Desert_Springs");
        check(desert && desert->author == "Zee" && desert->version == "1.2.3" && desert->title == "Desert_Springs" &&
                  desert->provides_layout,
              "The team becomes the author and the manifest gives the version");

        make_zip(sources / L"authored.zip", {{"manifest.json", R"({"name":"Park","author":"Someone","version_number":"1.0.0"})"},
            {"icon.png", "png"}, {"README.md", "readme"}, {"parks/bam.park.json", "{}"}});
        options.folder = "Team-Park";
        check(launcher_mods::install(store, sources / L"authored.zip", true, {}, cancel, options) == "Team-Park",
              "A park package installs");
        const auto park = installed("Team-Park");
        check(park && park->author == "Someone" && park->park_maps == std::vector<std::string>{"bam"},
              "An author the manifest already names is kept");

        make_zip(sources / L"empty.zip", {{"manifest.json", manifest}, {"icon.png", "png"}, {"README.md", "readme"}});
        options.folder = "Team-Empty";
        check(error_of([&] { launcher_mods::install(store, sources / L"empty.zip", true, {}, cancel, options); })
                      .find("nothing for ReSkate to load") != std::string::npos &&
                  !fs::exists(store / L"Team-Empty"),
              "Thunderstore installs refuse a package with nothing to load");
        check(install(store, sources / L"empty.zip") == "empty", "A plain install still takes a description-only mod");
    }

    std::error_code ignored;
    fs::remove_all(root, ignored);
    if (failures) {
        std::cerr << failures << " mod manager check(s) failed\n";
        return 1;
    }
    std::cout << "Mod manager checks passed.\n";
    return 0;
}
