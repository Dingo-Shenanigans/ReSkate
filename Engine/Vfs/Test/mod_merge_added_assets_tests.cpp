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
#include "Engine/Resource/shader_lookup.h"
#include "Engine/Vfs/game_archives.h"
#include "Engine/Vfs/mod_catalog.h"
#include "Engine/Vfs/mod_merge_internal.h"
#include "Engine/Vfs/variant_additions_native.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Vfs/native_db.h"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <span>
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

// ---- Chunk metadata: one record a chunk, whose it is and a texture's first mip ---------------------
struct Record {
    std::uint64_t owner{};
    std::uint32_t firstMip{};
    bool operator==(const Record&) const = default;
};
struct Chunk { fb::Guid guid; Bytes payload; Record record; };
db::Node number(unsigned type, const char* name, std::uint64_t value, std::size_t size) {
    db::Node node;
    node.type = type;
    node.named = true;
    node.name = name;
    for (std::size_t byte = 0; byte < size; ++byte) node.owned.push_back(static_cast<unsigned char>(value >> (8 * byte)));
    return node;
}
// The list as the game writes it: the chunks' records in the order of their guids.
Bytes chunk_list(std::vector<Chunk> chunks) {
    std::ranges::sort(chunks, {}, &Chunk::guid);
    db::Node list;
    list.type = 1;
    list.named = true;
    list.name = "chunkMeta";
    list.terminated = true;
    for (const auto& chunk : chunks) {
        db::Node meta;
        meta.type = 2;
        meta.named = true;
        meta.name = "meta";
        meta.terminated = true;
        meta.children.push_back(number(8, "firstMip", chunk.record.firstMip, 4));
        db::Node row;
        row.type = 2;
        row.terminated = true;
        row.children.push_back(number(9, "h64", chunk.record.owner, 8));
        row.children.push_back(std::move(meta));
        list.children.push_back(std::move(row));
    }
    const auto written = db::write(list);
    const auto* bytes = reinterpret_cast<const std::byte*>(written.data());
    return {bytes, bytes + written.size()};
}
std::vector<Record> records_in(std::span<const std::byte> list) {
    const std::span<const unsigned char> bytes(reinterpret_cast<const unsigned char*>(list.data()), list.size());
    std::vector<Record> records;
    for (const auto& row : db::read(bytes, "chunk metadata", nullptr, {.unique_fields = false, .max_entries = bytes.size()}).children) {
        Record record;
        if (const auto* owner = row.field("h64"); owner && owner->payload().size() == 8) std::memcpy(&record.owner, owner->payload().data(), 8);
        if (const auto* meta = row.field("meta"))
            if (const auto* mip = meta->field("firstMip"); mip && mip->payload().size() == 4) std::memcpy(&record.firstMip, mip->payload().data(), 4);
        records.push_back(record);
    }
    return records;
}

fb::BinaryBundle manifest_of(const std::vector<Ebx>& assets, const std::vector<Resource>& resources, const std::vector<Chunk>& chunks = {}) {
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
    for (const auto& chunk : chunks) {
        fb::BundleAsset entry;
        entry.kind = fb::AssetKind::chunk;
        entry.guid = chunk.guid;
        entry.sha1 = sha(chunk.guid.string(), 0);
        entry.originalSize = chunk.payload.size();
        entry.logicalSize = static_cast<std::uint32_t>(chunk.payload.size());
        manifest.chunks.push_back(std::move(entry));
    }
    if (!chunks.empty()) manifest.chunkMetadata = chunk_list(chunks);
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

    // The game's copy of the bundle in another of its TOCs (a level's), in the game's archive.
    void game(const std::string& toc, const std::vector<Ebx>& assets, const std::vector<Chunk>& chunks) {
        const auto path = catalog.data_root / "Data" / "Win32" / "pkg" / "cas_01.cas";
        auto archive = read(path);
        std::vector<fb::BundleFileInfo> files;
        const auto store = [&](const Bytes& bytes) {
            files.push_back({{false, package, 1}, static_cast<std::uint32_t>(archive.size()), static_cast<std::uint32_t>(bytes.size())});
            archive.insert(archive.end(), bytes.begin(), bytes.end());
        };
        store(fb::write_binary_bundle(manifest_of(assets, {}, chunks)));
        for (const auto& asset : assets) store(encoded(asset.payload));
        for (const auto& chunk : chunks) store(encoded(chunk.payload));
        write(path, archive);
        const std::vector<fb::TocBundle> bundles{{bundle_name, fb::write_bundle_region(files), 1}};
        write(catalog.data_root / "Data" / "Win32" / fs::path(toc), fb::write_patch_toc(bundles));
    }

    // A mod with its own copy of a bundle: the manifest first (raw), then every payload.
    void add(const std::string& name, bool levels, const std::string& toc, const std::vector<std::string>& superbundles,
             const std::vector<Ebx>& assets, const std::vector<Resource>& resources = {}, const char* bundle = bundle_name,
             const std::vector<Chunk>& chunks = {}) {
        const auto listing = fb::write_binary_bundle(manifest_of(assets, resources, chunks));
        Bytes archive(listing);
        std::vector<fb::BundleFileInfo> files{{{true, package, 1}, 0, static_cast<std::uint32_t>(listing.size())}};
        const auto store = [&](const Bytes& payload) {
            const auto bytes = encoded(payload);
            files.push_back({{true, package, 1}, static_cast<std::uint32_t>(archive.size()), static_cast<std::uint32_t>(bytes.size())});
            archive.insert(archive.end(), bytes.begin(), bytes.end());
        };
        for (const auto& asset : assets) store(asset.payload);
        for (const auto& resource : resources) store(resource.payload);
        for (const auto& chunk : chunks) store(chunk.payload);
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
constexpr char second_map_toc[] = "levels/test/map2.toc";
constexpr char second_map_superbundle[] = "Win32/levels/test/map2";

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

// A mod's resident list (reskate-plugin.json, read from its folder as the launcher reads it) keeps
// an asset nothing names, one its plugin picks at run time, with its additions: the map's copy of the
// bundle gets it too. Without the list it stays out (carried_into_the_maps_copy).
void resident_assets_go_with_the_additions(int baseline) {
    Fixture fixture("resident");
    fixture.add("map", true, map_toc, {map_superbundle}, game_copy());
    fixture.add("music", false, shared_toc, {}, music_assets(), music_resources());
    const std::string manifest = std::string(R"({"api": 1, "entry": "plugin/main.lua", "resident": [{"bundle": ")") + bundle_name +
                                 R"(", "roots": ["Test/Unrelated", "ui/menus/"]}]})";
    const auto bytes = std::as_bytes(std::span(manifest));
    write(fixture.catalog.root / "music" / mods::plugin_manifest_file, bytes);
    std::vector<std::string> notes;
    const auto described = mods::describe_mod(fixture.catalog.root / "music", "music", notes);
    expect(notes.empty() && described.resident.size() == 1 && mods::resident_root(described, "WIN32/test/shared", "test/unrelated") &&
               mods::resident_root(described, bundle_name, "ui/menus/a/b") && !mods::resident_root(described, bundle_name, "test/other") &&
               !mods::resident_root(described, second_bundle, "test/unrelated"),
           "resident: the list is read, by bundle, a root ending in / a prefix");
    const std::string broken = R"({"api": 1, "entry": "plugin/main.lua", "resident": [{"roots": []}]})";
    write(fixture.catalog.root / "music" / mods::plugin_manifest_file, std::as_bytes(std::span(broken)));
    notes.clear();
    expect(mods::describe_mod(fixture.catalog.root / "music", "music", notes).resident.empty() && notes.size() == 1,
           "resident: a list without its bundle is a note, and keeps nothing");
    write(fixture.catalog.root / "music" / mods::plugin_manifest_file, bytes);
    fixture.catalog.mods.back().resident = described.resident;
    const auto report = mods::merge_mods(fixture.catalog);
    expect(report.issue.empty() && report.built, "resident: the merge builds the patch\n" + describe(report));
    const auto files = fixture.merged_files(map_toc);
    expect(files == baseline + 4, "resident: the map's copy gained the three named files and the resident one (" + std::to_string(files) +
           " files, " + std::to_string(baseline) + " without the music mod)\n" + describe(report));
}

