// The shader program lookup's merge: the rows mods add are combined onto the base, and a row
// that points past the base's shader records is left out (the game reads such an index outside
// its shader database).
#include "Engine/Resource/shader_lookup.h"
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
// The merged rows, read back.
std::map<std::uint64_t, std::vector<std::uint32_t>> rows_of(const fb::shader::MergedTable& merged) {
    std::size_t at = read<std::uint32_t>(merged.resourceMeta, 0);
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
    check(merged.added == 2 && rows.contains(2) && rows.contains(5), "Rows onto the base's programs are merged");
    check(merged.outside == 2 && !rows.contains(3) && !rows.contains(4),
          "Rows pointing past the base's records are left out, also when only one of their indices does");
    check(rows.size() == 3 && rows.at(1) == std::vector<std::uint32_t>{0}, "The base's own rows stay as they were");
    check(read<std::uint32_t>(merged.resourceMeta, 8) == merged.resource.size() - read<std::uint32_t>(merged.resourceMeta, 0),
          "The lookup size in the metadata matches the merged rows");
    for (const auto& [key, values] : rows)
        for (const auto value : values) check(value < 3, "No merged row points past the base's records");
    if (failures) return 1;
    std::cout << "shader lookup tests passed\n";
    return 0;
}
