#include "toc.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace dingosdk::frostbite {

namespace {

constexpr std::uint32_t fnvPrime = 0x01000193U;
constexpr std::uint32_t compressedNames = 4;

std::size_t align_up(const std::size_t value, const std::size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

std::vector<std::byte> lower_bytes(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return {reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

std::array<std::byte, 16> chunk_key(const Guid& guid) {
    return guid.bytes;
}

template <typename Key>
struct PerfectHash {
    std::vector<std::int32_t> map;
    std::vector<std::size_t> order;
};

template <typename Key>
PerfectHash<Key> perfect_hash(const std::vector<Key>& keys) {
    const auto size = keys.size();
    PerfectHash<Key> result;
    result.map.assign(size, -1);
    result.order.assign(size, std::numeric_limits<std::size_t>::max());
    if (size == 0) return result;
    std::vector<std::size_t> keyOrder(size);
    std::iota(keyOrder.begin(), keyOrder.end(), 0);
    std::ranges::sort(keyOrder, [&keys](const auto left, const auto right) { return keys[left] < keys[right]; });
    for (std::size_t index = 1; index < keyOrder.size(); ++index) {
        if (keys[keyOrder[index - 1]] == keys[keyOrder[index]]) {
            throw std::invalid_argument("TOC perfect-hash keys must be unique");
        }
    }
    std::vector<std::vector<std::size_t>> buckets(size);
    for (std::size_t index = 0; index < size; ++index) {
        buckets[toc_hash(keys[index]) % size].push_back(index);
    }
    std::stable_sort(buckets.begin(), buckets.end(), [](const auto& left, const auto& right) {
        return left.size() > right.size();
    });
    std::vector<bool> used(size);
    std::size_t bucketIndex{};
    while (bucketIndex < buckets.size() && buckets[bucketIndex].size() > 1) {
        const auto& bucket = buckets[bucketIndex];
        std::uint32_t seed = 1;
        std::vector<std::size_t> indices;
        for (;;) {
            indices.clear();
            bool collision{};
            for (const auto member : bucket) {
                const auto index = toc_hash(keys[member], seed) % size;
                if (used[index] || std::ranges::find(indices, index) != indices.end()) {
                    collision = true;
                    break;
                }
                indices.push_back(index);
            }
            if (!collision) break;
            if (seed == std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("could not build TOC perfect hash");
            ++seed;
        }
        result.map[toc_hash(keys[bucket.front()]) % size] = static_cast<std::int32_t>(seed);
        for (std::size_t index = 0; index < bucket.size(); ++index) {
            result.order[indices[index]] = bucket[index];
            used[indices[index]] = true;
        }
        ++bucketIndex;
    }
    std::size_t freeIndex{};
    while (bucketIndex < buckets.size() && !buckets[bucketIndex].empty()) {
        while (used[freeIndex]) freeIndex = (freeIndex + 1) % size;
        const auto member = buckets[bucketIndex].front();
        result.map[toc_hash(keys[member]) % size] = -static_cast<std::int32_t>(freeIndex) - 1;
        result.order[freeIndex] = member;
        used[freeIndex] = true;
        freeIndex = (freeIndex + 1) % size;
        ++bucketIndex;
    }
    return result;
}

template <typename Key>
std::optional<std::size_t> hash_lookup(const std::vector<std::int32_t>& map,
                                       const std::vector<Key>& orderedKeys,
                                       const Key& key) {
    if (map.empty() || map.size() != orderedKeys.size()) return std::nullopt;
    const auto value = map[toc_hash(key) % map.size()];
    const auto index = value < 0
        ? static_cast<std::size_t>(-value - 1)
        : static_cast<std::size_t>(toc_hash(key, static_cast<std::uint32_t>(value)) % map.size());
    if (index >= orderedKeys.size() || orderedKeys[index] != key) return std::nullopt;
    return index;
}

Guid guid_from_reversed_bytes(std::span<const std::byte> bytes) {
    if (bytes.size() != 16) throw std::invalid_argument("TOC GUID is truncated");
    Guid result;
    std::ranges::reverse_copy(bytes, result.bytes.begin());
    return result;
}

std::string c_string_at(const std::span<const std::byte> data, const std::size_t offset) {
    if (offset >= data.size()) throw std::out_of_range("TOC name offset is invalid");
    std::string value;
    for (auto index = offset; index < data.size(); ++index) {
        const auto byte = std::to_integer<std::uint8_t>(data[index]);
        if (byte == 0) return value;
        value.push_back(static_cast<char>(byte));
    }
    throw std::runtime_error("TOC name is unterminated");
}

void expect_table(const BinaryReader& reader, std::size_t count, std::size_t entrySize, const char* what) { // before sizing by a file's count
    if (count > reader.remaining() / entrySize) throw std::out_of_range(what);
}

class HuffmanNames final {
public:
    HuffmanNames(const std::span<const std::byte> data,
                 const std::size_t namesOffset,
                 const std::size_t namesCount,
                 const std::size_t tableOffset,
                 const std::size_t tableCount) {
        BinaryReader reader(data);
        reader.seek(namesOffset);
        expect_table(reader, namesCount, 4, "TOC name count exceeds the file");
        words_.reserve(namesCount);
        for (std::size_t index = 0; index < namesCount; ++index) words_.push_back(reader.u32(Endian::big));
        reader.seek(tableOffset);
        std::unordered_map<std::uint32_t, std::size_t> byValue;
        std::optional<std::size_t> left;
        std::uint32_t counter{};
        for (std::size_t index = 0; index < tableCount; ++index) {
            const auto value = reader.u32(Endian::big);
            auto found = byValue.find(value);
            std::size_t node;
            if (found == byValue.end()) {
                node = nodes_.size();
                nodes_.push_back({value});
                byValue.emplace(value, node);
            } else {
                node = found->second;
            }
            if (!left) {
                left = node;
                continue;
            }
            const auto parent = nodes_.size();
            nodes_.push_back({counter, *left, node});
            byValue.try_emplace(counter, parent);
            root_ = parent;
            ++counter;
            left.reset();
        }
        if (left) throw std::runtime_error("TOC Huffman tree has an unmatched branch");
    }

    [[nodiscard]] std::string decode(std::size_t bit) const {
        if (!root_) return {};
        const auto bitCount = words_.size() * 32;
        std::string result;
        while (bit < bitCount) {
            auto node = *root_;
            while (!nodes_[node].leaf()) {
                if (bit >= bitCount) throw std::runtime_error("TOC Huffman name exceeds its bitstream");
                const auto one = ((words_[bit >> 5U] >> (bit & 31U)) & 1U) != 0;
                node = one ? nodes_[node].right : nodes_[node].left;
                ++bit;
            }
            const auto character = static_cast<std::uint16_t>(~nodes_[node].value);
            if (character == 0) return result;
            if (character > 0x7F) throw std::runtime_error("TOC Huffman name is not ASCII");
            result.push_back(static_cast<char>(character));
        }
        throw std::runtime_error("TOC Huffman name is unterminated");
    }

private:
    struct Node {
        std::uint32_t value{};
        std::size_t left{std::numeric_limits<std::size_t>::max()};
        std::size_t right{std::numeric_limits<std::size_t>::max()};
        [[nodiscard]] bool leaf() const noexcept { return left == std::numeric_limits<std::size_t>::max(); }
    };
    std::vector<std::uint32_t> words_;
    std::vector<Node> nodes_;
    std::optional<std::size_t> root_;
};

}

std::uint32_t toc_hash(const std::span<const std::byte> key, std::uint32_t seed) noexcept {
    for (const auto byte : key) {
        const auto signedByte = static_cast<std::int8_t>(std::to_integer<std::uint8_t>(byte));
        seed = (seed * fnvPrime) ^ static_cast<std::uint32_t>(static_cast<std::int32_t>(signedByte));
    }
    return seed % fnvPrime;
}

std::vector<std::byte> write_patch_toc(const std::span<const TocBundle> bundles,
                                       const std::span<const TocChunk> chunks,
                                       const std::uint32_t flags) {
    if ((flags & compressedNames) != 0) throw std::invalid_argument("patch TOC writer uses uncompressed names");
    std::vector<std::vector<std::byte>> bundleKeys;
    bundleKeys.reserve(bundles.size());
    for (const auto& bundle : bundles) bundleKeys.push_back(lower_bytes(bundle.name));
    const auto bundleHash = perfect_hash(bundleKeys);

    std::vector<std::array<std::byte, 16>> chunkKeys;
    chunkKeys.reserve(chunks.size());
    for (const auto& chunk : chunks) chunkKeys.push_back(chunk_key(chunk.guid));
    const auto chunkHash = perfect_hash(chunkKeys);

    std::vector<std::byte> names;
    std::vector<std::uint32_t> nameOffsets(bundles.size());
    for (std::size_t orderedIndex = 0; orderedIndex < bundleHash.order.size(); ++orderedIndex) {
        const auto sourceIndex = bundleHash.order[orderedIndex];
        if (names.size() > std::numeric_limits<std::uint32_t>::max()) throw std::overflow_error("TOC names exceed 4 GiB");
        nameOffsets[orderedIndex] = static_cast<std::uint32_t>(names.size());
        const auto& name = bundles[sourceIndex].name;
        names.insert(names.end(), reinterpret_cast<const std::byte*>(name.data()),
                     reinterpret_cast<const std::byte*>(name.data() + name.size()));
        names.push_back(std::byte{});
    }

    std::vector<std::uint32_t> chunkWords;
    std::vector<std::byte> chunkTable;
    chunkTable.reserve(chunks.size() * 20);
    for (const auto sourceIndex : chunkHash.order) {
        const auto& chunk = chunks[sourceIndex];
        auto reversed = chunk_key(chunk.guid);
        std::ranges::reverse(reversed);
        chunkTable.insert(chunkTable.end(), reversed.begin(), reversed.end());
        BinaryWriter indexWriter;
        if (chunk.removed) {
            indexWriter.i32(-1, Endian::big);
        } else {
            if (chunkWords.size() > 0xFFFFFFU) throw std::overflow_error("TOC chunk data index exceeds 24 bits");
            indexWriter.u32((0x80U << 24U) | static_cast<std::uint32_t>(chunkWords.size()), Endian::big);
            const auto id = (static_cast<std::uint64_t>(chunk.location.patch ? 1 : 0) << 48U) |
                            (static_cast<std::uint64_t>(chunk.location.installChunk) << 16U) |
                            chunk.location.archive;
            chunkWords.push_back(static_cast<std::uint32_t>(id >> 32U));
            chunkWords.push_back(static_cast<std::uint32_t>(id));
            chunkWords.push_back(chunk.offset);
            chunkWords.push_back(chunk.size);
        }
        chunkTable.insert(chunkTable.end(), indexWriter.data().begin(), indexWriter.data().end());
    }

    constexpr std::size_t headerSize = 48;
    auto position = headerSize;
    const auto bundleHashOffset = position;
    position = align_up(position + bundles.size() * 4, 8);
    const auto bundleDataOffset = position;
    position = align_up(position + bundles.size() * 16, 4);
    const auto chunkHashOffset = position;
    position = align_up(position + chunks.size() * 4, 4);
    const auto chunkGuidOffset = position;
    position = align_up(position + chunkTable.size(), 4);
    const auto chunkDataOffset = position;
    position += chunkWords.size() * 4;
    const auto namesOffset = position;
    position = align_up(position + names.size(), 4);
    std::vector<std::uint64_t> regionOffsets(bundles.size());
    for (std::size_t orderedIndex = 0; orderedIndex < bundleHash.order.size(); ++orderedIndex) {
        regionOffsets[orderedIndex] = position;
        position = align_up(position + bundles[bundleHash.order[orderedIndex]].region.size(), 4);
    }
    if (position > std::numeric_limits<std::uint32_t>::max()) throw std::overflow_error("TOC exceeds 4 GiB");

    BinaryWriter writer;
    writer.seek(tocEnvelopeSize + position);
    writer.seek(0);
    writer.u8(0x00); writer.u8(0xD1); writer.u8(0xCE); writer.u8(0x01);
    writer.seek(tocEnvelopeSize);
    writer.u32(static_cast<std::uint32_t>(bundleHashOffset), Endian::big);
    writer.u32(static_cast<std::uint32_t>(bundleDataOffset), Endian::big);
    writer.i32(static_cast<std::int32_t>(bundles.size()), Endian::big);
    writer.u32(chunks.empty() ? 0U : static_cast<std::uint32_t>(chunkHashOffset), Endian::big);
    writer.u32(chunks.empty() ? 0U : static_cast<std::uint32_t>(chunkGuidOffset), Endian::big);
    writer.i32(static_cast<std::int32_t>(chunks.size()), Endian::big);
    writer.u32(static_cast<std::uint32_t>(chunkDataOffset), Endian::big);
    writer.u32(static_cast<std::uint32_t>(chunkDataOffset), Endian::big);
    writer.u32(static_cast<std::uint32_t>(namesOffset), Endian::big);
    writer.u32(static_cast<std::uint32_t>(chunkDataOffset), Endian::big);
    writer.i32(static_cast<std::int32_t>(chunkWords.size()), Endian::big);
    writer.u32(flags, Endian::big);
    writer.seek(tocEnvelopeSize + bundleHashOffset);
    for (const auto value : bundleHash.map) writer.i32(value, Endian::big);
    writer.seek(tocEnvelopeSize + bundleDataOffset);
    for (std::size_t orderedIndex = 0; orderedIndex < bundleHash.order.size(); ++orderedIndex) {
        const auto& bundle = bundles[bundleHash.order[orderedIndex]];
        if (bundle.region.size() > 0x3FFFFFFFU) throw std::overflow_error("TOC bundle region exceeds 30-bit size");
        writer.u32(nameOffsets[orderedIndex], Endian::big);
        writer.u32((static_cast<std::uint32_t>(bundle.loadFlag) << 30U) |
                   static_cast<std::uint32_t>(bundle.region.size()), Endian::big);
        writer.u64(regionOffsets[orderedIndex], Endian::big);
    }
    if (!chunks.empty()) {
        writer.seek(tocEnvelopeSize + chunkHashOffset);
        for (const auto value : chunkHash.map) writer.i32(value, Endian::big);
        writer.seek(tocEnvelopeSize + chunkGuidOffset);
        writer.bytes(chunkTable);
        writer.seek(tocEnvelopeSize + chunkDataOffset);
        for (const auto value : chunkWords) writer.u32(value, Endian::big);
    }
    writer.seek(tocEnvelopeSize + namesOffset);
    writer.bytes(names);
    for (std::size_t orderedIndex = 0; orderedIndex < bundleHash.order.size(); ++orderedIndex) {
        writer.seek(tocEnvelopeSize + regionOffsets[orderedIndex]);
        writer.bytes(bundles[bundleHash.order[orderedIndex]].region);
    }
    return writer.take();
}

TocDocument read_toc(const std::span<const std::byte> data,
                     const std::span<const std::byte> externalBundleData) {
    if (data.size() < tocEnvelopeSize + 48 || data[0] != std::byte{} || data[1] != std::byte{0xD1} ||
        data[2] != std::byte{0xCE} || data[3] != std::byte{0x01}) {
        throw std::invalid_argument("TOC envelope is invalid");
    }
    BinaryReader reader(data.subspan(tocEnvelopeSize));
    const auto bundleHashOffset = reader.u32(Endian::big);
    const auto bundleDataOffset = reader.u32(Endian::big);
    const auto bundleCount = reader.i32(Endian::big);
    const auto chunkHashOffset = reader.u32(Endian::big);
    const auto chunkGuidOffset = reader.u32(Endian::big);
    const auto chunkCount = reader.i32(Endian::big);
    static_cast<void>(reader.u32(Endian::big));
    static_cast<void>(reader.u32(Endian::big));
    const auto namesOffset = reader.u32(Endian::big);
    const auto chunkDataOffset = reader.u32(Endian::big);
    const auto dataCount = reader.i32(Endian::big);
    const auto flags = reader.u32(Endian::big);
    if (bundleCount < 0 || chunkCount < 0 || dataCount < 0) throw std::invalid_argument("TOC has negative table counts");
    std::optional<HuffmanNames> huffman;
    if ((flags & compressedNames) != 0) {
        const auto namesCount = reader.u32(Endian::big);
        const auto tableCount = reader.u32(Endian::big);
        const auto tableOffset = reader.u32(Endian::big);
        huffman.emplace(data.subspan(tocEnvelopeSize), namesOffset, namesCount, tableOffset, tableCount);
    }
    const auto bundlesSize = static_cast<std::size_t>(bundleCount);
    const auto chunksSize = static_cast<std::size_t>(chunkCount);
    TocDocument result;
    result.flags = flags;
    reader.seek(bundleHashOffset);
    expect_table(reader, bundlesSize, 4, "TOC bundle count exceeds the file");
    result.bundleHashMap.reserve(bundlesSize);
    for (std::size_t index = 0; index < bundlesSize; ++index) result.bundleHashMap.push_back(reader.i32(Endian::big));
    reader.seek(bundleDataOffset);
    expect_table(reader, bundlesSize, 16, "TOC bundle count exceeds the file");
    result.bundles.reserve(bundlesSize);
    for (std::size_t index = 0; index < bundlesSize; ++index) {
        const auto nameOffset = reader.u32(Endian::big);
        const auto sizeAndFlag = reader.u32(Endian::big);
        const auto regionOffset = reader.u64(Endian::big);
        const auto size = static_cast<std::size_t>(sizeAndFlag & 0x3FFFFFFFU);
        const auto loadFlag = static_cast<std::uint8_t>(sizeAndFlag >> 30U);
        const auto source = loadFlag == 1 ? data.subspan(tocEnvelopeSize) : externalBundleData;
        if (regionOffset > source.size() || size > source.size() - static_cast<std::size_t>(regionOffset)) {
            throw std::out_of_range("TOC bundle region exceeds the file");
        }
        TocBundle bundle;
        bundle.name = huffman ? huffman->decode(nameOffset)
                              : c_string_at(data.subspan(tocEnvelopeSize), namesOffset + nameOffset);
        bundle.loadFlag = loadFlag;
        const auto begin = source.begin() + static_cast<std::ptrdiff_t>(regionOffset);
        bundle.region.assign(begin, begin + static_cast<std::ptrdiff_t>(size));
        bundle.loadFlag = 1;
        result.bundles.push_back(std::move(bundle));
    }
    if (chunksSize) {
        reader.seek(chunkHashOffset);
        expect_table(reader, chunksSize, 4, "TOC chunk count exceeds the file");
        result.chunkHashMap.reserve(chunksSize);
        for (std::size_t index = 0; index < chunksSize; ++index) result.chunkHashMap.push_back(reader.i32(Endian::big));
        reader.seek(chunkDataOffset);
        expect_table(reader, static_cast<std::size_t>(dataCount), 4, "TOC chunk data count exceeds the file");
        std::vector<std::uint32_t> words(static_cast<std::size_t>(dataCount));
        for (auto& value : words) value = reader.u32(Endian::big);
        reader.seek(chunkGuidOffset);
        expect_table(reader, chunksSize, 20, "TOC chunk count exceeds the file");
        result.chunks.reserve(chunksSize);
        for (std::size_t index = 0; index < chunksSize; ++index) {
            const auto guidBytes = reader.view(16);
            const auto encodedIndex = reader.i32(Endian::big);
            TocChunk chunk;
            chunk.guid = guid_from_reversed_bytes(guidBytes);
            if (encodedIndex == -1) {
                chunk.removed = true;
            } else {
                const auto flag = static_cast<std::uint32_t>(encodedIndex) >> 24U;
                auto word = static_cast<std::uint32_t>(encodedIndex) & 0xFFFFFFU;
                if (flag != 0x80 || word > words.size() || words.size() - word < 4) {
                    throw std::invalid_argument("TOC chunk data reference is invalid");
                }
                const auto id = (static_cast<std::uint64_t>(words[word]) << 32U) | words[word + 1];
                chunk.location = {(id & (0xFFULL << 48U)) != 0,
                                  static_cast<std::uint32_t>((id >> 16U) & 0xFFFFFFFFULL),
                                  static_cast<std::uint16_t>(id)};
                chunk.offset = words[word + 2];
                chunk.size = words[word + 3];
            }
            result.chunks.push_back(chunk);
        }
    }
    return result;
}

void verify_toc(const TocDocument& document) {
    std::vector<std::vector<std::byte>> bundleKeys;
    bundleKeys.reserve(document.bundles.size());
    for (const auto& bundle : document.bundles) bundleKeys.push_back(lower_bytes(bundle.name));
    for (std::size_t index = 0; index < bundleKeys.size(); ++index) {
        const auto found = hash_lookup(document.bundleHashMap, bundleKeys, bundleKeys[index]);
        if (!found || *found != index) throw std::runtime_error("TOC bundle perfect hash does not resolve");
    }
    std::vector<std::array<std::byte, 16>> chunkKeys;
    chunkKeys.reserve(document.chunks.size());
    for (const auto& chunk : document.chunks) chunkKeys.push_back(chunk_key(chunk.guid));
    for (std::size_t index = 0; index < chunkKeys.size(); ++index) {
        const auto found = hash_lookup(document.chunkHashMap, chunkKeys, chunkKeys[index]);
        if (!found || *found != index) throw std::runtime_error("TOC chunk perfect hash does not resolve");
    }
}

}
