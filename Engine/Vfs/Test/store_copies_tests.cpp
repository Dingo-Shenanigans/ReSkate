// Mods that add copies of items the game's store sells (Engine/Vfs/mod_store_copies.h).
//
// ReSkate never hands out a store item, and tells one by its key. The mods that got round that
// (2026-10-05) all did it the same way: each store item again under a key of its own, which is
// in no catalogue and so was handed out like any mod's item. The check tells a copy by what it
// is made of, and the merge leaves a mod that adds one out whole, saying only that it could
// not be merged. What it must not do is take a mod's own item for one: mods fit textures of
// their own on the game's meshes, a store item's included, and that is theirs.
//
// Arguments: [<Skate folder> [<folder of mods>...]]. With the game and its content cache (which
// says what the store sells) the check is run on mods made here from the game's own items; the
// mods in each further folder are then checked and listed, to try it on real ones.
#include "Engine/Resource/binary_bundle.h"
#include "Engine/Resource/cas_codec.h"
#include "Engine/Resource/ebx_carry.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Resource/ebx_writer.h"
#include "Engine/Resource/toc.h"
#include "Engine/Vfs/content_cache.h"
#include "Engine/Vfs/content_catalogs.h"
#include "Engine/Vfs/mod_catalog.h"
#include "Engine/Vfs/mod_merge.h"
#include "Engine/Vfs/mod_merge_internal.h"
#include "Engine/Vfs/mod_store_copies.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace fb = dingosdk::frostbite;
using namespace dingosdk;
using namespace dingosdk::mods;
using namespace dingosdk::mods::detail;

