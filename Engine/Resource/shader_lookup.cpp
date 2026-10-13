#include "shader_lookup.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
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

namespace {
constexpr std::size_t programStride = 168;
struct ProgramBlob {
    Table source;
    std::span<const std::byte> payload;
    std::uint64_t records{};
    std::uint32_t count{};
    std::vector<std::uint32_t> relocations;
};
ProgramBlob program_blob(const Table& source) {
    const auto end = lookup_offset(source);
    const auto size = read<std::uint32_t>(source.resourceMeta, 0);
    if ((end - size) % 4) throw std::runtime_error("Program relocations are not aligned");
    ProgramBlob blob{source, source.resource.first(size)};
    blob.count = read<std::uint32_t>(blob.payload, 0);
    blob.records = read<std::uint64_t>(blob.payload, 4);
    if (blob.records > size || blob.count > (size - blob.records) / programStride)
        throw std::runtime_error("Program records exceed their payload");
    for (auto at = std::size_t{size}; at < end; at += 4) {
        const auto pointer = read<std::uint32_t>(source.resource, at);
        if (read<std::uint64_t>(blob.payload, pointer) > size)
            throw std::runtime_error("Program relocation points outside its payload");
        blob.relocations.push_back(pointer);
    }
    return blob;
}
struct ProgramArray { std::size_t pointer, stride; };
constexpr std::array programArrays{
    ProgramArray{0x10, 16}, ProgramArray{0x1c, 8}, ProgramArray{0x28, 8}, ProgramArray{0x34, 16},
    ProgramArray{0x40, 6}, ProgramArray{0x4c, 8}, ProgramArray{0x58, 2},
    ProgramArray{0x64, 8}, ProgramArray{0x70, 8}, ProgramArray{0x80, 16}, ProgramArray{0x8c, 32}
};
std::size_t record_at(const ProgramBlob& blob, std::uint32_t index) {
    if (index >= blob.count) throw std::runtime_error("Material references an absent shader program");
    return static_cast<std::size_t>(blob.records) + index * programStride;
}
std::span<const std::byte> program_array(const ProgramBlob& blob, std::size_t record, ProgramArray array) {
    const auto count = read<std::uint32_t>(blob.payload, record + array.pointer - 4);
    const auto pointer = read<std::uint64_t>(blob.payload, record + array.pointer);
    if (pointer > blob.payload.size() || count > (blob.payload.size() - pointer) / array.stride)
        throw std::runtime_error("Shader program array exceeds its payload");
    return blob.payload.subspan(static_cast<std::size_t>(pointer), count * array.stride);
}
bool same_program(const ProgramBlob& a, std::uint32_t ai, const ProgramBlob& b, std::uint32_t bi) {
    const auto aa = record_at(a, ai), ba = record_at(b, bi);
    for (std::size_t at = 0; at < programStride; ++at) {
        if (std::ranges::any_of(programArrays, [&](auto array) {
                return at >= array.pointer && at < array.pointer + 8; })) continue;
        if (a.payload[aa + at] != b.payload[ba + at]) return false;
    }
    for (const auto array : programArrays)
        if (!std::ranges::equal(program_array(a, aa, array), program_array(b, ba, array))) return false;
    return true;
}
std::uint32_t native_offset(std::size_t offset) {
    if (offset > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("Merged program database exceeds the native address range");
    return static_cast<std::uint32_t>(offset);
}
} // namespace

MergedTable merge_program_lookup(const Table& base, const std::span<const Table> edits) {
    std::vector<ProgramBlob> blobs{program_blob(base)};
    for (const auto& edit : edits) blobs.push_back(program_blob(edit));
    struct Reference { std::size_t blob; std::uint32_t program; };
    std::map<std::uint64_t, std::vector<Reference>> references;
    auto rows = read_program_rows(base);
    for (const auto& [key, values] : rows) for (const auto index : values) {
        record_at(blobs.front(), index);
        references[key].push_back({0, index});
    }
    const auto baseRows = rows;
    MergedTable merged;
    for (std::size_t b = 1; b < blobs.size(); ++b) {
        for (const auto& [key, values] : read_program_rows(blobs[b].source)) {
            for (const auto index : values) record_at(blobs[b], index);
            if (baseRows.contains(key)) continue;
            if (const auto previous = references.find(key); previous != references.end()) {
                bool same = previous->second.size() == values.size();
                for (std::size_t i = 0; same && i < values.size(); ++i) {
                    const auto ref = previous->second[i];
                    same = same_program(blobs[ref.blob], ref.program, blobs[b], values[i]);
                }
                if (!same) ++merged.conflicts;
                continue;
            }
            for (const auto index : values) references[key].push_back({b, index});
            ++merged.added;
        }
    }
    // Preserve the exact native prefix when edits only add aliases to stock programs.
    // Private programs need their own records and relocated backing data as well.
    using Source = std::pair<std::size_t, std::uint32_t>;
    std::map<Source, std::uint32_t> remapped;
    std::vector<Source> additions;
    for (const auto& [key, refs] : references) {
        auto& row = rows[key];
        row.clear();
        for (const auto ref : refs) {
            auto index = ref.program;
            if (ref.blob && (index >= blobs.front().count ||
                    !same_program(blobs.front(), index, blobs[ref.blob], index))) {
                const Source source{ref.blob, index};
                auto [entry, inserted] = remapped.try_emplace(source,
                    native_offset(std::size_t{blobs.front().count} + additions.size()));
                if (inserted) additions.push_back(source);
                index = entry->second;
            }
            row.push_back(index);
        }
    }
    merged.resourceMeta.assign(base.resourceMeta.begin(), base.resourceMeta.end());
    if (additions.empty()) {
        merged.resource.assign(base.resource.begin(), base.resource.begin() + lookup_offset(base));
    } else {
        merged.resource.assign(blobs.front().payload.begin(), blobs.front().payload.end());
        auto relocations = blobs.front().relocations;
        std::map<std::size_t, std::size_t> offsets;
        for (const auto& [b, index] : additions) {
            if (offsets.contains(b)) continue;
            merged.resource.resize((merged.resource.size() + 15) & ~std::size_t{15});
            const auto offset = merged.resource.size();
            offsets[b] = offset;
            const auto& blob = blobs[b];
            // Copy the complete backing graph, including opaque resource bindings.
            // This keeps runtime-mutated GUID arrays separate across mods.
            merged.resource.insert(merged.resource.end(), blob.payload.begin(), blob.payload.end());
            for (const auto pointer : blob.relocations) {
                const auto destination = native_offset(offset + pointer);
                const auto target = read<std::uint64_t>(blob.payload, pointer) + offset;
                std::memcpy(merged.resource.data() + destination, &target, 8);
                relocations.push_back(destination);
            }
        }
        merged.resource.resize((merged.resource.size() + 7) & ~std::size_t{7});
        const auto table = static_cast<std::uint64_t>(merged.resource.size());
        const auto count = native_offset(std::size_t{blobs.front().count} + additions.size());
        merged.resource.resize(native_offset(static_cast<std::size_t>(table) + std::size_t{count} * programStride));
        const auto copy_record = [&](const ProgramBlob& blob, std::uint32_t index,
                                     std::size_t offset, std::uint32_t destination) {
            const auto source = record_at(blob, index);
            const auto target = static_cast<std::size_t>(table) + destination * programStride;
            std::memcpy(merged.resource.data() + target, merged.resource.data() + offset + source, programStride);
            for (const auto pointer : blob.relocations)
                if (pointer >= source && pointer < source + programStride)
                    relocations.push_back(native_offset(target + pointer - source));
        };
        for (std::uint32_t i = 0; i < blobs.front().count; ++i) copy_record(blobs.front(), i, 0, i);
        for (const auto& source : additions)
            copy_record(blobs[source.first], source.second, offsets.at(source.first), remapped.at(source));
        std::memcpy(merged.resource.data(), &count, 4);
        std::memcpy(merged.resource.data() + 4, &table, 8);
        relocations.push_back(4);
        std::ranges::sort(relocations);
        relocations.erase(std::ranges::unique(relocations).begin(), relocations.end());
        const auto size = native_offset(merged.resource.size());
        for (const auto pointer : relocations) append(merged.resource, pointer);
        const auto relocationSize = native_offset(relocations.size() * 4);
        std::memcpy(merged.resourceMeta.data(), &size, 4);
        std::memcpy(merged.resourceMeta.data() + 4, &relocationSize, 4);
    }
    const auto lookup = merged.resource.size();
    append(merged.resource, native_offset(rows.size()));
    for (const auto& [key, values] : rows) {
        append(merged.resource, key);
        append(merged.resource, native_offset(values.size()));
        for (const auto value : values) append(merged.resource, value);
    }
    const auto size = native_offset(merged.resource.size() - lookup);
    std::memcpy(merged.resourceMeta.data() + 8, &size, 4);
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
