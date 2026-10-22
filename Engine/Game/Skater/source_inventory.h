#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace dingosdk::skater {
// Absolute addresses for the inspected build. This reader never calls native
// creation/enable methods and does not require an already-spawned local entity.
struct SourceInventoryLayout {
    std::uintptr_t client_vtable{}, partition_offset_address{}, managers_address{};
    std::uintptr_t manager_vtable{}, skater_source_vtable{}, skater_entity_vtable{};
};
struct SkaterSourceInfo {
    std::uintptr_t source{}, context{}, parent{}, data{}, blueprint{}, entity{};
    // Source+60 is copied to creation descriptor+38 by native RVA51ff87.
    // It is separate from Player+10c, which stores the player's team.
    std::uint32_t source_local_id{};
    std::array<std::uint8_t, 4> data_flags_e0{}; // raw data bytes, no inferred enable meaning
    std::string blueprint_name;
    bool context_matches{}, parent_context_matches{}, entity_type_matches{};
};
struct SourceInventory {
    bool available{};
    std::uintptr_t context{};
    std::uint32_t partition{}, managers{}, entries{};
    std::vector<SkaterSourceInfo> sources;
    std::string issue;
};
namespace source_inventory_detail {
inline constexpr std::uintptr_t highest = 0x00007fffffffffffULL;
inline bool range(std::uintptr_t address, std::size_t bytes) noexcept {
    return address >= 0x10000 && bytes && bytes <= highest && address <= highest - bytes;
}
struct Unavailable { const char* issue; };
struct Watch {
    std::uintptr_t address{};
    std::size_t bytes{};
    std::array<unsigned char, 16> value{};
};
template<class Read> class Reader {
public:
    explicit Reader(Read& read) : read_(read) {}
    template<class T> T get(std::uintptr_t object, std::uintptr_t offset = 0) {
        static_assert(sizeof(T) <= 16);
        if (!range(object, 1) || offset > highest - object || !range(object + offset, sizeof(T)))
            throw Unavailable{"Source inventory pointer is unavailable."};
        Watch watch{object + offset, sizeof(T)};
        if (!read_(watch.address, watch.value.data(), watch.bytes))
            throw Unavailable{"Source inventory memory is unavailable."};
        T value{};
        std::memcpy(&value, watch.value.data(), sizeof(value));
        watches_.push_back(watch);
        return value;
    }
    std::string name(std::uintptr_t address) {
        std::string value;
        for (std::uintptr_t i = 0; i < 256; ++i) {
            char byte{};
            if (!range(address, 256) || !read_(address + i, &byte, 1))
                throw Unavailable{"Source blueprint name is unavailable."};
            if (!byte) return value;
            if (static_cast<unsigned char>(byte) < 0x20)
                throw Unavailable{"Source blueprint name contains a control character."};
            value += byte;
        }
        throw Unavailable{"Source blueprint name is not terminated within 256 bytes."};
    }
    void verify() {
        for (const auto& watch : watches_) {
            std::array<unsigned char, 16> after{};
            if (!read_(watch.address, after.data(), watch.bytes) ||
                std::memcmp(after.data(), watch.value.data(), watch.bytes))
                throw Unavailable{"Source inventory changed during observation; retry on the client thread."};
        }
    }
private:
    Read& read_;
    std::vector<Watch> watches_;
};
}
// Read returns true only after copying every requested byte. Use at a validated
// local client-tick boundary. Headers and source identities are re-read before
// publication; names are diagnostic text and never become spawn instructions.
template<class Read>
SourceInventory read_client_skater_sources(std::uintptr_t client,
    const SourceInventoryLayout& layout, Read&& read) {
    using namespace source_inventory_detail;
    SourceInventory result;
    try {
        Reader reader(read);
        if (!layout.client_vtable || reader.template get<std::uintptr_t>(client) != layout.client_vtable)
            throw Unavailable{"Source inventory client type differs."};
        result.context = reader.template get<std::uintptr_t>(client, 8);
        const auto offset = reader.template get<std::uint32_t>(layout.partition_offset_address);
        if (offset > 0x1000000) throw Unavailable{"Source inventory partition offset exceeds the supported bound."};
        result.partition = reader.template get<std::uint32_t>(result.context, offset);
        if (result.partition > 4) throw Unavailable{"Source inventory partition exceeds the supported bound."};
        for (unsigned kind = 0; kind < 7; ++kind) {
            const auto index = (std::uintptr_t{result.partition} * 7 + kind) * 8;
            const auto manager = reader.template get<std::uintptr_t>(layout.managers_address, index);
            if (!manager) continue;
            ++result.managers;
            if (reader.template get<std::uintptr_t>(manager) != layout.manager_vtable ||
                reader.template get<std::uintptr_t>(manager, 0x18) != result.context)
                throw Unavailable{"Source inventory manager ownership differs."};
            const auto begin = reader.template get<std::uintptr_t>(manager, 0xa0);
            const auto end = reader.template get<std::uintptr_t>(manager, 0xa8);
            if (begin > end || end > highest || begin % 8 || end % 8 ||
                (end && begin < 0x10000) || (end - begin) % 0x18 || (end - begin) / 0x18 > 64)
                throw Unavailable{"Source inventory collection exceeds the supported bound."};
            result.entries += static_cast<std::uint32_t>((end - begin) / 0x18);
            for (auto entry = begin; entry < end; entry += 0x18) {
                const auto source = reader.template get<std::uintptr_t>(entry, 8);
                if (!source || reader.template get<std::uintptr_t>(source) != layout.skater_source_vtable) continue;
                SkaterSourceInfo info;
                info.source = source;
                info.context = reader.template get<std::uintptr_t>(source, 0x20);
                info.parent = reader.template get<std::uintptr_t>(source, 0x30);
                info.data = reader.template get<std::uintptr_t>(source, 0x38);
                info.source_local_id = reader.template get<std::uint32_t>(source, 0x60);
                info.entity = reader.template get<std::uintptr_t>(source, 0x68);
                info.context_matches = info.context == result.context;
                info.parent_context_matches = info.parent &&
                    reader.template get<std::uintptr_t>(info.parent, 0x20) == result.context;
                info.entity_type_matches = info.entity &&
                    reader.template get<std::uintptr_t>(info.entity) == layout.skater_entity_vtable;
                if (info.data) {
                    info.blueprint = reader.template get<std::uintptr_t>(info.data, 0xd0) & ~std::uintptr_t{4};
                    info.data_flags_e0 = reader.template get<std::array<std::uint8_t, 4>>(info.data, 0xe0);
                    if (info.blueprint) {
                        const auto name = reader.template get<std::uintptr_t>(info.blueprint, 0x18);
                        if (name) info.blueprint_name = reader.name(name);
                    }
                }
                result.sources.push_back(std::move(info));
            }
        }
        reader.verify();
        result.available = true;
    } catch (const Unavailable& error) {
        result.issue = error.issue;
        result.sources.clear(); // Never expose an incomplete snapshot as an actionable source.
    }
    return result;
}
}
