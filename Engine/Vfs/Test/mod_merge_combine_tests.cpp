// Tests how the merge combines edits. If the merge wrote a copy, it reads that copy from the patch.
// Identical copies count as one copy. The highest-priority mod still wins a tie.
#include "Engine/Vfs/mod_catalog.h"
#include "Engine/Vfs/mod_merge_internal.h"
#include "Engine/Vfs/native_db.h"
#include "Engine/Resource/cas_codec.h"
#include "Engine/Resource/toc.h"

#include <Windows.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace db = dingosdk::native_db;
using namespace dingosdk;
using namespace dingosdk::mods::detail;

namespace {
constexpr std::uint32_t package = 0x1234;
using Bytes = std::vector<std::byte>;

int failures = 0;
void expect(bool ok, const std::string& message) {
    if (ok) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

fb::Sha1 sha(unsigned seed) {
    fb::Sha1 value;
    value.bytes[0] = static_cast<std::byte>(seed);
    return value;
}
std::pair<std::string, std::uint16_t> key(std::string directory, unsigned archive) {
    return {std::move(directory), static_cast<std::uint16_t>(archive)};
}
Contribution copy(unsigned seed, bool base = false, std::vector<std::byte> meta = {}) {
    return {"mod" + std::to_string(seed), {}, sha(seed), std::move(meta), base};
}

// A live merge appends two archives of one mod to archive 1.
// The map visits the later block first.
ArchivePlacement appended() {
    ArchivePlacement placement;
    placement.at[key("pkg", 2)] = {1, 1000};
    placement.at[key("pkg", 3)] = {1, 0};
    return placement;
}

void reverse_mapping() {
    const auto placement = appended();
    expect(placement.at.begin()->second.offset == 1000, "the fixture visits the later block first");
    // Origin 3 is 1000 bytes at offset 0, origin 2 is 500 bytes at offset 1000.
    for (const auto& [origin, length] : std::vector<std::pair<unsigned, std::uint32_t>>{{3, 1000}, {2, 500}}) {
        const auto& spot = placement.at.at(key("pkg", origin));
        for (const std::uint32_t at : {0u, 1u, length / 2, length - 1}) {
            std::uint16_t archive = spot.archive;
            auto offset = static_cast<std::uint32_t>(at + spot.offset);
            expect(unshift(placement, "pkg", archive, offset) && archive == origin && offset == at,
                   "unshift reverses shift for origin " + std::to_string(origin) + " at offset " + std::to_string(at));
        }
    }
    std::uint16_t archive{};
    std::uint32_t offset{};

    // An empty archive that the merge appends before another archive starts at the same offset.
    ArchivePlacement empty;
    empty.at[key("pkg", 4)] = {1, 200};
    empty.at[key("pkg", 5)] = {1, 200};
    archive = 1;
    offset = 210;
    expect(unshift(empty, "pkg", archive, offset) && archive == 5 && offset == 10, "a tie selects the later archive");

    ArchivePlacement launch;
    launch.at[key("pkg", 1)] = {2, 0};
    launch.at[key("pkg", 3)] = {3, 0};
    archive = 2;
    offset = 77;
    expect(unshift(launch, "pkg", archive, offset) && archive == 1 && offset == 77, "a re-indexed archive maps to its origin");
    archive = 3;
    offset = 5;
    expect(unshift(launch, "pkg", archive, offset) && archive == 3 && offset == 5, "an archive with its original index does not change");
    archive = 2;
    offset = 9;
    expect(!unshift(launch, "other", archive, offset) && archive == 2 && offset == 9, "a block in a different package does not change");
    archive = 7;
    expect(!unshift(launch, "pkg", archive, offset) && archive == 7, "an archive that the mod does not contain does not change");
}

void read_backs() {
    const auto placement = appended();
    const std::string directory = "pkg";
    const fb::BundleFileInfo file{{true, 1, 1}, 1010, 4};
    const auto written = read_back("mods/a", "out", file, true, &placement, &directory);
    expect(written.root == "out" && written.file.location.archive == 1 && written.file.offset == 1010,
           "the merge reads its own copy from the patch, without change");
    const auto own = read_back("mods/a", "out", file, false, &placement, &directory);
    expect(own.root == "mods/a" && own.file.location.archive == 2 && own.file.offset == 10,
           "the merge reads the copy of a mod from the mod's folder, at the original offset");
    const auto unknown = read_back("mods/a", "out", file, false, &placement, nullptr);
    expect(unknown.root == "mods/a" && unknown.file.offset == 1010, "a copy in an unknown package does not change");
}

std::vector<unsigned> seeds(const std::vector<const Contribution*>& edits) {
    std::vector<unsigned> result;
    for (const auto* edit : edits) result.push_back(static_cast<unsigned>(edit->sha1.bytes[0]));
    return result;
}

void edits() {
    const auto base = copy(0, true);
    const std::vector<Contribution> xyx{base, copy(1), copy(2), copy(1)};
    expect(seeds(distinct_edits(xyx, base)) == std::vector<unsigned>{2, 1}, "[x, y, x] keeps the last x: [y, x]");

    expect(distinct_edits({base, copy(1), copy(1)}, base).size() == 1, "[x, x] is one edit");
    expect(seeds(distinct_edits({base, copy(1), copy(2)}, base)) == std::vector<unsigned>{1, 2}, "[x, y] does not change");
    expect(distinct_edits({base, copy(0), copy(1)}, base).size() == 1, "a copy of the base is not an edit");
    expect(distinct_edits({base, copy(0, false, {std::byte{1}})}, base).empty(),
           "a copy of the base with different metadata is not an edit");
    expect(distinct_edits({base, copy(1, false, {std::byte{1}}), copy(1, false, {std::byte{2}})}, base).size() == 2,
           "the same sha1 with different metadata is a separate edit");
    const std::vector<Contribution> aab{base, copy(1), copy(1), copy(2)};
    const auto kept = distinct_edits(aab, base);
    expect(kept.size() == 2 && kept[0] == &aab[2] && kept[1] == &aab[3], "{A, A', B} keeps [A', B]");
}

Bytes bytes_of(std::string_view text) {
    Bytes bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    return bytes;
}
void write(const fs::path& path, std::span<const std::byte> bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                static_cast<std::streamsize>(bytes.size()));
}
db::Node text(std::string_view name, std::string_view value) {
    auto node = db::make_named_record(name, value).children.front();
    if (name.empty()) node.named = false;
    return node;
}
db::Node container(unsigned type, std::string_view name, std::vector<db::Node> children) {
    db::Node node;
    node.type = type;
    node.named = !name.empty();
    node.name = name;
    node.terminated = true;
    node.children = std::move(children);
    return node;
}

// layout.toc with one install chunk, `package`. Its archive 1 is in Win32/pkg.
Bytes layout_toc(const std::vector<std::string>& extra = {}) {
    db::Node index;
    index.type = 8;
    index.named = true;
    index.name = "persistentIndex";
    index.owned = {package & 0xFF, (package >> 8) & 0xFF, 0, 0};
    db::Node files;
    files.type = 19;
    files.named = true;
    files.name = "layeredInstallChunkFiles";
    files.owned = {1, 0, package & 0xFF, (package >> 8) & 0xFF, 0, 0, 0, 0};
    std::vector<db::Node> superbundles{db::make_named_record("name", "Win32/globals")};
    for (const auto& name : extra) superbundles.push_back(db::make_named_record("name", name));
    const auto root = container(2, "", {
        container(1, "superBundles", std::move(superbundles)),
        container(2, "installManifest", {container(1, "installChunks", {
            container(2, "", {text("name", "pkg"), index, container(1, "superbundles", {})})})}),
        files});
    const auto tree = db::write(root);
    Bytes file(db::envelope_size + tree.size());
    std::memcpy(file.data(), db::magic, sizeof(db::magic));
    std::memcpy(file.data() + db::envelope_size, tree.data(), tree.size());
    return file;
}

fb::BundleAsset ebx(std::string name) {
    fb::BundleAsset asset;
    asset.kind = fb::AssetKind::ebx;
    asset.name = std::move(name);
    asset.sha1.bytes[0] = static_cast<std::byte>(asset.name.size());
    asset.originalSize = asset.name.size();
    return asset;
}

struct Fixture {
    fs::path root;
    mods::Catalog catalog;

