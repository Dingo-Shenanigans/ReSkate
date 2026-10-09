#include "binary_bundle.h"

#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>

namespace dingosdk::frostbite {
namespace {

constexpr std::uint32_t salt = 0x7065636E;
constexpr std::uint32_t standardMagic = 0xED1CEDB8;

std::uint64_t encoded_identifier(const CasIdentifier& value) {
    return (static_cast<std::uint64_t>(value.patch ? 1 : 0) << 48U) |
           (static_cast<std::uint64_t>(value.installChunk) << 16U) | value.archive;
}

CasIdentifier decoded_identifier(std::uint64_t value) {
    return {(value & (0xFFULL << 48U)) != 0,
            static_cast<std::uint32_t>((value >> 16U) & 0xFFFFFFFFULL),
            static_cast<std::uint16_t>(value & 0xFFFFU)};
}

std::size_t checked_add(std::size_t left, std::size_t right, const char* label) {
    if (right > std::numeric_limits<std::size_t>::max() - left) throw std::overflow_error(label);
    return left + right;
}

std::string string_at(std::span<const std::byte> data, std::size_t offset) {
    if (offset >= data.size()) throw std::out_of_range("Binary bundle string offset is invalid");
    const auto* begin = reinterpret_cast<const char*>(data.data() + offset);
    const auto* end = static_cast<const char*>(std::memchr(begin, 0, data.size() - offset));
    if (!end) throw std::runtime_error("Binary bundle string is unterminated");
    return {begin, end};
}

} // namespace

BundleRegion read_bundle_region(const std::span<const std::byte> data, const std::size_t offset) {
    if (offset > data.size() || data.size() - offset < 0x24)
        throw std::out_of_range("Bundle region header is truncated");
    BinaryReader reader(data.subspan(offset));
    const auto metadataOffset = reader.i32(Endian::big);
    const auto metadataSize = reader.i32(Endian::big);
    const auto locationOffset = reader.u32(Endian::big);
    const auto total = reader.i32(Endian::big);
    const auto dataOffset = reader.u32(Endian::big);
    const auto resourceOffset = reader.u32(Endian::big);
    const auto chunkOffset = reader.u32(Endian::big);
    static_cast<void>(reader.u32(Endian::big));
    const auto repeatedTotal = reader.i32(Endian::big);
    if (metadataOffset < 0 || metadataSize < 0 || total < 0 || total != repeatedTotal ||
        resourceOffset != dataOffset || chunkOffset != dataOffset)
        throw std::invalid_argument("Bundle region header is invalid");
    const auto count = static_cast<std::size_t>(total);
    if (locationOffset > reader.size() || count > reader.size() - locationOffset || dataOffset > reader.size())
        throw std::out_of_range("Bundle region tables exceed the region");
    if ((metadataOffset == 0) != (metadataSize == 0))
        throw std::invalid_argument("Bundle region inline metadata range is invalid");

    BundleRegion result;
    if (metadataSize != 0) {
        const auto begin = static_cast<std::size_t>(metadataOffset);
        const auto size = static_cast<std::size_t>(metadataSize);
        if (begin > reader.size() || size > reader.size() - begin)
            throw std::out_of_range("Bundle inline manifest exceeds the region");
        result.inlineManifest.assign(data.begin() + static_cast<std::ptrdiff_t>(offset + begin),
                                     data.begin() + static_cast<std::ptrdiff_t>(offset + begin + size));
    }

    BinaryReader files(data.subspan(offset + dataOffset));
    CasIdentifier current;
    bool hasCurrent{};
    result.files.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto flag = std::to_integer<std::uint8_t>(data[offset + locationOffset + index]);
        if (flag == 1) {
            const auto encoded = files.u32(Endian::big);
            current = {(encoded & 0x00FF0000U) != 0, (encoded >> 8U) & 0xFFU,
                       static_cast<std::uint16_t>(encoded & 0xFFU)};
            hasCurrent = true;
        } else if (flag != 0) {
            current = decoded_identifier(files.u64(Endian::big));
            hasCurrent = true;
        }
        if (!hasCurrent) throw std::invalid_argument("First bundle file has no CAS identifier");
        const auto fileOffset = files.u32(Endian::big);
        const auto fileSize = files.u32(Endian::big);
        result.files.push_back({current, fileOffset, fileSize});
    }
    return result;
}

