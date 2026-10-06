// Assets a mod ADDS to a bundle (new songs, say) while another mod ships its own copy of that
// bundle (a map does, for its levels). The merged bundle is built from one mod's copy, so
// without the carrying step the added assets are missing from it whenever the map's copy wins.
//
// The fixture is a small made-up game and two mods in a temp folder, written with the repo's
// own writers, so it needs no game files:
//   game   bundle "win32/test/shared": test/playlist, test/other
//          bundle "win32/test/second": test/playlist again, test/extra
//   map    a level mod: its own level superbundle (a new TOC) with a copy of that bundle, as a
//          custom map carries the core assets for its level; it edits nothing
//   music  an asset mod that changes test/playlist to name a new song, and adds the song, the
//          wave the song names, the wave's sound-bank resource, and an asset nothing names.
#include "Engine/Resource/binary_bundle.h"
#include "Engine/Resource/cas_codec.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Resource/toc.h"
#include "Engine/Vfs/mod_catalog.h"
#include "Engine/Vfs/native_db.h"

#include <Windows.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace fb = dingosdk::frostbite;
namespace db = dingosdk::native_db;
using namespace dingosdk;

namespace {
using Bytes = std::vector<std::byte>;
constexpr std::uint32_t package = 0x1234;
constexpr char bundle_name[] = "win32/test/shared";
constexpr char second_bundle[] = "win32/test/second";

int failures = 0;
void expect(bool ok, const std::string& what) {
    if (ok) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}

// ---- Bytes ---------------------------------------------------------------------------------------
void put32(Bytes& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::byte>(value >> shift));
}
void put_bytes(Bytes& out, std::string_view text) {
    for (const char c : text) out.push_back(static_cast<std::byte>(c));
}
void put_guid(Bytes& out, const fb::Guid& guid) { out.insert(out.end(), guid.bytes.begin(), guid.bytes.end()); }
fb::Guid guid(std::uint8_t seed) {
    fb::Guid value;
    value.bytes.fill(static_cast<std::byte>(seed));
    return value;
}
void write(const fs::path& path, std::span<const std::byte> bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}
Bytes read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    Bytes bytes(static_cast<std::size_t>(in.tellg()));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

