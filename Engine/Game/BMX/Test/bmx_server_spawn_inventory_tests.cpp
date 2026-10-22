#include "Engine/Game/Skater/server_spawn_inventory.h"
#include "Engine/Game/Skater/server_spawn_event_observer.h"
#include <cstdlib>
#include <iostream>
#include <map>

namespace {
void check(bool valid, const char* text) {
    if (!valid) { std::cerr << text << '\n'; std::exit(1); }
}
struct Fixture {
    static constexpr std::uintptr_t server_object = 0x10000, context_object = 0x20000;
    static constexpr std::uintptr_t manager_object = 0x30000, players_array = 0x40000;
    static constexpr std::uintptr_t player_object = 0x50000, spawner_object = 0x60000;
    static constexpr std::uintptr_t pending_array = 0x70000, weak_handle = 0x80000;
    static constexpr std::uintptr_t capacity_value = 0x90000, property_object = 0xa0000;
    static constexpr std::uintptr_t event_object = 0xf0000;
    static constexpr std::uintptr_t player_state_manager = 0x100000, player_state = 0x110000;
    static constexpr dingosdk::skater::ServerSpawnInventoryLayout layout{
        0xb0000, 0x140100000, 0xc0000, 0xd0000, 0x140200000,
        0xe0000, 0x140300000, 0x140400000, 0x140500000, 0x140600000, 0x140700000};
    static constexpr std::uintptr_t registry_head = layout.registry_address + 14 * 16;
    std::map<std::uintptr_t, unsigned char> bytes;
    std::uintptr_t mutate_address{};
    bool mutated{};
    template<class T> void put(std::uintptr_t address, const T& value) {
        const auto* raw = reinterpret_cast<const unsigned char*>(&value);
        for (std::size_t i = 0; i < sizeof(T); ++i) bytes[address + i] = raw[i];
    }
    bool read(std::uintptr_t address, void* out, std::size_t size) {
        auto* raw = static_cast<unsigned char*>(out);
        for (std::size_t i = 0; i < size; ++i) {
            const auto found = bytes.find(address + i);
            if (found == bytes.end()) return false;
            raw[i] = found->second;
        }
        if (address == mutate_address && !mutated) {
            mutated = true;
            put<std::uintptr_t>(address, 0);
        }
        return true;
    }
    Fixture() {
        put(layout.server_pointer_address, server_object); put(server_object, layout.server_vtable);
        put(server_object + 8, context_object);
        put<std::uint32_t>(layout.partition_offset_address, 0x80);
        put<std::uint32_t>(layout.manager_offset_address, 0x100);
        put<std::uint32_t>(context_object + 0x80, 1);
        put(context_object + 0x100, manager_object); put(manager_object, layout.manager_vtable);
        put(manager_object + 0xc8, players_array); put(manager_object + 0xd0, players_array + 8);
        put(players_array, player_object); put(player_object, layout.player_vtable);
        put<std::uint32_t>(player_object + 0x40, 42);
        put<std::uint32_t>(player_object + 0x10c, 7);
        put<std::uint8_t>(player_object + 0x44, 0); put<std::uint8_t>(player_object + 0x70, 0);
        put<std::uint8_t>(player_object + 0x2e3, 0);
        put(player_object + 0x290, player_state_manager); put(player_object + 0x298, player_state_manager);
        put(player_state_manager, layout.player_state_manager_vtable);
        put(player_state_manager + 0x3f0, player_object); put(player_state_manager + 0x460, player_state);
        put(player_state, layout.player_state_vtable);
        put<std::uintptr_t>(layout.registry_address + 9 * 16, 0);
        put(registry_head, spawner_object + 0x50);
        put<std::uintptr_t>(spawner_object + 0x50, 0); put(spawner_object + 0x58, registry_head);
        put(spawner_object, layout.spawner_vtable); put(spawner_object + 0x20, context_object);
        put<std::uint64_t>(spawner_object + 0x28, 0x20);
        put<std::uintptr_t>(layout.spawner_vtable + 0xe0, 0x142cd95f0);
        put<std::int32_t>(spawner_object + 0x12c, 0);
        put<std::uint8_t>(spawner_object + 0x250, 1);
        put(spawner_object + 0x190, spawner_object + 0x188);
        put(spawner_object + 0x220, std::array<std::uintptr_t, 2>{0, capacity_value});
        put<std::int32_t>(capacity_value, 1);
        put(spawner_object + 0x378, pending_array); put(spawner_object + 0x380, pending_array + 8);
        put(spawner_object + 0x388, pending_array + 8);
        put(pending_array, weak_handle); put(weak_handle, player_object + 8);
        put<std::uint32_t>(event_object + 8, dingosdk::skater::server_spawn_event_id);
        put(event_object + 0x20, weak_handle);
    }
    auto snapshot() {
        return dingosdk::skater::read_server_spawn_inventory(layout,
            [&](std::uintptr_t at, void* out, std::size_t size) { return read(at, out, size); });
    }
    auto event_sample() {
        return dingosdk::skater::sample_server_spawn_event(dingosdk::skater::ServerSpawnEventLayout{
            layout.server_pointer_address, layout.server_vtable, layout.spawner_vtable, layout.player_vtable},
            spawner_object, event_object,
            [&](std::uintptr_t at, void* out, std::size_t size) { return read(at, out, size); });
    }
};
}
int main() {
    using F = Fixture;
    auto memory = F{};
    auto inventory = memory.snapshot();
    check(inventory.available && inventory.players.size() == 1 && inventory.spawners.size() == 1,
        "A server without a local skater must remain diagnosable");
    check(inventory.registry_entries == 1 && inventory.partition == 1 && inventory.players[0].team == 7 &&
        inventory.players[0].native_player_id == 42,
        "Server/player registry counts or team differ; team must not use the creation PlayerID");
    check(inventory.players[0].handoff_state_ready &&
        std::string{inventory.players[0].handoff_state_status} == "ready",
        "Owned native player handoff state was not identified without an actor");
    for (const auto field : {F::player_object + 0x298, F::player_state_manager, F::player_state_manager + 0x3f0,
        F::player_state_manager + 0x460, F::player_state}) {
        memory = F{}; memory.put<std::uintptr_t>(field, 0);
        const auto rejected = memory.snapshot();
        check(rejected.available && !rejected.players[0].handoff_state_ready,
            "Wrong alias/type/owner or missing state was reported as native handoff readiness");
    }
    memory = F{}; memory.mutate_address = F::player_state_manager + 0x3f0;
    const auto changed_owner = memory.snapshot();
    check(!changed_owner.available && changed_owner.players.empty(), "Changed handoff ownership published an incoherent snapshot");
    memory = F{}; inventory = memory.snapshot();
    check(!inventory.players[0].spawn_ready && !inventory.spawners[0].pending[0].spawn_ready &&
        inventory.spawners[0].pending[0].current_member, "Unready human gate was misreported");
    check(inventory.spawners[0].capacity_available && inventory.spawners[0].capacity == 1 &&
        inventory.spawners[0].capacity_allows && !inventory.spawners[0].veto_present &&
        inventory.spawners[0].spawn_enabled_250 == 1,
        "Passive capacity or empty veto list differs");
    memory.put<std::uint8_t>(F::player_object + 0x2e3, 1);
    check(memory.snapshot().spawners[0].pending[0].spawn_ready, "Human readiness did not reach pending request");
    memory = F{}; memory.put<std::uint8_t>(F::player_object + 0x44, 1);
    check(memory.snapshot().players[0].spawn_ready, "Native bot readiness alternative was lost");
    memory = F{}; memory.put<std::int32_t>(F::capacity_value, 0);
    check(!memory.snapshot().spawners[0].capacity_allows, "Full capacity gate was reported open");
    memory.put<std::int32_t>(F::capacity_value, -1);
    check(memory.snapshot().spawners[0].capacity_allows, "Negative/unlimited native capacity was rejected");
    memory = F{}; memory.put<std::uint8_t>(F::spawner_object + 0x250, 0);
    check(memory.snapshot().spawners[0].spawn_enabled_250 == 0,
        "Disabled Spawn-event processing was hidden by capacity/readiness reporting");
    memory = F{};
    memory.put(F::spawner_object + 0x190, F::property_object);
    check(memory.snapshot().spawners[0].veto_present, "Veto presence was not observed without invoking it");
    memory = F{};
    memory.put(F::spawner_object + 0x220, std::array<std::uintptr_t, 2>{F::property_object, 0});
    memory.put<std::uint8_t>(F::property_object + 0x24, 0x40);
    memory.put(F::property_object + 8, F::layout.int32_type); memory.put(F::property_object, F::capacity_value);
    check(memory.snapshot().spawners[0].capacity_available, "Exact Int32 property passive fast path failed");
    memory.put<std::uintptr_t>(F::property_object + 8, 0x140600000);
    inventory = memory.snapshot();
    check(inventory.available && !inventory.spawners[0].capacity_available,
        "Unknown dynamic capacity should stay undecoded, without native evaluation");
    memory = F{}; memory.put<std::uintptr_t>(F::weak_handle, 0);
    inventory = memory.snapshot();
    check(inventory.available && !inventory.spawners[0].pending[0].resolved, "Expired weak handle was dereferenced");
    memory = F{}; memory.put<std::uintptr_t>(F::pending_array, 0);
    check(memory.snapshot().available, "Null pending entry is a valid queue observation");
    memory = F{}; memory.put(F::weak_handle, F::player_object + 0x1008);
    memory.put<std::uintptr_t>(F::player_object + 0x1000, F::layout.player_vtable);
    inventory = memory.snapshot();
    check(inventory.available && inventory.spawners[0].pending[0].resolved &&
        !inventory.spawners[0].pending[0].current_member && !inventory.spawners[0].pending[0].spawn_ready,
        "Foreign pending player borrowed readiness from the current manager member");
    memory = F{}; memory.put<std::uintptr_t>(F::spawner_object, 0x140600000);
    inventory = memory.snapshot();
    check(inventory.available && !inventory.spawners[0].known_type && inventory.spawners[0].pending.empty(),
        "Unknown spawner subtype was interpreted using the known subtype layout");
    memory = F{}; memory.put<std::uintptr_t>(F::registry_head, 0);
    inventory = memory.snapshot();
    check(inventory.available && inventory.spawners.empty(), "Empty server spawn registry was rejected");
    memory = F{}; memory.put(F::spawner_object + 0x50, F::spawner_object + 0x50);
    check(!memory.snapshot().available, "Cyclic registry accepted");
    memory = F{}; memory.put<std::uintptr_t>(F::spawner_object + 0x58, 0);
    check(!memory.snapshot().available, "Broken registry backlink accepted");
    memory = F{}; memory.put(F::spawner_object + 0x20, F::player_object);
    check(!memory.snapshot().available, "Foreign spawner context accepted");
    memory = F{}; memory.put<std::uint64_t>(F::spawner_object + 0x28, 0x30);
    check(!memory.snapshot().available, "Wrong flagged partition registry accepted");
    memory = F{}; memory.put(F::spawner_object + 0x380, F::pending_array + 129 * 8);
    check(!memory.snapshot().available, "Oversized pending queue accepted");
    memory = F{}; memory.mutate_address = F::weak_handle;
    inventory = memory.snapshot();
    check(!inventory.available && inventory.players.empty() && inventory.spawners.empty(),
        "Changed weak reference published an incoherent snapshot");
    memory = F{}; memory.mutate_address = F::layout.server_pointer_address;
    check(!memory.snapshot().available, "Replaced native server accepted");
    memory = F{};
    auto before_event = memory.event_sample();
    check(before_event.owned && before_event.event_readable && before_event.enabled == 1 &&
        before_event.pending_readable && before_event.pending == 1 && before_event.raw_weak_present &&
        before_event.raw_weak_resolved && before_event.raw_player_type_matches,
        "Fixed event observer lost valid raw payload/pending stages");
    dingosdk::skater::ServerSpawnEventCounters counts;
    counts.before(before_event);
    memory.put(F::spawner_object + 0x380, F::pending_array + 16);
    memory.put(F::spawner_object + 0x388, F::pending_array + 16);
    counts.after(before_event, memory.event_sample());
    check(counts.snapshot().spawn == 1 && counts.snapshot().pending_increased == 1 &&
        counts.snapshot().pending_added == 1 && counts.snapshot().pending_before_max == 1 &&
        counts.snapshot().pending_after_max == 2, "Observer failed to count actual handler queue growth");
    memory.put<std::uint8_t>(F::spawner_object + 0x250, 0);
    const auto disabled_event = memory.event_sample();
    check(disabled_event.owned && !disabled_event.raw_weak_present,
        "Disabled event unnecessarily interpreted payload fields");
    counts.before(disabled_event);
    check(counts.snapshot().disabled == 1 && counts.snapshot().raw_weak_present == 1,
        "Observer conflated disabled Spawn events with valid raw payload stages");
    memory = F{}; memory.put<std::uintptr_t>(F::weak_handle, 0);
    check(memory.event_sample().raw_weak_present && !memory.event_sample().raw_weak_resolved,
        "Observer resolved an expired raw weak reference");
    memory = F{}; memory.put<std::uintptr_t>(F::event_object + 0x20, 0);
    check(!memory.event_sample().raw_weak_present, "Observer accepted a missing raw weak reference");
    memory = F{}; memory.put<std::uintptr_t>(F::player_object, F::layout.manager_vtable);
    check(memory.event_sample().raw_weak_resolved && !memory.event_sample().raw_player_type_matches,
        "Observer accepted another object type as a raw ServerPlayer payload");
    memory = F{}; memory.put<std::uint32_t>(F::event_object + 8, 42);
    const auto unrelated_event = memory.event_sample();
    counts.before(unrelated_event);
    counts.after(unrelated_event, unrelated_event);
    check(counts.snapshot().spawn == 2 && !unrelated_event.raw_weak_present,
        "An unrelated native event affected Spawn counts or interpreted its payload");
    memory = F{}; memory.put(F::spawner_object + 0x388, F::pending_array + 129 * 8);
    check(memory.event_sample().owned && !memory.event_sample().pending_readable,
        "Oversized native queue bounds accepted by fixed event observer");
    memory = F{}; memory.put<std::uintptr_t>(F::spawner_object + 0x20, F::player_object);
    const auto foreign_event = memory.event_sample();
    counts.before(foreign_event);
    counts.after(before_event, foreign_event);
    check(!foreign_event.owned && counts.snapshot().owner_rejected == 1 &&
        counts.snapshot().post_owner_changed == 1 && counts.snapshot().pending_added == 1,
        "Foreign event or changed ownership published a successful native queue append");
    memory = F{}; memory.mutate_address = F::layout.server_pointer_address;
    check(!memory.event_sample().owned, "Changed canonical server accepted by event observer");
    memory = F{}; memory.mutate_address = F::spawner_object + 0x20;
    check(!memory.event_sample().owned, "Changed native source context accepted by event observer");
    std::cout << "BMX passive server spawn inventory checks passed\n";
}
