#include "shader_block_depot.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>

namespace dingosdk::frostbite::shader_depot {
namespace {

template <typename Type> Type read_at(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(Type))
        throw std::runtime_error("shader block depot is cut short");
    Type value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(Type));
    return value;
}

template <typename Type> void put(std::vector<std::byte>& bytes, std::size_t offset, Type value) {
    if (bytes.size() < offset + sizeof(Type)) bytes.resize(offset + sizeof(Type));
    std::memcpy(bytes.data() + offset, &value, sizeof(Type));
}

std::size_t round16(std::size_t value) { return (value + 15) / 16 * 16; }

} // namespace

Depot read(const Resource& depot) {
    if (depot.resourceMeta.size() < 16) throw std::runtime_error("shader block depot metadata is not 16 bytes");
    const auto dataSize = read_at<std::uint32_t>(depot.resourceMeta, 0);
    const auto fixupSize = read_at<std::uint32_t>(depot.resourceMeta, 4);
    Depot result;
    result.version = read_at<std::uint32_t>(depot.resourceMeta, 8);
    // The game takes the number of rows as 16 bits.
    const auto rows = static_cast<std::size_t>(read_at<std::uint16_t>(depot.resourceMeta, 12));
    if (static_cast<std::size_t>(dataSize) + fixupSize != depot.resource.size())
        throw std::runtime_error("shader block depot is not the size its metadata gives");
    const auto data = depot.resource.first(dataSize);
    if (rows && 16 + rows * 16 > data.size()) throw std::runtime_error("shader block depot rows run past its data");
    std::map<std::uint64_t, std::size_t> blockAt;   // head offset -> block
    for (std::size_t index = 0; index < rows; ++index) {
        const auto key = read_at<std::uint64_t>(data, 16 + index * 16);
        const auto head = read_at<std::uint64_t>(data, 16 + index * 16 + 8);
        if (head == 0 || head > data.size() || data.size() - head < 24)
            throw std::runtime_error("shader block depot row points outside its data");
        auto found = blockAt.find(head);
        if (found == blockAt.end()) {
            const auto start = read_at<std::uint64_t>(data, static_cast<std::size_t>(head));
            Block block;
            block.identity = read_at<std::uint64_t>(data, static_cast<std::size_t>(head) + 8);
            const auto size = read_at<std::uint64_t>(data, static_cast<std::size_t>(head) + 16);
            if (size == 0 || start > data.size() || data.size() - start < size)
                throw std::runtime_error("shader block depot block runs past its data");
            block.bytes.assign(data.begin() + static_cast<std::ptrdiff_t>(start),
                               data.begin() + static_cast<std::ptrdiff_t>(start + size));
            found = blockAt.emplace(head, result.blocks.size()).first;
            result.blocks.push_back(std::move(block));
        }
        result.rows.push_back({key, found->second});
    }
    return result;
}

