// Cosmetic mods whose item lists name an item the merged bundle does not hold.
//
// Every cosmetic mod ships its own copy of the game's shared customization bundle: the game's
// assets, one of the game's item lists with the mod's item appended, and the item. The merge
// combines the lists, and a bundle keeps one asset per name. The game reads each entry of an
// item list at startup without checking it, so an entry whose item is not in the bundle is a
// crash on every launch (Skate.exe+0x891e07, 2026-10-04). Two ways to get one:
//   * two mods each add a different item under the same asset name, and only one can stay;
//   * a mod's list names an item no installed mod ships (it was built on top of another mod).
// Either way the entry has to go, and everything else of every mod has to stay. (A bundle only
// one mod ships is not merged at all, it passes through as that mod built it, so every case
// here has two mods shipping the shared bundle.)
//
// The same mods also show what else the merge does with two copies of one of the game's assets:
//   * an item two mods both list is in the merged list once;
//   * values two mods changed in place in one asset are both in the merged asset, and where that
//     cannot be done the merge keeps one mod's copy whole and says whose changes are missing;
//   * the list other mods' copies of the bundle are given leaves out an item that is not in them.
//
// The mods are made here from the installed game's own list and one of its items.
// Arguments: <Skate folder> [<folder>]; skipped when the game is not there. With a second
// argument the two mods of the first case are also left in that folder, stamped for this game,
// to try a build against in the game itself.
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Resource/binary_bundle.h"
#include "Engine/Resource/cas_codec.h"
#include "Engine/Resource/ebx_carry.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Resource/ebx_writer.h"
#include "Engine/Resource/toc.h"
#include "Engine/Vfs/mod_catalog.h"
#include "Engine/Vfs/mod_merge_internal.h"

#include <Windows.h>

#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>

namespace fs = std::filesystem;
namespace fb = dingosdk::frostbite;
using namespace dingosdk;
using namespace dingosdk::mods::detail;

