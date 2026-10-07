// A bundle's chunk metadata has one record for each of its chunks: whose the chunk is (the hash of
// the resource's name) and, for a texture, the first mip the bundle holds. In the game's bundles
// the records are in the order of the chunks' guids, which is not the order the chunks are listed
// in. A mod's tool starts from the game's list and writes a record where each chunk it adds or
// replaces is LISTED: right for a chunk it adds at the end, but a replaced chunk's record lands
// on another chunk's, which is then gone from the mod's list.
//
// When two mods each ship a copy of one game bundle with chunks of their own (two costumes built
// on the same game costume), the merged bundle needs a list that describes every chunk and is in
// the game's form. With the game's list alone, or with records taken from where the mods' lists
// have them, the costumes' meshes and textures never streamed in.
#include "Engine/Vfs/mod_merge_internal.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <optional>

namespace db = dingosdk::native_db;
namespace fb = dingosdk::frostbite;
using namespace dingosdk::mods::detail;

namespace {
using Bytes = std::vector<std::byte>;

int failures = 0;
void check(bool condition, const char* what) {
    if (!condition) { std::cerr << "FAILED: " << what << "\n"; ++failures; }
}

// A chunk's guid: the first byte decides where it sorts.
fb::Guid guid(unsigned char first) {
    fb::Guid value;
    value.bytes.fill(std::byte{0x11});
    value.bytes[0] = static_cast<std::byte>(first);
    return value;
}
// One chunk's record: the hash of what it belongs to, and for a texture its first mip.
struct Record {
    std::uint64_t owner{};
    std::optional<std::uint32_t> firstMip;
    bool operator==(const Record&) const = default;
};
db::Node number(unsigned type, const char* name, std::uint64_t value, std::size_t size) {
    db::Node node;
    node.type = type;
    node.named = true;
    node.name = name;
    for (std::size_t byte = 0; byte < size; ++byte) node.owned.push_back(static_cast<unsigned char>(value >> (8 * byte)));
    return node;
}
Bytes list_of(const std::vector<Record>& records) {
    db::Node list;
    list.type = 1;
    list.named = true;
    list.name = "chunkMeta";
    list.terminated = true;
    for (const auto& record : records) {
        db::Node meta;
        meta.type = 2;
        meta.named = true;
        meta.name = "meta";
        meta.terminated = true;
        if (record.firstMip) meta.children.push_back(number(8, "firstMip", *record.firstMip, 4));
        db::Node row;
        row.type = 2;
        row.terminated = true;
        row.children.push_back(number(9, "h64", record.owner, 8));
        row.children.push_back(std::move(meta));
        list.children.push_back(std::move(row));
    }
    const auto written = db::write(list);
    const auto* bytes = reinterpret_cast<const std::byte*>(written.data());
    return {bytes, bytes + written.size()};
}
// What a list says, record by record; a record that says nothing reads as owner 0.
std::vector<Record> read_list(const Bytes& list) {
    const std::span<const unsigned char> bytes(reinterpret_cast<const unsigned char*>(list.data()), list.size());
    std::size_t used{};
    const auto root = db::read(bytes, "chunk metadata", &used);
    check(used == bytes.size() && root.type == 1 && root.name == "chunkMeta", "the merged list is one list, named as the game names it");
    std::vector<Record> records;
    for (const auto& row : root.children) {
        Record record;
        if (const auto* owner = row.field("h64"); owner && owner->payload().size() == 8)
            std::memcpy(&record.owner, owner->payload().data(), 8);
        if (const auto* meta = row.field("meta"))
            if (const auto* mip = meta->field("firstMip"); mip && mip->payload().size() == 4) {
                std::uint32_t value{};
                std::memcpy(&value, mip->payload().data(), 4);
                record.firstMip = value;
            }
        records.push_back(record);
    }
    return records;
}
ChunkRecord chunk(unsigned char first, std::size_t copy, std::size_t index, std::size_t shipped = ChunkRecord::none) {
    return {guid(first), copy, index, shipped};
}
} // namespace

