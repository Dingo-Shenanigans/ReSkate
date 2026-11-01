// The shader program lookup's merge: the rows mods add are combined onto the base, and a row
// that points past the base's shader records is left out (the game reads such an index outside
// its shader database).
#include "Engine/Resource/shader_lookup.h"
#ifdef _WIN32
#include <Windows.h>
#endif
#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>
#include <vector>

namespace fb = dingosdk::frostbite;

namespace {
int failures = 0;
void check(bool condition, const char* what) {
    if (!condition) { std::cerr << "FAILED: " << what << "\n"; ++failures; }
}
template <typename Type> void append(std::vector<std::byte>& bytes, Type value) {
    const auto at = bytes.size();
    bytes.resize(at + sizeof(value));
    std::memcpy(bytes.data() + at, &value, sizeof(value));
}
template <typename Type> Type read(const std::vector<std::byte>& bytes, std::size_t at) {
    Type value{};
    std::memcpy(&value, bytes.data() + at, sizeof(value));
    return value;
}
// A program lookup: a blob of `records` records (only its count matters here), no relocations,
// and the rows (key -> record indices).
struct Lookup {
    std::vector<std::byte> resource, meta;
    fb::shader::Table table() const { return {resource, meta}; }
};
Lookup lookup(std::uint32_t records, const std::map<std::uint64_t, std::vector<std::uint32_t>>& rows) {
    Lookup out;
    append(out.resource, records);
    append(out.resource, std::uint32_t{0}); // the rest of the blob
    const auto program = static_cast<std::uint32_t>(out.resource.size());
    append(out.resource, static_cast<std::uint32_t>(rows.size()));
    for (const auto& [key, values] : rows) {
        append(out.resource, key);
        append(out.resource, static_cast<std::uint32_t>(values.size()));
        for (const auto value : values) append(out.resource, value);
    }
    append(out.meta, program);
    append(out.meta, std::uint32_t{0});
    append(out.meta, static_cast<std::uint32_t>(out.resource.size() - program));
    append(out.meta, std::uint32_t{0});
    return out;
}
// A complete preserved stock heap with an appended program array. This is
// intentionally different from merely sharing record counts or numeric indices.
Lookup preserved_lookup(bool extended, const std::map<std::uint64_t, std::vector<std::uint32_t>>& rows) {
    Lookup out;
    const std::uint32_t stockSize = 208;
    const std::uint64_t start = extended ? 208 : 16;
    const std::uint32_t count = extended ? 2 : 1;
    const std::uint32_t size = extended ? 544 : stockSize;
    out.resource.resize(size);
    const std::uint64_t heap = 192;
    std::memcpy(out.resource.data(), &count, 4);
    std::memcpy(out.resource.data() + 4, &start, 8);
    // The first array's count/pointer, within the stock record at 16.
    const std::uint32_t one = 1;
    std::memcpy(out.resource.data() + 28, &one, 4);
    std::memcpy(out.resource.data() + 32, &heap, 8);
    out.resource[192] = std::byte{7};
    if (extended) std::memcpy(out.resource.data() + 208, out.resource.data() + 16, 168);
    append(out.resource, std::uint32_t{4});
    append(out.resource, std::uint32_t{32});
    if (extended) append(out.resource, std::uint32_t{224});
    append(out.resource, static_cast<std::uint32_t>(rows.size()));
    for (const auto& [key, values] : rows) {
        append(out.resource, key);
        append(out.resource, static_cast<std::uint32_t>(values.size()));
        for (const auto v : values) append(out.resource, v);
    }
    const std::uint32_t fix = extended ? 12 : 8;
    append(out.meta, size); append(out.meta, fix);
    append(out.meta, static_cast<std::uint32_t>(out.resource.size() - size - fix));
    append(out.meta, std::uint32_t{});
    return out;
}

// The merged rows, read back.
std::map<std::uint64_t, std::vector<std::uint32_t>> rows_of(const fb::shader::MergedTable& merged) {
    std::size_t at = static_cast<std::size_t>(read<std::uint32_t>(merged.resourceMeta, 0)) + read<std::uint32_t>(merged.resourceMeta, 4);
    const auto count = read<std::uint32_t>(merged.resource, at);
    at += 4;
    std::map<std::uint64_t, std::vector<std::uint32_t>> rows;
    for (std::uint32_t row = 0; row < count; ++row) {
        const auto key = read<std::uint64_t>(merged.resource, at);
        const auto values = read<std::uint32_t>(merged.resource, at + 8);
        at += 12;
        auto& out = rows[key];
        for (std::uint32_t value = 0; value < values; ++value, at += 4) out.push_back(read<std::uint32_t>(merged.resource, at));
    }
    return rows;
}
} // namespace