namespace {
using Bytes = std::vector<std::byte>;
constexpr char shared_bundle[] = "win32/characters/customization/configs/cas_main_sharedbundle";
constexpr char list_type[] = "delmaritemcollectionasset";

int failures = 0;
void expect(bool ok, const std::string& what) {
    if (ok) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}

fb::Guid guid(std::uint8_t seed) {
    fb::Guid value;
    for (std::size_t index = 0; index < value.bytes.size(); ++index)
        value.bytes[index] = static_cast<std::byte>(static_cast<std::uint8_t>(seed + index));
    return value;
}

// The shared bundle as one data layer has it: the game's, a mod's or the merged patch's.
struct Shared {
    fb::TocBundle bundle;
    Listing listing;
};
Shared shared_in(const CasStore& store, const fs::path& root, const fs::path& baseRoot, const fs::path& gameRoot) {
    for (const auto& bundle : fb::read_toc(read_file(root / L"Win32" / L"items.toc")).bundles) {
        if (lower(bundle.name) != shared_bundle) continue;
        if (auto listing = list_bundle(store, root, baseRoot, bundle, gameRoot)) return {bundle, std::move(*listing)};
        break;
    }
    throw std::runtime_error(std::string("no readable ") + shared_bundle + " in " + root.string());
}

bool list_name(std::string_view name) {
    const auto text = lower(name);
    return text.starts_with("items/") && text.find("collection") != std::string::npos;
}

// The items a list names, by the guid of each one's own document.
std::vector<fb::Guid> listed(const fb::ebx::Document& list) {
    const auto* root = list.root();
    const auto* items = root && root->object ? root->object->find("Items") : nullptr;
    const auto* entries = items ? std::get_if<fb::ebx::Value::Array>(&items->value.data) : nullptr;
    if (!entries) throw std::runtime_error("an item list has no Items");
    std::vector<fb::Guid> result;
    for (const auto& entry : *entries) {
        const auto* pointer = std::get_if<fb::ebx::PointerReference>(&entry.data);
        if (!pointer || pointer->kind != fb::ebx::PointerKind::external)
            throw std::runtime_error("an item list entry is not a reference to another document");
        result.push_back(list.imports.at(static_cast<std::size_t>(pointer->index)).fileGuid);
    }
    return result;
}

// The game's list with more items appended, the way a cosmetic mod ships it.
Bytes list_with(const fb::ebx::Document& game, std::initializer_list<fb::ebx::ImportReference> items) {
    auto list = fb::ebx::detail::clone_document(game);
    for (auto& field : list.instances.front().object->fields) {
        if (field.name != "Items") continue;
        auto& entries = std::get<fb::ebx::Value::Array>(field.value.data);
        for (const auto& item : items) {
            fb::ebx::Value entry;
            entry.data = fb::ebx::PointerReference{fb::ebx::PointerKind::external,
                                                   static_cast<std::int32_t>(list.imports.size())};
            list.imports.push_back(item);
            entries.push_back(std::move(entry));
        }
    }
    return fb::ebx::write_document(list);
}

// One of the game's items under a name and identity of its own: what a cosmetic mod adds.
struct Item {
    std::string name;
    fb::ebx::ImportReference identity;
    Bytes payload;
};
Item item_from(const fb::ebx::Document& donor, const std::string& name, std::uint8_t seed) {
    auto item = fb::ebx::detail::clone_document(donor);
    auto& root = item.instances.front();
    item.fileGuid = guid(seed);
    root.instanceGuid = guid(static_cast<std::uint8_t>(seed + 0x40));
    const auto key = name.substr(name.rfind('/') + 1);
    std::uint32_t hash = 5381;   // the game's own hash of an item's key
    for (const unsigned char character : key) hash = hash * 33U ^ character;
    for (auto& field : root.object->fields) {
        if (field.name == "Name") field.value.data = name;
        else if (field.name == "Key") field.value.data = key;
        else if (field.name == "HashedAssetKey") {
            if (std::holds_alternative<std::int64_t>(field.value.data)) field.value.data = static_cast<std::int64_t>(hash);
            else field.value.data = static_cast<std::uint64_t>(hash);
        }
    }
    return {name, {item.fileGuid, root.instanceGuid}, fb::ebx::write_document(item)};
}

// What the mods are made from: the game's shared bundle, its shortest item list and an item.
struct Game {
    fs::path root, base;
    Shared shared;
    std::size_t listIndex{};
    std::string listName;
    fb::ebx::Document list;
    std::size_t listed{};
    std::size_t itemIndex{};
    std::string itemName;
    fb::ebx::Document item;
};
Game read_game(const fs::path& root, const CasStore& store) {
    Game game{root, root / L"Data"};
    game.shared = shared_in(store, game.base, game.base, root);
    const auto& assets = game.shared.listing.manifest.ebx;
    bool list{}, item{};
    for (std::size_t index = 0; index < assets.size(); ++index) {
        const auto name = lower(assets[index].name);
        if (!name.starts_with("items/") || (item && !list_name(name))) continue;
        auto document = fb::ebx::read_document(read_asset(store, game.base, game.base, game.shared.listing, index, root));
        const auto type = lower(document.rootType);
        if (type == list_type) {
            const auto count = listed(document).size();
            if (list && count >= game.listed) continue;
            list = true;
            game.listIndex = index;
            game.listName = assets[index].name;
            game.listed = count;
            game.list = std::move(document);
        } else if (!item && type.find("itemasset") != std::string::npos) {
            item = true;
            game.itemIndex = index;
            game.itemName = assets[index].name;
            game.item = std::move(document);
        }
    }
    if (!list || !item) throw std::runtime_error("the game's shared bundle has no item list or no item");
    return game;
}

// A cosmetic mod's folder: its copy of the shared bundle in items.toc (the game's assets, the
// item list replaced and its items added) and one archive holding the manifest and those payloads.
// With no list the mod leaves the game's as it is, as a mod does whose items go in another one.
// `changed` replaces assets of the game's, by their place in the bundle.
void write_mod(const fs::path& directory, const CasStore& store, const Game& game, const Bytes* list,
               std::initializer_list<Item> items, const std::map<std::size_t, Bytes>& changed = {}) {
    auto manifest = game.shared.listing.manifest;
    const auto& shipped = game.shared.listing.files;
    std::vector<fb::BundleFileInfo> files(shipped.begin() + static_cast<std::ptrdiff_t>(game.shared.listing.first), shipped.end());
    const auto installChunk = shipped.front().location.installChunk;
    Bytes archive;
    const auto stored = [&](std::span<const std::byte> bytes) {
        const fb::BundleFileInfo file{{true, installChunk, 1}, static_cast<std::uint32_t>(archive.size()),
                                      static_cast<std::uint32_t>(bytes.size())};
        archive.insert(archive.end(), bytes.begin(), bytes.end());
        return file;
    };
    const auto encoded = [](const Bytes& payload) { return fb::encode_cas(payload, {.compression = fb::CasCompression::raw}); };
    if (list) {
        manifest.ebx[game.listIndex].sha1 = sha1_of(*list);
        manifest.ebx[game.listIndex].originalSize = list->size();
        files[game.listIndex] = stored(encoded(*list));
    }
    for (const auto& [index, payload] : changed) {
        manifest.ebx[index].sha1 = sha1_of(payload);
        manifest.ebx[index].originalSize = payload.size();
        files[index] = stored(encoded(payload));
    }
    for (const auto& item : items) {
        fb::BundleAsset asset;
        asset.kind = fb::AssetKind::ebx;
        asset.name = item.name;
        asset.sha1 = sha1_of(item.payload);
        asset.originalSize = item.payload.size();
        files.insert(files.begin() + static_cast<std::ptrdiff_t>(manifest.ebx.size()), stored(encoded(item.payload)));
        manifest.ebx.push_back(std::move(asset));
    }
    files.insert(files.begin(), stored(fb::write_binary_bundle(manifest)));
    const std::vector<fb::TocBundle> bundles{{game.shared.bundle.name, fb::write_bundle_region(files), 1}};
    fs::create_directories(directory / L"Win32" / fs::path(store.directory(installChunk)));
    write_file(directory / L"Win32" / L"items.toc", fb::write_patch_toc(bundles));
    write_file(directory / L"Win32" / fs::path(store.directory(installChunk)) / L"cas_01.cas", archive);
    fs::copy_file(game.base / L"layout.toc", directory / L"layout.toc", fs::copy_options::overwrite_existing);
}

// A Mods folder of its own beside nothing, merged against the installed game.
struct Merge {
    fs::path root;
    mods::Catalog catalog;