int main() {
    // The game's bundle lists five chunks: something of its own, a texture, a mesh in two chunks
    // around another texture. Its list has their records by guid: 10, 30, 50, 60, 70.
    const Record mesh{0x1111}, textureA{0xAAAA, 2}, textureB{0xBBBB, 3}, other{0x9999};
    const std::vector<fb::Guid> shipped{guid(0x50), guid(0x30), guid(0x70), guid(0x10), guid(0x60)};
    const std::vector<Record> game{mesh, textureA, other, textureB, mesh};
    constexpr std::size_t gameList = 0;

    // One costume adds a texture (guid 20) and a mesh chunk (guid 80), listed after the game's five,
    // with their records at those two places.
    const Record firstTexture{0xF001, 0}, firstMesh{0xF002};
    auto first = game;
    first.insert(first.end(), {firstTexture, firstMesh});
    // Another, made with an older tool, replaces the game's second texture (guid 60, listed fifth)
    // with one whose first mip is 1: its record went over the fifth record, the mesh's, with no hash.
    // It also adds a texture (guid 40).
    const Record secondTexture{0x5001, 0};
    auto second = game;
    second[4] = Record{0, 1};
    second.push_back(secondTexture);
    const std::vector<Bytes> lists{list_of(game), list_of(first), list_of(second)};

    // The merged bundle: the game's five as the higher-priority mod has them, then each mod's own.
    const std::vector<ChunkRecord> merged{chunk(0x50, 2, 0, 0), chunk(0x30, 2, 1, 1), chunk(0x70, 2, 2, 2), chunk(0x10, 2, 3, 3),
                                          chunk(0x60, 2, 4, 4), chunk(0x20, 1, 5), chunk(0x80, 1, 6), chunk(0x40, 2, 5)};
    const auto replaced = Record{textureB.owner, 1};
    const std::vector<Record> expected{mesh, firstTexture, textureA, secondTexture, other, replaced, mesh, firstMesh};
    const auto list = read_list(merge_chunk_metadata(lists, merged, gameList, shipped));
    check(list == expected, "a record for every chunk, in the order of the chunks' guids");
    check(list.size() == merged.size(), "as many records as chunks");
    check(std::ranges::count(list, mesh) == 2, "the record the older tool wrote over is back: the mesh has both of its");
    check(std::ranges::find(list, replaced) != list.end() && std::ranges::find(list, textureB) == list.end(),
          "the replaced texture keeps its own name and takes the first mip the mod gave it");
    check(std::ranges::none_of(list, [](const Record& record) { return record.owner == 0; }), "no record is left without a hash");
    // The fault this is for: the game's own list is three records short, and the mods' lists have
    // their records where the chunks are listed, which is not where the game looks.
    check(read_list(lists[gameList]).size() + 3 == merged.size(), "the game's own list is three records short of the merged bundle");

    // No mod's chunk in it: the game's list is not written again.
    const std::vector<ChunkRecord> untouched{chunk(0x50, 0, 0, 0), chunk(0x30, 0, 1, 1), chunk(0x70, 0, 2, 2), chunk(0x10, 0, 3, 3), chunk(0x60, 0, 4, 4)};
    check(merge_chunk_metadata(lists, untouched, gameList, shipped) == lists[gameList], "a bundle that is all the game's keeps the game's list as it is");
    // One mod's copy alone is still written in the game's form: its additions go where their guids put them.
    const std::vector<ChunkRecord> oneMod{chunk(0x50, 1, 0, 0), chunk(0x30, 1, 1, 1), chunk(0x70, 1, 2, 2), chunk(0x10, 1, 3, 3),
                                          chunk(0x60, 1, 4, 4), chunk(0x20, 1, 5), chunk(0x80, 1, 6)};
    check(read_list(merge_chunk_metadata(lists, oneMod, gameList, shipped)) ==
              std::vector<Record>{mesh, firstTexture, textureA, other, textureB, mesh, firstMesh},
          "one mod's additions are put in guid order with the game's");
    // A newer tool writes the replaced texture's own hash with its first mip; the first mip is what counts.
    auto newer = game;
    newer[4] = Record{textureB.owner, 0};
    const std::vector<Bytes> withNewer{list_of(game), list_of(newer)};
    const std::vector<ChunkRecord> keepsNewer{chunk(0x50, 1, 0, 0), chunk(0x30, 1, 1, 1), chunk(0x70, 1, 2, 2), chunk(0x10, 1, 3, 3), chunk(0x60, 1, 4, 4)};
    check(read_list(merge_chunk_metadata(withNewer, keepsNewer, gameList, shipped)) ==
              std::vector<Record>{mesh, textureA, other, Record{textureB.owner, 0}, mesh},
          "a replaced texture's new first mip goes into its own record, and the record written over is back");
    // A mod that only carries the game's version of a chunk another mod replaced changes nothing of it.
    const std::vector<ChunkRecord> carried{chunk(0x50, 1, 0, 0), chunk(0x30, 1, 1, 1), chunk(0x70, 1, 2, 2), chunk(0x10, 1, 3, 3),
                                           chunk(0x60, 1, 4, 4), chunk(0x20, 1, 5), chunk(0x80, 1, 6)};
    check(read_list(merge_chunk_metadata(lists, carried, gameList, shipped))[4] == textureB, "a chunk a mod only carries keeps the game's record");

    // A chunk whose copy has no list, or a shorter one, still takes up a place.
    const std::vector<ChunkRecord> unknown{chunk(0x50, 0, 0, 0), chunk(0x30, 0, 1, 1), chunk(0x70, 0, 2, 2), chunk(0x10, 0, 3, 3),
                                           chunk(0x60, 0, 4, 4), chunk(0x20, ChunkRecord::none, 5), chunk(0x80, 1, 9)};
    check(read_list(merge_chunk_metadata(lists, unknown, gameList, shipped)) ==
              std::vector<Record>{mesh, Record{}, textureA, other, textureB, mesh, Record{}},
          "a chunk no list describes gets a record that says nothing, and the others stay in step");

    // A bundle the game has no list for (it has no chunks there, or does not ship the bundle): every
    // chunk is a mod's, with the record of the copy it came in. Maps each add their loading screen
    // to one such bundle.
    const std::vector<Bytes> modsOnly{list_of({firstTexture, firstMesh}), list_of({firstTexture, firstMesh, secondTexture})};
    const std::vector<ChunkRecord> theirs{chunk(0x90, 1, 0), chunk(0x20, 1, 1), chunk(0x40, 1, 2)};
    check(merge_chunk_metadata(modsOnly, theirs) == modsOnly[1], "a bundle that is one mod's copy alone keeps that copy's list");
    const std::vector<ChunkRecord> both{chunk(0x90, 0, 0), chunk(0x20, 0, 1), chunk(0x40, 1, 2)};
    check(read_list(merge_chunk_metadata(modsOnly, both)) == std::vector<Record>{firstMesh, secondTexture, firstTexture},
          "and otherwise each chunk's record from the copy it came in, by guid like any other");
    // A game list that is not one record a chunk is of some other kind: nothing is put in order by it.
    const std::vector<Bytes> shortGame{list_of({mesh, textureA}), list_of(first)};
    check(read_list(merge_chunk_metadata(shortGame, oneMod, gameList, shipped)) == first,
          "a game list that is not a record a chunk leaves the records where the copies have them");

    // No chunks, no list to write: what the first copy has stays.
    check(merge_chunk_metadata({}, {}).empty(), "no list from no lists");
    check(merge_chunk_metadata(lists, {}, gameList, shipped) == lists[gameList], "a bundle without chunks keeps the first copy's list");
    const std::vector<Bytes> none{Bytes{}, Bytes{}};
    const std::vector<ChunkRecord> bare{chunk(0x10, ChunkRecord::none, 0), chunk(0x20, ChunkRecord::none, 1)};
    check(merge_chunk_metadata(none, bare).empty(), "nor from copies that have none");

    // A list that is needed and cannot be read is the caller's to deal with.
    const std::vector<Bytes> broken{lists[0], Bytes(7, std::byte{0x41})};
    bool threw{};
    try { (void)merge_chunk_metadata(broken, oneMod, gameList, shipped); }
    catch (const std::exception&) { threw = true; }
    check(threw, "an unreadable list throws");

    if (failures) {
        std::cerr << "chunk metadata merge: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "chunk metadata merge: ok\n";
    return 0;
}