int main() {
    const auto base = lookup(3, {{1, {0}}});
    // One mod adds a row onto a program the game has; another brings programs of its own and
    // points rows at them (indices 3 and 4, which the base's three records do not have).
    const auto good = lookup(3, {{1, {0}}, {2, {2}}});
    const auto own = lookup(5, {{1, {0}}, {3, {4}}, {4, {1, 3}}, {5, {1}}});
    const std::vector<fb::shader::Table> edits{good.table(), own.table()};
    const auto merged = fb::shader::merge_program_lookup(base.table(), edits);
    const auto rows = rows_of(merged);
    check(merged.added == 1 && rows.contains(2) && !rows.contains(5), "Rows onto the base's programs are merged");
    check(merged.outside == 3 && !rows.contains(3) && !rows.contains(4),
          "Rows pointing past the base's records are left out, also when only one of their indices does");
    check(rows.size() == 2 && rows.at(1) == std::vector<std::uint32_t>{0}, "The base's own rows stay as they were");
    check(read<std::uint32_t>(merged.resourceMeta, 8) == merged.resource.size() - read<std::uint32_t>(merged.resourceMeta, 0),
          "The lookup size in the metadata matches the merged rows");
    for (const auto& [key, values] : rows)
        for (const auto value : values) check(value < 3, "No merged row points past the base's records");
    auto changed = good;
    changed.resource[4] = std::byte{99};
    const std::vector<fb::shader::Table> foreign{changed.table()};
    const auto refused = fb::shader::merge_program_lookup(base.table(), foreign);
    check(refused.outside == 1 && !rows_of(refused).contains(2),
          "Equal record counts cannot establish program identity");
    const auto selected = lookup(5, {{1, {0}}, {9, {4}}});
    const auto alias = lookup(5, {{1, {0}}, {9, {4}}, {10, {3}}});
    const std::vector<fb::shader::Table> same{alias.table(), alias.table()};
    const auto retained = fb::shader::merge_program_lookup(selected.table(), same);
    check(retained.added == 1 && retained.conflicts == 0 && rows_of(retained).contains(9),
          "Selected custom rows survive and duplicate aliases are not conflicts");
    check(std::equal(selected.resource.begin(), selected.resource.begin() + 8,
                     retained.resource.begin()), "Opaque program bytes remain unchanged");
    check(retained.resource.size() == alias.resource.size(), "Aliases never duplicate program heaps");
    const auto stock = preserved_lookup(false, {{1, {0}}});
    const auto stockAlias = preserved_lookup(false, {{1, {0}}, {2, {0}}});
    const auto extension = preserved_lookup(true, {{1, {0}}, {9, {1}}});
    for (const auto bytes : {std::size_t{0}, std::size_t{4}, std::size_t{12}, std::size_t{32}, std::size_t{544}}) {
        auto truncated = extension;
        truncated.resource.resize(bytes);
        check(!fb::shader::preserves_stock_programs(truncated.table(), stock.table()),
              "A retained table's metadata cannot authorize reads past its actual payload");
    }
    auto shortMeta = extension;
    shortMeta.meta.resize(8);
    check(!fb::shader::preserves_stock_programs(shortMeta.table(), stock.table()), "Short retained metadata is refused");
    auto inflatedCount = extension;
    const std::uint32_t impossibleCount = 0xffffffffU;
    std::memcpy(inflatedCount.resource.data(), &impossibleCount, sizeof(impossibleCount));
    check(!fb::shader::preserves_stock_programs(inflatedCount.table(), stock.table()),
          "A declared private program count must fit inside the retained heap");
#ifdef _WIN32
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const auto page = static_cast<std::size_t>(system.dwPageSize);
    auto* guarded = static_cast<std::byte*>(VirtualAlloc(nullptr, page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(guarded != nullptr, "Allocate guarded shader-table regression buffer");
    if (guarded) {
        auto* tiny = guarded + page - 12;
        std::memcpy(tiny, extension.resource.data(), 12);
        DWORD previous{};
        const bool protectedPage = VirtualProtect(guarded + page, page, PAGE_NOACCESS, &previous) != 0;
        check(protectedPage, "Protect the page immediately after the truncated table");
        if (protectedPage)
            check(!fb::shader::preserves_stock_programs({{tiny, 12}, extension.meta}, stock.table()),
                  "Direct graph selection refuses truncation before touching the guard page");
        VirtualFree(guarded, 0, MEM_RELEASE);
    }
#endif
    const std::vector<fb::shader::Table> stockEdits{stockAlias.table()};
    check(fb::shader::only_stock_aliases(stockAlias.table(), stock.table()) &&
          !fb::shader::only_stock_aliases(extension.table(), stock.table()),
          "Selection distinguishes stock aliases from a private graph");
    const auto replacement = preserved_lookup(false, {{1, {0, 0}}, {2, {0}}});
    check(!fb::shader::only_stock_aliases(replacement.table(), stock.table()),
          "Changing an original material row is not an alias-only edit");
    const auto compatible = fb::shader::merge_program_lookup(extension.table(), stockEdits, stock.table());
    check(compatible.added == 1 && compatible.outside == 0 && rows_of(compatible).contains(9),
          "Verified stock aliases coexist with an appended private program array");
    check(std::equal(extension.resource.begin(), extension.resource.begin() + 556, compatible.resource.begin()),
          "Stock alias merge leaves the selected graph and all relocations untouched");
    auto changedHeap = extension;
    changedHeap.resource[192] = std::byte{8};
    check(fb::shader::merge_program_lookup(changedHeap.table(), stockEdits, stock.table()).outside == 1,
          "A changed stock heap cannot borrow stock aliases");
    auto missingFixup = extension;
    const std::uint32_t other = 0;
    std::memcpy(missingFixup.resource.data() + 552, &other, 4);
    check(fb::shader::merge_program_lookup(missingFixup.table(), stockEdits, stock.table()).outside == 1,
          "A copied record without its relocation cannot borrow stock aliases");
    auto extraFixup = extension;
    const std::uint32_t unexpected = 64, extraSize = 16;
    const auto insertion = extraFixup.resource.begin() + 556;
    const auto* encoded = reinterpret_cast<const std::byte*>(&unexpected);
    extraFixup.resource.insert(insertion, encoded, encoded + 4);
    std::memcpy(extraFixup.meta.data() + 4, &extraSize, 4);
    check(fb::shader::merge_program_lookup(extraFixup.table(), stockEdits, stock.table()).outside == 1,
          "Extra fixups cannot reinterpret the unchanged stock heap");
    const auto invalidAlias = preserved_lookup(false, {{1, {0}}, {2, {1}}});
    const std::vector<fb::shader::Table> invalidEdits{invalidAlias.table()};
    check(fb::shader::merge_program_lookup(extension.table(), invalidEdits, stock.table()).outside == 1,
          "Stock aliases cannot address the selected graph's private program slots");
    check(fb::shader::merge_program_lookup(extension.table(), stockEdits).outside == 1,
          "The stock exception requires the caller's actual game table");
    auto otherCustom = preserved_lookup(true, {{1, {0}}, {12, {0}}, {13, {1}}});
    otherCustom.resource[400] = std::byte{9}; // a different private record, leaving stock intact
    const std::vector<fb::shader::Table> competing{otherCustom.table()};
    const auto sharedAliases = fb::shader::merge_program_lookup(extension.table(), competing, stock.table());
    check(sharedAliases.added == 1 && sharedAliases.outside == 1 && rows_of(sharedAliases).contains(12) &&
          !rows_of(sharedAliases).contains(13), "A non-retained custom graph contributes proven stock aliases, never its private indices");
    const auto stockRetained = fb::shader::merge_program_lookup(stock.table(), competing, stock.table());
    check(rows_of(stockRetained).contains(12) && !rows_of(stockRetained).contains(13),
          "Stock aliases from an extended graph survive when stock is retained too");
    const auto clash = preserved_lookup(false, {{1, {0}}, {9, {0}}});
    const std::vector<fb::shader::Table> clashing{clash.table()};
    const auto graphOwnerWins = fb::shader::merge_program_lookup(extension.table(), clashing, stock.table());
    check(graphOwnerWins.conflicts == 1 && graphOwnerWins.added == 0 &&
          rows_of(graphOwnerWins).at(9) == std::vector<std::uint32_t>{1},
          "Retained graph rows win clashes with a stock-alias winner, and the disagreement is reported");
    const auto sameGraphClash = lookup(5, {{1, {0}}, {9, {3}}});
    const std::vector<fb::shader::Table> graphClashes{sameGraphClash.table()};
    const auto selectedWins = fb::shader::merge_program_lookup(selected.table(), graphClashes);
    check(selectedWins.conflicts == 1 && rows_of(selectedWins).at(9) == std::vector<std::uint32_t>{4},
          "The selected contributor wins a same-graph clash with a lower-priority edit");
    const auto invalidWinner = lookup(5, {{1, {0}}, {99, {5}}});
    const auto filtered = fb::shader::merge_program_lookup(invalidWinner.table(), {});
    check(filtered.removed == 1 && filtered.outside == 1 && !rows_of(filtered).contains(99),
          "The retained winner's rows must be inside its own program count");
    auto corruptEdit = extension;
    corruptEdit.resource.resize(12);
    const std::vector<fb::shader::Table> corrupt{corruptEdit.table()};
    const auto kept = fb::shader::merge_program_lookup(stock.table(), corrupt, stock.table());
    check(kept.outside == 1 && rows_of(kept).contains(1), "Malformed edits cannot discard an intact retained table");
    if (failures) return 1;
    std::cout << "shader lookup tests passed\n";
    return 0;
}