std::vector<std::byte> write_bundle_region(const std::span<const BundleFileInfo> files,
                                           const std::span<const std::byte> inlineManifest) {
    BinaryWriter fileTable;
    std::vector<std::byte> flags;
    flags.reserve(files.size());
    std::optional<CasIdentifier> previous;
    for (const auto& file : files) {
        if (previous && *previous == file.location) {
            flags.push_back(std::byte{});
        } else {
            flags.push_back(std::byte{0x80});
            fileTable.u64(encoded_identifier(file.location), Endian::big);
            previous = file.location;
        }
        fileTable.u32(file.offset, Endian::big);
        fileTable.u32(file.size, Endian::big);
    }
    if (!flags.empty()) flags.front() |= std::byte{0x04};

    constexpr std::size_t dataOffset = 0x24;
    const auto metadataOffset = inlineManifest.empty()
        ? std::size_t{0}
        : checked_add(dataOffset, fileTable.data().size(), "Bundle region size overflow");
    const auto locationOffset = inlineManifest.empty()
        ? checked_add(dataOffset, fileTable.data().size(), "Bundle region size overflow")
        : checked_add(metadataOffset, inlineManifest.size(), "Bundle region size overflow");
    const auto totalSize = checked_add(locationOffset, flags.size(), "Bundle region size overflow");
    if (totalSize > std::numeric_limits<std::uint32_t>::max() ||
        files.size() > std::numeric_limits<std::int32_t>::max())
        throw std::overflow_error("Bundle region exceeds native limits");

    BinaryWriter output;
    output.seek(totalSize);
    output.seek(0);
    output.i32(static_cast<std::int32_t>(metadataOffset), Endian::big);
    output.i32(static_cast<std::int32_t>(inlineManifest.size()), Endian::big);
    output.u32(static_cast<std::uint32_t>(locationOffset), Endian::big);
    output.i32(static_cast<std::int32_t>(files.size()), Endian::big);
    output.u32(static_cast<std::uint32_t>(dataOffset), Endian::big);
    output.u32(static_cast<std::uint32_t>(dataOffset), Endian::big);
    output.u32(static_cast<std::uint32_t>(dataOffset), Endian::big);
    output.u32(0, Endian::big);
    output.i32(static_cast<std::int32_t>(files.size()), Endian::big);
    output.seek(dataOffset);
    output.bytes(fileTable.data());
    if (!inlineManifest.empty()) output.bytes(inlineManifest);
    output.bytes(flags);
    return output.take();
}

BinaryBundle read_binary_bundle(const std::span<const std::byte> data) {
    BinaryReader reader(data);
    if (reader.remaining() < 36) throw std::invalid_argument("Binary bundle header is truncated");
    const auto declaredSize = reader.u32(Endian::big);
    if (declaredSize != data.size() - 4)
        throw std::invalid_argument("Binary bundle size does not match its stream");
    const auto magicOffset = reader.position();
    auto magic = reader.u32(Endian::big) ^ salt;
    auto endian = Endian::big;
    if (magic != standardMagic) {
        reader.seek(magicOffset);
        magic = reader.u32(Endian::little) ^ salt;
        endian = Endian::little;
    }
    if (magic != standardMagic) throw std::invalid_argument("Binary bundle magic is unsupported");
    const auto total = reader.u32(endian);
    const auto ebxCount = reader.u32(endian);
    const auto resourceCount = reader.u32(endian);
    const auto chunkCount = reader.u32(endian);
    if (static_cast<std::uint64_t>(ebxCount) + resourceCount + chunkCount != total)
        throw std::invalid_argument("Binary bundle asset counts do not add up");
    const auto strings = static_cast<std::size_t>(reader.u32(endian)) + 4;
    const auto metadataOffset = reader.u32(endian);
    const auto metadataSize = reader.u32(endian);
    if (strings > data.size()) throw std::out_of_range("Binary bundle string table is out of range");
    const auto tables = std::uint64_t{ebxCount} * 28 + std::uint64_t{resourceCount} * 56 + std::uint64_t{chunkCount} * 44; // SHA-1 plus entry bytes
    if (tables > reader.remaining()) throw std::out_of_range("Binary bundle asset counts exceed the file");

    std::vector<Sha1> hashes(total);
    for (auto& hash : hashes) hash = reader.sha1();
    BinaryBundle result;
    result.ebx.resize(ebxCount);
    for (std::uint32_t index = 0; index < ebxCount; ++index) {
        auto& asset = result.ebx[index];
        asset.kind = AssetKind::ebx;
        asset.sha1 = hashes[index];
        const auto nameOffset = reader.u32(endian);
        asset.originalSize = reader.u32(endian);
        asset.name = string_at(data, checked_add(strings, nameOffset, "Binary bundle string offset overflow"));
    }
    result.resources.resize(resourceCount);
    for (std::uint32_t index = 0; index < resourceCount; ++index) {
        auto& asset = result.resources[index];
        asset.kind = AssetKind::resource;
        asset.sha1 = hashes[ebxCount + index];
        const auto nameOffset = reader.u32(endian);
        asset.originalSize = reader.u32(endian);
        asset.name = string_at(data, checked_add(strings, nameOffset, "Binary bundle string offset overflow"));
    }
    for (auto& asset : result.resources) asset.resourceType = reader.u32(endian);
    for (auto& asset : result.resources) asset.resourceMeta = reader.bytes(16);
    for (auto& asset : result.resources) asset.resourceId = reader.u64(endian);
    result.chunks.resize(chunkCount);
    for (std::uint32_t index = 0; index < chunkCount; ++index) {
        auto& asset = result.chunks[index];
        asset.kind = AssetKind::chunk;
        asset.sha1 = hashes[ebxCount + resourceCount + index];
        asset.guid = reader.guid(endian);
        asset.name = asset.guid.string();
        asset.logicalOffset = reader.u32(endian);
        asset.logicalSize = reader.u32(endian);
        asset.originalSize = (asset.logicalOffset & 0xFFFFU) | asset.logicalSize;
    }
    if (metadataSize) {
        const auto start = checked_add(std::size_t{4}, metadataOffset, "Binary bundle metadata offset overflow");
        if (start > data.size() || metadataSize > data.size() - start)
            throw std::out_of_range("Binary bundle metadata is out of range");
        result.chunkMetadata.assign(data.begin() + static_cast<std::ptrdiff_t>(start),
                                    data.begin() + static_cast<std::ptrdiff_t>(start + metadataSize));
    }
    return result;
}

