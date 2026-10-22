#include "bmx_spawn_diagnostics.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Core/Platform/user_data_paths.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/image_identity.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/server_spawn.h"
#include "Engine/Game/Skater/server_spawn_inventory.h"
#include "Engine/Game/Skater/server_spawn_event_observer.h"
#include <array>
#include <atomic>
#include <sstream>

namespace dingosdk::bmx {
namespace {
using EventHandler = void(__fastcall*)(std::uintptr_t, std::uintptr_t);
std::atomic<EventHandler> original_handler{};
std::atomic<std::uintptr_t> observer_base{};
std::atomic<bool> observer_prepared{};
skater::ServerSpawnEventCounters event_counts;

skater::ServerSpawnEventSample event_sample(std::uintptr_t base, std::uintptr_t source, std::uintptr_t event) noexcept {
    return skater::sample_server_spawn_event(skater::ServerSpawnEventLayout{
        base + addr::engine::game_server, base + addr::engine::server_vtable,
        base + addr::server_spawn::spawner_vtable, base + addr::engine::server_player_vtable}, source, event,
        [](std::uintptr_t address, void* target, std::size_t size) noexcept { return memory::peek_bytes(address, target, size); });
}

// Supported binary proof: RVA2b4d259..269 restores the dispatcher's frame,
// moves self/event to RCX/RDX, then jumps [self.vtable+28]. The complete
// RVA2ccb6b0..91f handler reads no incoming R8/R9 or stack arguments and
// tail-forwards RCX/RDX to its base handler. The routed caller at 2b8b2da
// ignores a return, and dispatcher's queued branches return unrelated AL/RAX.
void __fastcall observe_event(std::uintptr_t source, std::uintptr_t event) {
    const auto incoming_error = GetLastError();
    const auto original = original_handler.load(std::memory_order_acquire);
    const auto base = observer_base.load(std::memory_order_acquire);
    const auto before = event_sample(base, source, event);
    event_counts.before(before);
    SetLastError(incoming_error);
    // Published before hook_enable; the permanent relay also remains callable
    // before attachment and after detachment. Forward exactly once.
    original(source, event);
    const auto original_error = GetLastError();
    if (before.owned && before.event_readable && before.event_id == skater::server_spawn_event_id)
        event_counts.after(before, event_sample(base, source, event));
    SetLastError(original_error);
}
}

bool initialize_spawn_diagnostics(std::uintptr_t base) noexcept {
    const auto incoming_error = GetLastError();
    bool result{};
    try {
        std::array<wchar_t, 2> flag{};
        if (GetEnvironmentVariableW(L"RESKATE_BMX_FLOW_DIAGNOSTICS", flag.data(), static_cast<DWORD>(flag.size())) == 1 &&
            flag[0] == L'1' && user_data::override_root() && supported_build::running_image_matches(base)) {
            if (observer_prepared.load(std::memory_order_acquire)) result = observer_base.load(std::memory_order_acquire) == base;
            else {
                // Entire first 32 bytes, not a wildcard pattern. The vtable is
                // also checked so another function at the RVA cannot qualify.
                static constexpr std::array<std::uint8_t, 32> prefix{
                    0x40,0x53,0x41,0x54,0x41,0x57,0x48,0x83,0xec,0x40,0x80,0xb9,0x50,0x02,0x00,0x00,
                    0x00,0x4c,0x8b,0xfa,0x48,0x8b,0xd9,0x0f,0x84,0x43,0x02,0x00,0x00,0x8b,0x42,0x08};
                std::array<std::uint8_t, 32> actual{};
                std::uintptr_t vtable_handler{};
                auto* target = reinterpret_cast<void*>(base + addr::server_spawn::spawner_event_handler);
                if (memory::peek(base + addr::server_spawn::spawner_vtable + 0x28, vtable_handler) &&
                    vtable_handler == reinterpret_cast<std::uintptr_t>(target) &&
                    memory::peek_bytes(reinterpret_cast<std::uintptr_t>(target), actual.data(), actual.size()) && actual == prefix) {
                    void* relay{};
                    if (hook_prepare(target, reinterpret_cast<void*>(&observe_event), &relay) == HookOk && relay) {
                        observer_base.store(base, std::memory_order_release);
                        original_handler.store(reinterpret_cast<EventHandler>(relay), std::memory_order_release);
                        if (hook_enable(target) == HookOk) {
                            observer_prepared.store(true, std::memory_order_release);
                            result = true;
                        } else (void)hook_remove(target);
                    }
                }
            }
        }
    } catch (...) { // A requested diagnostic never prevents normal boot.
        result = false;
    }
    SetLastError(incoming_error);
    return result;
}

std::string spawn_diagnostics(std::uintptr_t base) {
    const auto inventory = skater::read_server_spawn_inventory(skater::ServerSpawnInventoryLayout{
        base + addr::engine::game_server, base + addr::engine::server_vtable,
        base + addr::engine::context_partition_offset, base + addr::server_spawn::spawner_registry,
        base + addr::server_spawn::spawner_vtable, base + addr::engine::context_player_manager_offset,
        base + addr::engine::server_player_manager_vtable, base + addr::engine::server_player_vtable,
        base + addr::engine::int32_type, base + addr::server_spawn::player_state_manager_vtable,
        base + addr::server_spawn::player_state_vtable},
        [](std::uintptr_t address, void* target, std::size_t size) { return memory::peek_bytes(address, target, size); });
    std::ostringstream out;
    const auto counts = event_counts.snapshot();
    out << "Native Spawn event observer: prepared=" << observer_prepared.load(std::memory_order_acquire)
        << "; calls=" << counts.calls << "; owner_rejected=" << counts.owner_rejected
        << "; spawn=" << counts.spawn << "; disabled250=" << counts.disabled
        << "; gate_unreadable=" << counts.unreadable_gate
        << "; raw_payload_weak=" << counts.raw_weak_present << "; raw_payload_resolved=" << counts.raw_weak_resolved
        << "; raw_payload_server_player_type=" << counts.raw_player_type_matches
        << "; pending_increased=" << counts.pending_increased << "; pending_added=" << counts.pending_added
        << "; pending_before_max=" << counts.pending_before_max << "; pending_after_max=" << counts.pending_after_max
        << "; post_owner_changed=" << counts.post_owner_changed << '\n';
    out << "Native server spawn inventory: " << (inventory.available ? "available" : "unavailable") << "; " << inventory.issue
        << "; partition=" << inventory.partition << "; players=" << inventory.players.size()
        << "; registry_entries=" << inventory.registry_entries;
    for (const auto& player : inventory.players)
        out << "\n  server player native_id=" << player.native_player_id << "; team=" << player.team
            << "; vtable=" << std::hex << player.vtable - base << std::dec
            << "; bot44=" << static_cast<unsigned>(player.bot) << "; spectator70=" << static_cast<unsigned>(player.spectator)
            << "; ready2e3=" << static_cast<unsigned>(player.ready) << "; native_spawn_ready=" << player.spawn_ready
            << "; handoff_state_ready=" << player.handoff_state_ready << "; handoff_state=" << player.handoff_state_status;
    unsigned shown{};
    for (const auto& info : inventory.spawners) {
        if (++shown > 32) break;
        out << "\n  spawner vtable=" << std::hex << info.vtable - base << std::dec << "; known=" << info.known_type
            << "; registry=" << info.registry_index << "; context=" << info.context_matches;
        if (!info.known_type) continue;
        out << "; spawn_method=" << std::hex << info.spawn_method - base << std::dec
            << "; spawn_enabled250=" << static_cast<unsigned>(info.spawn_enabled_250)
            << "; count=" << info.count << "; capacity=";
        if (info.capacity_available) out << info.capacity << "; capacity_allows=" << info.capacity_allows;
        else out << "dynamic/unavailable";
        out << "; veto_callbacks=" << info.veto_present << "; pending=" << info.pending.size();
        for (const auto& pending : info.pending)
            out << "\n    pending resolved=" << pending.resolved << "; current_server_member=" << pending.current_member
                << "; native_spawn_ready=" << pending.spawn_ready;
    }
    return out.str();
}
}