namespace {
using Bytes = std::vector<std::byte>;
constexpr char shared_bundle[] = "win32/characters/customization/configs/cas_main_sharedbundle";

int failures = 0;
void check(bool ok, const std::string& what) {
    if (ok) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}

ItemContent made_of(std::string key, std::vector<std::string> looks, std::vector<std::string> shapes, std::string data) {
    return {std::move(key), std::move(looks), std::move(shapes), std::move(data)};
}

// What counts as a copy, on items written out here.
void rules() {
    StoreItems game;
    game.add(made_of("Own_Store_Hoodie", {"asset 3 store_hoodie_ap"}, {"asset 1 hoodie_dmpreset"}, "a"), true);
    game.add(made_of("Own_Free_Hoodie", {"asset 3 free_hoodie_ap"}, {"asset 1 hoodie_dmpreset"}, "b"), false);
    game.add(made_of("Own_Store_Coat", {"asset 3 store_coat_ap"}, {"asset 1 coat_dmpreset"}, "c"), true);
    game.add(made_of("Own_Store_Costume", {}, {"asset 1 costume_dmpreset"}, "d"), true);
    game.add(made_of("Own_Store_Title", {}, {}, ""), true);
    game.add(made_of("Own_Free_Title", {}, {}, ""), false);
    game.add(made_of("Own_Store_Voice", {}, {}, "e"), true);
    game.add(made_of("Own_Store_Frame", {}, {}, "f"), true);
    game.add(made_of("Own_Free_Frame", {}, {}, "f"), false);

    const auto copies = [&](std::vector<std::string> looks, std::vector<std::string> shapes, std::string data = "mod") {
        return game.original(made_of("Own_Mod_Item", std::move(looks), std::move(shapes), std::move(data)));
    };
    check(copies({"asset 3 store_hoodie_ap"}, {"asset 1 hoodie_dmpreset"}) == "Own_Store_Hoodie", "a store item's look is that store item");
    check(copies({"asset 3 junk_ap", "asset 3 store_hoodie_ap"}, {}) == "Own_Store_Hoodie", "and stays it with more beside it");
    check(copies({"asset 3 free_hoodie_ap"}, {"asset 1 hoodie_dmpreset"}).empty(), "a free item's look is free");
    check(copies({"asset 3 mod_ap"}, {"asset 1 coat_dmpreset"}).empty(), "a mod's own look on a store item's shape is the mod's");
    check(copies({}, {"asset 1 coat_dmpreset"}).empty(), "the shape of a store item that has a look is not that item");
    check(copies({}, {"asset 1 costume_dmpreset"}) == "Own_Store_Costume", "a store costume that is only a shape is told by it");
    check(copies({"asset 3 mod_ap"}, {"asset 1 costume_dmpreset"}) == "Own_Store_Costume", "and a look of the mod's own over it is still that costume");
    check(copies({}, {"asset 1 hoodie_dmpreset"}).empty(), "a shape free items use is free");
    {
        // A whole costume the store sells with looks of its own (colourways): its shape is still it.
        auto suit = made_of("Own_Store_Suit", {"asset 20 store_suit_ap"}, {"asset 1 suit_dmpreset"}, "s");
        suit.costume = true;
        game.add(suit, true);
        check(copies({"asset 20 mod_ap"}, {"asset 1 suit_dmpreset"}) == "Own_Store_Suit", "a mod's look on a store costume's mesh is that costume");
        check(copies({"asset 20 mod_ap", "asset 21 mod_two_ap"}, {"asset 1 suit_dmpreset"}) == "Own_Store_Suit", "however many looks it brings");
        auto free_suit = made_of("Own_Free_Suit", {"asset 20 free_suit_ap"}, {"asset 1 free_suit_dmpreset"}, "t");
        free_suit.costume = true;
        game.add(free_suit, false);
        check(copies({"asset 20 mod_ap"}, {"asset 1 free_suit_dmpreset"}).empty(), "on a free costume's it is the mod's");
    }
    check(copies({}, {}, "e") == "Own_Store_Voice", "a store item's data, field for field, is that store item");
    check(copies({}, {}, "f").empty(), "data a free item has as well is free");

    // Costumes by their geometry. A mesh is rows of points here: `count` of them from `first`.
    const auto points = [](float first, std::size_t count) {
        std::vector<std::array<float, 3>> positions;
        for (std::size_t i = 0; i < count; ++i) positions.push_back({first + static_cast<float>(i) * 0.01f, 1.0f, -0.25f});
        return mesh_points(positions);
    };
    // A mesh of the mod's own making: nowhere near any of the game's.
    const auto own = [](std::size_t count) {
        std::vector<std::array<float, 3>> positions;
        for (std::size_t i = 0; i < count; ++i) positions.push_back({static_cast<float>(i) * 0.01f, 7.0f, 3.5f});
        return mesh_points(positions);
    };
    const auto joined = [](std::vector<std::uint64_t> a, const std::vector<std::uint64_t>& b) {
        a.insert(a.end(), b.begin(), b.end());
        std::sort(a.begin(), a.end());
        a.erase(std::unique(a.begin(), a.end()), a.end());
        return a;
    };
    check(mesh_point({1.0f, 2.0f, 3.0f}) == mesh_point({1.0001f, 2.0001f, 2.9999f}) &&
          mesh_point({1.0f, 2.0f, 3.0f}) != mesh_point({1.001f, 2.0f, 3.0f}), "a vertex is told to half a millimetre");
    check(mesh_points({{1, 2, 3}, {1, 2, 3}, {0, 0, 0}}).size() == 2, "a place is counted once");
    StoreMeshes meshes;
    // Three store costumes and a free one; the first two share a body (2000 points) under them.
    const auto body = points(0, 2000), astronaut = points(100, 10000), reaper = points(300, 8000);
    meshes.add("Own_Store_Astronaut", true, joined(astronaut, body));
    meshes.add("Own_Store_Reaper", true, joined(reaper, body));
    meshes.add("Own_Store_Astronaut", true, points(100, 4000));  // a coarser level of detail: the same costume
    meshes.add("Own_Free_Suit", false, points(-200, 9000));
    meshes.add("Own_Store_Robe", true, joined(points(-400, 6000), points(-200, 3000)));  // half of it a free costume's
    check(meshes.original(joined(astronaut, body)) == "Own_Store_Astronaut", "a store costume's mesh is that costume");
    check(meshes.original(astronaut) == "Own_Store_Astronaut", "under any name, and without the body");
    check(meshes.original(points(100, 1200)) == "Own_Store_Astronaut", "a part of it is that costume too");
    check(meshes.original(joined(points(100, 1200), own(30000))) == "Own_Store_Astronaut",
          "and stays it inside a much larger mesh of the mod's own");
    check(meshes.original(joined(points(100, 700), own(1000))) == "Own_Store_Astronaut",
          "a small part is, when it is much of the mod's mesh");
    check(meshes.original(points(100, 1200)) == "Own_Store_Astronaut" && meshes.original(joined(points(100, 1300), own(30000))) == "Own_Store_Astronaut",
          "a tenth of the costume's finest level is enough, whatever its coarser levels add");
    check(meshes.original(joined(points(100, 700), own(30000))).empty(), "a small part of a costume in a large mesh is not");
    check(meshes.original(points(100, 200)).empty() && meshes.original(points(100, 299)).empty(), "nor are a few vertices");
    check(meshes.original(body).empty(), "the body two costumes share is nobody's");
    check(meshes.original(joined(body, own(5000))).empty(), "so a mod's own costume on it is the mod's");
    check(meshes.original(points(-200, 9000)).empty() && meshes.original(points(-200, 3000)).empty(), "a free costume's mesh is free");
    check(meshes.original(points(-400, 6000)) == "Own_Store_Robe", "what only the store costume has of it is still its own");
    check(meshes.original(own(20000)).empty(), "a mesh of the mod's own is the mod's");
    check(meshes.original(joined(points(100, 1500), points(300, 3000))) == "Own_Store_Reaper", "of two it takes from, the one it takes most of");
    check(StoreMeshes{}.original(astronaut).empty(), "nothing is taken when no costume was read");
    check(copies({}, {}, "").empty(), "data with nothing in it says nothing");
    check(copies({}, {}).empty(), "an item of the mod's own is no copy");
}

fb::Guid guid(std::uint8_t seed) {
    fb::Guid value;
    for (std::size_t index = 0; index < value.bytes.size(); ++index)
        value.bytes[index] = static_cast<std::byte>(static_cast<std::uint8_t>(seed + index));
    return value;
}

// The game's shared customization bundle, which holds every item, and two of them to make mods
// from: one the store sells and one it does not, both clothing (a look fitted on a shape).
struct Game {
    fs::path root, base;
    fb::TocBundle bundle;
    Listing listing;
    fb::ebx::Document sold, free;
    std::string soldKey, freeKey;
    // A player-card background the store sells: its picture is another asset, and it is kept
    // outside items/.
    fb::ebx::Document card;
    std::string cardKey;
};
Game read_game(const fs::path& root, const CasStore& store, const StoreItem& sold) {
    Game game{root, root / L"Data"};
    bool found{};
    for (const auto& bundle : fb::read_toc(read_file(game.base / L"Win32" / L"items.toc")).bundles) {
        if (lower(bundle.name) != shared_bundle) continue;
        if (auto listing = list_bundle(store, game.base, game.base, bundle, root)) {
            game.bundle = bundle;
            game.listing = std::move(*listing);
            found = true;
        }
        break;
    }
    if (!found) throw std::runtime_error(std::string("no readable ") + shared_bundle);
    const auto& assets = game.listing.manifest.ebx;
    for (std::size_t index = 0; index < assets.size() && (game.soldKey.empty() || game.freeKey.empty() || game.cardKey.empty()); ++index) {
        const auto name = lower(assets[index].name);
        if (!name.starts_with("items/cust_") && !name.starts_with("thumbnail/pcard/bg/ownable/")) continue;
        auto document = fb::ebx::read_document(read_asset(store, game.base, game.base, game.listing, index, root));
        const auto item = item_content(document);
        if (!item || item->looks.size() != 1) continue;
        const auto store_item = sold(lower(item->key));
        if (store_item && game.cardKey.empty() && item->shapes.empty() && item->looks.front().starts_with("picture ")) {
            game.cardKey = item->key;
            game.card = std::move(document);
            continue;
        }
        if (item->shapes.empty() || !item->looks.front().starts_with("asset 3 ")) continue;
        auto& key = store_item ? game.soldKey : game.freeKey;
        if (!key.empty()) continue;
        key = item->key;
        (store_item ? game.sold : game.free) = std::move(document);
    }
    if (game.soldKey.empty() || game.freeKey.empty() || game.cardKey.empty())
        throw std::runtime_error("the game has no store item, free item or store card background to copy");
    return game;
}

// The item's data, to change in a copy of it.
fb::ebx::Object& item_data(fb::ebx::Document& item) {
    for (auto& field : item.instances.front().object->fields) {
        if (field.name != "ItemData") continue;
        const auto& pointer = std::get<fb::ebx::PointerReference>(field.value.data);
        return *item.instances.at(static_cast<std::size_t>(pointer.index)).object;
    }
    throw std::runtime_error("an item has no ItemData");
}
fb::ebx::Value& field_of(fb::ebx::Object& object, std::string_view name) {
    for (auto& field : object.fields)
        if (field.name == name) return field.value;
    throw std::runtime_error("an item has no " + std::string(name));
}

// One of the game's items under a name and identity of its own: what a cosmetic mod adds.
struct Item {
    std::string name;
    Bytes payload;
};
Item item_from(const fb::ebx::Document& donor, const std::string& name, std::uint8_t seed,
               const std::function<void(fb::ebx::Document&)>& change = {}) {
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
    if (change) change(item);
    return {name, fb::ebx::write_document(item)};
}

// A cosmetic mod's folder: its copy of the shared bundle in items.toc (the game's assets and its
// own items) and one archive holding the manifest and those items.
Mod write_mod(const fs::path& mods, const std::string& name, const CasStore& store, const Game& game,
              std::initializer_list<Item> items) {
    const auto directory = mods / name;
    auto manifest = game.listing.manifest;
    const auto& shipped = game.listing.files;
    std::vector<fb::BundleFileInfo> files(shipped.begin() + static_cast<std::ptrdiff_t>(game.listing.first), shipped.end());
    const auto installChunk = shipped.front().location.installChunk;
    Bytes archive;
    const auto stored = [&](std::span<const std::byte> bytes) {
        const fb::BundleFileInfo file{{true, installChunk, 1}, static_cast<std::uint32_t>(archive.size()),
                                      static_cast<std::uint32_t>(bytes.size())};
        archive.insert(archive.end(), bytes.begin(), bytes.end());
        return file;
    };
    for (const auto& item : items) {
        fb::BundleAsset asset;
        asset.kind = fb::AssetKind::ebx;
        asset.name = item.name;
        asset.sha1 = sha1_of(item.payload);
        asset.originalSize = item.payload.size();
        files.insert(files.begin() + static_cast<std::ptrdiff_t>(manifest.ebx.size()),
                     stored(fb::encode_cas(item.payload, {.compression = fb::CasCompression::raw})));
        manifest.ebx.push_back(std::move(asset));
    }
    files.insert(files.begin(), stored(fb::write_binary_bundle(manifest)));
    const std::vector<fb::TocBundle> bundles{{game.bundle.name, fb::write_bundle_region(files), 1}};
    fs::create_directories(directory / L"Win32" / fs::path(store.directory(installChunk)));
    write_file(directory / L"Win32" / L"items.toc", fb::write_patch_toc(bundles));
    write_file(directory / L"Win32" / fs::path(store.directory(installChunk)) / L"cas_01.cas", archive);
    fs::copy_file(game.base / L"layout.toc", directory / L"layout.toc", fs::copy_options::overwrite_existing);
    Mod mod;
    mod.name = name;
    mod.directory = directory;
    mod.provides_layout = true;
    return mod;
}

std::string describe(const StoreCopies& copies, const std::vector<std::string>& notes) {
    std::string text;
    for (const auto& [copy, original] : copies.items) text += "\n  " + copy + " copies " + original;
    for (const auto& note : notes) text += "\n  note: " + note;
    return text;
}

// Mods made from the game's own items, checked against the game.
void made_mods(const Game& game, const CasStore& store, const StoreItem& sold) {
    const auto root = fs::temp_directory_path() / ("reskate-store-copies-" + std::to_string(GetCurrentProcessId()));
    std::error_code error;
    fs::remove_all(root, error);
    Catalog catalog;
    catalog.data_root = game.root;
    catalog.root = root / "Mods";
    catalog.present = true;

    // The store item under another key, as the mods did it; and again with its colour changed, so
    // it is no longer the same data but still names the store item's appearance.
    const auto plain = item_from(game.sold, "items/reskate_test/own_test_copy", 0x10);
    const auto tinted = item_from(game.sold, "items/reskate_test/own_test_tinted", 0x20, [](fb::ebx::Document& item) {
        auto& colour = std::get<std::shared_ptr<fb::ebx::Object>>(field_of(item_data(item), "BaseColor").data);
        field_of(*colour, "x").data = 0.75;
    });
    // A mod's own item on the store item's shape: every appearance it names is the mod's.
    const auto own = item_from(game.sold, "items/reskate_test/own_test_own_look", 0x30, [](fb::ebx::Document& item) {
        for (auto& entry : std::get<fb::ebx::Value::Array>(field_of(item_data(item), "AssetPaths").data)) {
            auto& path = *std::get<std::shared_ptr<fb::ebx::Object>>(entry.data);
            const auto& kind = field_of(path, "AssetTypeId").data;
            const auto shape = std::holds_alternative<std::int64_t>(kind) ? std::get<std::int64_t>(kind) < 3
                                                                           : std::get<std::uint64_t>(kind) < 3;
            if (!shape) field_of(path, "AssetName").data = std::string("ReSkate_Test_Own_Look_AP");
        }
    });
    // An item the store does not sell, under another key.
    const auto freebie = item_from(game.free, "items/reskate_test/own_test_free_copy", 0x50);
    // A store card background under another key: no preset or texture name, only its picture.
    const auto card = item_from(game.card, "items/reskate_test/own_test_card", 0x60);

    catalog.mods.push_back(write_mod(catalog.root, "honest", store, game, {own, freebie}));
    catalog.mods.push_back(write_mod(catalog.root, "copies", store, game, {plain, tinted, card}));
    catalog.mods.push_back(write_mod(catalog.root, "nothing", store, game, {}));
    std::vector<std::string> notes;
    const auto found = check_store_copies(catalog, sold, &notes);
    const auto said = describe(found, notes);
    const auto store_key = lower(game.soldKey);
    check(found.game_items > 3000, "the game's items were read (" + std::to_string(found.game_items) + ")");
    check(found.items.size() == 3, "three of the five items are copies of a store item" + said);
    check(found.items.contains("own_test_copy") && found.items.at("own_test_copy") == store_key,
          "the store item under another key is a copy of it" + said);
    check(found.items.contains("own_test_tinted") && found.items.at("own_test_tinted") == store_key,
          "and still is with its colour changed" + said);
    check(found.items.contains("own_test_card") && found.items.at("own_test_card") == lower(game.cardKey),
          "a store card background under another key is a copy of it" + said);
    check(!found.items.contains("own_test_own_look"), "a look of the mod's own on the store item's shape is not" + said);
    check(!found.items.contains("own_test_free_copy"), "a copy of an item the store does not sell is not" + said);
    check(found.mods.size() == 1 && found.mods.front().mod == "copies" && found.mods.front().count == 3,
          "only the mod that adds them is named" + said);
    check(notes.empty(), "nothing went unread" + said);
    // With no store at all (no content cache) nothing is anyone's copy.
    check(check_store_copies(catalog, [](const std::string&) { return false; }).items.empty(), "nothing is a copy when the store sells nothing");

    // The merge builds nothing while such a mod is among them, and says whose problem it is, so
    // the caller leaves that mod out; without it the others merge as they always did. The
    // problem is one line that tells the mod's author nothing: no item, no count, no reason.
    const auto told = [](const MergeReport& report) {
        std::string text = "\n  issue: '" + report.issue + "'";
        for (const auto& [mod, list] : report.problems)
            for (const auto& problem : list) text += "\n  " + mod + ": " + problem;
        return text;
    };
    const auto refused = merge_mods(catalog);
    check(refused.issue.empty() && !refused.built, "nothing is built with a mod that copies store items among them" + told(refused));
    check(refused.problems.size() == 1 && refused.problems.contains("copies") &&
          refused.problems.at("copies") == std::vector<std::string>{std::string(store_copies_problem)},
          "and the problem is that mod's alone, and only the one line" + told(refused));
    const auto lower_case = [](std::string text) { return lower(text); };
    for (const auto* word : {"store", "copy", "copies", "item", "own_"})
        check(lower_case(std::string(store_copies_problem)).find(word) == std::string::npos &&
              lower_case(std::string(store_copies_check)).find(word) == std::string::npos,
              std::string("what the mod is told does not say '") + word + "'");
    check(!fs::exists(catalog.root / generated_folder / L"layout.toc", error), "no patch was written for them");
    std::erase_if(catalog.mods, [](const Mod& mod) { return mod.name == "copies"; });
    const auto merged = merge_mods(catalog);
    check(merged.issue.empty() && merged.built && merged.problems.empty(), "the other mods merge without it" + told(merged));
    fs::remove_all(root, error);
}

// Real mods, listed rather than judged.
void installed_mods(const fs::path& game, const fs::path& folder, const StoreItem& sold) {
    Catalog catalog;
    catalog.data_root = game;
    catalog.root = fs::temp_directory_path() / L"reskate-store-copies-unused";
    catalog.present = true;
    std::error_code error;
    for (fs::directory_iterator it(folder, error), end; it != end && !error; it.increment(error)) {
        if (!it->is_directory(error) || it->path().filename().wstring().starts_with(L".") ||
            !fs::exists(it->path() / L"layout.toc", error)) continue;
        Mod mod;
        mod.name = it->path().filename().string();
        mod.directory = it->path();
        mod.provides_layout = true;
        catalog.mods.push_back(std::move(mod));
    }
    std::vector<std::string> notes;
    const auto start = std::chrono::steady_clock::now();
    const auto found = check_store_copies(catalog, sold, &notes);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    std::cout << folder.string() << ": " << catalog.mods.size() << " mod(s) checked in " << ms << " ms against "
              << found.game_items << " of the game's items, " << found.mod_meshes << " mesh level(s) against "
              << found.game_costumes << " of the game's costumes, " << found.items.size() << " copy(ies) of store items in "
              << found.mods.size() << " of them\n";
    for (const auto& source : found.mods)
        std::cout << "  " << source.mod << ": " << source.count << ", e.g. " << source.example << " is " << source.original
                  << (source.by_mesh ? " (by its mesh)" : " (by what its item is fitted on)") << '\n';
    for (const auto& mesh : found.meshes_seen)
        std::cout << "  mesh " << mesh.mod << ": " << mesh.mesh << ", " << mesh.points << " points, " << mesh.closest.shared
                  << " of them only in " << (mesh.closest.key.empty() ? std::string("no store costume") : mesh.closest.key + " (" +
                         std::to_string(mesh.closest.costume_points) + " points)") << '\n';
    for (const auto& note : notes) std::cout << "  note: " << note << '\n';
}
} // namespace

