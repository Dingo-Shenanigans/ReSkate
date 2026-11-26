// Shader block depots: read, written in the game's form, and two mods' copies of one put together.
//
// Before 2026-11-18 the merge kept one mod's copy of a depot whole: two mods that each added
// materials to one of the game's depots lost the lower mod's rows, and its material keys drew with
// the default block. The cases, on depots made here:
//   * a depot written from what was read is the same bytes;
//   * every row of both mods is in the merged depot, the game's rows first;
//   * a key two mods give different blocks is the later mod's, and is reported;
//   * a mod's copy that carries a game row unchanged does not undo another mod's change to it.
// With the game's folder as argument, every depot of the game's superbundles is also read and
// written again, and has to be its own bytes (Studio holds its own model to the same).
#include "Engine/Resource/shader_block_depot.h"
#include "Engine/Vfs/game_bundles.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace fb = dingosdk::frostbite;
namespace depot = dingosdk::frostbite::shader_depot;

namespace {
int failures = 0;
void expect(bool ok, const std::string& what) {
    if (ok) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}

// A block of `count` float parameters, told apart by `seed`.
depot::Block block(std::uint64_t identity, std::uint8_t seed, std::uint32_t count = 1) {
    depot::Block result;
    result.identity = identity;
    const auto put = [&](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const std::byte*>(data);
        result.bytes.insert(result.bytes.end(), bytes, bytes + size);
    };
    put(&count, 4);
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::uint64_t hash = 0x1000U + seed + index;
        const std::uint32_t type = 0x2BB65C2D, elements = 1;
        put(&hash, 8);
        put(&type, 4);
        put(&elements, 4);   // u16 count, u16 top of the name hash
        put(&elements, 4);   // the value: u32 number of elements, then a float
        const float value = static_cast<float>(seed);
        put(&value, 4);
    }
    return result;
}

depot::Depot game_depot() {
    depot::Depot result;
    result.blocks = {block(0xA, 1, 2), block(0xB, 2)};
    result.rows = {{0x100, 0}, {0x101, 1}, {0x102, 0}};
    return result;
}
std::uint64_t block_of(const depot::Depot& read, std::uint64_t key) {
    for (const auto& row : read.rows)
        if (row.key == key) return read.blocks[row.block].identity;
    return 0;
}
depot::Resource view(const depot::Written& written) { return {written.resource, written.resourceMeta}; }

void written_as_read() {
    const auto written = depot::write(game_depot());
    const auto again = depot::write(depot::read(view(written)));
    expect(again.resource == written.resource && again.resourceMeta == written.resourceMeta, "round trip: written again, the same bytes");
    const auto read = depot::read(view(written));
    expect(read.rows.size() == 3 && read.blocks.size() == 2 && read.rows[2].block == 0, "round trip: rows share a block as they did");
}

void rows_of_two_mods() {
    const auto base = depot::write(game_depot());
    auto first = game_depot(), second = game_depot();
    first.blocks.push_back(block(0xC, 3));
    first.rows.push_back({0x200, 2});
    second.blocks.push_back(block(0xD, 4));
    second.blocks.push_back(block(0xE, 5));
    second.rows.push_back({0x300, 2});
    second.rows.push_back({0x200, 3});   // the first mod's key, with a block of its own
    const auto a = depot::write(first), b = depot::write(second);
    const std::vector<depot::Resource> edits{view(a), view(b)};
    const auto merged = depot::merge(view(base), edits);
    const auto read = depot::read(view(merged.depot));
    expect(read.rows.size() == 5, "two mods: the game's three rows and both mods' keys (" + std::to_string(read.rows.size()) + " rows)");
    expect(read.rows[0].key == 0x100 && read.rows[1].key == 0x101 && read.rows[2].key == 0x102, "two mods: the game's rows first, in order");
    expect(block_of(read, 0x300) == 0xD && block_of(read, 0x200) == 0xE, "two mods: a key both give is the later mod's");
    expect(merged.added == 2 && merged.changed == 0 && merged.clashes.size() == 1 && merged.clashes[0].key == 0x200 &&
               merged.clashes[0].earlier == 0 && merged.clashes[0].later == 1,
           "two mods: two keys added, and the one they disagree on reported with both mods");
    expect(depot::write(read).resource == merged.depot.resource, "two mods: the merged depot is in the game's form");
}

void a_carried_game_row_changes_nothing() {
    const auto base = depot::write(game_depot());
    auto changed = game_depot(), carried = game_depot();
    changed.blocks.push_back(block(0xF, 6));
    changed.rows[1].block = 2;            // the game's key 0x101 drawn with other parameters
    carried.blocks.push_back(block(0x10, 7));
    carried.rows.push_back({0x400, 2});   // adds a key, carries the game's rows as they are
    const auto a = depot::write(changed), b = depot::write(carried);
    const std::vector<depot::Resource> edits{view(a), view(b)};
    const auto merged = depot::merge(view(base), edits);
    const auto read = depot::read(view(merged.depot));
    expect(block_of(read, 0x101) == 0xF && block_of(read, 0x400) == 0x10,
           "carried row: the lower mod's change to a game key stays, and the higher mod's key is added");
    expect(merged.changed == 1 && merged.added == 1 && merged.clashes.empty(), "carried row: one change, one key, no disagreement");
}

// Every depot of the game, read and written again.
void every_game_depot(const fs::path& root) {
    const dingosdk::vfs::GameData game(root);
    std::size_t depots{}, rows{}, empty{}, differ{};
    std::string example;
    for (const auto& entry : fs::recursive_directory_iterator(root / L"Data" / L"Win32")) {
        if (entry.path().extension() != L".toc") continue;
        const auto relative = fs::relative(entry.path(), root / L"Data").generic_string();
        fb::TocDocument toc;
        try {
            toc = game.read_toc(relative);
        } catch (const std::exception&) {
            continue;
        }
        for (const auto& listed : toc.bundles) {
            const auto bundle = game.read_bundle(toc, listed.name);
            if (!bundle) continue;
            for (std::size_t index = 0; index < bundle->manifest.resources.size(); ++index) {
                const auto& asset = bundle->manifest.resources[index];
                if (asset.resourceType != depot::resourceType) continue;
                const auto* payload = bundle->payload(fb::AssetKind::resource, index);
                if (!payload) continue;
                const auto bytes = game.read(*payload);
                ++depots;
                try {
                    const auto read = depot::read({bytes, asset.resourceMeta});
                    if (read.rows.empty()) { ++empty; continue; }
                    rows += read.rows.size();
                    const auto written = depot::write(read);
                    if (written.resource != bytes || written.resourceMeta != asset.resourceMeta) {
                        if (!differ++) example = asset.name;
                    }
                } catch (const std::exception& error) {
                    if (!differ++) example = asset.name + " (" + error.what() + ")";
                }
            }
        }
    }
    std::cout << depots << " game depots (" << empty << " with no rows), " << rows << " rows\n";
    expect(depots > 0, "game: depots were found");
    expect(differ == 0, "game: " + std::to_string(differ) + " depot(s) are not their own bytes written again, e.g. " + example);
}
} // namespace

int main(int argc, char** argv) try {
    written_as_read();
    rows_of_two_mods();
    a_carried_game_row_changes_nothing();
    if (argc > 1 && fs::exists(fs::path(argv[1]) / L"Data" / L"Win32")) every_game_depot(argv[1]);
    else std::cout << "No game given; the game's depots are skipped.\n";
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "shader block depot: ok\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
