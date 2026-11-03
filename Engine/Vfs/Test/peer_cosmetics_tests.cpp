// Cosmetics fetched for other players (Engine/Vfs/peer_cosmetics.h): the store read back, which of
// them each policy deletes at launch, the launch's cleanup in a Mods folder, and which packages
// count as cosmetics and nothing else.
#include "Engine/Vfs/peer_cosmetics.h"
#include "Engine/Vfs/mod_list.h"

#include <filesystem>
#include <fstream>
#include <iostream>

using namespace dingosdk::mods;
namespace store = dingosdk::mods::peer_cosmetics;
namespace fs = std::filesystem;

namespace {
int failures = 0;
void check(bool condition, const char *what) {
    if (condition) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}
constexpr std::int64_t day = 24 * 60 * 60, now = 1'800'000'000;
void write_file(const fs::path &path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}
std::vector<std::string> folders(const std::vector<store::Removal> &removals) {
    std::vector<std::string> out;
    for (const auto &removal : removals) out.push_back(removal.folder);
    return out;
}
} // namespace

int main(int argc, char **argv) {
    {
        store::Store s;
        s.policy = {store::Cleanup::size, 30, 4096, true, 350};
        s.mods["Nolan-Pack"] = {"1.2.3", "Ann", now - 3 * day, now - day, false};
        s.mods["Muska-Decks"] = {"1.0.0", "", now, now, true};
        check(store::from_json(store::to_json(s)) == s, "the store reads back as written");
        check(store::from_json("") == store::Store{}, "no file: an empty store");
        const auto loose = store::from_json(R"({"policy": {"cleanup": "sometimes", "days": 9999, "max_mb": 5},
            "mods": {"../up": {"version": "1"}, "Ok-Mod": {"version": 3, "fetched": 50, "seen": 10}}})");
        check(loose.policy.cleanup == store::Cleanup::unused && loose.policy.days == 365 && loose.policy.max_mb == 100,
              "an unknown cleanup is the default; days and size kept in range");
        check(!loose.policy.automatic && !store::from_json(R"({"policy": {"automatic": 1}})").policy.automatic,
              "fetching on its own is off unless the file says true");
        check(loose.policy.automatic_mb == 200 &&
                  store::from_json(R"({"policy": {"automatic_mb": 5}})").policy.automatic_mb == 10 &&
                  store::from_json(R"({"policy": {"automatic_mb": 99999}})").policy.automatic_mb == 1024,
              "the size fetched on its own is 200 MB unless set, and kept in range");
        check(loose.mods.size() == 1 && loose.mods.contains("Ok-Mod"), "a folder name that is not a mod's is dropped");
        check(loose.mods.at("Ok-Mod").version.empty() && loose.mods.at("Ok-Mod").seen == 50, "seen is never before fetched");
    }
    {
        store::Store s;
        s.mods["Old"] = {"1", "", now - 40 * day, now - 20 * day, false};
        s.mods["Recent"] = {"1", "", now - 40 * day, now - 2 * day, false};
        s.mods["Marked"] = {"1", "", now, now, true};
        const std::map<std::string, std::uint64_t, std::less<>> sizes{
            {"Old", 300ull << 20}, {"Recent", 300ull << 20}, {"Marked", 900ull << 20}};
        s.policy = {store::Cleanup::unused, 14, 2048};
        check(folders(store::expired(s, now, sizes)) == std::vector<std::string>{"Old", "Marked"} ||
                  folders(store::expired(s, now, sizes)) == std::vector<std::string>{"Marked", "Old"},
              "unused: the one not seen for 14 days, and the one marked");
        s.policy.days = 30;
        check(folders(store::expired(s, now, sizes)) == std::vector<std::string>{"Marked"}, "a longer wait keeps it");
        s.policy = {store::Cleanup::size, 14, 400};
        check(folders(store::expired(s, now, sizes)) == std::vector<std::string>{"Old", "Marked"},
              "size: the least recently seen goes until the rest fit (a marked one is not counted)");
        s.policy.max_mb = 700;
        check(folders(store::expired(s, now, sizes)) == std::vector<std::string>{"Marked"}, "everything fits");
        s.policy.cleanup = store::Cleanup::every_launch;
        check(store::expired(s, now, sizes).size() == 3, "every launch: all of them");
        s.policy.cleanup = store::Cleanup::never;
        check(folders(store::expired(s, now, sizes)) == std::vector<std::string>{"Marked"}, "never: only one marked");
    }
    const fs::path base = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "reskate-peer-cosmetics-tests";
    std::error_code error;
    fs::remove_all(base, error);
    {
        const auto mod = base / "cosmetics";
        write_file(mod / "layout.toc", "x");
        write_file(mod / "Win32" / "items.toc", "x");
        write_file(mod / "manifest.json", "{}");
        write_file(mod / "icon.png", "x");
        check(store::only_cosmetics(mod), "an item pack is cosmetics only");
        write_file(mod / "Win32" / "globals.toc", "x");
        check(store::only_cosmetics(mod), "and with globals, where its items register");
        const auto map = base / "map";
        write_file(map / "layout.toc", "x");
        write_file(map / "Win32" / "levels" / "x" / "x.toc", "x");
        write_file(map / "Win32" / "items.toc", "x");
        check(!store::only_cosmetics(map), "a level bundle is more than cosmetics");
        const auto plugin = base / "plugin";
        write_file(plugin / "layout.toc", "x");
        write_file(plugin / "Win32" / "items.toc", "x");
        write_file(plugin / "reskate-plugin.json", "{}");
        check(!store::only_cosmetics(plugin), "a plugin is more than cosmetics");
        const auto script = base / "script";
        write_file(script / "Win32" / "items.toc", "x");
        write_file(script / "extra" / "run.lua", "x");
        check(!store::only_cosmetics(script), "a script is more than cosmetics");
        const auto empty = base / "nothing";
        write_file(empty / "manifest.json", "{}");
        check(!store::only_cosmetics(empty), "no items at all is not a cosmetics package");
    }
    {
        // A launch's cleanup: the expired mod's folder and its mods.json row go, the rest stays.
        const auto data = base / "game";
        const auto mods = data / "Mods";
        for (const char *name : {"Mine", "Old-Pack", "New-Pack"}) {
            write_file(mods / name / "layout.toc", "x");
            write_file(mods / name / "Win32" / "items.toc", "x");
        }
        save_mod_order(mods, {{{.name = "Mine"}, true}, {{.name = "Old-Pack"}, true}, {{.name = "New-Pack"}, true}});
        store::Store s;
        s.mods["Old-Pack"] = {"1", "", now - 30 * day, now - 30 * day, false};
        s.mods["New-Pack"] = {"1", "", now, now, false};
        s.mods["Gone-Pack"] = {"1", "", now, now, false};
        check(store::write(mods, s), "the store is written");
        const auto lines = store::clean_up(mods, now);
        check(!fs::exists(mods / "Old-Pack") && fs::exists(mods / "New-Pack") && fs::exists(mods / "Mine"),
              "only the unused fetched mod is deleted");
        const auto after = store::read(mods);
        check(after.mods.size() == 1 && after.mods.contains("New-Pack"), "it and the one whose folder went are forgotten");
        const auto list = scan_mods(data);
        bool listed_old{}, listed_mine{};
        for (const auto &entry : list.entries) {
            listed_old = listed_old || entry.mod.name == "Old-Pack";
            listed_mine = listed_mine || entry.mod.name == "Mine";
        }
        check(!listed_old && listed_mine && list.missing.empty(), "its mods.json row is gone, the player's own stays");
        check(lines.size() == 2, "a log line for each change");
        check(store::clean_up(mods, now).empty(), "nothing more to do the next time");
    }
    fs::remove_all(base, error);
    if (failures) std::cerr << failures << " peer cosmetics test(s) failed\n";
    else std::cout << "peer cosmetics tests passed\n";
    return failures ? 1 : 0;
}
