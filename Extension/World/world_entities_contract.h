#pragma once
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/skater_entities.h"
#include <cstdint>
#include <limits>
#include <string_view>

// The checks behind world_entities.cpp, as SmokeyDawg58 wrote them for his fork's gates (its source
// and asset contracts): pure over the reads they are given, so they test.
namespace dingosdk::world_entities::detail {
// The native creation parent bus. A ClientSkaterSource can keep its spawned entity slot empty after
// player replacement while the verified local player owns the current skater; its same-context CAS
// source still supplies the bus, as native_skater_spawn::find_source does for remote actors.
template<class Pointer, class UInt32, class Byte, class Name>
std::uintptr_t object_parent_bus(std::uintptr_t base, std::uintptr_t context, Pointer pointer, UInt32 uint32, Byte byte, Name name) {
    if (!base || !context) return 0;
    const auto player_offset = uint32(base + addr::engine::context_player_manager_offset);
    if (player_offset > 0x1000000) return 0;
    const auto players = pointer(context + player_offset);
    if (!players || pointer(players) != base + addr::engine::local_player_manager_vtable) return 0;
    const auto first = pointer(players + 0x4c8), last = pointer(players + 0x4d0);
    if (!first || last < first || last - first != 8) return 0;
    const auto player = pointer(first);
    if (!player || pointer(player) != base + addr::engine::local_player_vtable || pointer(player + 0x78) != context ||
        byte(player + 0x45) != 1 || byte(player + 0x44))
        return 0;
    const auto local = pointer(player + 0xb8);
    if (!local || pointer(local) != base + addr::engine::skater_entity_vtable || pointer(local + 0x20) != context || pointer(local + 0xf8) != player)
        return 0;

    const auto partition_offset = uint32(base + addr::engine::context_partition_offset);
    if (partition_offset > 0x1000000) return 0;
    const auto partition = uint32(context + partition_offset);
    if (partition > 4) return 0;
    std::uintptr_t fallback{};
    for (unsigned kind = 0; kind < 7; ++kind) {
        const auto manager = pointer(base + addr::engine::source_managers + (std::uintptr_t(partition) * 7 + kind) * 8);
        if (!manager || pointer(manager) != base + addr::engine::source_manager_vtable || pointer(manager + 0x18) != context) continue;
        const auto begin = pointer(manager + 0xa0), end = pointer(manager + 0xa8);
        if (!begin || end < begin || (end - begin) % 0x18 || end - begin > 64 * 0x18) continue;
        for (auto at = begin; at < end; at += 0x18) {
            const auto source = pointer(at + 8);
            if (!source || pointer(source) != base + addr::engine::client_skater_source_vtable || pointer(source + 0x20) != context) continue;
            const auto data = pointer(source + 0x38), bus = pointer(source + 0x30);
            if (!data || !bus || pointer(bus + 0x20) != context) continue;
            const auto blueprint = pointer(data + 0xd0) & ~std::uintptr_t{4};
            if (!blueprint || name(pointer(blueprint + 0x18)) != "Gameplay/Characters/CharacterBlueprint_Physics_Skater_RSP_CAS") continue;
            // create_bus derives its owner from this bus. A matching context alone is not enough if
            // the owner was already torn down.
            const auto owner = pointer(bus + 8);
            if (!owner || !pointer(owner + 0x48)) continue;
            const auto entity = pointer(source + 0x68);
            if (entity == local) return bus;
            if (!entity && !fallback) fallback = bus;
        }
    }
    return fallback;
}

// Objects may be created from the first level entry (13 or 21) until a world is left (14, 22, 3)
// or the game shuts down (24); leaving releases them.
class WorldObjectLifetime {
    bool blocked_ = true, shutdown_{};
public:
    bool blocked() const noexcept { return blocked_; }
    bool before_transition(unsigned next) noexcept {
        if (next == 13 || next == 21) {
            if (!shutdown_) blocked_ = false;
            return false;
        }
        if (next != 14 && next != 22 && next != 3 && next != 24) return false;
        const bool release = !blocked_;
        blocked_ = true;
        if (next == 24) shutdown_ = true;
        return release;
    }
};

// A world object blueprint (ObjectBlueprint, or a type derived from it): never a skater's blueprint.
template<class ReadPointer>
bool object_blueprint(std::uintptr_t asset, std::uintptr_t base, ReadPointer read) {
    constexpr auto maximum = std::numeric_limits<std::uintptr_t>::max();
    if (!asset || asset > maximum - 8 || !base || base > maximum - addr::skater_entities::object_blueprint_type) return false;
    auto type = read(asset + 8);
    for (unsigned depth = 0; type && depth < 16; ++depth) {
        if (type == base + addr::skater_entities::object_blueprint_type) return true;
        if (type > maximum - 0x20) return false;
        type = read(type + 0x20);
    }
    return false;
}
// A world can stream a bundle out and back without changing the client context. Readable old
// allocation bytes alone do not prove residency: the name must still resolve to the same asset.
template<class ReadPointer>
bool object_resident(std::uintptr_t cached, std::uintptr_t resolved, std::uintptr_t base, ReadPointer read) {
    return cached && resolved == cached && object_blueprint(resolved, base, read);
}
} // namespace dingosdk::world_entities::detail
