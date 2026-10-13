#include "shader_lookup.h"

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

MergedTable merge_program_lookup(const Table& base, const std::span<const Table> edits) {
    const auto start = lookup_offset(base);
    auto rows = read_program_rows(base);
    const auto baseRows = rows;

    MergedTable merged;
    for (const auto& edit : edits) {
        for (const auto& [key, values] : read_program_rows(edit)) {
            if (baseRows.contains(key)) continue;
            const auto [row, inserted] = rows.try_emplace(key, values);
            if (inserted) ++merged.added;
            else if (row->second != values) ++merged.conflicts;
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
