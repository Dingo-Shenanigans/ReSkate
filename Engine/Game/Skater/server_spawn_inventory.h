#pragma once
#include "source_inventory.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace dingosdk::skater {
// Absolute addresses for one inspected executable. This passive reader never
// invokes Spawn, readiness veto callbacks, variant conversion, or native getters.
struct ServerSpawnInventoryLayout {
    std::uintptr_t server_pointer_address{}, server_vtable{};
    std::uintptr_t partition_offset_address{}, registry_address{}, spawner_vtable{};
    std::uintptr_t manager_offset_address{}, manager_vtable{}, player_vtable{}, int32_type{};
    std::uintptr_t player_state_manager_vtable{}, player_state_vtable{};
};
struct ServerSpawnPlayer {
    std::uintptr_t address{}, vtable{};
    // Native ServerPlayer+10c is the team; the creation PlayerID is at +40.
    std::uint32_t native_player_id{}, team{};
    std::uint8_t bot{}, spectator{}, ready{};
    // Native helper RVA2eb2300 is exactly (player+2e3 || player+44).
    bool spawn_ready{};
    // Only the already-owned player state needed by native RVA2eaa830 is
    // observed. This does not prove actor entry0, client presentation or physics.
    bool handoff_state_ready{};
    const char* handoff_state_status{"unavailable"};
};
struct PendingSpawnPlayer {
    std::uintptr_t handle{}, player{}, vtable{};
    bool resolved{}, current_member{}, spawn_ready{};
};
struct ServerSpawnerInfo {
    std::uintptr_t address{}, vtable{}, context{}, spawn_method{};
    std::uint32_t registry_index{};
    bool known_type{}, context_matches{}, veto_present{};
    std::uint8_t spawn_enabled_250{}; // RVA2ccb6ba gates Spawn-event processing on this raw byte.
    std::int32_t count{}, capacity{};
    bool capacity_available{}, capacity_allows{};
    std::array<std::uintptr_t, 2> capacity_variant{};
    std::vector<PendingSpawnPlayer> pending;
};
struct ServerSpawnInventory {
    bool available{};
    std::uintptr_t server{}, context{}, manager{};
    std::uint32_t partition{}, registry_entries{};
    std::vector<ServerSpawnPlayer> players;
    std::vector<ServerSpawnerInfo> spawners;
    std::string issue;
};
namespace server_spawn_inventory_detail {
using source_inventory_detail::Unavailable;
using source_inventory_detail::highest;
inline void array_bounds(std::uintptr_t begin, std::uintptr_t end, std::size_t limit) {
    if (begin > end || end > highest || begin % 8 || end % 8 ||
        (end && begin < 0x10000) || (end - begin) / 8 > limit)
        throw Unavailable{"Server spawn array exceeds the supported bound."};
}
// RVA4651af0's passive fast paths: an absent/non-dynamic property returns
// variant+8; a dynamic property whose TypeInfo is exactly primitive Int32
// returns property+0. Other dynamic values stay undecoded: no callback runs.
template<class Reader>
void capacity(Reader& reader, const ServerSpawnInventoryLayout& layout, ServerSpawnerInfo& info) {
    info.capacity_variant = reader.template get<std::array<std::uintptr_t, 2>>(info.address, 0x220);
    const auto property = info.capacity_variant[0];
    auto value = info.capacity_variant[1];
    if (property && (reader.template get<std::uint8_t>(property, 0x24) & 0x40)) {
        if (!layout.int32_type || reader.template get<std::uintptr_t>(property, 8) != layout.int32_type) return;
        value = reader.template get<std::uintptr_t>(property);
    }
    if (!value) return;
    info.capacity = reader.template get<std::int32_t>(value);
    info.capacity_available = true;
    info.capacity_allows = info.capacity < 0 || info.count < info.capacity;
}
template<class Reader>
void handoff_state(Reader& reader, const ServerSpawnInventoryLayout& layout, ServerSpawnPlayer& info) {
    const auto manager = reader.template get<std::uintptr_t>(info.address, 0x290);
    const auto alias = reader.template get<std::uintptr_t>(info.address, 0x298);
    if (!manager) { info.handoff_state_status = "missing_manager"; return; }
    if (manager % 8) { info.handoff_state_status = "manager_pointer_rejected"; return; }
    if (alias != manager) { info.handoff_state_status = "manager_alias_differs"; return; }
    if (!layout.player_state_manager_vtable ||
        reader.template get<std::uintptr_t>(manager) != layout.player_state_manager_vtable) {
        info.handoff_state_status = "manager_type_differs"; return;
    }
    if (reader.template get<std::uintptr_t>(manager, 0x3f0) != info.address) {
        info.handoff_state_status = "manager_owner_differs"; return;
    }
    const auto state = reader.template get<std::uintptr_t>(manager, 0x460);
    if (!state) { info.handoff_state_status = "missing_state"; return; }
    if (state % 8) { info.handoff_state_status = "state_pointer_rejected"; return; }
    if (!layout.player_state_vtable || reader.template get<std::uintptr_t>(state) != layout.player_state_vtable) {
        info.handoff_state_status = "state_type_differs"; return;
    }
    info.handoff_state_status = "ready";
    info.handoff_state_ready = true;
}
}
// Observe at a verified local game-update boundary. Every primitive read,
// list link, weak handle, registry head and owner is checked again before the
// result is published. A changing snapshot is discarded rather than merged.
template<class Read>
ServerSpawnInventory read_server_spawn_inventory(const ServerSpawnInventoryLayout& layout, Read&& read) {
    using namespace server_spawn_inventory_detail;
    ServerSpawnInventory result;
    try {
        source_inventory_detail::Reader reader(read);
        result.server = reader.template get<std::uintptr_t>(layout.server_pointer_address);
        if (!layout.server_vtable || reader.template get<std::uintptr_t>(result.server) != layout.server_vtable)
            throw Unavailable{"Server spawn inventory server type differs."};
        result.context = reader.template get<std::uintptr_t>(result.server, 8);
        const auto partition_offset = reader.template get<std::uint32_t>(layout.partition_offset_address);
        const auto manager_offset = reader.template get<std::uint32_t>(layout.manager_offset_address);
        if (partition_offset > 0x1000000 || manager_offset > 0x1000000)
            throw Unavailable{"Server spawn context offsets exceed the supported bound."};
        result.partition = reader.template get<std::uint32_t>(result.context, partition_offset);
        if (result.partition > 4) throw Unavailable{"Server spawn partition exceeds the supported bound."};
        result.manager = reader.template get<std::uintptr_t>(result.context, manager_offset);
        if (!layout.manager_vtable || reader.template get<std::uintptr_t>(result.manager) != layout.manager_vtable)
            throw Unavailable{"Server spawn player manager type differs."};
        const auto begin = reader.template get<std::uintptr_t>(result.manager, 0xc8);
        const auto end = reader.template get<std::uintptr_t>(result.manager, 0xd0);
        array_bounds(begin, end, 64);
        for (auto entry = begin; entry < end; entry += 8) {
            ServerSpawnPlayer info;
            info.address = reader.template get<std::uintptr_t>(entry);
            info.vtable = reader.template get<std::uintptr_t>(info.address);
            if (!layout.player_vtable || info.vtable != layout.player_vtable)
                throw Unavailable{"Server spawn player type differs."};
            info.native_player_id = reader.template get<std::uint32_t>(info.address, 0x40);
            info.team = reader.template get<std::uint32_t>(info.address, 0x10c);
            info.bot = reader.template get<std::uint8_t>(info.address, 0x44);
            info.spectator = reader.template get<std::uint8_t>(info.address, 0x70);
            info.ready = reader.template get<std::uint8_t>(info.address, 0x2e3);
            info.spawn_ready = info.ready != 0 || info.bot != 0;
            handoff_state(reader, layout, info);
            result.players.push_back(info);
        }
        // Registration RVA2b766f0: node=spawner+50, index=partition+8
        // when owner.flags&10, else partition+13. Lists terminate at null.
        std::vector<std::uintptr_t> visited;
        for (const auto bias : {8u, 13u}) {
            const auto index = result.partition + bias;
            const auto head = layout.registry_address + std::uintptr_t{index} * 16;
            auto previous = head;
            auto node = reader.template get<std::uintptr_t>(head);
            unsigned traversed{};
            while (node) {
                if (++traversed > 128 || node < 0x10050 ||
                    std::find(visited.begin(), visited.end(), node) != visited.end())
                    throw Unavailable{"Server spawn registry cycles or exceeds the supported bound."};
                visited.push_back(node);
                if (reader.template get<std::uintptr_t>(node, 8) != previous)
                    throw Unavailable{"Server spawn registry backlink differs."};
                const auto next = reader.template get<std::uintptr_t>(node);
                ServerSpawnerInfo info;
                info.address = node - 0x50;
                info.registry_index = index;
                info.vtable = reader.template get<std::uintptr_t>(info.address);
                info.context = reader.template get<std::uintptr_t>(info.address, 0x20);
                const auto flags = reader.template get<std::uint64_t>(info.address, 0x28);
                info.context_matches = info.context == result.context;
                if (!info.context_matches || ((flags & 0x10) != 0) != (bias == 8))
                    throw Unavailable{"Server spawn registry context or partition ownership differs."};
                ++result.registry_entries;
                info.known_type = layout.spawner_vtable && info.vtable == layout.spawner_vtable;
                if (info.known_type) {
                    info.spawn_method = reader.template get<std::uintptr_t>(info.vtable, 0xe0);
                    info.spawn_enabled_250 = reader.template get<std::uint8_t>(info.address, 0x250);
                    info.count = reader.template get<std::int32_t>(info.address, 0x12c);
                    const auto veto = reader.template get<std::uintptr_t>(info.address, 0x190);
                    if (!veto) throw Unavailable{"Server spawn veto-list head is unavailable."};
                    info.veto_present = veto != info.address + 0x188;
                    capacity(reader, layout, info);
                    const auto first = reader.template get<std::uintptr_t>(info.address, 0x378);
                    const auto last = reader.template get<std::uintptr_t>(info.address, 0x380);
                    array_bounds(first, last, 128);
                    for (auto at = first; at < last; at += 8) {
                        PendingSpawnPlayer pending;
                        pending.handle = reader.template get<std::uintptr_t>(at);
                        // RVA2cd0eb0..eca resolves *handle as player+8. Expired
                        // handles/null entries are ordinary pending-queue states.
                        if (pending.handle) {
                            const auto interface_pointer = reader.template get<std::uintptr_t>(pending.handle);
                            if (interface_pointer) {
                                if (interface_pointer < 0x10008) throw Unavailable{"Server spawn weak reference is invalid."};
                                pending.player = interface_pointer - 8;
                                pending.vtable = reader.template get<std::uintptr_t>(pending.player);
                                pending.resolved = true;
                                for (const auto& player : result.players) {
                                    if (pending.player != player.address || pending.vtable != player.vtable) continue;
                                    pending.current_member = true;
                                    pending.spawn_ready = player.spawn_ready;
                                }
                            }
                        }
                        info.pending.push_back(pending);
                    }
                }
                result.spawners.push_back(std::move(info));
                previous = node;
                node = next;
            }
        }
        reader.verify();
        result.available = true;
    } catch (const Unavailable& error) {
        result.issue = error.issue;
        result.players.clear();
        result.spawners.clear();
    }
    return result;
}
}