int main(int argc, char** argv) try {
    rules();
    {
        // No mods: nothing to read, the game's own files included.
        std::vector<std::string> notes;
        check(check_store_copies(Catalog{}, [](const std::string&) { return true; }, &notes).items.empty() && notes.empty(),
              "no mods, no copies and nothing to say");
    }
    std::error_code error;
    if (argc < 2 || !fs::exists(fs::path(argv[1]) / L"Data" / L"Win32" / L"items.toc", error)) {
        std::cout << "No game given; the check was not run on mods.\n";
    } else if (!content_cache::installed()) {
        std::cout << "No content cache, so nothing says what the store sells; the check was not run on mods.\n";
    } else {
        const auto catalogs = content_cache::read_catalogs(content_cache::directory());
        const StoreItem sold = [&](const std::string& key) { return catalogs.reserved(key); };
        const fs::path root = argv[1];
        const auto layout = vfs::read_layout(root / L"Data" / L"layout.toc");
        const CasStore store(root / L"Data", fs::temp_directory_path() / L"reskate-store-copies-unused", layout.root);
        const auto game = read_game(root, store, sold);
        made_mods(game, store, sold);
        std::cout << "store copies: made from " << game.soldKey << " and " << game.cardKey << " (sold) and "
                  << game.freeKey << " (free)\n";
        for (int index = 2; index < argc; ++index) installed_mods(root, argv[index], sold);
    }
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "store copies: ok\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