// The files under a folder, by path inside it, with their bytes.
std::map<std::string, Bytes> snapshot(const fs::path& folder) {
    std::map<std::string, Bytes> files;
    for (const auto& entry : fs::recursive_directory_iterator(folder))
        if (entry.is_regular_file()) files[entry.path().lexically_relative(folder).generic_string()] = read(entry.path());
    return files;
}

// The music mod with a resident list, its Win32 folder a junction to a store elsewhere (a test
// root linking a mod's folders to an install's did this). Its files are where the folder shows
// them, Win32/...: the paths fs::relative gives through the link (../../../store/...) made the
// merge take the mod's own TOC for the game's and write the merged TOCs and archive links over
// the mod's real files, so the next merge read its bundles from the wrong archives and left the
// whole mod out ("Raw CAS block size does not match its header", 2026-10-20).
void linked_win32_stays_in_the_patch(int baseline) {
    Fixture fixture("linked");
    fixture.add("map", true, map_toc, {map_superbundle}, game_copy());
    fixture.add("music", false, shared_toc, {}, music_assets(), music_resources());
    const auto directory = fixture.catalog.root / "music";
    const auto store = fixture.root / "store" / "music-Win32";
    fs::create_directories(store.parent_path());
    fs::rename(directory / "Win32", store);
    const auto link = directory / "Win32";
    const bool linked = _wsystem((L"mklink /J \"" + link.wstring() + L"\" \"" + store.wstring() + L"\" >nul").c_str()) == 0;
    expect(linked, "linked: a junction could be made for the check");
    if (!linked) return;
    const std::string manifest = std::string(R"({"api": 1, "entry": "plugin/main.lua", "resident": [{"bundle": ")") + bundle_name +
                                 R"(", "roots": ["test/unrelated"]}]})";
    write(directory / mods::plugin_manifest_file, std::as_bytes(std::span(manifest)));
    std::vector<std::string> notes;
    fixture.catalog.mods.back().resident = mods::describe_mod(directory, "music", notes).resident;
    const auto files = mods::detail::scan(directory);
    expect(files.tocs == std::vector<std::string>{"Win32/test_shared.toc"} &&
               files.archives == std::vector<std::string>{"Win32/pkg/cas_01.cas"},
           "linked: the mod's files are found as Win32/..., not through the link (" +
               (files.tocs.empty() ? std::string("none") : files.tocs.front()) + ")");
    const auto before = snapshot(store);
    for (const auto pass : {"first", "second"}) {
        const auto report = mods::merge_mods(fixture.catalog);
        expect(report.issue.empty() && report.built && report.problems.empty(),
               std::string("linked: the ") + pass + " merge builds the patch with no problem\n" + describe(report));
        expect(fixture.merged_files(map_toc) == baseline + 4,
               std::string("linked: after the ") + pass + " merge the map's copy has the resident asset too\n" + describe(report));
        expect(snapshot(store) == before, std::string("linked: the ") + pass + " merge leaves the mod's own files as they were");
    }
    RemoveDirectoryW(link.c_str());
    // Whatever names a file, nothing the merge writes leaves its output folder.
    const auto refused = [](const fs::path& relative) {
        try { mods::detail::inside("C:/patch", relative); } catch (const std::exception&) { return true; }
        return false;
    };
    expect(mods::detail::inside("C:/patch", "Win32/pkg/./cas_02.cas") == fs::path("C:/patch/Win32/pkg/cas_02.cas") &&
               refused("../../../reskate/mods/x/win32/items.toc") && refused("Win32/../../x.toc") && refused("C:/x.toc") &&
               refused("/x.toc") && refused("D:x.toc") && refused(""),
           "linked: a path outside the output folder is refused");
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