std::vector<std::byte> write_binary_bundle(const BinaryBundle& bundle) {
    const auto total = checked_add(checked_add(bundle.ebx.size(), bundle.resources.size(),
                                               "Binary bundle count overflow"),
                                   bundle.chunks.size(), "Binary bundle count overflow");
    if (total > std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("Binary bundle has too many assets");
    std::vector<std::byte> strings;
    std::vector<std::uint32_t> nameOffsets;
    nameOffsets.reserve(bundle.ebx.size() + bundle.resources.size());
    const auto appendName = [&](const std::string& name) {
        if (strings.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::overflow_error("Binary bundle string table is too large");
        nameOffsets.push_back(static_cast<std::uint32_t>(strings.size()));
        strings.insert(strings.end(), reinterpret_cast<const std::byte*>(name.data()),
                       reinterpret_cast<const std::byte*>(name.data() + name.size()));
        strings.push_back(std::byte{});
    };
    for (const auto& asset : bundle.ebx) appendName(asset.name);
    for (const auto& asset : bundle.resources) appendName(asset.name);

    constexpr std::size_t shaStart = 36;
    const auto ebxStart = checked_add(shaStart, total * 20, "Binary bundle size overflow");
    const auto resourceStart = checked_add(ebxStart, bundle.ebx.size() * 8, "Binary bundle size overflow");
    const auto resourceTypeStart = checked_add(resourceStart, bundle.resources.size() * 8, "Binary bundle size overflow");
    const auto resourceMetaStart = checked_add(resourceTypeStart, bundle.resources.size() * 4, "Binary bundle size overflow");
    const auto resourceIdStart = checked_add(resourceMetaStart, bundle.resources.size() * 16, "Binary bundle size overflow");
    const auto chunkStart = checked_add(resourceIdStart, bundle.resources.size() * 8, "Binary bundle size overflow");
    const auto stringStart = checked_add(chunkStart, bundle.chunks.size() * 24, "Binary bundle size overflow");
    const auto metadataStart = checked_add(stringStart, strings.size(), "Binary bundle size overflow");
    const auto length = checked_add(metadataStart, bundle.chunkMetadata.size(), "Binary bundle size overflow");
    if (length > std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("Binary bundle exceeds 4 GiB");

    BinaryWriter writer;
    writer.seek(length);
    writer.seek(0);
    writer.u32(static_cast<std::uint32_t>(length - 4), Endian::big);
    writer.u32(standardMagic ^ salt);
    writer.u32(static_cast<std::uint32_t>(total));
    writer.u32(static_cast<std::uint32_t>(bundle.ebx.size()));
    writer.u32(static_cast<std::uint32_t>(bundle.resources.size()));
    writer.u32(static_cast<std::uint32_t>(bundle.chunks.size()));
    writer.u32(static_cast<std::uint32_t>(stringStart - 4));
    writer.u32(bundle.chunkMetadata.empty() ? 0U : static_cast<std::uint32_t>(metadataStart - 4));
    writer.u32(static_cast<std::uint32_t>(bundle.chunkMetadata.size()));
    for (const auto& asset : bundle.ebx) writer.sha1(asset.sha1);
    for (const auto& asset : bundle.resources) writer.sha1(asset.sha1);
    for (const auto& asset : bundle.chunks) writer.sha1(asset.sha1);
    for (std::size_t index = 0; index < bundle.ebx.size(); ++index) {
        writer.u32(nameOffsets[index]);
        writer.u32(static_cast<std::uint32_t>(bundle.ebx[index].originalSize));
    }
    for (std::size_t index = 0; index < bundle.resources.size(); ++index) {
        writer.u32(nameOffsets[bundle.ebx.size() + index]);
        writer.u32(static_cast<std::uint32_t>(bundle.resources[index].originalSize));
    }
    for (const auto& asset : bundle.resources) writer.u32(asset.resourceType);
    for (const auto& asset : bundle.resources) {
        if (asset.resourceMeta.size() != 16)
            throw std::invalid_argument("Binary bundle resource metadata must be 16 bytes");
        writer.bytes(asset.resourceMeta);
    }
    for (const auto& asset : bundle.resources) writer.u64(asset.resourceId);
    for (const auto& asset : bundle.chunks) {
        writer.guid(asset.guid);
        writer.u32(asset.logicalOffset);
        writer.u32(asset.logicalSize);
    }
    writer.bytes(strings);
    writer.bytes(bundle.chunkMetadata);
    return writer.take();
}

} // namespace dingosdk::frostbite
