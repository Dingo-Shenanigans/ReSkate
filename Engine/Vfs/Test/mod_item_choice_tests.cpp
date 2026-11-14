// The items a player leaves out of a mod (Mod::items_off, Engine/Vfs/mod_catalog.h items_left_out).
//
// A pack of 200 decks of which a player wants a few: the items left out and what only they use
// (their thumbnails, appearance presets, textures, meshes and chunks) are not merged, as if the pack
// had been built without them. What must never happen is the other way round: an asset left out
// that something still in the game names, which is a crash or an item that shows nothing. So what
// goes is decided on what every document names (plan_left_out), anything unsure stays, and these
// tests hold the result to that: nothing that stays names anything that went.
//
// Arguments: [<Skate folder> <scratch folder> <mod folder>...]. Without them only the rules and the
// kept choices are tested. With them each mod is merged whole and with items left out, alone and
// then all together, into the scratch folder (put it on the mods' drive, so their archives link
// rather than copy), and the two patches are compared asset by asset. With RESKATE_ITEM_SHEETS set to
// a folder, each mod's thumbnails as the launcher lists them are written there as <mod>.bmp, to look at.
#include "Engine/Resource/binary_bundle.h"
#include "Engine/Resource/cas_codec.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Resource/toc.h"
#include "Engine/Vfs/mod_catalog.h"
#include "Engine/Vfs/mod_item_sources.h"
#include "Engine/Vfs/mod_items.h"
#include "Engine/Vfs/mod_merge.h"
#include "Engine/Vfs/mod_merge_internal.h"
#include "Engine/Vfs/mod_store_copies.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace fb = dingosdk::frostbite;
using namespace dingosdk;
using namespace dingosdk::mods;
using namespace dingosdk::mods::detail;