// ---- An EBX document with no instances: a file GUID and the files it imports. -----------------
// That is all the merge reads from an asset to follow what it refers to.
void chunk(Bytes& out, const char (&id)[5], const Bytes& body) {
    put_bytes(out, std::string_view(id, 4));
    put32(out, static_cast<std::uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    if (body.size() & 1) out.push_back(std::byte{0});
}
Bytes ebx_document(const fb::Guid& file, const std::vector<fb::Guid>& imports) {
    Bytes fixup;
    put_guid(fixup, file);
    for (int i = 0; i < 6; ++i) put32(fixup, 0);   // types, signatures, exported, instances, pointers, resource refs
    put32(fixup, static_cast<std::uint32_t>(imports.size()));
    for (const auto& import : imports) {
        put_guid(fixup, import);
        put_guid(fixup, guid(0xEE));               // the imported class
    }
    for (int i = 0; i < 2; ++i) put32(fixup, 0);   // import relocations, type-info references
    for (int i = 0; i < 3; ++i) put32(fixup, 0);   // array, boxed-value and string sections
    const Bytes data(16);
    Bytes extra;
    put32(extra, 0);                               // arrays
    put32(extra, 0);                               // boxed values
    Bytes reflection;
    for (int i = 0; i < 5; ++i) put32(reflection, 0);   // signatures, types, fields, groups, mappings
    Bytes body;
    put_bytes(body, std::string_view("EBX\0", 4));
    chunk(body, "EFIX", fixup);
    chunk(body, "EBXD", data);
    chunk(body, "EBXX", extra);
    chunk(body, "REFL", reflection);
    Bytes riff;
    put_bytes(riff, "RIFF");
    put32(riff, static_cast<std::uint32_t>(body.size()));
    riff.insert(riff.end(), body.begin(), body.end());
    return riff;
}

// ---- The fixture's files -----------------------------------------------------------------------
// layout.toc with one install chunk, `package`, whose archives live in Win32/pkg.
Bytes layout_toc(const std::vector<std::string>& extra = {}) {
    const auto leaf = [](unsigned type, const char* name, std::vector<unsigned char> payload) {
        db::Node node;
        node.type = type;
        node.named = true;
        node.name = name;
        node.owned = std::move(payload);
        return node;
    };
    const auto container = [](unsigned type, const char* name, std::vector<db::Node> children) {
        db::Node node;
        node.type = type;
        node.named = *name != '\0';
        node.name = name;
        node.terminated = true;
        node.children = std::move(children);
        return node;
    };
    const auto records = [](const std::vector<std::string>& names) {
        std::vector<db::Node> rows;
        for (const auto& name : names) rows.push_back(db::make_named_record("name", name));
        return rows;
    };
    const auto low = static_cast<unsigned char>(package & 0xFF), high = static_cast<unsigned char>(package >> 8);
    const auto chunk_entry = container(2, "", {db::make_named_record("name", "pkg").children.front(),
                                              leaf(8, "persistentIndex", {low, high, 0, 0}),
                                              container(1, "superbundles", records(extra))});
    const auto root = container(2, "", {
        container(1, "superBundles", [&] { auto rows = records({"Win32/globals"}); for (auto& row : records(extra)) rows.push_back(std::move(row)); return rows; }()),
        container(2, "installManifest", {container(1, "installChunks", {chunk_entry})}),
        leaf(19, "layeredInstallChunkFiles", {1, 0, low, high, 0, 0, 0, 0})});
    const auto tree = db::write(root);
    Bytes file(db::envelope_size + tree.size());
    std::memcpy(file.data(), db::magic, sizeof(db::magic));
    std::memcpy(file.data() + db::envelope_size, tree.data(), tree.size());
    return file;
}

Bytes encoded(const Bytes& payload) { return fb::encode_cas(payload, {.compression = fb::CasCompression::raw}); }

fb::Sha1 sha(const std::string& name, unsigned version) {
    fb::Sha1 value;
    for (std::size_t i = 0; i < name.size(); ++i) value.bytes[i % 19] ^= static_cast<std::byte>(name[i]);
    value.bytes[19] = static_cast<std::byte>(version);
    return value;
}

struct Ebx { std::string name; unsigned version; Bytes payload; };
struct Resource { std::string name; Bytes payload; };

fb::BinaryBundle manifest_of(const std::vector<Ebx>& assets, const std::vector<Resource>& resources) {
    fb::BinaryBundle manifest;
    for (const auto& asset : assets) {
        fb::BundleAsset entry;
        entry.kind = fb::AssetKind::ebx;
        entry.name = asset.name;
        entry.sha1 = sha(asset.name, asset.version);
        entry.originalSize = asset.payload.size();
        manifest.ebx.push_back(std::move(entry));
    }
    for (const auto& resource : resources) {
        fb::BundleAsset entry;
        entry.kind = fb::AssetKind::resource;
        entry.name = resource.name;
        entry.sha1 = sha(resource.name, 0);
        entry.resourceId = 7;
        entry.resourceType = 0xb2c465f6;
        entry.resourceMeta.assign(16, std::byte{0});
        entry.originalSize = resource.payload.size();
        manifest.resources.push_back(std::move(entry));
    }
    return manifest;
}

struct Fixture {
    fs::path root;
    mods::Catalog catalog;

    explicit Fixture(const std::string& name) {
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        root = fs::path(temp) / ("reskate-merge-added-" + std::to_string(GetCurrentProcessId()) + "-" + name);
        fs::remove_all(root);
        catalog.data_root = root / "game";
        catalog.root = catalog.data_root / "Mods";
        catalog.present = true;
        // The game: two assets in one bundle, and a second bundle that has the playlist as well
        // (the game keeps a list in several bundles), stored in the game's own archive.
        const std::vector<Ebx> assets{{"test/playlist", 0, ebx_document(guid(1), {})}, {"test/other", 0, ebx_document(guid(2), {})}};
        const std::vector<Ebx> second{{"test/playlist", 0, ebx_document(guid(1), {})}, {"test/extra", 0, ebx_document(guid(3), {})}};
        Bytes archive;
        const auto region = [&](const std::vector<Ebx>& list) {
            // The asset list is the region's first file, as it is in every bundle of the game:
            // a bundle one mod alone ships then passes through as that mod built it.
            const auto listing = fb::write_binary_bundle(manifest_of(list, {}));
            std::vector<fb::BundleFileInfo> files{{{false, package, 1}, static_cast<std::uint32_t>(archive.size()),
                                                   static_cast<std::uint32_t>(listing.size())}};
            archive.insert(archive.end(), listing.begin(), listing.end());
            for (const auto& asset : list) {
                const auto payload = encoded(asset.payload);
                files.push_back({{false, package, 1}, static_cast<std::uint32_t>(archive.size()), static_cast<std::uint32_t>(payload.size())});
                archive.insert(archive.end(), payload.begin(), payload.end());
            }
            return fb::write_bundle_region(files);
        };
        const std::vector<fb::TocBundle> bundles{{bundle_name, region(assets), 1}, {second_bundle, region(second), 1}};
        write(catalog.data_root / "Data" / "layout.toc", layout_toc());
        write(catalog.data_root / "Data" / "Win32" / "test_shared.toc", fb::write_patch_toc(bundles));
        write(catalog.data_root / "Data" / "Win32" / "pkg" / "cas_01.cas", archive);
    }
    ~Fixture() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    // A mod with its own copy of a bundle: the manifest first (raw), then every payload.
    void add(const std::string& name, bool levels, const std::string& toc, const std::vector<std::string>& superbundles,
             const std::vector<Ebx>& assets, const std::vector<Resource>& resources = {}, const char* bundle = bundle_name) {
        const auto listing = fb::write_binary_bundle(manifest_of(assets, resources));
        Bytes archive(listing);
        std::vector<fb::BundleFileInfo> files{{{true, package, 1}, 0, static_cast<std::uint32_t>(listing.size())}};
        const auto store = [&](const Bytes& payload) {
            const auto bytes = encoded(payload);
            files.push_back({{true, package, 1}, static_cast<std::uint32_t>(archive.size()), static_cast<std::uint32_t>(bytes.size())});
            archive.insert(archive.end(), bytes.begin(), bytes.end());
        };
        for (const auto& asset : assets) store(asset.payload);
        for (const auto& resource : resources) store(resource.payload);
        const std::vector<fb::TocBundle> bundles{{bundle, fb::write_bundle_region(files), 1}};
        const auto directory = catalog.root / name;
        write(directory / "layout.toc", layout_toc(superbundles));
        write(directory / "Win32" / fs::path(toc), fb::write_patch_toc(bundles));
        write(directory / "Win32" / "pkg" / "cas_01.cas", archive);
        mods::Mod mod;
        mod.name = name;
        mod.directory = directory;
        mod.provides_layout = true;
        mod.provides_levels = levels;
        catalog.mods.push_back(std::move(mod));
    }

    // How many files the merged copy of a bundle in `toc` lists, or -1 when the merge left none.
    int merged_files(const std::string& relative, const std::string& name = bundle_name) const {
        const auto toc = catalog.root / mods::generated_folder / "Win32" / fs::path(relative);
        if (!fs::exists(toc)) return -1;
        for (const auto& bundle : fb::read_toc(read(toc)).bundles)
            if (bundle.name == name) return static_cast<int>(fb::read_bundle_region(bundle.region).files.size());
        return -1;
    }
};

// The game's copy of the bundle, as a level mod carries it.
std::vector<Ebx> game_copy() {
    return {{"test/playlist", 0, ebx_document(guid(1), {})}, {"test/other", 0, ebx_document(guid(2), {})}};
}
// The music mod's bundle: the playlist now names the song (a change), the song names its wave,
// and one more asset nothing names.
std::vector<Ebx> music_assets() {
    return {{"test/playlist", 1, ebx_document(guid(1), {guid(10)})},
            {"test/other", 0, ebx_document(guid(2), {})},
            {"test/song", 0, ebx_document(guid(10), {guid(11)})},
            {"test/wave", 0, ebx_document(guid(11), {})},
            {"test/unrelated", 0, ebx_document(guid(12), {})}};
}
std::vector<Resource> music_resources() { return {{"test/wave", {std::byte{1}, std::byte{2}, std::byte{3}}}}; }

bool noted(const mods::MergeReport& report, const std::string& text) {
    for (const auto& note : report.notes)
        if (note == text) return true;
    return false;
}
std::string describe(const mods::MergeReport& report) {
    std::string text = "issue: '" + report.issue + "'";
    for (const auto& note : report.notes) text += "\n  note: " + note;
    for (const auto& [mod, problems] : report.problems)
        for (const auto& problem : problems) text += "\n  " + mod + ": " + problem;
    return text;
}

constexpr char shared_toc[] = "test_shared.toc";
constexpr char map_toc[] = "levels/test/map.toc";
constexpr char map_superbundle[] = "Win32/levels/test/map";

// The level mod alone: what its copy of the bundle holds with nothing carried into it.
int map_only() {
    Fixture fixture("map-only");
    fixture.add("map", true, map_toc, {map_superbundle}, game_copy());
    const auto report = mods::merge_mods(fixture.catalog);
    expect(report.issue.empty() && report.built, "map only: the merge builds the patch\n" + describe(report));
    return fixture.merged_files(map_toc);
}

// The music mod edits the game's TOC; the map's copy of the bundle is in its own TOC, so nothing
// of the music mod reaches it unless the merge carries it. The playlist change always did (it is a
// change to an asset the map's copy has); the song, its wave and the wave's resource are what
// the changed playlist leads to. The asset nothing names must stay out. Either mod may come first.
void carried_into_the_maps_copy(bool map_first, int baseline) {
    const std::string order = map_first ? "map first" : "music first";
    Fixture fixture(map_first ? "map-first" : "music-first");
    const auto add_map = [&] { fixture.add("map", true, map_toc, {map_superbundle}, game_copy()); };
    const auto add_music = [&] { fixture.add("music", false, shared_toc, {}, music_assets(), music_resources()); };
    if (map_first) { add_map(); add_music(); } else { add_music(); add_map(); }
    const auto report = mods::merge_mods(fixture.catalog);
    expect(report.issue.empty() && report.built, order + ": the merge builds the patch\n" + describe(report));
    expect(noted(report, std::string("map: ") + bundle_name + ": 3 asset(s) added by other mods, e.g. test/song"),
           order + ": the map's copy receives them\n" + describe(report));
    const auto files = fixture.merged_files(map_toc);
    expect(files == baseline + 3, order + ": the map's copy gained those three files and not the unnamed asset (" +
           std::to_string(files) + " files, " + std::to_string(baseline) + " without the music mod)");
}

// Another asset mod ships the game's copy of the bundle in the TOC the music mod ships, the way
// cosmetic mods share one. That copy merges with the music mod's bundle, which already has the
// song, so nothing is carried into it (no "added by other mods" note), and the merged bundle is
// exactly the union: the manifest, the music mod's five EBX and its resource.
void copy_in_the_adders_toc_is_left_alone() {
    Fixture fixture("same-toc");
    fixture.add("music", false, shared_toc, {}, music_assets(), music_resources());
    fixture.add("other", false, shared_toc, {}, game_copy());
    const auto report = mods::merge_mods(fixture.catalog);
    expect(report.issue.empty() && report.built, "same TOC: the merge builds the patch\n" + describe(report));
    for (const auto& note : report.notes)
        expect(note.find("added by other mods") == std::string::npos,
               "same TOC: nothing is carried into a copy in the adder's own TOC: " + note);
    const auto expected = static_cast<int>(1 + music_assets().size() + music_resources().size());
    const auto files = fixture.merged_files(shared_toc);
    expect(files == expected, "same TOC: the merged bundle is the union, " + std::to_string(expected) + " files (" +
           std::to_string(files) + " files)");
}
// Two asset mods each add test/song, each named by a change of its own (the rival's is to the
// other asset). Only one can be carried into the map's copy: the higher-priority mod's. When the
// two are different assets the merge says whose the name went to; the same asset added by both
// is no clash and is not reported. The bundle the two mods share keeps one asset per name as
// well, and says so the same way.
void same_name_from_two_mods(bool different) {
    const std::string label = different ? "name clash" : "same asset twice";
    Fixture fixture(different ? "name-clash" : "same-twice");
    const auto song = guid(different ? 20 : 10);
    fixture.add("music", false, shared_toc, {}, music_assets(), music_resources());
    fixture.add("rival", false, shared_toc, {},
                {{"test/playlist", 0, ebx_document(guid(1), {})},
                 {"test/other", 1, ebx_document(guid(2), {song})},
                 {"test/song", different ? 1u : 0u, ebx_document(song, {})}});
    fixture.add("map", true, map_toc, {map_superbundle}, game_copy());
    const auto report = mods::merge_mods(fixture.catalog);
    expect(report.issue.empty() && report.built, label + ": the merge builds the patch\n" + describe(report));
    const bool said = noted(report, "rival: 1 added asset(s) share a name with ones music adds, e.g. test/song; "
                                    "other mods' copies of the bundle get music's");
    expect(said == different, label + (different ? ": the merge says whose asset the name went to\n"
                                                 : ": nothing is reported\n") + describe(report));
    const bool kept = noted(report, std::string("rival: ") + bundle_name + ": 1 added asset(s) share a name with ones "
                                    "music adds, e.g. test/song; the merged bundle keeps music's");
    expect(kept == different, label + (different ? ": the merge says whose asset the shared bundle keeps\n"
                                                 : ": nothing is reported about the shared bundle\n") + describe(report));
    expect(noted(report, std::string("map: ") + bundle_name + ": 3 asset(s) added by other mods, e.g. test/song"),
           label + ": the map's copy still receives the first mod's three\n" + describe(report));
}

// Two music mods both change test/playlist to add their own songs. Both mods' songs,
// waves and resources must be carried into the map's copy of the bundle, not just the first mod's.
void two_music_mods_both_carried_into_maps_copy(int baseline) {
    Fixture fixture("two-music-mods");
    const auto song1 = guid(10), wave1 = guid(11);
    const auto song2 = guid(20), wave2 = guid(21);
    fixture.add("music1", false, shared_toc, {},
                {{"test/playlist", 1, ebx_document(guid(1), {song1})},
                 {"test/other", 0, ebx_document(guid(2), {})},
                 {"test/song1", 0, ebx_document(song1, {wave1})},
                 {"test/wave1", 0, ebx_document(wave1, {})}},
                {{"test/wave1", {std::byte{1}}}});
    fixture.add("music2", false, shared_toc, {},
                {{"test/playlist", 2, ebx_document(guid(1), {song2})},
                 {"test/other", 0, ebx_document(guid(2), {})},
                 {"test/song2", 0, ebx_document(song2, {wave2})},
                 {"test/wave2", 0, ebx_document(wave2, {})}},
                {{"test/wave2", {std::byte{2}}}});
    fixture.add("map", true, map_toc, {map_superbundle}, game_copy());
    const auto report = mods::merge_mods(fixture.catalog);
    expect(report.issue.empty() && report.built, "two music mods: the merge builds the patch\n" + describe(report));
    expect(noted(report, std::string("map: ") + bundle_name + ": 6 asset(s) added by other mods, e.g. test/song1"),
           "two music mods: the map's copy receives all six assets from both mods\n" + describe(report));
    const auto files = fixture.merged_files(map_toc);
    expect(files == baseline + 6, "two music mods: the map's copy gained 6 files (3 per music mod): " +
           std::to_string(files) + " vs " + std::to_string(baseline + 6));
}

// The game keeps the playlist in a second bundle too, as it keeps the music playlist in eight. A
// mod that ships its own copy of that one (it changed something else in it) takes the music mod's
// playlist there. The song, its wave and the wave's resource were added beside the playlist in
// the first bundle; they have to be beside it in this one as well, or the list names a song its
// bundle does not hold. The asset nothing names stays out, and the music mod's own bundle is as
// that mod built it. Either mod may come first.
void carried_into_another_bundle_with_the_list(bool music_first) {
    const std::string order = music_first ? "second bundle, music first" : "second bundle, music last";
    Fixture fixture(music_first ? "second-bundle-music-first" : "second-bundle-music-last");
    const auto add_music = [&] { fixture.add("music", false, shared_toc, {}, music_assets(), music_resources()); };
    const auto add_tweak = [&] {
        fixture.add("tweak", false, shared_toc, {},
                    {{"test/playlist", 0, ebx_document(guid(1), {})}, {"test/extra", 1, ebx_document(guid(3), {})}}, {}, second_bundle);
    };
    if (music_first) { add_music(); add_tweak(); } else { add_tweak(); add_music(); }
    const auto report = mods::merge_mods(fixture.catalog);
    expect(report.issue.empty() && report.built, order + ": the merge builds the patch\n" + describe(report));
    expect(noted(report, std::string("tweak: ") + second_bundle + ": 1 asset(s) take another mod's change, e.g. test/playlist from music"),
           order + ": the copy of the second bundle takes the changed playlist\n" + describe(report));
    expect(noted(report, std::string("tweak: ") + second_bundle + ": 3 asset(s) added by other mods, e.g. test/song"),
           order + ": and with it the song, its wave and the wave's resource\n" + describe(report));
    const auto files = fixture.merged_files(shared_toc, second_bundle);
    expect(files == 1 + 2 + 3, order + ": that copy lists its manifest, its two assets and those three (" + std::to_string(files) + " files)");
    const auto own = fixture.merged_files(shared_toc);
    const auto built = static_cast<int>(1 + music_assets().size() + music_resources().size());
    expect(own == built, order + ": the music mod's own bundle is as it built it (" + std::to_string(own) + " files, " +
           std::to_string(built) + " built)");
}
} // namespace

int main() try {
    const auto baseline = map_only();
    expect(baseline > 0, "map only: the map's copy exists (" + std::to_string(baseline) + ")");
    carried_into_the_maps_copy(true, baseline);
    carried_into_the_maps_copy(false, baseline);
    copy_in_the_adders_toc_is_left_alone();
    same_name_from_two_mods(true);
    same_name_from_two_mods(false);
    two_music_mods_both_carried_into_maps_copy(baseline);
    carried_into_another_bundle_with_the_list(true);
    carried_into_another_bundle_with_the_list(false);
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "mod merge added assets: ok\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