// Two mods edit the playlist and two others carry the game's copy of the bundle untouched. Only
// the copies equal to the game's are left out for combining: every test/other, but neither edit
// of the playlist (the carriers' playlists take the edits, so they are not the game's either).
void copies_equal_to_the_games_are_not_edits() {
    Fixture fixture("untouched-copies");
    const std::vector<Ebx> music1{{"test/playlist", 1, ebx_document(guid(1), {guid(10)})},
                                  {"test/other", 0, ebx_document(guid(2), {})},
                                  {"test/song1", 0, ebx_document(guid(10), {})}};
    const std::vector<Ebx> music2{{"test/playlist", 2, ebx_document(guid(1), {guid(20)})},
                                  {"test/other", 0, ebx_document(guid(2), {})},
                                  {"test/song2", 0, ebx_document(guid(20), {})}};
    fixture.add("carrier1", false, shared_toc, {}, game_copy());
    fixture.add("music1", false, shared_toc, {}, music1);
    fixture.add("music2", false, shared_toc, {}, music2);
    fixture.add("carrier2", false, shared_toc, {}, game_copy());
    const auto report = mods::merge_mods(fixture.catalog);
    expect(report.issue.empty() && report.built, "untouched copies: the merge builds the patch\n" + describe(report));
    expect(report.unchangedCopies == 4, "untouched copies: " + std::to_string(report.unchangedCopies) +
                                            " copies equal to the game's were left out, not 4");
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

// A mod replaced while the game runs (a map at another version): the merge reads its new files
// where it puts them now, not where the old ones lay.
void replaced_while_the_game_runs() {
    Fixture fixture("live-replaced");
    fixture.add("music", false, shared_toc, {}, music_assets(), music_resources());
    auto report = mods::merge_mods(fixture.catalog);
    expect(report.built && report.issue.empty(), "replaced: the launch's merge\n" + describe(report));
    const auto noted = [&](std::string_view text) {
        return std::any_of(report.notes.begin(), report.notes.end(), [&](const std::string& note) { return note.find(text) != std::string::npos; });
    };
    // Untouched, it stays where the launch put it.
    report = mods::merge_mods(fixture.catalog, {}, {.live = true});
    expect(report.built && report.issue.empty() && !noted("placed again"), "replaced: an unchanged mod was placed again\n" + describe(report));
    // The same folder with other files in it.
    const auto before = fixture.merged_files(shared_toc);
    fs::remove_all(fixture.catalog.mods.back().directory);
    fixture.catalog.mods.clear();
    auto more = music_assets();
    more.push_back({"test/another", 0, ebx_document(guid(77), {})});
    fixture.add("music", false, shared_toc, {}, more, music_resources());
    report = mods::merge_mods(fixture.catalog, {}, {.live = true});
    expect(report.built && report.issue.empty(), "replaced: the merge with the new files\n" + describe(report));
    expect(noted("placed again") && noted("added while the game runs"), "replaced: a changed mod kept the old one's place\n" + describe(report));
    expect(fixture.merged_files(shared_toc) == before + 1, "replaced: the new file is not in the merged copy (" +
           std::to_string(fixture.merged_files(shared_toc)) + ", " + std::to_string(before) + " before)");
    // And it is not placed a third time.
    report = mods::merge_mods(fixture.catalog, {}, {.live = true});
    expect(report.built && report.issue.empty() && !noted("placed again"), "replaced: placed again without a change\n" + describe(report));
}
// The merged patch, read the way the merge reads it.
struct Patch {
    explicit Patch(const Fixture& fixture)
        : root(fixture.catalog.root / mods::generated_folder), base(fixture.catalog.data_root / "Data"),
          game(fixture.catalog.data_root), layout(vfs::read_layout(root / "layout.toc")), store(base, root, layout.root) {}
    // The bytes a placement names; empty, with a FAIL, when they are not on disk.
    Bytes bytes(const fb::CasIdentifier& location, std::uint32_t offset, std::uint32_t size, const std::string& what) {
        try {
            const auto& from = location.patch ? root : base;
            const auto length = store.archive_size(from, location);
            expect(length && std::uint64_t{offset} + size <= *length,
                   what + " points at byte " + std::to_string(offset) + " of " + store.describe(location) +
                       (length ? ", past its end" : ", which is not there"));
            if (!length || std::uint64_t{offset} + size > *length) return {};
            return store.read(from, location, offset, size);
        } catch (const std::exception& error) {
            expect(false, what + ": " + error.what());
            return {};
        }
    }
    // Where the merged copy of the bundle in `toc` keeps `asset`.
    std::optional<fb::BundleFileInfo> file(const std::string& toc, const std::string& asset) {
        const auto path = root / "Win32" / fs::path(toc);
        if (!fs::exists(path)) return std::nullopt;
        for (const auto& bundle : fb::read_toc(read(path)).bundles) {
            if (bundle.name != bundle_name) continue;
            const auto listing = mods::detail::list_bundle(store, root, base, bundle, game);
            if (!listing) return std::nullopt;
            for (std::size_t index = 0; index < listing->manifest.ebx.size(); ++index)
                if (listing->manifest.ebx[index].name == asset) return listing->files.at(listing->first + index);
        }
        return std::nullopt;
    }
    fs::path root, base, game;
    vfs::Layout layout;
    mods::detail::CasStore store;
};
// How many times `part` is in `whole`.
std::size_t occurrences(const Bytes& whole, const Bytes& part) {
    std::size_t count{};
    for (auto at = whole.begin(); (at = std::search(at, whole.end(), part.begin(), part.end())) != whole.end(); ++at) ++count;
    return count;
}

// Two maps each carry the music mod's song. Both copies read the one payload.
void carried_payload_written_once() {
    Fixture fixture("carried-once");
    fixture.add("music", false, shared_toc, {}, music_assets(), music_resources());
    fixture.add("map", true, map_toc, {map_superbundle}, game_copy());
    fixture.add("map2", true, second_map_toc, {second_map_superbundle}, game_copy());
    const auto report = mods::merge_mods(fixture.catalog);
    expect(report.issue.empty() && report.built, "carried once: the merge builds the patch\n" + describe(report));
    if (!report.built) return;
    Patch patch(fixture);
    const auto first = patch.file(map_toc, "test/song");
    const auto second = patch.file(second_map_toc, "test/song");
    expect(first && second, "carried once: both maps' copies hold test/song");
    if (!first || !second) return;
    expect(first->location == second->location && first->offset == second->offset,
           "carried once: the two copies read different payloads (byte " + std::to_string(first->offset) + " and byte " +
               std::to_string(second->offset) + ")");
    const auto song = encoded(ebx_document(guid(10), {guid(11)}));
    expect(patch.bytes(first->location, first->offset, first->size, "carried once: test/song") == song,
           "carried once: the placement does not hold the song's bytes");
    const auto size = patch.store.archive_size(patch.root, first->location);
    const auto archive = size ? patch.bytes(first->location, 0, static_cast<std::uint32_t>(*size), "carried once: its archive") : Bytes{};
    expect(occurrences(archive, song) == 1,
           "carried once: the song is in its archive " + std::to_string(occurrences(archive, song)) + " times");
}

// Every placement in every merged TOC lies inside an archive that exists, and the merged layout
// declares only real archives.
void every_placement_lands_in_an_archive() {
    Fixture fixture("placements");
    fixture.add("music", false, shared_toc, {}, music_assets(), music_resources());
    // The song's audio as a TOC chunk of the music mod's own, named by the song (its import):
    // it follows the song into the maps' TOCs.
    const Bytes wave{std::byte{7}, std::byte{8}, std::byte{9}, std::byte{10}};
    {
        const auto toc = fixture.catalog.root / "music" / "Win32" / shared_toc;
        const auto archive = fixture.catalog.root / "music" / "Win32" / "pkg" / "cas_01.cas";
        auto document = fb::read_toc(read(toc));
        auto bytes = read(archive);
        const auto location = fb::read_bundle_region(document.bundles.front().region).files.front().location;
        document.chunks.push_back({guid(11), location, static_cast<std::uint32_t>(bytes.size()), static_cast<std::uint32_t>(wave.size())});
        bytes.insert(bytes.end(), wave.begin(), wave.end());
        write(archive, bytes);
        write(toc, fb::write_patch_toc(document.bundles, document.chunks, document.flags));
    }
    fixture.add("map", true, map_toc, {map_superbundle}, game_copy());
    fixture.add("map2", true, second_map_toc, {second_map_superbundle}, game_copy());
    const auto report = mods::merge_mods(fixture.catalog);
    expect(report.issue.empty() && report.built, "placements: the merge builds the patch\n" + describe(report));
    if (!report.built) return;
    Patch patch(fixture);
    std::size_t checked{};
    std::set<std::string> withAudio;
    for (const auto& entry : fs::recursive_directory_iterator(patch.root / "Win32")) {
        if (entry.path().extension() != ".toc") continue;
        const auto document = fb::read_toc(read(entry.path()));
        for (const auto& bundle : document.bundles)
            for (const auto& file : fb::read_bundle_region(bundle.region).files) {
                (void)patch.bytes(file.location, file.offset, file.size, "placements: " + bundle.name);
                ++checked;
            }
        for (const auto& chunk : document.chunks) {
            const auto held = patch.bytes(chunk.location, chunk.offset, chunk.size, "placements: chunk " + chunk.guid.string());
            if (chunk.guid != guid(11)) continue;
            const auto name = fs::relative(entry.path(), patch.root / "Win32").generic_string();
            expect(held == wave, "placements: " + name + "'s chunk does not hold the song's audio");
            if (held == wave) withAudio.insert(name);
        }
    }
    expect(checked > 0, "placements: the merged TOCs hold placements");
    expect(withAudio.contains(map_toc) && withAudio.contains(second_map_toc), "placements: both maps' TOCs carry the song's audio chunk");
    const auto* table = patch.layout.root.field("layeredInstallChunkFiles");
    expect(table != nullptr, "placements: the merged layout declares archives");
    if (table)
        vfs::for_each_install_chunk_file(table->payload(), [&](std::uint32_t, std::uint16_t archive) {
            expect(!mods::detail::CasStore::placeholder(archive), "placements: the layout declares placeholder archive " + std::to_string(archive));
        });
}

// A mod archive numbered like a placeholder takes a free number: its placements land on its bytes,
// and the layout never declares the placeholder.
void a_mod_archive_numbered_like_a_placeholder() {
    Fixture fixture("placeholder-number");
    fixture.add("music", false, shared_toc, {}, music_assets(), music_resources());
    fixture.add("map", true, map_toc, {map_superbundle}, game_copy());
    const auto mod = fixture.catalog.root / "map";
    const auto toc = mod / "Win32" / map_toc;
    auto document = fb::read_toc(read(toc));
    for (auto& bundle : document.bundles) {
        auto region = fb::read_bundle_region(bundle.region);
        for (auto& file : region.files) file.location.archive = mods::detail::CasStore::carried_archive;
        bundle.region = fb::write_bundle_region(region.files, region.inlineManifest);
    }
    write(toc, fb::write_patch_toc(document.bundles, document.chunks, document.flags));
    fs::rename(mod / "Win32" / "pkg" / "cas_01.cas", mod / "Win32" / "pkg" / vfs::archive_file(mods::detail::CasStore::carried_archive));
    const auto report = mods::merge_mods(fixture.catalog);
    expect(report.issue.empty() && report.built, "placeholder number: the merge builds the patch\n" + describe(report));
    if (!report.built) return;
    Patch patch(fixture);
    const auto other = patch.file(map_toc, "test/other");
    expect(other && !mods::detail::CasStore::placeholder(other->location) &&
               patch.bytes(other->location, other->offset, other->size, "placeholder number: test/other") ==
                   encoded(ebx_document(guid(2), {})),
           "placeholder number: the map's own asset does not read its bytes");
    if (const auto* table = patch.layout.root.field("layeredInstallChunkFiles"))
        vfs::for_each_install_chunk_file(table->payload(), [&](std::uint32_t, std::uint16_t archive) {
            expect(!mods::detail::CasStore::placeholder(archive), "placeholder number: the layout declares archive " + std::to_string(archive));
        });
}

// The game keeps the bundle in a level's TOC too, its chunks listed in an order of their own and
// their records by guid. Two map mods ship that level's copy (as SmokeyDawg58's mod and the
// built-in throwdown_menu pack both ship the game's level root bundles), made as Studio makes
// one: the chunks listed by guid, their records in the same order. The music mod's song names a
// texture chunk of its own, which is carried into the maps' copies after their chunks, and the
// list written for those copies has its record at its place by guid. Read back by where each
// chunk is listed, every chunk after it by guid had its neighbour's record once the level was
// merged with the game's copy: the carried texture another chunk's, and the game's own textures
// the first mip of the one before (23 textures in each level root bundle, 2026-10-20).
void carried_chunk_keeps_the_records_in_step() {
    Fixture fixture("carried-chunk-records");
    const Chunk low{guid(0x10), {std::byte{1}}, {0x1010, 1}}, mid{guid(0x30), {std::byte{3}}, {0x3030, 2}},
                high{guid(0x50), {std::byte{5}}, {0x5050, 3}};
    const Chunk texture{guid(0x20), {std::byte{2}, std::byte{2}}, {0x2020, 0}};
    fixture.game(map_toc, game_copy(), {high, low, mid});
    fixture.add("map", true, map_toc, {map_superbundle}, game_copy(), {}, bundle_name, {low, mid, high});
    fixture.add("map2", true, map_toc, {map_superbundle}, game_copy(), {}, bundle_name, {low, mid, high});
    auto assets = music_assets();
    assets[2].payload = ebx_document(guid(10), {guid(11), texture.guid});   // the song names the texture's chunk
    fixture.add("music", false, shared_toc, {}, assets, music_resources(), bundle_name, {texture});
    const auto report = mods::merge_mods(fixture.catalog);
    expect(report.issue.empty() && report.built, "chunk records: the merge builds the patch\n" + describe(report));
    if (!report.built) return;
    Patch patch(fixture);
    std::optional<mods::detail::Listing> listing;
    for (const auto& bundle : fb::read_toc(read(patch.root / "Win32" / fs::path(map_toc))).bundles)
        if (bundle.name == bundle_name) listing = mods::detail::list_bundle(patch.store, patch.root, patch.base, bundle, patch.game);
    expect(listing.has_value(), "chunk records: the map's copy of the bundle can be read");
    if (!listing) return;
    const auto& chunks = listing->manifest.chunks;
    expect(std::ranges::any_of(chunks, [&](const fb::BundleAsset& chunk) { return chunk.guid == texture.guid; }),
           "chunk records: the texture's chunk is carried into the map's copy\n" + describe(report));
    const std::vector<Record> expected{low.record, texture.record, mid.record, high.record};
    const auto records = records_in(listing->manifest.chunkMetadata);
    std::string got;
    for (const auto& record : records) got += " " + std::to_string(record.owner) + "/" + std::to_string(record.firstMip);
    expect(chunks.size() == expected.size() && records == expected,
           "chunk records: each chunk has its own record, with its own first mip, at its place by guid (got" + got + ")");
}

// A merge built again from the same mods writes the same bytes.
void two_merges_write_the_same_bytes() {
    Fixture fixture("same-bytes");
    fixture.add("music", false, shared_toc, {}, music_assets(), music_resources());
    fixture.add("map", true, map_toc, {map_superbundle}, game_copy());
    fixture.add("map2", true, second_map_toc, {second_map_superbundle}, game_copy());
    const auto patch = fixture.catalog.root / mods::generated_folder;
    auto report = mods::merge_mods(fixture.catalog);
    expect(report.issue.empty() && report.built, "same bytes: the first merge\n" + describe(report));
    const auto archive = read(patch / "Win32" / "pkg" / "cas_01.cas");
    const auto toc = read(patch / "Win32" / fs::path(second_map_toc));
    fs::remove(patch / mods::detail::stamp_file);
    report = mods::merge_mods(fixture.catalog);
    expect(report.issue.empty() && report.built && !report.reused, "same bytes: the second merge\n" + describe(report));
    expect(read(patch / "Win32" / "pkg" / "cas_01.cas") == archive, "same bytes: the patch's own archive differs");
    expect(read(patch / "Win32" / fs::path(second_map_toc)) == toc, "same bytes: the second map's TOC differs");
}

void a_manifest_after_the_first_payload_keeps_asset_order() {
    Fixture fixture("manifest-order");
    const auto assets = game_copy();
    fixture.add("native", false, shared_toc, {}, assets);
    const auto root = fixture.catalog.root / "native";
    const auto toc = fb::read_toc(read(root / "Win32" / fs::path(shared_toc)));
    auto bundle = toc.bundles.front();
    auto region = fb::read_bundle_region(bundle.region);
    // [manifest, first, second] becomes [first, manifest, second].
    std::swap(region.files[0], region.files[1]);
    bundle.region = fb::write_bundle_region(region.files);
    const auto layout = vfs::read_layout(root / "layout.toc");
    mods::detail::CasStore store(fixture.catalog.data_root / "Data", fixture.root / "unused", layout.root);
    const auto listing = mods::detail::list_bundle(store, root, fixture.catalog.data_root / "Data", bundle,
                                                  fixture.catalog.data_root);
    expect(listing.has_value(), "a manifest after a payload is found");
    if (!listing) return;
    for (std::size_t index = 0; index < assets.size(); ++index)
        expect(mods::detail::read_asset(store, root, fixture.catalog.data_root / "Data", *listing, index,
                                        fixture.catalog.data_root) == assets[index].payload,
               "finding a non-first manifest preserves each asset's payload, not the manifest bytes");
}

void native_variant_additions_preserve_replacements_and_removals(bool donor_first) {
    Fixture fixture(donor_first ? "cooperative-first" : "cooperative-last");
    auto donor_assets = game_copy();
    donor_assets.push_back({"test/private", 1, ebx_document(guid(10), {})});
    fixture.add("donor", false, shared_toc, {}, donor_assets);
    if (donor_first) {
        const auto path = fixture.catalog.root / "donor" / "Win32" / fs::path(shared_toc);
        auto toc = fb::read_toc(read(path));
        auto region = fb::read_bundle_region(toc.bundles.front().region);
        std::swap(region.files[0], region.files[1]);
        toc.bundles.front().region = fb::write_bundle_region(region.files);
        write(path, fb::write_patch_toc(toc.bundles));
    }
    // The selected variant replaces the playlist and deliberately drops test/other.
    fixture.add("choice", false, shared_toc, {}, {{"test/playlist", 1, ebx_document(guid(1), {guid(1)})}});
    auto& donor = fixture.catalog.mods[0];
    auto& choice = fixture.catalog.mods[1];
    mods::Mod owner;
    owner.name = "Pack"; owner.directory = fixture.catalog.root / "Pack";
    owner.variants = mods::Variants{"decks", {}, {bundle_name}};
    owner.variant_additions.accept = {bundle_name};
    donor.variant_additions.contribute = {bundle_name};
    const auto projection = mods::detail::admit_variant_additions(fixture.catalog.data_root, owner, choice, donor, {});
    expect(projection.at(bundle_name) == std::set<std::string>{"0:test/private"}, "native admission projects exactly the private EBX");
    donor.variant_projection = projection;
    donor.variant_targets[bundle_name] = choice.name;
    choice.variant_authority.insert(bundle_name);
    if (!donor_first) std::swap(fixture.catalog.mods[0], fixture.catalog.mods[1]);
    const auto report = mods::merge_mods(fixture.catalog);
    expect(report.issue.empty() && report.built, "native cooperative merge builds\n" + describe(report));
    Patch patch(fixture);
    expect(!patch.file(shared_toc, "test/other"), "a stock original the selected variant removed is not resurrected");
    const auto private_file = patch.file(shared_toc, "test/private");
    expect(private_file && fb::decode_cas(patch.bytes(private_file->location, private_file->offset, private_file->size, "private addition"),
                                        {fixture.catalog.data_root}) == donor_assets.back().payload,
           "private addition retains its original payload in either priority order");
    const auto playlist = patch.file(shared_toc, "test/playlist");
    expect(playlist.has_value(), "the variant's replacement remains present");
    if (playlist) {
        const auto document = fb::ebx::read_document(fb::decode_cas(patch.bytes(playlist->location, playlist->offset,
            playlist->size, "variant replacement"), {fixture.catalog.data_root}));
        expect(document.imports.size() == 1 && document.imports.front().fileGuid == guid(1),
               "the variant's replacement wins over donor scaffold in either priority order");
    }

    auto without_donor = fixture.catalog;
    std::erase_if(without_donor.mods, [](const auto& m) { return m.name == "donor"; });
    auto retry = mods::merge_mods(without_donor);
    expect(retry.issue.empty() && retry.built, "retry with the donor removed clears stale authority and still merges");
    mods::reconcile_variant_additions(without_donor);
    expect(std::ranges::all_of(without_donor.mods, [](const auto& m) { return m.variant_authority.empty(); }),
           "variant authority needs a surviving donor");
    auto without_variant = fixture.catalog;
    std::erase_if(without_variant.mods, [](const auto& m) { return m.name == "choice"; });
    mods::reconcile_variant_additions(without_variant);
    expect(std::ranges::none_of(without_variant.mods, [](const auto& m) { return m.name == "donor"; }),
           "retry removes a donor when its verified variant was removed");
    retry = mods::merge_mods(without_variant);
    expect(retry.issue.empty() && retry.built, "retry without either member still merges the remaining catalogue");

    // Native readers must detect an added asset hidden behind a new name but the game's GUID.
    auto collision_assets = game_copy();
    collision_assets.push_back({"test/renamed", 1, ebx_document(guid(2), {})});
    fixture.add("collision", false, shared_toc, {}, collision_assets);
    auto& collision = fixture.catalog.mods.back();
    collision.variant_additions.contribute = {bundle_name};
    bool refused{};
    try { (void)mods::detail::admit_variant_additions(fixture.catalog.data_root, owner, fixture.catalog.mods[donor_first ? 1 : 0], collision, {}); }
    catch (const std::exception&) { refused = true; }
    expect(refused, "native renamed partition GUID collision is refused");
    // The donor passed admission, then its TOC became unreadable before a job
    // collected sources. Keep the selected variant usable and blame that donor.
    write(fixture.catalog.root / "donor" / "Win32" / fs::path(shared_toc), Bytes{std::byte{0xff}});
    retry = mods::merge_mods(fixture.catalog);
    expect(retry.issue.empty() && retry.problems.contains("donor"),
           "a TOC lost after admission is a donor problem, not a merge-wide issue");
}

void catalog_checks_native_cooperation_again_when_the_variant_changes() {
    Fixture fixture("catalog-cooperation");
    struct LocalAppData {
        std::wstring previous;
        explicit LocalAppData(const fs::path& root) {
            const auto size = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
            if (size) {
                previous.resize(size);
                const auto copied = GetEnvironmentVariableW(L"LOCALAPPDATA", previous.data(), size);
                previous.resize(copied);
            }
            SetEnvironmentVariableW(L"LOCALAPPDATA", root.wstring().c_str());
        }
        ~LocalAppData() { SetEnvironmentVariableW(L"LOCALAPPDATA", previous.empty() ? nullptr : previous.c_str()); }
    } appdata(fixture.root / "appdata");
    auto added = game_copy();
    added.push_back({"test/private", 1, ebx_document(guid(10), {})});
    fixture.add("donor", false, shared_toc, {}, added);
    fixture.add("Pack/decks/standard", false, shared_toc, {}, game_copy());
    fixture.add("Pack/decks/collision", false, shared_toc, {}, added);
    const auto text = [&](const fs::path& path, std::string_view value) {
        Bytes bytes; put_bytes(bytes, value); write(path, bytes);
    };
    const auto stamp = "ReSkate Studio native Patch v1\nskate_sha256=" + std::string(supported_build::game_sha256) + "\n";
    text(fixture.catalog.root / "donor" / mods::studio_marker_file, stamp);
    text(fixture.catalog.root / "Pack" / mods::studio_marker_file, stamp);
    const auto owner_manifest = fixture.catalog.root / "Pack" / mods::plugin_manifest_file;
    text(owner_manifest, R"({"api":1,"entry":"plugin/main.lua","variants":{"folder":"decks","order":["standard"],"displaces":["win32/test/shared"]},"variant_additions":{"accept":["win32/test/shared"]}})");
    text(fixture.catalog.root / "donor" / mods::plugin_manifest_file,
         R"({"variant_additions":{"contribute":["win32/test/shared"]}})");
    auto catalog = mods::load_catalog(fixture.catalog.data_root, {}, false);
    const auto find = [](const mods::Catalog& c) -> const mods::Mod* {
        const auto at = std::ranges::find_if(c.mods, [](const auto& m) { return m.name == "donor"; });
        return at == c.mods.end() ? nullptr : &*at;
    };
    expect(find(catalog) && find(catalog)->variant_projection.contains(bundle_name), "catalog admits native additions after both author permissions and delivery checks");
    // Change the default choice without writing the real user's variants file.
    text(owner_manifest, R"({"api":1,"entry":"plugin/main.lua","variants":{"folder":"decks","order":["collision"],"displaces":["win32/test/shared"]},"variant_additions":{"accept":["win32/test/shared"]}})");
    catalog = mods::load_catalog(fixture.catalog.data_root, {}, false);
    expect(!find(catalog), "catalog checks the newly selected variant and refuses its identity collision");
    text(owner_manifest, R"({"api":1,"entry":"plugin/main.lua","variants":{"folder":"decks","order":["standard"],"displaces":["win32/test/shared"]}})");
    catalog = mods::load_catalog(fixture.catalog.data_root, {}, false);
    expect(!find(catalog), "catalog retains strict displacement when the variant owner has not opted in");
    text(owner_manifest, R"({"api":1,"entry":"plugin/main.lua","variants":{"folder":"decks","order":["standard"],"displaces":["win32/test/shared"]},"variant_additions":{"accept":["win32/test/shared"]}})");
    auto rival = game_copy();
    rival.push_back({"test/renamed-private", 1, ebx_document(guid(10), {})});
    fixture.add("rival", false, shared_toc, {}, rival);
    text(fixture.catalog.root / "rival" / mods::studio_marker_file, stamp);
    text(fixture.catalog.root / "rival" / mods::plugin_manifest_file,
         R"({"variant_additions":{"contribute":["win32/test/shared"]}})");
    for (const auto* order : {
        R"({"schema":1,"mods":[{"name":"donor","enabled":true},{"name":"rival","enabled":true}]})",
        R"({"schema":1,"mods":[{"name":"rival","enabled":true},{"name":"donor","enabled":true}]})"}) {
        text(fixture.catalog.root / "mods.json", order);
        catalog = mods::load_catalog(fixture.catalog.data_root, {}, false);
        expect(!find(catalog) && std::ranges::none_of(catalog.mods, [](const auto& m) { return m.name == "rival"; }),
               "two private contributors reusing a GUID are both refused in either priority order");
    }
}
// `second_custom`: a higher-priority custom pack too, with its own bytecode, so both custom graphs can
// be kept; the higher one's must be.
void shader_selection_checks_the_winner(bool delivered, bool changed_heap, bool alias_first, bool overwritten = false,
                                       bool encoder_available = true, bool second_custom = false) {
    Fixture fixture("shader-selection-" + std::to_string(delivered) + std::to_string(changed_heap) +
                    std::to_string(alias_first) + std::to_string(overwritten) + std::to_string(second_custom));
    const auto table = [](bool extended) {
        Resource resource{"test/program_lookup", Bytes(extended ? 544 : 208)};
        const std::uint32_t count = extended ? 2 : 1;
        const std::uint64_t start = extended ? 208 : 16;
        std::memcpy(resource.payload.data(), &count, 4);
        std::memcpy(resource.payload.data() + 4, &start, 8);
        resource.payload[192] = std::byte{7};
        put32(resource.payload, 4); // the header pointer's relocation
        return resource;
    };
    const auto rows = [](Resource resource, const std::map<std::uint64_t, std::uint32_t>& values) {
        put32(resource.payload, static_cast<std::uint32_t>(values.size()));
        for (const auto& [key, index] : values) {
            for (int shift = 0; shift < 64; shift += 8) resource.payload.push_back(static_cast<std::byte>(key >> shift));
            put32(resource.payload, 1); put32(resource.payload, index);
        }
        return resource;
    };
    auto stock = rows(table(false), {{1, 0}});
    auto aliases = rows(table(false), {{1, 0}, {2, 0}});
    auto custom = rows(table(true), {{1, 0}, {2, 0}, {5, 0}, {9, 1}, {99, 2}});
    if (changed_heap) custom.payload[192] = std::byte{8};
    auto higher = rows(table(true), {{1, 0}, {2, 0}, {11, 1}});
    higher.payload[400] = std::byte{3}; // its own second program: another graph than the lower pack's
    const auto write_layer = [&](const fs::path& root, bool patch, const Resource& programs, bool code, unsigned code_version,
                                 const char* code_name = "shaders/bytecode/test", std::uint32_t code_id = 43) {
        fs::create_directories(root / "Win32" / "pkg");
        write(root / "layout.toc", layout_toc());
        fb::BinaryBundle manifest;
        fb::BundleAsset asset;
        asset.kind = fb::AssetKind::resource; asset.name = programs.name; asset.resourceId = 42;
        asset.resourceType = fb::shader::programLookupType;
        asset.sha1 = mods::detail::sha1_of(programs.payload); asset.originalSize = programs.payload.size();
        const auto size = programs.payload.size() > 544 ? 544U : 208U;
        put32(asset.resourceMeta, size); put32(asset.resourceMeta, 4);
        put32(asset.resourceMeta, static_cast<std::uint32_t>(programs.payload.size() - size - 4)); put32(asset.resourceMeta, 0);
        manifest.resources.push_back(asset);
        const Bytes bytecode{static_cast<std::byte>(code_version)};
        if (code) {
            asset.name = code_name; asset.resourceId = code_id; asset.resourceType = fb::shader::bytecodeType;
            asset.sha1 = mods::detail::sha1_of(bytecode); asset.originalSize = bytecode.size(); asset.resourceMeta.assign(16, std::byte{0});
            manifest.resources.push_back(asset);
        }
        auto archive = fb::write_binary_bundle(manifest);
        std::vector<fb::BundleFileInfo> files{{{patch, package, 1}, 0, static_cast<std::uint32_t>(archive.size())}};
        const auto append_payload = [&](const Bytes& payload) {
            const auto bytes = encoded(payload);
            files.push_back({{patch, package, 1}, static_cast<std::uint32_t>(archive.size()), static_cast<std::uint32_t>(bytes.size())});
            archive.insert(archive.end(), bytes.begin(), bytes.end());
        };
        append_payload(programs.payload);
        if (code) append_payload(bytecode);
        write(root / "Win32" / "pkg" / "cas_01.cas", archive);
        const std::vector<fb::TocBundle> bundles{{bundle_name, fb::write_bundle_region(files), 1}};
        write(root / "Win32" / fs::path(shared_toc), fb::write_patch_toc(bundles));
    };
    write_layer(fixture.catalog.data_root / "Data", false, stock, false, 0);
    write_layer(fixture.catalog.root / "custom", true, custom, delivered, 1);
    write_layer(fixture.catalog.root / "aliases", true, aliases, overwritten, 2);
    if (second_custom) write_layer(fixture.catalog.root / "higher", true, higher, true, 3, "shaders/bytecode/higher", 44);
    // The first mod listed has the highest priority.
    auto order = alias_first ? std::vector<const char*>{"aliases", "custom"} : std::vector<const char*>{"custom", "aliases"};
    if (second_custom) order = {"aliases", "higher", "custom"};
    for (const auto* name : order) {
        mods::Mod mod; mod.name = name; mod.directory = fixture.catalog.root / name; mod.provides_layout = true;
        fixture.catalog.mods.push_back(std::move(mod));
    }
    const auto report = mods::merge_mods(fixture.catalog);
    expect(report.issue.empty() && report.built, "shader graph selection builds\n" + describe(report));
    Patch patch(fixture);
    const auto toc = fb::read_toc(read(patch.root / "Win32" / fs::path(shared_toc)));
    const auto listing = mods::detail::list_bundle(patch.store, patch.root, patch.base, toc.bundles.front(), patch.game);
    expect(listing && !listing->manifest.resources.empty(), "shader result has its native resource manifest");
    if (!listing || listing->manifest.resources.empty()) return;
    const auto program = std::find_if(listing->manifest.resources.begin(), listing->manifest.resources.end(),
        [](const auto& resource) { return resource.resourceType == fb::shader::programLookupType; });
    expect(program != listing->manifest.resources.end(), "program lookup resource survives");
    if (program == listing->manifest.resources.end()) return;
    const auto program_index = listing->manifest.ebx.size() + static_cast<std::size_t>(program - listing->manifest.resources.begin());
    const auto payload = mods::detail::read_asset(patch.store, patch.root, patch.base, *listing, program_index, patch.game);
    if (payload.size() < 12) { expect(false, "program lookup payload is complete"); return; }
    if (!encoder_available) {
        // This case starts before any codec has been loaded. A failed rebuild
        // must retain the selected alias payload AND its matching metadata.
        expect(payload == aliases.payload, "failed encoding keeps the selected payload");
        std::uint32_t kept_size{}; std::memcpy(&kept_size, program->resourceMeta.data(), 4);
        expect(kept_size == 208, "failed encoding does not publish replacement graph metadata");
        expect(std::any_of(report.notes.begin(), report.notes.end(), [](const auto& note) {
            return note.find("edits could not be combined") != std::string::npos;
        }), "missing encoder exercises the failed-rebuild path");
        return;
    }
    std::uint32_t count{}; std::memcpy(&count, payload.data(), 4);
    const bool private_graph = delivered && !changed_heap && !overwritten;
    expect(count == (private_graph ? 2U : 1U), "winning and promoted custom graphs need both stock preservation and retained bundle-owned bytecode");
    const auto& meta = program->resourceMeta;
    std::uint32_t size{}, fixups{}, number{};
    std::memcpy(&size, meta.data(), 4); std::memcpy(&fixups, meta.data() + 4, 4);
    std::size_t at = static_cast<std::size_t>(size) + fixups;
    if (at + 4 > payload.size()) { expect(false, "program lookup section is bounded"); return; }
    std::memcpy(&number, payload.data() + at, 4); at += 4;
    std::set<std::uint64_t> keys;
    for (std::uint32_t row = 0; row < number; ++row) {
        if (at + 12 > payload.size()) { expect(false, "program row header is bounded"); return; }
        std::uint64_t key{}; std::uint32_t values{};
        std::memcpy(&key, payload.data() + at, 8); std::memcpy(&values, payload.data() + at + 8, 4); at += 12;
        keys.insert(key);
        for (std::uint32_t value = 0; value < values; ++value) {
            if (at + 4 > payload.size()) { expect(false, "program row indices are bounded"); return; }
            std::uint32_t index{}; std::memcpy(&index, payload.data() + at, 4); at += 4;
            expect(index < count, "native output rows stay in the retained graph's range");
        }
    }
    if (second_custom) {
        expect(count == 2 && keys.contains(11) && !keys.contains(9),
               "of two usable custom graphs under an alias-only winner, the higher-priority one is kept");
        return;
    }
    expect(keys.contains(2) && !keys.contains(99), "safe stock aliases survive and invalid winning rows are actually written out");
    expect(keys.contains(9) == private_graph, "unproved graphs cannot leak private program indices into stock");
    expect(keys.contains(5) == !changed_heap, "a non-retained custom contributor still contributes a proven stock alias");
}
} // namespace