    explicit Fixture(const std::string& name) {
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        root = fs::path(temp) / ("reskate-merge-combine-" + std::to_string(GetCurrentProcessId()) + "-" + name);
        fs::remove_all(root);
        catalog.data_root = root / "game";
        catalog.root = catalog.data_root / "Mods";
        catalog.present = true;
        write(catalog.data_root / "Data" / "layout.toc", layout_toc());
        write(catalog.data_root / "Data" / "Win32" / "pkg" / "cas_01.cas", bytes_of("BASE"));
    }
    ~Fixture() {
        std::error_code error;
        fs::remove_all(root, error);
    }
    fs::path add(const std::string& name, const std::string& tocName, std::span<const std::byte> toc,
                 std::string_view archive = "cas_01.cas", std::span<const std::byte> payload = {}) {
        const auto directory = catalog.root / name;
        write(directory / "layout.toc", layout_toc());
        write(directory / "Win32" / tocName, toc);
        const auto data = bytes_of(name.substr(0, 4));
        write(directory / "Win32" / "pkg" / archive, payload.empty() ? std::span<const std::byte>(data) : payload);
        mods::Mod mod;
        mod.name = name;
        mod.directory = directory;
        mod.provides_layout = true;
        catalog.mods.push_back(mod);
        return directory;
    }
    fs::path output() const { return catalog.root / mods::generated_folder; }
};

bool noted(const mods::MergeReport& report, std::string_view prefix) {
    for (const auto& note : report.notes)
        if (note.starts_with(prefix)) return true;
    return false;
}

std::string describe(const mods::MergeReport& report) {
    std::string text = "issue: '" + report.issue + "'";
    for (const auto& [mod, problems] : report.problems)
        for (const auto& problem : problems) text += "\n  " + mod + ": " + problem;
    return text;
}

// A raw cas payload. Encoding it does not need Oodle.
Bytes encoded(std::string_view text) {
    return fb::encode_cas(bytes_of(text), {.compression = fb::CasCompression::raw});
}
fb::BundleAsset asset_with(std::uint8_t version) {
    auto asset = ebx("x/asset");
    asset.sha1.bytes[1] = static_cast<std::byte>(version);
    return asset;
}

// Two mods edit x/asset. A third mod contains the game's copy of x/asset.
// In the patch, that copy receives the change from the mod with higher priority.
// The merge must read that copy from the patch, not from the folder of the third mod.
void taken_change_reads_from_the_patch() {
    Fixture fixture("taken-change");
    const auto baseBytes = encoded("base");
    {
        fb::BinaryBundle manifest;
        manifest.ebx = {asset_with(0)};
        const std::vector<fb::BundleFileInfo> files{{{false, package, 1}, 0, static_cast<std::uint32_t>(baseBytes.size())}};
        const std::vector<fb::TocBundle> bundles{{"win32/test/shared", fb::write_bundle_region(files, fb::write_binary_bundle(manifest)), 1}};
        write(fixture.catalog.data_root / "Data" / "Win32" / "test_shared.toc", fb::write_patch_toc(bundles));
        write(fixture.catalog.data_root / "Data" / "Win32" / "pkg" / "cas_01.cas", baseBytes);
    }
    // Highest priority first. "two" contains the game's copy. "one" and "three" edit it.
    struct Copy { std::string name; std::uint8_t version; std::string text; };
    for (const auto& [name, version, text] : {Copy{"two", 0, "base"}, Copy{"one", 1, "one's edit"}, Copy{"three", 2, "three's edit"}}) {
        fb::BinaryBundle manifest;
        manifest.ebx = {asset_with(version)};
        const auto listing = fb::write_binary_bundle(manifest);
        const auto payload = encoded(text);
        Bytes archive(listing);
        archive.insert(archive.end(), payload.begin(), payload.end());
        const std::vector<fb::BundleFileInfo> files{
            {{true, package, 1}, 0, static_cast<std::uint32_t>(listing.size())},
            {{true, package, 1}, static_cast<std::uint32_t>(listing.size()), static_cast<std::uint32_t>(payload.size())}};
        const std::vector<fb::TocBundle> bundles{{"win32/test/shared", fb::write_bundle_region(files), 1}};
        fixture.add(name, "test_shared.toc", fb::write_patch_toc(bundles), "cas_01.cas", archive);
    }
    const auto report = mods::merge_mods(fixture.catalog);
    expect(report.issue.empty() && report.built, "taken change: the merge builds the patch\n" + describe(report));
    bool taken{}, attempted{};
    for (const auto& note : report.notes) {
        taken |= note.starts_with("two: win32/test/shared: 1 asset(s) take another mod's change");
        if (!note.starts_with("x/asset: kept the highest-priority copy")) continue;
        attempted = true;
        // The payloads are not EBX. So the merge cannot combine them, but only after it reads both copies.
        for (const auto* read_failure : {"Cannot", "Truncated", "Unsupported CAS"})
            expect(note.find(read_failure) == std::string::npos, "taken change: the merge read every copy: " + note);
    }
    expect(taken, "taken change: the game's copy received the change of mod one");
    expect(attempted, "taken change: the merge tried to combine the two distinct edits");
}
} // namespace

int main() {
    reverse_mapping();
    read_backs();
    edits();
    taken_change_reads_from_the_patch();
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "mod merge combine: ok\n";
    return 0;
}