namespace {
int failures = 0;
void check(bool ok, const std::string& what) {
    if (ok) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}

fb::Guid guid(unsigned number) {
    fb::Guid out;
    out.bytes[0] = static_cast<std::byte>(number & 0xff);
    out.bytes[1] = static_cast<std::byte>(number >> 8);
    out.bytes[15] = std::byte{0x7e};
    return out;
}
std::string_view last_part(std::string_view name) {
    const auto slash = name.rfind('/');
    return slash == std::string_view::npos ? name : name.substr(slash + 1);
}

// ------------------------------------------------------------------ the rules

// A small pack as real ones are laid out: two items, each with a thumbnail and a preset, a texture
// the presets share, and a mesh only the first item has (blueprint, mesh, variation database).
enum : unsigned { itemA, cdnA, thumbA, presetA, texA, texShared, itemB, presetB, texB, blueprintA, meshA, variationA, count };
std::vector<AssetLinks> pack() {
    std::vector<AssetLinks> own(count);
    const auto make = [&](unsigned at, std::string name, std::string type, std::vector<unsigned> imports, std::vector<std::string> strings = {}) {
        own[at].file = guid(at + 1);
        own[at].name = std::move(name);
        own[at].type = std::move(type);
        for (const auto target : imports) own[at].imports.push_back(guid(target + 1));
        own[at].strings = std::move(strings);
        own[at].strings.push_back(own[at].name);   // every asset holds its own name
    };
    make(itemA, "items/cust_tops/own_a", "delmarbaseitemasset", {cdnA}, {"own_a", "gen_shirt_dmpreset", "a_ap", "g1_geometry"});
    own[itemA].item = "own_a";
    make(cdnA, "thumbnail/cdn/img_own_a", "dingocdntexture", {thumbA});
    make(thumbA, "thumbnail/tool/own_a", "textureasset", {});
    make(presetA, "characters/customization/reskate/cust_tops/a_ap", "appearanceshaderexpressionpreset", {texA, texShared});
    make(texA, "characters/customization/reskate/cust_tops/a_0", "textureasset", {});
    make(texShared, "characters/customization/reskate/cust_tops/shared_0", "textureasset", {});
    make(itemB, "items/cust_tops/own_b", "delmarbaseitemasset", {}, {"own_b", "gen_shirt_dmpreset", "b_ap"});
    own[itemB].item = "own_b";
    make(presetB, "characters/customization/reskate/cust_tops/b_ap", "appearanceshaderexpressionpreset", {texB, texShared});
    make(texB, "characters/customization/reskate/cust_tops/b_0", "textureasset", {});
    make(blueprintA, "characters/maincharacters/reskate/g1_geometry", "objectblueprint", {meshA});
    make(meshA, "characters/maincharacters/reskate/g1_mesh", "skinnedmeshasset", {});
    make(variationA, "characters/maincharacters/reskate/g1_variation_2", "meshvariationdatabase", {meshA});
    return own;
}
std::vector<const AssetLinks*> pointers(const std::vector<AssetLinks>& list) {
    std::vector<const AssetLinks*> out;
    for (const auto& links : list) out.push_back(&links);
    return out;
}
std::string which(const std::vector<bool>& gone) {
    std::string text;
    for (std::size_t at = 0; at < gone.size(); ++at)
        if (gone[at]) text += (text.empty() ? "" : ",") + std::to_string(at);
    return text;
}
// What every plan must hold to: nothing that stays names, by import or by name, anything that goes.
// An item list may name an item that goes; the merge takes its entry out.
void sound(const std::vector<AssetLinks>& own, const std::vector<AssetLinks>& others, const LeftOutPlan& plan, const std::string& label) {
    std::set<fb::Guid> goneGuids;
    std::set<std::string, std::less<>> goneNames, goneItems;
    for (std::size_t at = 0; at < own.size(); ++at) {
        if (!plan.gone[at]) continue;
        goneGuids.insert(own[at].file);
        auto& names = own[at].item.empty() ? goneNames : goneItems;
        names.insert(own[at].name);
        names.insert(std::string(last_part(own[at].name)));
    }
    const auto clean = [&](const AssetLinks& links, const std::string& who) {
        for (const auto& import : links.imports)
            check(!goneGuids.contains(import), label + ": " + who + " stays and imports an asset that went");
        // (An item's own key is what it is called: it names no other item.)
        for (const auto& text : links.strings)
            check(!goneNames.contains(text) && (text == links.item || !goneItems.contains(text)),
                  label + ": " + who + " stays and names " + text + ", which went");
    };
    for (std::size_t at = 0; at < own.size(); ++at)
        if (!plan.gone[at]) clean(own[at], own[at].name);
    for (const auto& other : others)
        if (!other.item_list) clean(other, "another document");
}
void rules() {
    const auto own = pack();
    const auto expect = [&](const std::string& label, const std::vector<AssetLinks>& mine, const std::vector<AssetLinks>& others,
                            const std::set<std::string>& off, std::vector<unsigned> gone, std::size_t items, std::vector<std::string> stayed = {}) {
        const auto refs = pointers(others);
        const auto plan = plan_left_out(mine, refs, off);
        std::vector<bool> wanted(mine.size());
        for (const auto at : gone) wanted[at] = true;
        check(plan.gone == wanted, label + ": what goes is " + which(plan.gone) + ", not " + which(wanted));
        check(plan.items == items, label + ": " + std::to_string(plan.items) + " item(s) go, not " + std::to_string(items));
        check(plan.stayed == stayed, label + ": the items that stay are not the ones expected");
        sound(mine, others, plan, label);
    };
    expect("nothing left out", own, {}, {}, {}, 0);
    expect("a key the mod has no item for", own, {}, {"own_other"}, {}, 0);
    // One item: it, its thumbnail, its preset, its own texture and its mesh; the shared texture stays.
    expect("one item", own, {}, {"own_a"}, {itemA, cdnA, thumbA, presetA, texA, blueprintA, meshA, variationA}, 1);
    expect("the other item", own, {}, {"own_b"}, {itemB, presetB, texB}, 1);
    // Both: what they shared has nobody left.
    expect("both items", own, {}, {"own_a", "own_b"},
           {itemA, cdnA, thumbA, presetA, texA, texShared, itemB, presetB, texB, blueprintA, meshA, variationA}, 2);
    {
        // The game's item list, as the mod changed it, names every item: no reason for one to stay.
        AssetLinks list;
        list.item_list = true;
        list.imports = {guid(itemA + 1), guid(itemB + 1)};
        expect("an item list names it", own, {list}, {"own_a"}, {itemA, cdnA, thumbA, presetA, texA, blueprintA, meshA, variationA}, 1);
        // Anything else that names the item keeps it, and all of it.
        AssetLinks bundle;
        bundle.imports = {guid(itemA + 1)};
        expect("another document imports the item", own, {list, bundle}, {"own_a"}, {}, 0, {"own_a"});
    }
    {
        // Another mod's item wears this mod's preset, by its name: the preset and its textures stay.
        AssetLinks wearer;
        wearer.item = "own_elsewhere";
        wearer.strings = {"a_ap"};
        expect("another mod's item names the preset", own, {wearer}, {"own_a"}, {itemA, cdnA, thumbA, blueprintA, meshA, variationA}, 1);
        // And by its whole name.
        wearer.strings = {"characters/maincharacters/reskate/g1_mesh"};
        expect("another document names the mesh whole", own, {wearer}, {"own_a"}, {itemA, cdnA, thumbA, presetA, texA, blueprintA}, 1);
    }
    {
        // The mod's plugin asks for the variation database by name (its resident list): it stays,
        // and so does the mesh it names; the blueprint only the item named still goes.
        auto listed = own;
        listed[variationA].resident = true;
        expect("a resident asset", listed, {}, {"own_a"}, {itemA, cdnA, thumbA, presetA, texA, blueprintA}, 1);
    }
    {
        // A variation database that also describes a mesh that stays is not the item's alone.
        auto two = own;
        two.push_back({});
        two.back().file = guid(100);
        two.back().name = "characters/maincharacters/reskate/g2_mesh";
        two.back().type = "skinnedmeshasset";
        two[variationA].imports.push_back(guid(100));
        expect("a variation database of two meshes", two, {}, {"own_a"}, {itemA, cdnA, thumbA, presetA, texA, blueprintA}, 1);
    }
    {
        // Only a mesh variation database goes for naming what goes: a preset no item names stays,
        // and keeps the textures it names.
        auto spare = own;
        spare.push_back({});
        spare.back().file = guid(101);
        spare.back().name = "characters/customization/reskate/cust_tops/spare_ap";
        spare.back().type = "appearanceshaderexpressionpreset";
        spare.back().imports = {guid(texA + 1)};
        expect("a preset no item names", spare, {}, {"own_a"}, {itemA, cdnA, thumbA, presetA, blueprintA, meshA, variationA}, 1);
    }
    {
        // One document in two of the mod's bundles goes from both, or stays in both.
        auto twice = own;
        twice.push_back(own[texA]);
        expect("a document in two bundles", twice, {}, {"own_a"}, {itemA, cdnA, thumbA, presetA, texA, blueprintA, meshA, variationA, count}, 1);
        AssetLinks user;
        user.imports = {guid(texA + 1)};
        expect("a document in two bundles, named", twice, {user}, {"own_a"}, {itemA, cdnA, thumbA, presetA, blueprintA, meshA, variationA}, 1);
    }
    {
        // An item whose key is the last part of another item's asset name does not name that item.
        // (A's asset is items/cust_tops/own_a; A is now called own_first and B own_a.) What is not
        // an item is still found by a key: A's thumbnail, thumbnail/tool/own_a, stays with B.
        auto alike = own;
        alike[itemA].item = "own_first";
        alike[itemA].strings = {"own_first", "a_ap", "g1_geometry", alike[itemA].name};
        alike[itemB].item = "own_a";
        alike[itemB].strings = {"own_a", "b_ap", alike[itemB].name};
        expect("a key that is another item's name", alike, {}, {"own_first"}, {itemA, cdnA, presetA, texA, blueprintA, meshA, variationA}, 1);
    }
    {
        // Two items that name each other's preset: neither preset goes while one item stays.
        auto crossed = own;
        crossed[itemB].strings.push_back("a_ap");
        expect("an item that stays names the preset", crossed, {}, {"own_a"}, {itemA, cdnA, thumbA, blueprintA, meshA, variationA}, 1);
    }
}

// ------------------------------------------------------------- the choices kept

void choices() {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const auto root = fs::path(temp) / ("reskate-item-choice-" + std::to_string(GetCurrentProcessId()));
    std::error_code error;
    fs::remove_all(root, error);
    // The choices go to %LOCALAPPDATA%\ReSkate\items-left-out.json: here, the test's own folder.
    SetEnvironmentVariableW(L"LOCALAPPDATA", (root / "appdata").wstring().c_str());
    check(items_left_out().empty(), "with no file, nothing is left out");
    check(save_items_left_out("Some-Pack", {"own_a", "Own_B"}), "a choice is kept");
    check(items_left_out("some-pack") == std::set<std::string>{"own_a", "own_b"}, "and read again by the folder's name in any case, the keys lower-case");
    check(save_items_left_out("Other", {"own_c"}) && items_left_out().size() == 2, "a second mod's choice is kept beside it");
    check(save_items_left_out("Some-Pack", {"own_b"}) && items_left_out("Some-Pack") == std::set<std::string>{"own_b"},
          "a choice replaces the mod's earlier one");
    check(save_items_left_out("Some-Pack", {"not a key", ""}) && items_left_out("Some-Pack").empty() && items_left_out().size() == 1,
          "nothing left out removes the mod's entry, and a key that is none is not kept");
    {
        std::ofstream file(root / "appdata" / "ReSkate" / "items-left-out.json", std::ios::binary | std::ios::trunc);
        file << "{ this is not json";
    }
    check(items_left_out().empty(), "an unreadable file leaves nothing out");
    check(save_items_left_out("Fresh", {"own_d"}) && items_left_out("fresh") == std::set<std::string>{"own_d"}, "and is written again by the next choice");
    fs::remove_all(root, error);
}

// ------------------------------------------------------------------ real mods

// One asset of a merged patch.
struct Held {
    fb::Sha1 sha1;
    std::uint64_t size{};
    bool patched{};          // its payload is in the patch (a mod's, or one the merge wrote)
};
struct Patch {
    // By "superbundle|bundle" (lower case), then asset_key_text.
    std::map<std::string, std::map<std::string, Held>> bundles;
    // The patched documents: where each is, and what it refers to.
    struct Document {
        std::string bundle, key;
        AssetLinks links;
    };
    std::vector<Document> documents;
    // The patched resources and documents as bytes, by bundle, for the chunks they name.
    std::map<std::string, std::vector<std::pair<std::string, std::vector<std::byte>>>> bytes;
    std::map<std::string, std::set<std::uint64_t>> resources;   // by bundle: the ids of its resources
};
Patch read_patch(const fs::path& game, const fs::path& output) {
    Patch patch;
    const auto baseRoot = game / L"Data";
    const auto layout = vfs::read_layout(output / L"layout.toc");
    const CasStore store(baseRoot, output.parent_path() / L"unused", layout.root);
    for (const auto& relative : scan(output).tocs) {
        const auto toc = fb::read_toc(read_file(output / fs::path(relative)));
        for (const auto& bundle : toc.bundles) {
            const auto region = fb::read_bundle_region(bundle.region);
            if (std::none_of(region.files.begin(), region.files.end(), [](const fb::BundleFileInfo& file) { return file.location.patch; }))
                continue;
            const auto listing = list_bundle(store, output, baseRoot, bundle, game);
            if (!listing) throw std::runtime_error(bundle.name + " of the merged patch cannot be listed");
            const auto name = ascii_lower(relative) + '|' + ascii_lower(bundle.name);
            auto& held = patch.bundles[name];
            const auto& manifest = listing->manifest;
            std::size_t at = listing->first;
            const auto take = [&](const std::vector<fb::BundleAsset>& assets, std::size_t first) {
                for (std::size_t index = 0; index < assets.size(); ++index, ++at) {
                    const auto& asset = assets[index];
                    const bool patched = at < listing->files.size() && listing->files[at].location.patch;
                    held[asset_key_text(asset)] = {asset.sha1, asset.originalSize, patched};
                    if (asset.kind == fb::AssetKind::resource) patch.resources[name].insert(asset.resourceId);
                    if (!patched || asset.kind == fb::AssetKind::chunk) continue;
                    auto bytes = read_asset(store, output, baseRoot, *listing, first + index, game);
                    if (asset.kind == fb::AssetKind::ebx) {
                        auto links = asset_links(fb::ebx::read_document(bytes));
                        links.name = ascii_lower(asset.name);
                        links.item_list = item_list_name(asset.name);
                        patch.documents.push_back({name, asset_key_text(asset), std::move(links)});
                    }
                    patch.bytes[name].emplace_back(asset_key_text(asset), std::move(bytes));
                }
            };
            take(manifest.ebx, 0);
            take(manifest.resources, manifest.ebx.size());
            take(manifest.chunks, manifest.ebx.size() + manifest.resources.size());
        }
    }
    return patch;
}

std::string told(const MergeReport& report) {
    std::string text = "issue: '" + report.issue + "'";
    for (const auto& [mod, problems] : report.problems)
        for (const auto& problem : problems) text += "\n  " + mod + ": " + problem;
    return text;
}

// Merges `folders` whole and then with `off` left out of each (by mod name), and holds the second
// patch against the first. Returns how many assets fewer it has.
void compare(const fs::path& game, const fs::path& scratch, const std::string& label, const std::vector<fs::path>& folders,
             const std::map<std::string, std::set<std::string>>& off, const std::map<std::string, std::set<std::string>>& keys) {
    const auto merge = [&](const std::string& name, bool choose, MergeReport& report) {
        Catalog catalog;
        catalog.data_root = game;
        catalog.root = scratch / (label + "-" + name) / L"Mods";
        catalog.present = true;
        std::error_code error;
        fs::remove_all(catalog.root.parent_path(), error);
        fs::create_directories(catalog.root, error);
        for (const auto& folder : folders) {
            Mod mod;
            mod.name = folder.filename().string();
            mod.directory = folder;
            mod.provides_layout = true;
            if (choose)
                if (const auto found = off.find(mod.name); found != off.end()) mod.items_off = found->second;
            catalog.mods.push_back(std::move(mod));
        }
        const auto start = std::chrono::steady_clock::now();
        report = merge_mods(catalog);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        check(report.issue.empty() && report.built && report.problems.empty(), label + ", " + name + ": the merge builds the patch and keeps every mod\n" + told(report));
        std::cout << "  " << label << ", " << name << ": merged in " << ms << " ms\n";
        return catalog.root / generated_folder;
    };
    MergeReport wholeReport, chosenReport;
    const auto wholeOutput = merge("whole", false, wholeReport);
    const auto chosenOutput = merge("chosen", true, chosenReport);
    if (!wholeReport.built || !chosenReport.built) return;
    for (const auto& note : chosenReport.notes)
        if (note.find("left out") != std::string::npos || note.find("loaded all the same") != std::string::npos)
            std::cout << "    note: " << note << '\n';
    const auto whole = read_patch(game, wholeOutput), chosen = read_patch(game, chosenOutput);

    // What the game's own copy of a bundle lists, read when the bundle lost something.
    std::map<std::string, std::set<std::string>> gameKeys;
    const auto game_has = [&](const std::string& bundle, const std::string& key) {
        auto found = gameKeys.find(bundle);
        if (found == gameKeys.end()) {
            found = gameKeys.emplace(bundle, std::set<std::string>{}).first;
            const auto baseRoot = game / L"Data";
            const auto bar = bundle.find('|');
            const auto layout = vfs::read_layout(baseRoot / L"layout.toc");
            const CasStore store(baseRoot, scratch / L"unused", layout.root);
            std::error_code error;
            if (const auto path = baseRoot / fs::path(bundle.substr(0, bar)); fs::is_regular_file(path, error)) {
                const auto toc = fb::read_toc(read_file(path));
                for (const auto& shipped : toc.bundles) {
                    if (ascii_lower(shipped.name) != bundle.substr(bar + 1)) continue;
                    if (const auto listing = list_bundle(store, baseRoot, baseRoot, shipped, game)) {
                        for (const auto& asset : listing->manifest.ebx) found->second.insert(asset_key_text(asset));
                        for (const auto& asset : listing->manifest.resources) found->second.insert(asset_key_text(asset));
                        for (const auto& asset : listing->manifest.chunks) found->second.insert(asset_key_text(asset));
                    }
                }
            }
        }
        return found->second.contains(key);
    };
    // Nothing is in the second patch that was not in the first, and what is in both is the same
    // asset, but for the item lists (their entries for the items left out are gone).
    std::size_t fewer{}, lists{};
    std::uint64_t bytes{};
    std::map<std::string, std::set<std::string>> missing;   // by bundle: the keys that went
    for (const auto& [bundle, assets] : whole.bundles) {
        const auto there = chosen.bundles.find(bundle);
        // (A bundle the mods only passed through is not listed apart when nothing is left of theirs.)
        for (const auto& [key, held] : assets) {
            const auto* other = there == chosen.bundles.end() ? nullptr : (there->second.contains(key) ? &there->second.at(key) : nullptr);
            if (!other) {
                check(!game_has(bundle, key), label + ": " + bundle + " lost " + key + ", which the game's own copy of the bundle has");
                missing[bundle].insert(key);
                ++fewer;
                bytes += held.size;
                continue;
            }
            if (other->sha1 == held.sha1) continue;
            const auto name = key.substr(key.find(':') + 1);
            // (A bundle's reference table and shader tables are combined from the copies, the same either way.)
            check(item_list_name(name), label + ": " + bundle + ": " + key + " is another asset with items left out");
            ++lists;
        }
    }
    for (const auto& [bundle, assets] : chosen.bundles) {
        const auto there = whole.bundles.find(bundle);
        for (const auto& [key, held] : assets)
            check(there != whole.bundles.end() && there->second.contains(key), label + ": " + bundle + " has " + key + " only with items left out");
    }

    // The items: every one left out is gone, every other one is there.
    std::set<std::string> present;
    for (const auto& document : chosen.documents)
        if (!document.links.item.empty()) present.insert(document.links.item);
    std::size_t left{}, stayed{};
    for (const auto& [mod, all] : keys) {
        const auto chosenOff = off.find(mod);
        for (const auto& key : all) {
            const bool out = chosenOff != off.end() && chosenOff->second.contains(key);
            if (!out) check(present.contains(key), label + ": " + key + " of " + mod + " was not left out and is not in the patch");
            else if (present.contains(key)) ++stayed;
            else ++left;
        }
    }

    // What went, by guid and by name, and that nothing still there names it.
    std::set<fb::Guid> goneGuids;
    std::set<std::string, std::less<>> goneNames, goneItems;
    std::set<fb::Guid> stillGuids;
    for (const auto& document : chosen.documents) stillGuids.insert(document.links.file);
    for (const auto& document : whole.documents) {
        const auto went = missing.find(document.bundle);
        if (went == missing.end() || !went->second.contains(document.key) || stillGuids.contains(document.links.file)) continue;
        goneGuids.insert(document.links.file);
        auto& names = document.links.item.empty() ? goneNames : goneItems;
        names.insert(document.links.name);
        names.insert(std::string(last_part(document.links.name)));
    }
    // (A name that an asset still there has too is not gone: one document in two bundles, one of
    // them another mod's.)
    for (const auto& document : chosen.documents)
        for (auto* names : {&goneNames, &goneItems}) {
            names->erase(document.links.name);
            names->erase(std::string(last_part(document.links.name)));
        }
    std::size_t dangling{};
    for (const auto& document : chosen.documents) {
        for (const auto& import : document.links.imports)
            if (goneGuids.contains(import)) {
                if (!dangling++) check(false, label + ": " + document.links.name + " is in the patch and imports " + import.string() + ", which was left out");
            }
        if (document.links.item_list) continue;   // (its strings are item keys of the game's)
        // (An item's own key is what it is called: it names no other item.)
        for (const auto& text : document.links.strings)
            if (goneNames.contains(text) || (text != document.links.item && goneItems.contains(text))) {
                if (!dangling++) check(false, label + ": " + document.links.name + " is in the patch and names " + text + ", which was left out");
            }
    }
    check(!dangling, label + ": " + std::to_string(dangling) + " reference(s) to what was left out");

    // A resource a document still there names by id is still in its bundle.
    std::size_t unresolved{};
    for (const auto& document : chosen.documents) {
        const auto before = whole.resources.find(document.bundle);
        const auto after = chosen.resources.find(document.bundle);
        if (before == whole.resources.end()) continue;
        for (const auto id : document.links.resources)
            if (before->second.contains(id) && (after == chosen.resources.end() || !after->second.contains(id))) {
                if (!unresolved++) check(false, label + ": " + document.links.name + " names a resource that was left out");
            }
    }
    // A chunk that went is named by nothing still there.
    std::size_t chunks{}, named{};
    for (const auto& [bundle, went] : missing)
        chunks += static_cast<std::size_t>(std::ranges::count_if(went, [](const std::string& key) { return key.starts_with("2:"); }));
    // (The chunks' ids are read from the first patch's manifests, where they are guids.)
    {
        const auto baseRoot = game / L"Data";
        const auto layout = vfs::read_layout(wholeOutput / L"layout.toc");
        const CasStore store(baseRoot, wholeOutput.parent_path() / L"unused", layout.root);
        for (const auto& relative : scan(wholeOutput).tocs) {
            const auto toc = fb::read_toc(read_file(wholeOutput / fs::path(relative)));
            for (const auto& bundle : toc.bundles) {
                const auto name = ascii_lower(relative) + '|' + ascii_lower(bundle.name);
                const auto went = missing.find(name);
                if (went == missing.end()) continue;
                const auto listing = list_bundle(store, wholeOutput, baseRoot, bundle, game);
                if (!listing) continue;
                std::vector<fb::Guid> ids;
                for (const auto& chunk : listing->manifest.chunks)
                    if (went->second.contains(asset_key_text(chunk))) ids.push_back(chunk.guid);
                if (ids.empty()) continue;
                const auto still = chosen.bytes.find(name);
                if (still == chosen.bytes.end()) continue;
                for (const auto& id : ids) {
                    auto swapped = id;
                    std::reverse(swapped.bytes.begin(), swapped.bytes.begin() + 4);
                    std::reverse(swapped.bytes.begin() + 4, swapped.bytes.begin() + 6);
                    std::reverse(swapped.bytes.begin() + 6, swapped.bytes.begin() + 8);
                    for (const auto& [key, payload] : still->second)
                        for (const fb::Guid* form : std::initializer_list<const fb::Guid*>{&id, &swapped})
                            if (std::search(payload.begin(), payload.end(), form->bytes.begin(), form->bytes.end()) != payload.end()) {
                                if (!named++) check(false, label + ": " + key + " in " + name + " names chunk " + id.string() + ", which was left out");
                            }
                }
            }
        }
    }
    // The file that says which mod each item is from has the items that are in the game only.
    for (const auto& source : read_item_sources(chosenOutput / item_sources_file)) {
        const auto chosenOff = off.find(source.mod);
        if (chosenOff == off.end()) continue;
        for (const auto& key : source.keys)
            check(!chosenOff->second.contains(key) || present.contains(key), label + ": " + key + " is listed as from " + source.mod + " and is not in the game");
    }
    std::cout << "  " << label << ": " << left << " item(s) left out" << (stayed ? ", " + std::to_string(stayed) + " asked for stay" : "") << ", "
              << fewer << " asset(s) fewer (" << chunks << " chunk(s)), " << (bytes >> 10) << " KB of documents and resources, " << lists
              << " item list(s) changed, " << dangling << " dangling, " << unresolved << " resource(s) unresolved, " << named << " chunk(s) still named\n";
}

// The items as the launcher's mod manager lists them (mod_items.h): the ones the mod adds, each once,
// with a thumbnail where the mod has one.
void listed_items(const fs::path& game, const fs::path& folder, const std::set<std::string>& keys) {
    constexpr std::uint32_t side = 96;
    const auto name = folder.filename().string();
    const auto start = std::chrono::steady_clock::now();
    const auto read = read_mod_items(game, folder, side);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    check(read.problem.empty(), name + ": its items are read (" + read.problem + ")");
    std::set<std::string> seen;
    std::size_t pictures{};
    for (const auto& item : read.items) {
        check(seen.insert(item.key).second, name + ": " + item.key + " is listed twice");
        check(keys.contains(item.key), name + ": " + item.key + " is listed and is not an item the merge reads of it");
        check(!item.category.empty() && item.asset.find(item.category) != std::string::npos, name + ": " + item.key + " has no kind");
        const bool picture = !item.thumbnail.rgba.empty();
        check(!picture || (item.thumbnail.width == side && item.thumbnail.height == side &&
                           item.thumbnail.rgba.size() == std::size_t{side} * side * 4), name + ": " + item.key + "'s thumbnail is not the size asked for");
        pictures += picture;
    }
    // (The merge's count has the game's own items the mod changes too; those are not listed.)
    check(!read.items.empty() || keys.empty(), name + ": the merge reads items of it and none is listed");
    std::cout << "  " << name << ": " << read.items.size() << " item(s) listed of " << keys.size() << " added or changed, " << pictures
              << " with a thumbnail, in " << ms << " ms\n";
    std::array<wchar_t, 1024> sheets{};
    const auto length = GetEnvironmentVariableW(L"RESKATE_ITEM_SHEETS", sheets.data(), static_cast<DWORD>(sheets.size()));
    if (!length || length >= sheets.size() || read.items.empty()) return;
    // A 24-bit BMP, ten thumbnails to a row, over grey so that what is see-through shows.
    const std::uint32_t columns = 10, rows = static_cast<std::uint32_t>((read.items.size() + columns - 1) / columns);
    const std::uint32_t width = columns * side, height = rows * side, stride = (width * 3 + 3) & ~3u;
    std::vector<std::uint8_t> pixels(std::size_t{stride} * height, 0x40);
    for (std::size_t at = 0; at < read.items.size(); ++at) {
        const auto& image = read.items[at].thumbnail;
        if (image.rgba.empty()) continue;
        const auto left = static_cast<std::uint32_t>(at % columns) * side, top = static_cast<std::uint32_t>(at / columns) * side;
        for (std::uint32_t y = 0; y < side; ++y)
            for (std::uint32_t x = 0; x < side; ++x) {
                const auto* from = &image.rgba[(std::size_t{y} * side + x) * 4];
                auto* to = &pixels[std::size_t{height - 1 - (top + y)} * stride + std::size_t{left + x} * 3];
                for (int channel = 0; channel < 3; ++channel)
                    to[channel] = static_cast<std::uint8_t>((from[2 - channel] * from[3] + 0x40 * (255 - from[3])) / 255);
            }
    }
    std::ofstream file(fs::path(std::wstring(sheets.data(), length)) / (name + ".bmp"), std::ios::binary | std::ios::trunc);
    const std::uint32_t size = 54 + static_cast<std::uint32_t>(pixels.size());
    const auto put = [&](std::uint32_t value, int bytes) { file.write(reinterpret_cast<const char*>(&value), bytes); };
    file.write("BM", 2);
    put(size, 4); put(0, 4); put(54, 4); put(40, 4); put(width, 4); put(height, 4); put(1, 2); put(24, 2); put(0, 4);
    put(static_cast<std::uint32_t>(pixels.size()), 4); put(2835, 4); put(2835, 4); put(0, 4); put(0, 4);
    file.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
}

void real_mods(const fs::path& game, const fs::path& scratch, const std::vector<fs::path>& folders) {
    // Each mod's items, as the merge reads them.
    Catalog catalog;
    catalog.data_root = game;
    catalog.root = scratch / L"unused";
    catalog.present = true;
    for (const auto& folder : folders) {
        Mod mod;
        mod.name = folder.filename().string();
        mod.directory = folder;
        mod.provides_layout = true;
        catalog.mods.push_back(std::move(mod));
    }
    std::vector<std::string> notes;
    const auto found = check_store_copies(catalog, [](const std::string&) { return false; }, &notes);
    std::map<std::string, std::set<std::string>> keys;
    for (const auto& added : found.added) keys[added.mod].insert(added.keys.begin(), added.keys.end());
    const auto every = [&](const std::string& mod, std::size_t step, std::size_t first) {
        std::set<std::string> out;
        std::size_t at = 0;
        for (const auto& key : keys[mod])
            if (at++ % step == first) out.insert(key);
        return out;
    };
    for (const auto& folder : folders) {
        const auto name = folder.filename().string();
        std::cout << name << ": " << keys[name].size() << " item(s)\n";
        listed_items(game, folder, keys[name]);
        if (keys[name].empty()) continue;
        compare(game, scratch, name + "-all", {folder}, {{name, keys[name]}}, {{name, keys[name]}});
        if (keys[name].size() > 1) {
            compare(game, scratch, name + "-half", {folder}, {{name, every(name, 2, 0)}}, {{name, keys[name]}});
            compare(game, scratch, name + "-one", {folder}, {{name, {*keys[name].begin()}}}, {{name, keys[name]}});
        }
    }
    if (folders.size() > 1) {
        // All together, every other item of every mod left out, and then the other half.
        for (const std::size_t first : {std::size_t{0}, std::size_t{1}}) {
            std::map<std::string, std::set<std::string>> off;
            for (const auto& folder : folders) off[folder.filename().string()] = every(folder.filename().string(), 2, first);
            compare(game, scratch, "together-" + std::to_string(first), folders, off, keys);
        }
    }
    std::error_code error;
    fs::remove_all(scratch, error);
}
} // namespace

int main(int argc, char** argv) try {
    rules();
    choices();
    std::error_code error;
    if (argc < 4 || !fs::exists(fs::path(argv[1]) / L"Data" / L"Win32" / L"items.toc", error)) {
        std::cout << "No game, scratch folder and mods given; no mod was merged.\n";
    } else {
        std::vector<fs::path> folders;
        for (int index = 3; index < argc; ++index) folders.emplace_back(argv[index]);
        real_mods(argv[1], argv[2], folders);
    }
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "mod item choice: ok\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
