// Launcher mod manager: install from .zip and folders, the Mods list and mods.json.
#include "Launcher/mod_manager.h"

#include "Engine/Vfs/builtin_pack_format.h"
#include "Engine/Vfs/builtin_packs.h"
#include <zstd.h>
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
    // A file of the installed copy held open: the replacement is refused and the copy stays whole.
    {
        const auto held = CreateFileW((mods / L"Cool Park" / L"layout.toc").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        bool refused = false;
        try { install(mods, sources / L"v2.zip", true); } catch (const std::exception&) { refused = true; }
        CloseHandle(held);
        check(refused && read(mods / L"Cool Park" / L"layout.toc") == "toc" && !fs::exists(mods / L".reskate-install"),
              "A copy in use is not replaced, and stays as it was");
    }
    check(install(mods, sources / L"v2.zip", true) == "Cool Park" && read(mods / L"Cool Park" / L"layout.toc") == "toc v2",
          "Replacing installs the new copy");
    check(!fs::exists(mods / L".reskate-install"), "The replaced copy is deleted, not left beside the mods");
    {
        std::atomic<bool> cancel{};
        unsigned callbacks{};
        make_zip(sources / L"cancelled.zip", {{"Cool Park/layout.toc", "must not replace"}});
        const auto issue = error_of([&] {
            launcher_mods::install(mods, sources / L"cancelled.zip", true,
                [&](float) { if (++callbacks == 2) cancel.store(true); }, cancel);
        });
        check(issue == "Install cancelled." && callbacks == 2,
              "Cancellation at the final safe installation boundary was ignored");
        check(read(mods / L"Cool Park" / L"layout.toc") == "toc v2" &&
                  !fs::exists(mods / L".reskate-install"),
              "Cancelled replacement damaged the existing mod or left staging files");
        write(sources / L"cancelled folder" / L"Cool Park" / L"layout.toc", "must not replace");
        cancel.store(false);
        callbacks = 0;
        check(error_of([&] {
            launcher_mods::install(mods, sources / L"cancelled folder", true,
                [&](float) { if (++callbacks == 2) cancel.store(true); }, cancel);
        }) == "Install cancelled." && read(mods / L"Cool Park" / L"layout.toc") == "toc v2",
              "Cancelled folder replacement did not preserve the installed mod");
    }

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

    // Links in a mod folder (issue #265): one inside it refuses the install and nothing outside is
    // written; links on the way to the folder or to the Mods folder are fine. Junctions need no
    // rights to make; a file symbolic link needs Developer Mode, so that case is skipped without it.
    {
        const auto place = root / L"links";
        const auto junction = [](const fs::path& link, const fs::path& target) {
            fs::create_directories(target);
            return _wsystem((L"mklink /J \"" + link.wstring() + L"\" \"" + target.wstring() + L"\" >nul").c_str()) == 0;
        };
        const auto real_game = place / L"real game";
        fs::create_directories(real_game);
        const bool made = junction(place / L"linked game", real_game);
        const auto linked_mods = launcher_mods::mods_root(place / L"linked game");
        write(place / L"outside" / L"sentinel.txt", "untouched");

        write(place / L"Junction Mod" / L"layout.toc", "toc");
        const bool inner = junction(place / L"Junction Mod" / L"stuff", place / L"outside");
        check(made && inner, "links: junctions could be made for the checks");
        const auto refused = error_of([&] { install(linked_mods, place / L"Junction Mod"); });
        check(refused.find("holds a link (stuff)") != std::string::npos && !fs::exists(linked_mods / L"Junction Mod") &&
                  read(place / L"outside" / L"sentinel.txt") == "untouched",
              ("A junction inside a mod folder refuses the install: " + refused).c_str());
        check(!fs::exists(linked_mods / L".reskate-install"), "A refused folder leaves no staging behind");

        write(place / L"Plain Mod" / L"layout.toc", "toc");
        check(install(linked_mods, place / L"Plain Mod") == "Plain Mod" && read(real_game / L"Mods" / L"Plain Mod" / L"layout.toc") == "toc",
              "A Mods folder reached through a junction still installs");
        junction(place / L"picked", place / L"Picked Mod");
        write(place / L"Picked Mod" / L"layout.toc", "toc");
        check(install(linked_mods, place / L"picked") == "picked", "A mod folder picked through a junction still installs");

        write(place / L"File Link Mod" / L"layout.toc", "toc");
        const auto link = place / L"File Link Mod" / L"linked.txt";
        if (CreateSymbolicLinkW(link.c_str(), (place / L"outside" / L"sentinel.txt").c_str(),
                                SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
            const auto file_refused = error_of([&] { install(linked_mods, place / L"File Link Mod"); });
            check(file_refused.find("holds a link (linked.txt)") != std::string::npos &&
                      read(place / L"outside" / L"sentinel.txt") == "untouched" && !fs::exists(linked_mods / L"File Link Mod"),
                  ("A file link inside a mod folder refuses the install: " + file_refused).c_str());
        } else {
            std::cout << "SKIP: file symbolic link (Windows error " << GetLastError() << "; needs Developer Mode)\n";
        }
        // Junctions first, so nothing below follows one.
        for (const auto* name : {L"linked game", L"picked", L"Junction Mod\\stuff"})
            RemoveDirectoryW((place / name).c_str());
    }

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

    // What a mod holds, for the Type column: a plugin is one only when its manifest names code.
    write(mods / L"Deep_Mod" / L"reskate-plugin.json", R"({"variants":{"folder":"decks"}})");
    for (const auto& entry : mods::scan_mods(game).entries)
        if (entry.mod.name == "Deep_Mod")
            check(!entry.mod.provides_plugin && mods::mod_contents(entry.mod) == std::vector<std::string>{"Parks", "Game data"},
                  "A reskate-plugin.json without an entry is no plugin");
    write(mods / L"Deep_Mod" / L"reskate-plugin.json", R"({"entry":"plugin/main.lua","permissions":[]})");
    write(mods / L"Deep_Mod" / L"reskate-music.json", "{}");
    write(mods / L"Deep_Mod" / L"reskate-objects.json", "{}");
    for (const auto& entry : mods::scan_mods(game).entries)
        if (entry.mod.name == "Deep_Mod")
            check(entry.mod.provides_plugin &&
                      mods::mod_contents(entry.mod) == std::vector<std::string>{"Parks", "Plugin", "Game data", "Objects", "Music"},
                  "Plugins, objects and music are listed with what else the mod holds");
    std::filesystem::remove(mods / L"Deep_Mod" / L"reskate-plugin.json");
    std::filesystem::remove(mods / L"Deep_Mod" / L"reskate-music.json");
    std::filesystem::remove(mods / L"Deep_Mod" / L"reskate-objects.json");
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

    // A big collection, with the longest names, can be switched off and reordered, and the
    // list reads back. (There was a limit of 64 mods once: past it nothing could be saved, so
    // no mod could be disabled.)
    {
        constexpr std::size_t collection = 300;
        const auto big_game = root / L"big";
        const auto big = launcher_mods::mods_root(big_game);
        for (std::size_t i = 0; i < collection; ++i) {
            auto name = std::to_string(i);
            name = std::string(4 - name.size(), '0') + name + "-";
            name.resize(mods::maximum_mod_name, 'x');
            write(big / name / L"layout.toc", "toc");
        }
        auto many = mods::scan_mods(big_game);
        check(many.issue.empty() && many.entries.size() == collection, "A big Mods folder is listed");
        for (std::size_t i = 0; i < many.entries.size(); i += 3) many.entries[i].enabled = false;
        std::swap(many.entries.front(), many.entries.back());
        std::string refusal;
        try { mods::save_mod_order(big, many.entries); } catch (const std::exception& failure) { refusal = failure.what(); }
        if (!refusal.empty()) std::cerr << "save refused: " << refusal << '\n';
        check(refusal.empty(), "A big mod list can be saved");
        const auto again = mods::scan_mods(big_game);
        check(again.issue.empty() && again.entries.size() == many.entries.size(), "A big mods.json reads back");
        bool same = again.entries.size() == many.entries.size();
        for (std::size_t i = 0; same && i < again.entries.size(); ++i)
            same = again.entries[i].mod.name == many.entries[i].mod.name && again.entries[i].enabled == many.entries[i].enabled;
        check(same, "Every mod keeps its place and whether it loads");
    }

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
        make_zip(sources / L"empty-list.zip", {{"manifest.json", R"({"name":"Nothing","dependencies":[]})"}});
        options.folder = "Team-Empty_List";
        check(error_of([&] { launcher_mods::install(store, sources / L"empty-list.zip", true, {}, cancel, options); })
                      .find("plugins and mod packs") != std::string::npos &&
                  !fs::exists(store / L"Team-Empty_List"),
              "An empty dependencies list is not a mod pack, and the refusal names plugins and packs");

        // A plugin-only package: reskate-plugin.json is something to load, at the top or one folder down.
        make_zip(sources / L"plugin.zip", {{"manifest.json", manifest}, {"icon.png", "png"}, {"README.md", "readme"},
            {"reskate-plugin.json", R"({"api":1})"}, {"main.luau", "-- plugin"}});
        options.folder = "Team-Plugin";
        check(launcher_mods::install(store, sources / L"plugin.zip", true, {}, cancel, options) == "Team-Plugin" &&
                  read(store / L"Team-Plugin" / L"main.luau") == "-- plugin" &&
                  read(store / L"Team-Plugin" / L"icon.png") == "png",
              "Thunderstore installs take a plugin-only package");
        make_zip(sources / L"nested-plugin.zip", {{"manifest.json", manifest}, {"icon.png", "png"}, {"README.md", "readme"},
            {"Plugin/reskate-plugin.json", R"({"api":1})"}, {"Plugin/main.luau", "-- nested"}});
        options.folder = "Team-Nested_Plugin";
        check(launcher_mods::install(store, sources / L"nested-plugin.zip", true, {}, cancel, options) == "Team-Nested_Plugin" &&
                  read(store / L"Team-Nested_Plugin" / L"reskate-plugin.json") == R"({"api":1})" &&
                  read(store / L"Team-Nested_Plugin" / L"main.luau") == "-- nested" &&
                  read(store / L"Team-Nested_Plugin" / L"icon.png") == "png" &&
                  read(store / L"Team-Nested_Plugin" / L"README.md") == "readme" &&
                  !fs::exists(store / L"Team-Nested_Plugin" / L"Plugin"),
              "A plugin one folder down is the mod, and the package files follow it in");
        const auto plugin_manifest = read(store / L"Team-Nested_Plugin" / L"manifest.json");
        check(plugin_manifest.find("Desert_Springs") != std::string::npos && plugin_manifest.find(R"("Zee")") != std::string::npos,
              "The carried manifest.json gets the team as author");

        // A mod pack: nothing of its own to load, only dependencies the launcher installs first.
        make_zip(sources / L"modpack.zip", {
            {"manifest.json", R"({"name":"Pack","version_number":"1.0.0","dependencies":["Zee-Desert_Springs-1.2.3"]})"},
            {"icon.png", "png"}, {"README.md", "readme"}});
        options.folder = "Team-Pack";
        check(launcher_mods::install(store, sources / L"modpack.zip", true, {}, cancel, options) == "Team-Pack" &&
                  read(store / L"Team-Pack" / L"icon.png") == "png",
              "Thunderstore installs take a mod pack that only lists dependencies");
        const auto pack = installed("Team-Pack");
        check(pack && pack->author == "Zee" && pack->version == "1.0.0", "A mod pack is listed with its team as author");
    }

    // A built-in pack in ReSkate's own format unpacks to exactly its files (a name with spaces
    // included), and one with a path that leaves its folder unpacks nothing.
    {
        const auto pack = [](const std::vector<std::pair<std::string, std::string>>& files) {
            std::string plain(mods::builtin_pack::magic.begin(), mods::builtin_pack::magic.end());
            const auto put = [&](auto value) { plain.append(reinterpret_cast<const char*>(&value), sizeof(value)); };
            put(static_cast<std::uint32_t>(files.size()));
            for (const auto& [name, bytes] : files) {
                put(static_cast<std::uint16_t>(name.size()));
                plain += name;
                put(static_cast<std::uint64_t>(bytes.size()));
            }
            for (const auto& file : files) plain += file.second;
            std::vector<unsigned char> packed(ZSTD_compressBound(plain.size()));
            packed.resize(ZSTD_compress(packed.data(), packed.size(), plain.data(), plain.size(), 19));
            return packed;
        };
        const auto target = root / L"builtin" / L"good";
        std::string issue;
        const auto good = pack({{"layout.toc", "toc"}, {"sounds/body/Body Impact 1.wav", "RIFF"}, {"decks/wide/layout.toc", "deck"}});
        check(mods::unpack_builtin_pack(good, target, issue) && issue.empty() && read(target / L"layout.toc") == "toc" &&
                  read(target / L"sounds" / L"body" / L"Body Impact 1.wav") == "RIFF" && read(target / L"decks" / L"wide" / L"layout.toc") == "deck",
              "A built-in pack unpacks to its files");
        const auto bad_target = root / L"builtin" / L"bad";
        check(!mods::unpack_builtin_pack(pack({{"../escape.txt", "x"}}), bad_target, issue) && !issue.empty() &&
                  !fs::exists(root / L"builtin" / L"escape.txt"),
              "A built-in pack may not write outside its folder");
        auto cut = good;
        cut.resize(cut.size() / 2);
        check(!mods::unpack_builtin_pack(cut, root / L"builtin" / L"cut", issue), "A cut-short built-in pack is refused");
        // The packs this build made (RESKATE_BUILTIN_PACKS: their folder), each unpacked whole.
        std::wstring built(32768, L'\0');
        built.resize(GetEnvironmentVariableW(L"RESKATE_BUILTIN_PACKS", built.data(), static_cast<DWORD>(built.size())));
        if (!built.empty()) {
            for (const auto& file : fs::directory_iterator(fs::path(built))) {
                if (file.path().extension() != L".rskpack") continue;
                std::ifstream in(file.path(), std::ios::binary);
                const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
                const auto out = root / L"builtin" / file.path().stem();
                std::size_t count{};
                std::uintmax_t size{};
                const bool ok = mods::unpack_builtin_pack(bytes, out, issue);
                for (const auto& entry : fs::recursive_directory_iterator(out))
                    if (entry.is_regular_file()) { ++count; size += entry.file_size(); }
                std::cout << "  " << file.path().filename().string() << ": " << (ok ? "unpacked " : "FAILED ") << count << " files, "
                          << size << " bytes" << (ok ? "" : " (" + issue + ")") << '\n';
                check(ok, "A built-in pack this build made unpacks");
            }
        }
    }

    // A retired mod (the deprecated first Board Break) is left out by ReSkate; every other mod is the player's.
    {
        const auto store = root / L"retired" / L"Mods";
        fs::create_directories(store);
        check(mods::kept_by_reskate(store, "fourth_studio-board_break") && mods::kept_by_reskate(store, "Fourth_Studio-Board_Break") &&
                  !mods::kept_by_reskate(store, "Fourth_Studio-Road_Rash") && !mods::kept_by_reskate(store, "Someone-Other_Mod") &&
                  !mods::kept_by_reskate(store, ""),
              "Only a retired mod is left out");
    }

    {
        // What the mods give the game's data, for emptying the game's shader cache when it changes: the mods
        // with a layout, in load order, with their files, and the variants in play.
        const auto park = root / L"signature" / L"Park", skin = root / L"signature" / L"Skin";
        write(park / L"layout.toc", "toc");
        write(skin / L"layout.toc", "toc");
        const std::vector<launcher_mods::DataMod> both{{"Park", park}, {"Skin", skin}}, swapped{{"Skin", skin}, {"Park", park}};
        const auto before = launcher_mods::game_data_signature(both, {});
        check(before == launcher_mods::game_data_signature(both, {}), "The same mods give the same signature");
        check(before != launcher_mods::game_data_signature(swapped, {}), "Another load order is another signature");
        check(before != launcher_mods::game_data_signature({{"Park", park}}, {}), "A mod taken out is another");
        check(before != launcher_mods::game_data_signature(both, {{"Skin", "red"}}), "Another variant is another");
        write(skin / L"Win32" / L"skin.sb", "bundle");
        check(before != launcher_mods::game_data_signature(both, {}), "A mod's files changing is another");
        // Emptying the cache: everything in it goes, the folder stays.
        const auto cache = root / L"cache";
        write(cache / L"6803938.PcDx12", "shaders");
        write(cache / L"nested" / L"cache.manifest", "manifest");
        check(launcher_mods::empty_folder(cache).empty() && fs::is_directory(cache) && fs::is_empty(cache), "The shader cache is emptied, its folder kept");
        check(launcher_mods::empty_folder(root / L"no such folder").empty(), "No cache folder is nothing to do");
        check(launcher_mods::game_shader_cache().filename() == L"cache" &&
                  launcher_mods::game_shader_cache().parent_path().filename() == L"Skate",
              "The game's shader cache is ReSkate's own, under Game/Skate");
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