int main(int argc, char** argv) try {
    // Rebuilt native resources use the game's Oodle encoder. The normal suite
    // needs no game files; supplying a game root enables these archive checks.
    if (argc > 1 && *argv[1]) {
        (void)fb::encode_cas(Bytes{std::byte{1}}, {fs::path(argv[1])});
        shader_selection_checks_the_winner(true, false, false);
        shader_selection_checks_the_winner(true, false, true);
        shader_selection_checks_the_winner(false, false, false);
        shader_selection_checks_the_winner(true, true, false);
        shader_selection_checks_the_winner(true, false, true, true);
        shader_selection_checks_the_winner(true, false, true, false, true, true);
    } else {
        shader_selection_checks_the_winner(true, false, true, false, false);
        std::cout << "Successful shader archive selection checks require a local game root; skipped\n";
    }
    catalog_checks_native_cooperation_again_when_the_variant_changes();
    native_variant_additions_preserve_replacements_and_removals(true);
    native_variant_additions_preserve_replacements_and_removals(false);
    a_manifest_after_the_first_payload_keeps_asset_order();
    replaced_while_the_game_runs();
    const auto baseline = map_only();
    expect(baseline > 0, "map only: the map's copy exists (" + std::to_string(baseline) + ")");
    carried_into_the_maps_copy(true, baseline);
    carried_into_the_maps_copy(false, baseline);
    resident_assets_go_with_the_additions(baseline);
    linked_win32_stays_in_the_patch(baseline);
    copy_in_the_adders_toc_is_left_alone();
    same_name_from_two_mods(true);
    same_name_from_two_mods(false);
    two_music_mods_both_carried_into_maps_copy(baseline);
    copies_equal_to_the_games_are_not_edits();
    carried_into_another_bundle_with_the_list(true);
    carried_into_another_bundle_with_the_list(false);
    carried_payload_written_once();
    every_placement_lands_in_an_archive();
    a_mod_archive_numbered_like_a_placeholder();
    two_merges_write_the_same_bytes();
    carried_chunk_keeps_the_records_in_step();
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
