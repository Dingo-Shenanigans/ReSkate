#include "shader_lookup.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>

namespace dingosdk::frostbite::shader {
namespace {

template <typename Type>
Type read(const std::span<const std::byte> bytes, const std::size_t offset) {
    if (offset > bytes.size() || sizeof(Type) > bytes.size() - offset)
        throw std::out_of_range("Shader lookup read exceeds its payload");
    Type result{};
    std::memcpy(&result, bytes.data() + offset, sizeof(result));
    return result;
}

template <typename Type>
void append(std::vector<std::byte>& bytes, const Type value) {
    const auto offset = bytes.size();
    bytes.resize(offset + sizeof(value));
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

using ProgramRows = std::map<std::uint64_t, std::vector<std::uint32_t>>;
using TextureRows = std::map<std::uint64_t, std::uint64_t>;

// Where the lookup section starts, after the compiled programs and relocations.
std::size_t lookup_offset(const Table& table) {
    if (table.resourceMeta.size() != 16)
        throw std::runtime_error("Program lookup metadata is not 16 bytes");
    const auto programSize = read<std::uint32_t>(table.resourceMeta, 0);
    const auto relocationSize = read<std::uint32_t>(table.resourceMeta, 4);
    const auto lookupSize = read<std::uint32_t>(table.resourceMeta, 8);
    const auto offset = static_cast<std::uint64_t>(programSize) + relocationSize;
    if (offset > table.resource.size() || lookupSize < 4 ||
        lookupSize != table.resource.size() - offset)
        throw std::runtime_error("Program lookup metadata does not match its payload");
    return static_cast<std::size_t>(offset);
}

// Equality includes the opaque heap, relocation list and graph metadata;
// lookup rows themselves can differ.
bool same_graph(const Table& left, const Table& right) {
    if (left.resourceMeta.size() != 16 || right.resourceMeta.size() != 16) return false;
    const auto end = lookup_offset(left);
    return end == lookup_offset(right) &&
        std::equal(left.resourceMeta.begin(), left.resourceMeta.begin() + 8, right.resourceMeta.begin()) &&
        std::equal(left.resourceMeta.begin() + 12, left.resourceMeta.end(), right.resourceMeta.begin() + 12) &&
        std::equal(left.resource.begin(), left.resource.begin() + static_cast<std::ptrdiff_t>(end), right.resource.begin());
}

// Some authoring tools retain the complete stock heap and append a new program
// array whose initial records are exact copies of the stock records. Stock-only
// aliases can still address those programs, without importing any bytes. This
// optional path recognises the pinned game's 168-byte records; an unfamiliar
// layout simply cannot use it, rather than failing the ordinary alias merge.
bool preserves_stock(const Table& retained, const Table& stock) {
    if (stock.resourceMeta.size() != 16) return false;
    try {
        (void)lookup_offset(retained);
        (void)lookup_offset(stock);
        const auto stockSize = read<std::uint32_t>(stock.resourceMeta, 0);
        const auto keptSize = read<std::uint32_t>(retained.resourceMeta, 0);
        const auto count = read<std::uint32_t>(stock.resource, 0);
        const auto keptCount = read<std::uint32_t>(retained.resource, 0);
        const auto start = read<std::uint64_t>(stock.resource, 4);
        const auto keptStart = read<std::uint64_t>(retained.resource, 4);
        const auto recordBytes = static_cast<std::uint64_t>(count) * 168;
        if (!count || count > keptCount || stockSize < 12 || keptSize < stockSize ||
            start < 12 || start > stockSize || recordBytes > stockSize - start ||
            keptStart < 12 || keptStart > keptSize || recordBytes > keptSize - keptStart ||
            static_cast<std::uint64_t>(keptCount) * 168 > keptSize - keptStart ||
            !std::equal(stock.resourceMeta.begin() + 12, stock.resourceMeta.end(), retained.resourceMeta.begin() + 12) ||
            !std::equal(stock.resource.begin() + 12, stock.resource.begin() + stockSize, retained.resource.begin() + 12) ||
            !std::equal(stock.resource.begin() + static_cast<std::ptrdiff_t>(start),
                        stock.resource.begin() + static_cast<std::ptrdiff_t>(start + recordBytes),
                        retained.resource.begin() + static_cast<std::ptrdiff_t>(keptStart))) return false;
        const auto relocations = [](const Table& table, std::uint32_t size) {
            const auto bytes = read<std::uint32_t>(table.resourceMeta, 4);
            if (bytes % 4) throw std::runtime_error("relocation layout");
            std::vector<std::uint32_t> fields;
            for (std::size_t at = 0; at < bytes; at += 4) {
                const auto field = read<std::uint32_t>(table.resource, static_cast<std::size_t>(size) + at);
                if (field > size || size - field < 8 || read<std::uint64_t>(table.resource, field) > size)
                    throw std::runtime_error("relocation bounds");
                fields.push_back(field);
            }
            std::ranges::sort(fields);
            if (std::adjacent_find(fields.begin(), fields.end()) != fields.end())
                throw std::runtime_error("duplicate relocation");
            return fields;
        };
        const auto fields = relocations(stock, stockSize);
        const auto keptFields = relocations(retained, keptSize);
        if (!std::binary_search(fields.begin(), fields.end(), 4U) ||
            !std::binary_search(keptFields.begin(), keptFields.end(), 4U)) return false;
        for (const auto field : fields) {
            if (!std::binary_search(keptFields.begin(), keptFields.end(), field)) return false;
            if (field >= start && field < start + recordBytes) {
                const auto copied = static_cast<std::uint32_t>(keptStart + field - start);
                if (!std::binary_search(keptFields.begin(), keptFields.end(), copied)) return false;
            }
        }
        // Additional fixups in either preserved region would reinterpret its
        // unchanged bytes as pointers, so membership must agree both ways.
        for (const auto field : keptFields) {
            if (field < stockSize && !std::binary_search(fields.begin(), fields.end(), field)) return false;
            if (field >= keptStart && field < keptStart + recordBytes) {
                const auto original = static_cast<std::uint32_t>(start + field - keptStart);
                if (!std::binary_search(fields.begin(), fields.end(), original)) return false;
            }
        }
        struct Array { std::size_t count, pointer, stride; };
        constexpr Array arrays[]{{0x0c,0x10,16},{0x18,0x1c,8},{0x30,0x34,16},
                                {0x3c,0x40,6},{0x48,0x4c,8},{0x54,0x58,2},{0x60,0x64,8}};
        for (std::uint32_t i = 0; i < count; ++i) for (const auto& array : arrays) {
            const auto record = static_cast<std::size_t>(start + i * 168ULL);
            const auto items = read<std::uint32_t>(stock.resource, record + array.count);
            const auto pointer = read<std::uint64_t>(stock.resource, record + array.pointer);
            if (pointer > stockSize || static_cast<std::uint64_t>(items) * array.stride > stockSize - pointer ||
                (items && !std::binary_search(fields.begin(), fields.end(), static_cast<std::uint32_t>(record + array.pointer)))) return false;
        }
        return true;
    } catch (const std::exception&) { return false; }
}

ProgramRows read_program_rows(const Table& table) {
    auto offset = lookup_offset(table);
    const auto count = read<std::uint32_t>(table.resource, offset);
    offset += 4;
    ProgramRows rows;
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto key = read<std::uint64_t>(table.resource, offset);
        const auto values = read<std::uint32_t>(table.resource, offset + 8);
        offset += 12;
        if (!values || values > 4096 ||
            static_cast<std::uint64_t>(values) * 4 > table.resource.size() - offset)
            throw std::runtime_error("Program lookup rows are invalid");
        auto& row = rows[key];
        row.reserve(values);
        for (std::uint32_t value = 0; value < values; ++value) {
            row.push_back(read<std::uint32_t>(table.resource, offset));
            offset += 4;
        }
    }
    if (offset != table.resource.size())
        throw std::runtime_error("Program lookup has trailing data");
    return rows;
}

TextureRows read_texture_rows(const Table& table) {
    if (table.resource.size() % 16)
        throw std::runtime_error("Texture lookup does not have its native layout");
    TextureRows rows;
    for (std::size_t offset = 0; offset < table.resource.size(); offset += 16)
        rows.emplace(read<std::uint64_t>(table.resource, offset),
                     read<std::uint64_t>(table.resource, offset + 8));
    return rows;
}

} // namespace

bool preserves_stock_programs(const Table& candidate, const Table& stock) {
    return preserves_stock(candidate, stock);
}
bool same_program_graph(const Table& candidate, const Table& other) {
    try { return same_graph(candidate, other); }
    catch (const std::exception&) { return false; }
}

bool only_stock_aliases(const Table& candidate, const Table& stock) {
    try {
        if (!same_graph(candidate, stock)) return false;
        const auto rows = read_program_rows(candidate);
        const auto original = read_program_rows(stock);
        const auto count = read<std::uint32_t>(stock.resource, 0);
        for (const auto& [key, values] : original) {
            const auto at = rows.find(key);
            if (at == rows.end() || at->second != values) return false;
        }
        for (const auto& [key, values] : rows) {
            (void)key;
            if (std::any_of(values.begin(), values.end(), [&](auto index) { return index >= count; })) return false;
        }
        return true;
    } catch (const std::exception&) { return false; }
}

MergedTable merge_program_lookup(const Table& base, const std::span<const Table> edits, const Table& stock) {
    const auto start = lookup_offset(base);
    const auto records = read<std::uint32_t>(base.resource, 0); // the base blob's record count
    auto rows = read_program_rows(base);
    const auto baseRows = rows;

    const bool stockPreserved = preserves_stock(base, stock);
    const auto stockRecords = stockPreserved ? read<std::uint32_t>(stock.resource, 0) : 0;
    MergedTable merged;
    for (auto at = rows.begin(); at != rows.end();) {
        if (std::any_of(at->second.begin(), at->second.end(), [&](auto index) { return index >= records; })) {
            at = rows.erase(at);
            ++merged.outside;
            ++merged.removed;
        } else ++at;
    }
    for (const auto& edit : edits) {
        try {
            // Indices have meaning only within their owning graph. Keep the selected
            // bundle's heap and relocations verbatim: importing another graph can
            // make the game instantiate programs before their compiled files load.
            const bool sameGraph = same_graph(base, edit);
            const bool stockGraph = stockPreserved && (same_graph(stock, edit) || preserves_stock(edit, stock));
            const auto available = sameGraph ? records : stockRecords;
            for (const auto& [key, values] : read_program_rows(edit)) {
                if (const auto held = baseRows.find(key); held != baseRows.end()) {
                    if (held->second != values) ++merged.conflicts;
                    continue;
                }
                if ((!sameGraph && !stockGraph) || std::any_of(values.begin(), values.end(), [&](std::uint32_t index) { return index >= available; })) {
                    ++merged.outside;
                    continue;
                }
                const auto [row, inserted] = rows.try_emplace(key, values);
                if (inserted) ++merged.added;
                else if (row->second != values) ++merged.conflicts;
            }
        } catch (const std::exception&) {
            // A malformed edit is not permission to discard a verified graph.
            ++merged.outside;
        }
    }

    merged.resource.assign(base.resource.begin(),
                           base.resource.begin() + static_cast<std::ptrdiff_t>(start));
    append(merged.resource, static_cast<std::uint32_t>(rows.size()));
    for (const auto& [key, values] : rows) {
        append(merged.resource, key);
        append(merged.resource, static_cast<std::uint32_t>(values.size()));
        for (const auto value : values) append(merged.resource, value);
    }
    merged.resourceMeta.assign(base.resourceMeta.begin(), base.resourceMeta.end());
    const auto size = static_cast<std::uint32_t>(merged.resource.size() - start);
    std::memcpy(merged.resourceMeta.data() + 8, &size, sizeof(size));
    return merged;
}

MergedTable merge_texture_lookup(const Table& base, const std::span<const Table> edits) {
    auto rows = read_texture_rows(base);
    const auto baseRows = rows;

    MergedTable merged;
    for (const auto& edit : edits) {
        for (const auto& [key, value] : read_texture_rows(edit)) {
            if (baseRows.contains(key)) continue;
            const auto [row, inserted] = rows.try_emplace(key, value);
            if (inserted) ++merged.added;
            else if (row->second != value) ++merged.conflicts;
        }
    }

    merged.resource.reserve(rows.size() * 16);
    for (const auto& [key, value] : rows) {
        append(merged.resource, key);
        append(merged.resource, value);
    }
    merged.resourceMeta.assign(base.resourceMeta.begin(), base.resourceMeta.end());
    return merged;
}

} // namespace dingosdk::frostbite::shader
