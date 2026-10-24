#include "Engine/Vfs/mod_catalog.h"
#include "Engine/Game/Build/supported_build.h"
#include <Windows.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

// Variants (Engine/Vfs/mod_list.h): a mod that brings several takes on the same game data, one of
// which loads, and what it displaces set aside.
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
bool listed(const std::vector<mods::Mod> &mods, std::string_view name) {
    return std::ranges::any_of(mods, [&](const mods::Mod &mod) { return mod.name == name; });
}
bool aside(const mods::Catalog &catalog, std::string_view name) {
    return std::ranges::find(catalog.set_aside, name) != catalog.set_aside.end();
}
} // namespace

int main() {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const auto root = fs::path(temp) / ("reskate-mod-variants-" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(root);
    // The choices go to %LOCALAPPDATA%\ReSkate\variants.json: here, the test's own folder.
    SetEnvironmentVariableW(L"LOCALAPPDATA", (root / "appdata").wstring().c_str());
    const auto mods_root = root / "Mods";
    const auto stamp = "ReSkate Studio native Patch v1\nskate_sha256=" + std::string(supported_build::game_sha256) + "\n";
    // A pack of two variants, "wide" listed first; its own layout brings the bundle they replace.
    write(mods_root / "Pack" / "layout.toc", "superbundles deck_bundle");
    write(mods_root / "Pack" / ".reskate-studio-patch", stamp);
    write(mods_root / "Pack" / "reskate-plugin.json",
          R"({"api":1,"entry":"plugin/main.lua","variants":{"folder":"decks","order":["wide"],"displaces":["DECK_BUNDLE"]}})");
    write(mods_root / "Pack" / "decks" / "standard" / "layout.toc", "a");
    write(mods_root / "Pack" / "decks" / "wide" / "layout.toc", "b");
    write(mods_root / "Pack" / "decks" / "notes" / "readme.txt", "no layout: no variant");
    // Another mod with a deck model of its own, and one that has nothing to do with decks.
    write(mods_root / "OtherDeck" / "layout.toc", "superbundles Deck_Bundle");
    write(mods_root / "OtherDeck" / ".reskate-studio-patch", stamp);
    write(mods_root / "Plain" / "layout.toc", "x");
    write(mods_root / "Plain" / ".reskate-studio-patch", stamp);
    // A malformed declaration is a note, and the mod loads as any other.
    write(mods_root / "Odd" / "layout.toc", "x");
    write(mods_root / "Odd" / ".reskate-studio-patch", stamp);
    write(mods_root / "Odd" / "reskate-plugin.json", R"({"api":1,"variants":{"folder":"../up"}})");

    const auto list = mods::scan_mods(root);
    const auto pack = std::ranges::find_if(list.entries, [](const mods::ModEntry &e) { return e.mod.name == "Pack"; });
    check(pack != list.entries.end() && pack->mod.variants && pack->mod.variants->displaces.front() == "deck_bundle",
          "The pack's variants were not read");
    const auto installed = mods::installed_variants(pack->mod);
    check(installed == std::vector<std::string>{"wide", "standard"}, "The variants are not in the order the pack gives");
    check(mods::variant_folder(pack->mod, "standard") == mods_root / "Pack" / "decks" / "standard", "A variant's folder is not its own");

    auto catalog = mods::load_catalog(root, {}, false);
    check(catalog.issue.empty(), "The catalog was rejected: " + catalog.issue);
    check(!catalog.mods.empty() && catalog.mods.front().name == "Pack#wide", "With nothing chosen, the first of the order does not load first");
    check(catalog.variants_in_play.contains("Pack") && catalog.variants_in_play.at("Pack") == "wide", "The variant in play is not the one loaded");
    check(aside(catalog, "Pack#standard") && aside(catalog, "Pack") && aside(catalog, "OtherDeck"),
          "The other variant, the pack's own layout or another deck model was not set aside");
    check(!listed(catalog.mods, "Pack") && !listed(catalog.mods, "OtherDeck") && listed(catalog.mods, "Plain") && listed(catalog.mods, "Odd"),
          "The wrong mods load");
    check(listed(catalog.inactive, "Pack#standard"), "A set-aside variant's archives are not placed, so it cannot be switched to live");
    check(std::ranges::any_of(catalog.notes, [](const std::string &n) { return n.find("Odd") != std::string::npos && n.find("variants") != std::string::npos; }),
          "A malformed declaration made no note");

    check(mods::save_variant("Pack", "Standard") && mods::chosen_variant("pack") == "standard", "A choice was not kept");
    check(fs::is_regular_file(root / "appdata" / "ReSkate" / "variants.json"), "The choice was not saved");
    catalog = mods::load_catalog(root, {}, false);
    check(!catalog.mods.empty() && catalog.mods.front().name == "Pack#standard" && aside(catalog, "Pack#wide"),
          "The chosen variant does not load");
    // A choice that is not installed falls back to the order's first.
    check(mods::save_variant("Pack", "gone"), "A choice was not kept");
    catalog = mods::load_catalog(root, {}, false);
    check(!catalog.mods.empty() && catalog.mods.front().name == "Pack#wide", "A missing choice does not fall back to the order");
    // Disabled, the pack loads none of its variants and sets nothing aside.
    write(mods_root / "mods.json", R"({"schema":1,"mods":[{"name":"Pack","enabled":false}]})");
    catalog = mods::load_catalog(root, {}, false);
    check(catalog.set_aside.empty() && catalog.variants_in_play.empty() && listed(catalog.mods, "OtherDeck"),
          "A disabled pack still decided what loads");
    check(mods::variant_of("Pack#wide") == "wide" && mods::variant_owner("Pack#wide") == "Pack" && mods::variant_owner("Pack").empty(),
          "A variant's entry does not name its variant and mod");
    fs::remove_all(root);
    std::cout << "Mod variants: order, choice, fallback, set aside, disabled and malformed passed.\n";
}