    Merge(const Game& game, const std::string& name) {
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        root = fs::path(temp) / ("reskate-item-lists-" + std::to_string(GetCurrentProcessId()) + "-" + name);
        fs::remove_all(root);
        catalog.data_root = game.root;
        catalog.root = root / "Mods";
        catalog.present = true;
    }
    ~Merge() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    // Mods are added highest priority first.
    fs::path add(const std::string& name) {
        mods::Mod mod;
        mod.name = name;
        mod.directory = catalog.root / name;
        mod.provides_layout = true;
        catalog.mods.push_back(mod);
        return mod.directory;
    }
};

// The merged shared bundle: the guid of every document in it, and what each item list names.
struct Merged {
    std::set<fb::Guid> held;
    std::map<std::string, std::vector<fb::Guid>> lists;
};
Merged read_merged(const Merge& merge, const Game& game) {
    const auto output = merge.catalog.root / mods::generated_folder;
    const auto layout = vfs::read_layout(output / L"layout.toc");
    const CasStore store(game.base, merge.root / L"unused", layout.root);
    const auto shared = shared_in(store, output, game.base, game.root);
    Merged merged;
    for (std::size_t index = 0; index < shared.listing.manifest.ebx.size(); ++index) {
        const auto bytes = read_asset(store, output, game.base, shared.listing, index, game.root);
        merged.held.insert(fb::ebx::read_file_guid(bytes));
        const auto& name = shared.listing.manifest.ebx[index].name;
        if (!list_name(name)) continue;
        const auto document = fb::ebx::read_document(bytes);
        if (lower(document.rootType) == list_type) merged.lists.emplace(name, listed(document));
    }
    return merged;
}

bool noted(const mods::MergeReport& report, const std::string& text) {
    return std::ranges::find(report.notes, text) != report.notes.end();
}
bool noted_part(const mods::MergeReport& report, std::string_view part) {
    return std::ranges::any_of(report.notes, [&](const std::string& note) { return note.find(part) != std::string::npos; });
}
std::string describe(const mods::MergeReport& report) {
    std::string text = "issue: '" + report.issue + "'";
    for (const auto& note : report.notes) text += "\n  note: " + note;
    for (const auto& [mod, problems] : report.problems)
        for (const auto& problem : problems) text += "\n  " + mod + ": " + problem;
    return text;
}

// What the game needs of any merge: every item every list names is in the bundle. Then how many
// entries the mods' list ended with, and that no mod was turned away over it.
std::vector<fb::Guid> merged_list(const Merge& merge, const Game& game, const mods::MergeReport& report,
                                  const std::string& label, std::size_t added) {
    expect(report.issue.empty() && report.built && report.problems.empty(),
           label + ": the merge builds the patch and keeps every mod\n" + describe(report));
    if (!report.built) return {};
    const auto merged = read_merged(merge, game);
    for (const auto& [name, items] : merged.lists)
        for (const auto& item : items)
            expect(merged.held.contains(item), label + ": " + name + " names " + item.string() +
                   ", which the merged bundle does not hold");
    const auto found = merged.lists.find(game.listName);
    expect(found != merged.lists.end(), label + ": the merged bundle still has " + game.listName);
    if (found == merged.lists.end()) return {};
    expect(found->second.size() == game.listed + added, label + ": the list has the game's " + std::to_string(game.listed) +
           " item(s) and " + std::to_string(added) + " more (" + std::to_string(found->second.size()) + ")\n" + describe(report));
    return found->second;
}
bool names(const std::vector<fb::Guid>& list, const Item& item) {
    return std::ranges::find(list, item.identity.fileGuid) != list.end();
}

constexpr char same_name[] = "items/reskate_test/own_same_name";

// Two mods add different items under one name. The higher-priority mod's is the one the bundle
// keeps, so the other's entry goes, and the merge says which mod lost an item to which.
void same_name_from_two_mods(const Game& game, const CasStore& store, const fs::path* keep) {
    Merge merge(game, "same-name");
    const auto upper = item_from(game.item, same_name, 0x10);
    const auto under = item_from(game.item, same_name, 0x20);
    const auto upperList = list_with(game.list, {upper.identity}), underList = list_with(game.list, {under.identity});
    const auto build = [&](const fs::path& top, const fs::path& bottom) {
        write_mod(top, store, game, &upperList, {upper});
        write_mod(bottom, store, game, &underList, {under});
    };
    const auto top = merge.add("upper"), bottom = merge.add("under");
    build(top, bottom);
    const auto report = mods::merge_mods(merge.catalog);
    const auto list = merged_list(merge, game, report, "same name", 1);
    expect(names(list, upper) && !names(list, under), "same name: the list keeps the higher-priority mod's item only");
    expect(noted(report, "under: " + game.shared.bundle.name + ": 1 added asset(s) share a name with ones upper adds, e.g. " +
                         same_name + "; the merged bundle keeps upper's"),
           "same name: the merge says whose asset the name went to\n" + describe(report));
    expect(noted(report, "under: " + game.listName + ": 1 item(s) left out of the list because the merged bundle does not "
                         "hold them, e.g. " + same_name + ", replaced by upper's asset of the same name"),
           "same name: the merge says which item was left out and why\n" + describe(report));
    if (!keep) return;
    // The same two for the game's own Mods folder, where a mod has to say what it was built for.
    const std::string stamp = "ReSkate Studio native Patch v1\nskate_sha256=" + std::string(supported_build::game_sha256) + "\n";
    for (const auto* name : {L"ReSkateTest-SameNameUpper", L"ReSkateTest-SameNameUnder"}) fs::create_directories(*keep / name);
    build(*keep / L"ReSkateTest-SameNameUpper", *keep / L"ReSkateTest-SameNameUnder");
    for (const auto* name : {L"ReSkateTest-SameNameUpper", L"ReSkateTest-SameNameUnder"})
        write_file(*keep / name / mods::studio_marker_file, std::as_bytes(std::span(stamp)));
    std::cout << "The two same-name mods are in " << keep->string() << ".\n";
}

// Two mods add items under names of their own: both stay, and nothing is said about leaving any out.
void different_names_from_two_mods(const Game& game, const CasStore& store) {
    Merge merge(game, "different-names");
    const auto first = item_from(game.item, "items/reskate_test/own_first", 0x10);
    const auto second = item_from(game.item, "items/reskate_test/own_second", 0x20);
    const auto firstList = list_with(game.list, {first.identity}), secondList = list_with(game.list, {second.identity});
    write_mod(merge.add("first"), store, game, &firstList, {first});
    write_mod(merge.add("second"), store, game, &secondList, {second});
    const auto report = mods::merge_mods(merge.catalog);
    const auto list = merged_list(merge, game, report, "different names", 2);
    expect(names(list, first) && names(list, second), "different names: the list has both mods' items");
    expect(!noted_part(report, "left out of the list") && !noted_part(report, "share a name"),
           "different names: nothing is reported\n" + describe(report));
}

// One mod alone changes the list, beside a mod that ships the bundle with the game's. Its list
// as it wrote it is what the bundle gets, unless that list names an item nothing ships: then
// that entry goes and its own item stays.
void one_mod_changes_the_list(const Game& game, const CasStore& store, bool missing) {
    const std::string label = missing ? "an item nothing ships" : "one mod's list";
    Merge merge(game, missing ? "missing-item" : "one-list");
    const auto own = item_from(game.item, "items/reskate_test/own_alone", 0x10);
    const fb::ebx::ImportReference absent{guid(0x70), guid(0x90)};
    const auto shipped = missing ? list_with(game.list, {own.identity, absent}) : list_with(game.list, {own.identity});
    write_mod(merge.add("alone"), store, game, &shipped, {own});
    write_mod(merge.add("bystander"), store, game, nullptr, {});
    const auto report = mods::merge_mods(merge.catalog);
    const auto list = merged_list(merge, game, report, label, 1);
    expect(names(list, own), label + ": the list has the mod's own item");
    const auto said = noted(report, "alone: " + game.listName + ": 1 item(s) left out of the list because the merged bundle "
                                    "does not hold them, e.g. the asset with guid " + absent.fileGuid.string() +
                                    ", which no enabled mod adds to the bundle");
    expect(said == missing, label + (missing ? ": the merge says which item was left out\n" : ": nothing is reported\n") +
           describe(report));
}

// Two packs that both carry one item, each with an item of its own besides. Both list the shared
// one, and it is one item: the merged list has it once.
void an_item_two_mods_both_list(const Game& game, const CasStore& store) {
    Merge merge(game, "listed-twice");
    const auto both = item_from(game.item, "items/reskate_test/own_in_both", 0x10);
    const auto first = item_from(game.item, "items/reskate_test/own_first", 0x20);
    const auto second = item_from(game.item, "items/reskate_test/own_second", 0x30);
    const auto firstList = list_with(game.list, {both.identity, first.identity});
    const auto secondList = list_with(game.list, {both.identity, second.identity});
    write_mod(merge.add("first"), store, game, &firstList, {both, first});
    write_mod(merge.add("second"), store, game, &secondList, {both, second});
    const auto report = mods::merge_mods(merge.catalog);
    const auto list = merged_list(merge, game, report, "listed by both", 3);
    expect(names(list, both) && names(list, first) && names(list, second), "listed by both: the list has all three items");
    expect(std::ranges::count(list, both.identity.fileGuid) == 1, "listed by both: the item both mods list is in it once");
    expect(noted(report, game.listName + ": combined 2 edits (0 instance(s), 3 list entries, 1 repeated entry(ies) listed once)"),
           "listed by both: the merge says one entry was a repeat\n" + describe(report));
}

// The game's item as a mod that retunes it ships it: the same document, changed in place.
Bytes item_with(const fb::ebx::Document& game, const std::function<void(fb::ebx::Object&)>& change) {
    auto item = fb::ebx::detail::clone_document(game);
    change(*item.instances.front().object);
    return fb::ebx::write_document(item);
}
fb::ebx::Value& field(fb::ebx::Object& object, std::string_view name) {
    for (auto& entry : object.fields)
        if (entry.name == name) return entry.value;
    throw std::runtime_error("the game's item has no " + std::string(name));
}
void set_hash(fb::ebx::Object& object, std::uint32_t hash) {
    auto& value = field(object, "HashedAssetKey");
    if (std::holds_alternative<std::int64_t>(value.data)) value.data = static_cast<std::int64_t>(hash);
    else value.data = static_cast<std::uint64_t>(hash);
}
std::uint32_t hash_of(const fb::ebx::Object& object) {
    const auto& value = field(const_cast<fb::ebx::Object&>(object), "HashedAssetKey");
    const auto* number = std::get_if<std::int64_t>(&value.data);
    return number ? static_cast<std::uint32_t>(*number) : static_cast<std::uint32_t>(std::get<std::uint64_t>(value.data));
}
// A change the merge cannot take value by value: the first list of the item made one shorter,
// or failing that its first reference cleared. False when the item has neither.
bool reshape(fb::ebx::Object& object) {
    for (auto& entry : object.fields)
        if (auto* list = std::get_if<fb::ebx::Value::Array>(&entry.value.data); list && !list->empty()) {
            list->pop_back();
            return true;
        }
    for (auto& entry : object.fields)
        if (auto* pointer = std::get_if<fb::ebx::PointerReference>(&entry.value.data);
            pointer && pointer->kind != fb::ebx::PointerKind::null) {
            *pointer = {};
            return true;
        }
    return false;
}

// The merged shared bundle's copy of the game's item, and the sha1 the bundle lists it under.
std::pair<fb::ebx::Document, fb::Sha1> merged_item(const Merge& merge, const Game& game) {
    const auto output = merge.catalog.root / mods::generated_folder;
    const auto layout = vfs::read_layout(output / L"layout.toc");
    const CasStore store(game.base, merge.root / L"unused", layout.root);
    const auto shared = shared_in(store, output, game.base, game.root);
    const auto& assets = shared.listing.manifest.ebx;
    for (std::size_t index = 0; index < assets.size(); ++index)
        if (assets[index].name == game.itemName)
            return {fb::ebx::read_document(read_asset(store, output, game.base, shared.listing, index, game.root)), assets[index].sha1};
    throw std::runtime_error("the merged bundle has no " + game.itemName);
}

// Two mods each ship the game's item with values of their own changed in place. Nothing was added
// to it, so the merge has no list to put together: it has the values. Each mod's are in the
// merged item, and where both changed one the higher mod's is.
void values_two_mods_changed(const Game& game, const CasStore& store, bool contested) {
    const std::string label = contested ? "one value from two mods" : "values from two mods";
    Merge merge(game, contested ? "one-value" : "two-values");
    const auto upper = item_with(game.item, [&](fb::ebx::Object& root) {
        if (contested) set_hash(root, 0x1111);
        else field(root, "Key").data = std::string("reskate_upper");
    });
    const auto under = item_with(game.item, [&](fb::ebx::Object& root) {
        set_hash(root, 0x2222);
        if (contested) field(root, "Key").data = std::string("reskate_under");
    });
    write_mod(merge.add("upper"), store, game, nullptr, {}, {{game.itemIndex, upper}});
    write_mod(merge.add("under"), store, game, nullptr, {}, {{game.itemIndex, under}});
    const auto report = mods::merge_mods(merge.catalog);
    expect(report.issue.empty() && report.built && report.problems.empty(),
           label + ": the merge builds the patch and keeps every mod\n" + describe(report));
    if (!report.built) return;
    const auto item = merged_item(merge, game).first;
    auto& root = *item.instances.front().object;
    expect(std::get<std::string>(field(root, "Key").data) == (contested ? "reskate_under" : "reskate_upper"),
           label + ": the merged item has the key one mod changed");
    expect(hash_of(root) == (contested ? 0x1111u : 0x2222u),
           label + (contested ? ": the value both changed is the higher mod's" : ": and the value the other mod changed"));
    expect(noted(report, game.itemName + ": combined 2 edits (0 instance(s), 0 list entries, " +
                         (contested ? "3 changed value(s), 1 disagreed)" : "2 changed value(s))")),
           label + ": the merge says what it combined\n" + describe(report));
}

// One of the two also changed the item in a way that is not a value (a list made shorter). That
// cannot be put together with the other mod's values, so the higher mod's copy stays whole, as
// it always did, and the merge now says whose changes are not in the game.
void a_change_that_does_not_combine(const Game& game, const CasStore& store) {
    Merge merge(game, "not-combined");
    bool reshaped{};
    const auto upper = item_with(game.item, [&](fb::ebx::Object& root) {
        field(root, "Key").data = std::string("reskate_upper");
        reshaped = reshape(root);
    });
    if (!reshaped) {
        std::cout << "The game's item has no list or reference to change; that case is skipped.\n";
        return;
    }
    const auto under = item_with(game.item, [&](fb::ebx::Object& root) { set_hash(root, 0x2222); });
    write_mod(merge.add("upper"), store, game, nullptr, {}, {{game.itemIndex, upper}});
    write_mod(merge.add("under"), store, game, nullptr, {}, {{game.itemIndex, under}});
    const auto report = mods::merge_mods(merge.catalog);
    expect(report.issue.empty() && report.built && report.problems.empty(),
           "not combined: the merge builds the patch and keeps every mod\n" + describe(report));
    if (!report.built) return;
    expect(merged_item(merge, game).second == sha1_of(upper), "not combined: the bundle has the higher mod's copy as that mod built it");
    expect(noted(report, game.itemName + ": kept upper's copy whole; the changes under made to it are of a kind that cannot "
                         "be combined with it and are not in the game"),
           "not combined: the merge says whose changes are missing\n" + describe(report));
}

// What other mods' copies of the bundle are given (a map carries one for its levels): one list
// for all of them, put together from every mod's. Under a name two mods share, such a copy gets
// the higher mod's asset, so the lower mod's entry for its own has to be out of that list too.
void the_list_other_copies_are_given(const Game& game, const CasStore& store) {
    Merge merge(game, "carried-list");
    const auto upper = item_from(game.item, same_name, 0x10);
    const auto under = item_from(game.item, same_name, 0x20);
    const auto upperList = list_with(game.list, {upper.identity}), underList = list_with(game.list, {under.identity});
    write_mod(merge.add("upper"), store, game, &upperList, {upper});
    write_mod(merge.add("under"), store, game, &underList, {under});
    std::vector<const mods::Mod*> order;
    std::map<const mods::Mod*, RelativeFiles> files;
    for (const auto& mod : merge.catalog.mods) {
        order.push_back(&mod);
        files.emplace(&mod, scan(mod.directory));
    }
    mods::MergeReport report;
    const auto overrides = collect_asset_overrides(order, files, store, game.base, game.root, report);
    const auto versions = overrides.changed.find(lower(game.listName));
    expect(versions != overrides.changed.end() && versions->second.size() == 1,
           "carried list: there is one changed list for other mods' copies\n" + describe(report));
    if (versions == overrides.changed.end() || versions->second.empty()) return;
    const auto& change = versions->second.begin()->second;
    const auto list = listed(fb::ebx::read_document(fb::decode_cas(change.encoded, {game.root})));
    expect(list.size() == game.listed + 1 && names(list, upper) && !names(list, under),
           "carried list: it has the game's items and the higher mod's, not the one no copy is given (" +
           std::to_string(list.size()) + " entries)");
    expect(change.names == std::set<fb::Guid>{upper.identity.fileGuid}, "carried list: the item that goes with it is the higher mod's");
    expect(noted(report, "under: " + game.listName + ": 1 entry(ies) left out of the copies other mods carry, where another "
                         "mod's asset of the same name stands in for what they name"),
           "carried list: the merge says which mod's entry was left out\n" + describe(report));
}
} // namespace

int main(int argc, char** argv) try {
    if (argc < 2 || !fs::exists(fs::path(argv[1]) / L"Data" / L"Win32" / L"items.toc")) {
        std::cout << "No game given; skipped.\n";
        return 0;
    }
    const fs::path root = argv[1];
    const auto layout = vfs::read_layout(root / L"Data" / L"layout.toc");
    const CasStore store(root / L"Data", fs::temp_directory_path() / L"reskate-item-lists-unused", layout.root);
    const auto game = read_game(root, store);
    const fs::path keep = argc > 2 ? fs::path(argv[2]) : fs::path{};
    same_name_from_two_mods(game, store, argc > 2 ? &keep : nullptr);
    different_names_from_two_mods(game, store);
    one_mod_changes_the_list(game, store, false);
    one_mod_changes_the_list(game, store, true);
    an_item_two_mods_both_list(game, store);
    values_two_mods_changed(game, store, false);
    values_two_mods_changed(game, store, true);
    a_change_that_does_not_combine(game, store);
    the_list_other_copies_are_given(game, store);
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "item list merge: ok (" << game.listName << ", " << game.listed << " item(s) in the game)\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