Written write(const Depot& depot) {
    if (depot.rows.size() > 0xFFFF) throw std::runtime_error("shader block depot has more rows than the game reads");
    // The blocks in the order the rows first use them, each once.
    std::vector<std::size_t> order, placeOf(depot.blocks.size(), static_cast<std::size_t>(-1));
    for (const auto& row : depot.rows) {
        if (row.block >= depot.blocks.size()) throw std::runtime_error("shader block depot row names no block");
        if (placeOf[row.block] == static_cast<std::size_t>(-1)) {
            placeOf[row.block] = order.size();
            order.push_back(row.block);
        }
    }
    std::vector<std::byte> data;
    put<std::uint64_t>(data, 0, 16);
    put<std::uint64_t>(data, 8, 0);
    const auto rowsEnd = 16 + depot.rows.size() * 16;
    data.resize(rowsEnd);
    std::vector<std::size_t> blockStart;
    for (const auto block : order) {
        blockStart.push_back(data.size());
        data.insert(data.end(), depot.blocks[block].bytes.begin(), depot.blocks[block].bytes.end());
    }
    data.resize(round16(data.size()));
    const auto headsStart = data.size();
    for (std::size_t place = 0; place < order.size(); ++place) {
        const auto at = headsStart + place * 24;
        put<std::uint64_t>(data, at, blockStart[place]);
        put<std::uint64_t>(data, at + 8, depot.blocks[order[place]].identity);
        put<std::uint64_t>(data, at + 16, depot.blocks[order[place]].bytes.size());
    }
    data.resize(round16(data.size()));
    std::vector<std::uint32_t> pointers;
    std::vector<bool> headed(order.size());
    for (std::size_t index = 0; index < depot.rows.size(); ++index) {
        const auto place = placeOf[depot.rows[index].block];
        const auto head = headsStart + place * 24;
        put<std::uint64_t>(data, 16 + index * 16, depot.rows[index].key);
        put<std::uint64_t>(data, 16 + index * 16 + 8, head);
        if (!headed[place]) {
            headed[place] = true;
            pointers.push_back(static_cast<std::uint32_t>(head));
        }
        pointers.push_back(static_cast<std::uint32_t>(16 + index * 16 + 8));
    }
    pointers.push_back(0);
    if (data.size() > 0xFFFFFFFFU) throw std::runtime_error("shader block depot is too large");
    Written written;
    written.resource = data;
    for (const auto pointer : pointers) put<std::uint32_t>(written.resource, written.resource.size(), pointer);
    put<std::uint32_t>(written.resourceMeta, 0, static_cast<std::uint32_t>(data.size()));
    put<std::uint32_t>(written.resourceMeta, 4, static_cast<std::uint32_t>(pointers.size() * 4));
    put<std::uint32_t>(written.resourceMeta, 8, depot.version);
    put<std::uint32_t>(written.resourceMeta, 12, static_cast<std::uint32_t>(depot.rows.size()));
    return written;
}

Merged merge(const Resource& base, std::span<const Resource> edits) {
    auto result = read(base);
    // Where each key's row is, and the block the game gives each of its own keys.
    std::map<std::uint64_t, std::size_t> rowOf;
    std::map<std::uint64_t, Block> game;
    for (std::size_t index = 0; index < result.rows.size(); ++index) {
        rowOf.emplace(result.rows[index].key, index);
        game.emplace(result.rows[index].key, result.blocks[result.rows[index].block]);
    }
    // The edit that last gave each key its block, for keys an edit set.
    std::map<std::uint64_t, std::size_t> setBy;
    const auto block_index = [&](const Block& block) {
        for (std::size_t index = 0; index < result.blocks.size(); ++index)
            if (result.blocks[index] == block) return index;
        result.blocks.push_back(block);
        return result.blocks.size() - 1;
    };
    Merged merged;
    for (std::size_t edit = 0; edit < edits.size(); ++edit) {
        const auto depot = read(edits[edit]);
        std::map<std::uint64_t, bool> seen;   // a key an edit lists twice counts once
        for (const auto& row : depot.rows) {
            if (!seen.emplace(row.key, true).second) continue;
            const auto& block = depot.blocks[row.block];
            const auto at = rowOf.find(row.key);
            if (at == rowOf.end()) {
                rowOf.emplace(row.key, result.rows.size());
                result.rows.push_back({row.key, block_index(block)});
                setBy[row.key] = edit;
                ++merged.added;
                continue;
            }
            auto& current = result.rows[at->second];
            // A mod's copy carries the game's rows it did not change: those change nothing,
            // whatever another mod did to them.
            const auto shipped = game.find(row.key);
            if (shipped != game.end() && shipped->second == block) continue;
            if (result.blocks[current.block] == block) continue;
            if (const auto earlier = setBy.find(row.key); earlier != setBy.end()) merged.clashes.push_back({row.key, earlier->second, edit});
            else ++merged.changed;
            current.block = block_index(block);
            setBy[row.key] = edit;
        }
    }
    // Blocks no row uses any more are left out by the writer; of the rest, two contents under
    // one identity are one block to the game.
    std::map<std::uint64_t, std::size_t> identities;
    std::vector<bool> used(result.blocks.size());
    for (const auto& row : result.rows) used[row.block] = true;
    for (std::size_t index = 0; index < result.blocks.size(); ++index) {
        if (!used[index]) continue;
        const auto [where, fresh] = identities.emplace(result.blocks[index].identity, index);
        if (!fresh && !(result.blocks[where->second] == result.blocks[index])) ++merged.identityClashes;
    }
    merged.depot = write(result);
    return merged;
}

} // namespace dingosdk::frostbite::shader_depot
