#include "gameplay_settings_override.h"
#include "gameplay_settings_internal.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dingosdk {
using namespace gameplay_settings_detail;
namespace {
constexpr std::uintptr_t settings_highest = memory::highest_user_address;

using SettingsLookup = std::uintptr_t (*)(std::uintptr_t, const void*);

bool settings_range(std::uintptr_t address, std::size_t size) {
    return address >= 0x10000 && size && size <= settings_highest && address <= settings_highest - size;
}

bool settings_writable(std::uintptr_t address, std::size_t size) {
    if (!settings_range(address, size)) return false;
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) ||
        memory.State != MEM_COMMIT || memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    if (address < start || memory.RegionSize > settings_highest - start ||
        address + size > start + memory.RegionSize) return false;
    switch (memory.Protect & 0xff) {
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

const char* group_name(overlay::OfflineFeatureGroup group) {
    switch (group) {
    case overlay::OfflineFeatureGroup::activities: return "activities";
    case overlay::OfflineFeatureGroup::fast_travel: return "fast travel";
    case overlay::OfflineFeatureGroup::progression: return "progression systems";
    case overlay::OfflineFeatureGroup::developer_menus: return "developer menus";
    case overlay::OfflineFeatureGroup::board_wear: return "board wear";
    case overlay::OfflineFeatureGroup::player_collision: return "player collision";
    default: return "all gameplay settings";
    }
}

std::uintptr_t resolve_type(SettingsState& state, SettingsType type_id,
    std::array<TypeResolution, static_cast<std::size_t>(SettingsType::count)>& cache) {
    const auto index = static_cast<std::size_t>(type_id);
    auto& cached = cache[index];
    if (cached.attempted) return cached.available ? cached.object : 0;
    cached.attempted = true;
    const auto& type = type_specs[index];
    std::uintptr_t manager{};
    std::uint32_t bucket_count{};
    std::uintptr_t buckets{};
    if (!memory::read(state.base + addr::engine::settings_manager, manager) || !settings_range(manager, 0xd8) ||
        !memory::read(manager + 0xc8, buckets) || !memory::read(manager + 0xd0, bucket_count) ||
        !settings_range(buckets, 8) || !bucket_count || bucket_count > 0x100000) return 0;
    ++state.lookups;
    const auto lookup = reinterpret_cast<SettingsLookup>(state.base + addr::engine::settings_lookup);
    const auto object = lookup(manager, reinterpret_cast<const void*>(state.base + type.handle_rva));
    std::uintptr_t vtable{}, object_type{}, manager_after{};
    if (!settings_range(object, type.object_size) || !memory::read(object, vtable) ||
        !memory::read(object + 8, object_type) || vtable != state.base + type.vtable_rva ||
        object_type != state.base + type.handle_rva ||
        !memory::read(state.base + addr::engine::settings_manager, manager_after) || manager_after != manager) return 0;
    cached.available = true;
    cached.object = object;
    return object;
}

std::vector<FieldSnapshot> snapshot_group(SettingsState& state, overlay::OfflineFeatureGroup group) {
    std::array<TypeResolution, static_cast<std::size_t>(SettingsType::count)> cache{};
    std::vector<FieldSnapshot> snapshots;
    snapshots.reserve(field_specs.size());
    for (std::size_t i = 0; i < field_specs.size(); ++i)
        if (field_specs[i].group == group) snapshots.push_back(snapshot_field(state, i, cache));
    settings_require(!snapshots.empty(), "Unknown gameplay settings group.");
    return snapshots;
}

bool write_field(SettingsState& state, const FieldSnapshot& snapshot, std::uint8_t value) {
    const auto address = snapshot.object + field_specs[snapshot.index].offset;
    if (!settings_writable(address, 1)) return false;
    SIZE_T count{};
    if (!WriteProcessMemory(GetCurrentProcess(),
            reinterpret_cast<void*>(address), &value, 1, &count) || count != 1)
        return false;
    std::uint8_t actual{};
    if (!memory::read(address, actual) || actual != value) return false;
    ++state.writes;
    return true;
}

std::size_t release_replaced_leases(SettingsState& state,
    overlay::OfflineFeatureGroup group, const std::vector<FieldSnapshot>& snapshots) {
    std::size_t released{};
    for (const auto& snapshot : snapshots) {
        auto& lease = state.leases[snapshot.index];
        if (lease.owned && lease.object != snapshot.object) {
            lease = {};
            ++released;
            ++state.abandoned;
        }
    }
    if (released) state.requested[group_index(group)] = false;
    return released;
}
} // namespace

namespace gameplay_settings_detail {
SettingsState& settings_state() {
    static auto* state = new SettingsState;
    return *state;
}

FieldSnapshot snapshot_field(SettingsState& state, std::size_t index,
    std::array<TypeResolution, static_cast<std::size_t>(SettingsType::count)>& cache) {
    const auto& field = field_specs[index];
    const auto object = resolve_type(state, field.type, cache);
    settings_require(object != 0, std::string(type_specs[static_cast<std::size_t>(field.type)].name) +
        " is not initialized.");
    std::uint8_t value{};
    settings_require(memory::read(object + field.offset, value) && value <= 1,
        std::string(field.name) + " is unavailable.");
    return {index, object, value};
}

void enable_group(SettingsState& state, overlay::OfflineFeatureGroup group) {
    auto snapshots = snapshot_group(state, group);
    release_replaced_leases(state, group, snapshots);
    for (const auto& snapshot : snapshots) {
        const auto& lease = state.leases[snapshot.index];
        settings_require(!lease.owned || lease.object == snapshot.object,
            std::string(field_specs[snapshot.index].name) + " settings object changed; restore is deferred.");
        settings_require(!lease.owned || snapshot.value == lease.applied || snapshot.value == lease.original,
            std::string(field_specs[snapshot.index].name) + " changed outside ReSkate.");
        settings_require(settings_writable(snapshot.object + field_specs[snapshot.index].offset, 1),
            std::string(field_specs[snapshot.index].name) + " is not writable.");
    }

    struct PendingChange { FieldSnapshot before; Lease previous; };
    std::vector<PendingChange> changes;
    for (const auto& snapshot : snapshots) {
        auto& lease = state.leases[snapshot.index];
        if (snapshot.value == 1) continue;
        changes.push_back({snapshot, lease});
        if (!lease.owned) lease = {true, snapshot.object, snapshot.value, 1};
        else lease.applied = 1;
        if (!write_field(state, snapshot, 1)) {
            bool rollback_complete = true;
            // Roll back every attempted write, including individually owned
            // fields that existed before this group transaction began.
            for (auto it = changes.rbegin(); it != changes.rend(); ++it) {
                auto& rollback_lease = state.leases[it->before.index];
                std::uint8_t current{};
                const auto address = it->before.object + field_specs[it->before.index].offset;
                if (memory::read(address, current) &&
                    (current == it->before.value ||
                     (current == 1 && write_field(state, it->before, it->before.value))))
                    rollback_lease = it->previous;
                else rollback_complete = false;
            }
            state.requested[group_index(group)] = false;
            settings_require(false, rollback_complete ?
                "Gameplay settings write failed; prior bytes and overrides were restored." :
                "Gameplay settings write failed and a restoration lease remains active.");
        }
    }
    for (const auto& snapshot : snapshots) {
        auto& lease = state.leases[snapshot.index];
        if (lease.owned && lease.original == 1) lease = {};
    }
    state.requested[group_index(group)] = true;
    state.model.status = std::string("Enabled process-local ") + group_name(group) + " overrides.";
}

void restore_group(SettingsState& state, overlay::OfflineFeatureGroup group) {
    auto snapshots = snapshot_group(state, group);
    const auto abandoned = release_replaced_leases(state, group, snapshots);
    for (const auto& snapshot : snapshots) {
        const auto& lease = state.leases[snapshot.index];
        settings_require(!lease.owned || lease.object == snapshot.object,
            std::string(field_specs[snapshot.index].name) + " settings object changed; restore is deferred.");
        settings_require(!lease.owned || snapshot.value == lease.applied || snapshot.value == lease.original,
            std::string(field_specs[snapshot.index].name) + " changed outside ReSkate.");
        settings_require(!lease.owned || snapshot.value == lease.original ||
                settings_writable(snapshot.object + field_specs[snapshot.index].offset, 1),
            std::string(field_specs[snapshot.index].name) + " is not writable for restoration.");
    }
    for (const auto& snapshot : snapshots) {
        auto& lease = state.leases[snapshot.index];
        if (!lease.owned) continue;
        if (snapshot.value == lease.applied) {
            settings_require(write_field(state, snapshot, lease.original),
                std::string(field_specs[snapshot.index].name) + " could not be restored.");
            ++state.restores;
        } else {
            settings_require(snapshot.value == lease.original,
                std::string(field_specs[snapshot.index].name) + " changed outside ReSkate.");
        }
        lease = {};
    }
    state.requested[group_index(group)] = false;
    state.model.status = abandoned ?
        std::string("Released replaced ") + group_name(group) +
            " settings identity without a stale write." :
        std::string("Restored process-local ") + group_name(group) + " settings.";
}

bool release_replaced_lease(SettingsState& state, const FieldSnapshot& snapshot) {
    auto& lease = state.leases[snapshot.index];
    if (!lease.owned || lease.object == snapshot.object) return false;
    lease = {};
    state.requested[group_index(field_specs[snapshot.index].group)] = false;
    ++state.abandoned;
    return true;
}

void restore_variable(SettingsState& state, std::size_t index) {
    std::array<TypeResolution, static_cast<std::size_t>(SettingsType::count)> cache{};
    const auto snapshot = snapshot_field(state, index, cache);
    const auto abandoned = release_replaced_lease(state, snapshot);
    state.requested[group_index(field_specs[index].group)] = false;
    auto& lease = state.leases[index];
    if (!lease.owned) {
        state.model.status = abandoned ?
            std::string("Released replaced ") + field_specs[index].name +
                " identity without a stale write." :
            std::string(field_specs[index].name) + " has no ReSkate override to restore.";
        return;
    }
    settings_require(lease.object == snapshot.object,
        std::string(field_specs[index].name) + " settings object changed; restore is deferred.");
    settings_require(snapshot.value == lease.applied || snapshot.value == lease.original,
        std::string(field_specs[index].name) + " changed outside ReSkate.");
    if (snapshot.value == lease.applied) {
        settings_require(settings_writable(snapshot.object + field_specs[index].offset, 1),
            std::string(field_specs[index].name) + " is not writable for restoration.");
        settings_require(write_field(state, snapshot, lease.original),
            std::string(field_specs[index].name) + " could not be restored.");
        ++state.restores;
    }
    lease = {};
    state.model.status = std::string("Restored process-local ") + field_specs[index].name + '.';
}

void set_variable(SettingsState& state, std::size_t index, bool enabled) {
    std::array<TypeResolution, static_cast<std::size_t>(SettingsType::count)> cache{};
    const auto snapshot = snapshot_field(state, index, cache);
    release_replaced_lease(state, snapshot);
    state.requested[group_index(field_specs[index].group)] = false;
    auto& lease = state.leases[index];
    settings_require(!lease.owned || lease.object == snapshot.object,
        std::string(field_specs[index].name) + " settings object changed; restore is deferred.");
    settings_require(!lease.owned || snapshot.value == lease.applied || snapshot.value == lease.original,
        std::string(field_specs[index].name) + " changed outside ReSkate.");

    const auto desired = static_cast<std::uint8_t>(enabled ? 1 : 0);
    if (snapshot.value == desired) {
        if (lease.owned && desired == lease.original) lease = {};
        state.model.status = std::string(field_specs[index].name) +
            (enabled ? " is enabled." : " is disabled.");
        return;
    }
    settings_require(settings_writable(snapshot.object + field_specs[index].offset, 1),
        std::string(field_specs[index].name) + " is not writable.");

    const bool newly_owned = !lease.owned;
    if (newly_owned) lease = {true, snapshot.object, snapshot.value, desired};
    if (!write_field(state, snapshot, desired)) {
        std::uint8_t current{};
        if (newly_owned && memory::read(snapshot.object + field_specs[index].offset, current) &&
            current == lease.original) lease = {};
        settings_require(false, lease.owned ?
            "Engine variable write failed and a restoration lease remains active." :
            "Engine variable write failed; the original byte was retained.");
    }
    if (lease.owned && desired == lease.original) {
        lease = {};
        ++state.restores;
    } else if (lease.owned) {
        lease.applied = desired;
    }
    state.model.status = std::string("Set process-local ") + field_specs[index].name +
        (enabled ? " to true." : " to false.");
}
} // namespace gameplay_settings_detail
} // namespace dingosdk
