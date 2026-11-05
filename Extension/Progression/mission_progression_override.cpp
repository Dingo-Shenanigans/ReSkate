#include "mission_progression_override.h"
#include "mission_progression_internal.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/mission_progression.h"

#include <Windows.h>
#include "Engine/Core/Hooks/hooks.h"
#include <intrin.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace dingosdk {
using namespace mission_progression_detail;
namespace {
namespace mission = addr::mission_progression;

constexpr std::uintptr_t image_size = supported_build::game_image_size;
constexpr std::size_t maximum_quest_id_length = 63;
constexpr std::int32_t quest_state_claimed = 5;

static_assert(mission::quest_lookup + mission::quest_lookup_prefix.size() <= image_size);
static_assert(mission::component_query + mission::component_query_prefix.size() <= image_size);
static_assert(mission::quest_status_query_call + mission::quest_status_query_call_prefix.size() <= image_size);
static_assert(mission::quest_status_query_return + mission::quest_state_consumer_prefix.size() <= image_size);
static_assert(mission::quest_status_query_call + mission::quest_status_query_call_prefix.size() ==
    mission::quest_status_query_return);

struct LookupTag {
    std::uintptr_t base{};
    std::uint64_t handle{};
    std::uint64_t generation{};
    std::size_t mission_index{};
    bool valid{};
    bool persisted{};
};

thread_local LookupTag thread_lookup_tag{};

struct PreserveError {
    DWORD value{GetLastError()};
    ~PreserveError() { SetLastError(value); }
};

bool valid_range(std::uintptr_t address, std::size_t size) noexcept {
    return address >= 0x10000 && size && size <= memory::highest_user_address &&
        address <= memory::highest_user_address - size;
}

template<class T>
bool safe_write(std::uintptr_t address, const T& value) noexcept {
    SIZE_T copied{};
    return valid_range(address, sizeof(value)) &&
        WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address),
            &value, sizeof(value), &copied) && copied == sizeof(value);
}

template<std::size_t N>
bool fingerprint_matches(
    std::uintptr_t address, const std::array<unsigned char, N>& expected) noexcept {
    std::array<unsigned char, N> actual{};
    return memory::read(address, actual) && actual == expected;
}

bool read_quest_id(const void* wrapper,
    std::array<char, maximum_quest_id_length + 1>& buffer,
    std::string_view& value) noexcept {
    std::uintptr_t text{};
    if (!memory::read(reinterpret_cast<std::uintptr_t>(wrapper), text)) return false;
    for (std::size_t index = 0; index != maximum_quest_id_length + 1; ++index) {
        char character{};
        if (!memory::read(text + index, character)) return false;
        if (!character) {
            value = std::string_view(buffer.data(), index);
            return index != 0;
        }
        const bool valid = (character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9') || character == '_';
        if (!valid || index == maximum_quest_id_length) return false;
        buffer[index] = character;
    }
    return false;
}

bool valid_quest_state(std::int32_t state) noexcept {
    return state >= 0 && state <= 6;
}

std::size_t progression_quest_index(std::string_view quest_id) noexcept {
    for (std::size_t index = 0; index != main_mission_ids.size(); ++index) {
        if (quest_id == main_mission_ids[index]) return index;
    }
    for (std::size_t index = 0; index != progression_unlock_quest_ids.size(); ++index) {
        if (quest_id == progression_unlock_quest_ids[index])
            return main_mission_ids.size() + index;
    }
    for (std::size_t index = 0; index != onboarding_dependency_quest_ids.size(); ++index) {
        if (quest_id == onboarding_dependency_quest_ids[index])
            return main_mission_ids.size() + progression_unlock_quest_ids.size() + index;
    }
    return override_quest_count;
}

bool lease_claimed_state(MainMissionOverrideState& state,
    std::size_t mission_index, std::uintptr_t component,
    std::uint64_t handle) noexcept {
    std::lock_guard lock(state.lease_mutex);
    auto& lease = state.leases[mission_index];
    if (lease.active &&
        (lease.component != component || lease.handle != handle)) {
        lease = {};
        ++state.identity_releases;
    }
    std::int32_t native_state{};
    if (!memory::read(component + 0x18, native_state) ||
        !valid_quest_state(native_state))
        return false;
    if (lease.active && native_state == quest_state_claimed) return true;
    if (lease.active) {
        lease = {};
        ++state.identity_releases;
    }
    if (native_state == quest_state_claimed) return true;
    if (!safe_write(component + 0x18, quest_state_claimed)) return false;
    lease = {component, handle, native_state, true};
    ++state.state_writes;
    return true;
}

bool restore_observed_lease(MainMissionOverrideState& state,
    std::size_t mission_index, std::uintptr_t component,
    std::uint64_t handle) noexcept {
    std::lock_guard lock(state.lease_mutex);
    auto& lease = state.leases[mission_index];
    if (!lease.active) return true;
    if (lease.component != component || lease.handle != handle) {
        lease = {};
        ++state.identity_releases;
        return true;
    }
    std::int32_t current{};
    if (!memory::read(component + 0x18, current)) return false;
    if (current != quest_state_claimed) {
        ++state.conditional_restore_skips;
        lease = {};
        return true;
    }
    if (!safe_write(component + 0x18, lease.original)) return false;
    ++state.state_restores;
    lease = {};
    return true;
}

bool apply_persisted_state(MainMissionOverrideState& state, std::size_t index,
    std::uintptr_t component, std::uint64_t handle) {
    const auto id = index < main_mission_ids.size() ? main_mission_ids[index] :
        index < main_mission_ids.size() + progression_unlock_quest_ids.size() ?
        progression_unlock_quest_ids[index - main_mission_ids.size()] :
        onboarding_dependency_quest_ids[index - main_mission_ids.size() - progression_unlock_quest_ids.size()];
    const auto lookup = state.profile_lookup.load(std::memory_order_acquire);
    std::int32_t desired{};
    if (!lookup || !lookup(id, desired) || (desired != 0 && desired != quest_state_claimed)) return false;
    std::int32_t current{};
    if (!memory::read(component + 0x18, current) || current < 0 || current > 6) return false;
    std::lock_guard lock(state.lease_mutex);
    const auto save = state.save_completion.load(std::memory_order_acquire);
    auto& reset = state.profile_resets[index];
    if (desired == 0 && save && reset.handle == handle && reset.component == component) {
        // A reset is applied once to this freshly resolved identity. Do not
        // pin it to zero while the player attempts the mission again.
        return current != quest_state_claimed || save(id);
    }
    if (current != desired) {
        if (!safe_write(component + 0x18, desired)) return false;
        ++state.profile_state_writes;
    }
    // Durable completion is never a temporary debug lease.
    state.leases[index] = {};
    reset = desired == 0 ? MainMissionOverrideState::ResetIdentity{handle, component} : MainMissionOverrideState::ResetIdentity{};
    if (index >= main_mission_ids.size() + progression_unlock_quest_ids.size())
        state.profile_resolved_mask.fetch_or(1U <<
            (index - main_mission_ids.size() - progression_unlock_quest_ids.size()));
    return true;
}

bool hydrate_resolved_quest(MainMissionOverrideState& state, std::size_t index,
    std::uint64_t handle) {
    // Both native views are non-owning and kept only for this lookup. The
    // constructor validates the entity generation through the ECS registry.
    std::array<std::uintptr_t, 3> context{};
    if (!state.construct_query || state.construct_query(context.data(), handle) != context.data() ||
        context[0] != handle || !context[1] || !context[2]) return false;
    std::uint32_t entity{};
    if (!memory::read(context[2], entity) || entity != static_cast<std::uint32_t>(handle)) return false;
    // Same query-result vtable, handle and type fields as the native quest
    // status query (see the build table).
    std::array<std::uintptr_t, 5> result{state.base + mission::query_result_vtable, 0, 0,
        handle, state.base + mission::quest_status_type};
    state.original_query.load(std::memory_order_acquire)(context.data(),
        reinterpret_cast<void*>(state.base + mission::quest_status_component_descriptor), result.data(), 0);
    return result[1] && result[2] && apply_persisted_state(state, index, result[2], handle);
}

std::uint64_t* quest_lookup_hook(
    std::uintptr_t manager, std::uint64_t* output, const void* quest_id_wrapper) {
    const auto incoming_error = GetLastError();
    auto& state = main_mission_override_state();
    const auto return_address = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const bool expression_lookup = state.base && return_address == state.base + mission::quest_expression_return;
    thread_lookup_tag = {};
    const auto original = state.original_lookup.load(std::memory_order_acquire);
    const auto gate = state.gate.load(std::memory_order_acquire);
    const auto profile_lookup = state.profile_lookup.load(std::memory_order_acquire);
    const bool tracking = gate == GateStatus::active || gate == GateStatus::restoring || profile_lookup;
    if (tracking) ++state.lookup_calls;
    SetLastError(incoming_error);
    std::uint64_t* result{};
    try {
        result = original(manager, output, quest_id_wrapper);
    } catch (...) {
        const auto native_error = GetLastError();
        if (tracking) ++state.native_exceptions;
        SetLastError(native_error);
        throw;
    }
    const auto native_error = GetLastError();
    if (tracking && output) {
        std::array<char, maximum_quest_id_length + 1> identifier{};
        std::string_view quest_id;
        std::uint64_t handle{};
        if (read_quest_id(quest_id_wrapper, identifier, quest_id)) {
            const auto mission_index = progression_quest_index(quest_id);
            if (mission_index == override_quest_count) {
                SetLastError(native_error);
                return result;
            }
            ++state.allowlisted_lookups;
            std::int32_t saved{};
            const bool persisted = profile_lookup && profile_lookup(quest_id, saved) &&
                (saved == 0 || saved == quest_state_claimed);
            if (!persisted && gate != GateStatus::active && gate != GateStatus::restoring) {
                SetLastError(native_error);
                return result;
            }
            if (memory::read(reinterpret_cast<std::uintptr_t>(output), handle) &&
                static_cast<std::uint32_t>(handle >> 32) != 0) {
                thread_lookup_tag = {state.base, handle,
                    state.generation.load(std::memory_order_acquire),
                    mission_index, true, persisted};
                ++state.tagged_handles;
                if (persisted && expression_lookup) {
                    ++state.profile_resolve_attempts;
                    bool hydrated{};
                    try { hydrated = hydrate_resolved_quest(state, mission_index, handle); }
                    catch (...) { /* Preserve the successful native lookup on adapter failure. */ }
                    if (hydrated) ++state.profile_resolve_matches;
                    else ++state.profile_rejected_resolves;
                    // No tag may leak into an unrelated later query.
                    thread_lookup_tag = {};
                }
            }
        }
    }
    SetLastError(native_error);
    return result;
}

std::uint64_t component_query_hook(
    const void* context, const void* component_descriptor,
    void* output, std::uint32_t mode) {
    const auto incoming_error = GetLastError();
    auto& state = main_mission_override_state();
    const auto return_address = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const auto original = state.original_query.load(std::memory_order_acquire);
    if (!state.base || return_address != state.base + mission::quest_status_query_return)
        return original(context, component_descriptor, output, mode);

    const auto gate = state.gate.load(std::memory_order_acquire);
    const bool active = gate == GateStatus::active;
    const bool restoring = gate == GateStatus::restoring;
    const bool tracking = active || restoring || state.profile_lookup.load(std::memory_order_acquire);
    if (!tracking) return original(context, component_descriptor, output, mode);

    const auto tag = thread_lookup_tag;
    thread_lookup_tag = {};
    if (!state.classify_caller(return_address, state.base))
        return original(context, component_descriptor, output, mode);
    ++state.query_calls;

    SetLastError(incoming_error);
    std::uint64_t native_result{};
    try {
        native_result = original(context, component_descriptor, output, mode);
    } catch (...) {
        const auto native_error = GetLastError();
        if (tracking) ++state.native_exceptions;
        SetLastError(native_error);
        throw;
    }
    const auto native_error = GetLastError();
    if (reinterpret_cast<std::uintptr_t>(component_descriptor) ==
            state.base + mission::quest_status_component_descriptor &&
        tag.valid && tag.base == state.base &&
        tag.generation == state.generation.load(std::memory_order_acquire)) {
        std::uint64_t context_handle{};
        std::uintptr_t result_guard{};
        std::uintptr_t component{};
        const auto context_address = reinterpret_cast<std::uintptr_t>(context);
        const auto output_address = reinterpret_cast<std::uintptr_t>(output);
        if (memory::read(context_address, context_handle) && context_handle == tag.handle &&
            memory::read(output_address + 8, result_guard) && result_guard &&
            memory::read(output_address + 16, component) && component &&
            (tag.persisted ? apply_persisted_state(state, tag.mission_index, component, tag.handle) : active
                ? lease_claimed_state(state, tag.mission_index, component, tag.handle)
                : restore_observed_lease(
                    state, tag.mission_index, component, tag.handle))) {
            ++state.exact_query_matches;
        } else {
            ++state.rejected_matches;
        }
    }
    if (restoring) {
        std::lock_guard lock(state.lease_mutex);
        if (!pending_lease_count(state)) {
            auto expected = GateStatus::restoring;
            state.gate.compare_exchange_strong(expected, GateStatus::prepared,
                std::memory_order_acq_rel);
        }
    }
    SetLastError(native_error);
    return native_result;
}

void remove_unenabled_hooks(MainMissionOverrideState& state,
    void* lookup_target, void* query_target) noexcept {
    bool failed{};
    if (hook_remove(query_target) != HookOk) failed = true;
    if (hook_remove(lookup_target) != HookOk) failed = true;
    if (failed) state.gate.store(GateStatus::rollback_failed, std::memory_order_release);
}

bool disable_after_enable_attempt(void* target) noexcept {
    const auto status = hook_disable(target);
    return status == HookOk || status == HookDisabled;
}

void retain_after_enable_attempt(MainMissionOverrideState& state,
    void* lookup_target, void* query_target, GateStatus failure) noexcept {
    // A failed enable can race with a thread that already entered the detour.
    // Keep both created hooks and their trampolines alive for process lifetime.
    const bool query_disabled = disable_after_enable_attempt(query_target);
    const bool lookup_disabled = disable_after_enable_attempt(lookup_target);
    state.gate.store(query_disabled && lookup_disabled
            ? failure : GateStatus::rollback_failed,
        std::memory_order_release);
}

} // namespace

namespace mission_progression_detail {
MainMissionOverrideState& main_mission_override_state() {
    static auto* value = new MainMissionOverrideState;
    return *value;
}

bool exact_status_query_caller(
    std::uintptr_t return_address, std::uintptr_t base) noexcept {
    return base && return_address == base + mission::quest_status_query_return;
}

std::size_t pending_lease_count(const MainMissionOverrideState& state) noexcept {
    std::size_t result{};
    for (const auto& lease : state.leases) {
        if (lease.active) ++result;
    }
    return result;
}
} // namespace mission_progression_detail

std::span<const std::string_view> current_main_mission_ids() noexcept {
    return main_mission_ids;
}

bool is_current_main_mission_id(std::string_view quest_id) noexcept {
    for (const auto expected : main_mission_ids) {
        if (quest_id == expected) return true;
    }
    return false;
}

std::span<const std::string_view> current_progression_unlock_quest_ids() noexcept {
    return progression_unlock_quest_ids;
}

bool is_current_progression_unlock_quest_id(std::string_view quest_id) noexcept {
    for (const auto expected : progression_unlock_quest_ids) {
        if (quest_id == expected) return true;
    }
    return false;
}

std::span<const std::string_view> current_onboarding_dependency_quest_ids() noexcept {
    return onboarding_dependency_quest_ids;
}

bool is_current_onboarding_dependency_quest_id(std::string_view quest_id) noexcept {
    for (const auto expected : onboarding_dependency_quest_ids) {
        if (quest_id == expected) return true;
    }
    return false;
}

bool prepare_main_mission_override(std::uintptr_t base) noexcept {
    PreserveError preserve;
    auto& state = main_mission_override_state();
    try {
        std::lock_guard lock(state.initialization_mutex);
        if (state.attempted) {
            const auto gate = state.gate.load(std::memory_order_acquire);
            return state.base == base &&
                (gate == GateStatus::prepared || gate == GateStatus::route_required ||
                 gate == GateStatus::restoring || gate == GateStatus::active);
        }
        state.attempted = true;
        state.base = base;
        if (!state.verify_image(base)) {
            state.gate.store(GateStatus::image_mismatch, std::memory_order_release);
            return false;
        }
        if (!fingerprint_matches(base + mission::quest_lookup, mission::quest_lookup_prefix)) {
            state.gate.store(GateStatus::lookup_fingerprint_mismatch, std::memory_order_release);
            return false;
        }
        if (!fingerprint_matches(base + mission::component_query, mission::component_query_prefix)) {
            state.gate.store(GateStatus::query_fingerprint_mismatch, std::memory_order_release);
            return false;
        }
        if (!fingerprint_matches(base + mission::quest_status_query_call,
                mission::quest_status_query_call_prefix)) {
            state.gate.store(GateStatus::callsite_fingerprint_mismatch, std::memory_order_release);
            return false;
        }
        if (!fingerprint_matches(base + mission::quest_status_query_return,
                mission::quest_state_consumer_prefix)) {
            state.gate.store(GateStatus::consumer_fingerprint_mismatch, std::memory_order_release);
            return false;
        }

        if (!fingerprint_matches(base + mission::quest_expression_call, mission::quest_expression_call_prefix) ||
            !fingerprint_matches(base + mission::query_context_constructor, mission::query_context_constructor_prefix) ||
            !fingerprint_matches(base + mission::query_context_initializer, mission::query_context_initializer_prefix)) {
            state.gate.store(GateStatus::profile_fingerprint_mismatch, std::memory_order_release);
            return false;
        }
        state.construct_query = reinterpret_cast<QueryContextConstructor>(base + mission::query_context_constructor);
        state.gate.store(GateStatus::preparing, std::memory_order_release);
        auto* lookup_target = reinterpret_cast<void*>(base + mission::quest_lookup);
        auto* query_target = reinterpret_cast<void*>(base + mission::component_query);
        QuestLookup lookup_trampoline{};
        ComponentQuery query_trampoline{};
#pragma warning(push)
#pragma warning(disable: 4191)
        const auto lookup_create = hook_prepare(lookup_target,
            reinterpret_cast<void*>(&quest_lookup_hook),
            reinterpret_cast<void**>(&lookup_trampoline));
#pragma warning(pop)
        if (lookup_create != HookOk) {
            state.gate.store(GateStatus::lookup_create_failed, std::memory_order_release);
            return false;
        }
#pragma warning(push)
#pragma warning(disable: 4191)
        const auto query_create = hook_prepare(query_target,
            reinterpret_cast<void*>(&component_query_hook),
            reinterpret_cast<void**>(&query_trampoline));
#pragma warning(pop)
        if (query_create != HookOk) {
            state.gate.store(hook_remove(lookup_target) == HookOk
                    ? GateStatus::query_create_failed : GateStatus::rollback_failed,
                std::memory_order_release);
            return false;
        }
        if (!lookup_trampoline || !query_trampoline) {
            remove_unenabled_hooks(state, lookup_target, query_target);
            if (state.gate.load(std::memory_order_acquire) != GateStatus::rollback_failed)
                state.gate.store(GateStatus::trampoline_missing, std::memory_order_release);
            return false;
        }
        state.original_lookup.store(lookup_trampoline, std::memory_order_release);
        state.original_query.store(query_trampoline, std::memory_order_release);
        if (hook_enable(lookup_target) != HookOk) {
            retain_after_enable_attempt(state, lookup_target, query_target,
                GateStatus::lookup_enable_failed);
            return false;
        }
        if (hook_enable(query_target) != HookOk) {
            retain_after_enable_attempt(state, lookup_target, query_target,
                GateStatus::query_enable_failed);
            return false;
        }
        state.gate.store(GateStatus::prepared, std::memory_order_release);
        return true;
    } catch (...) {
        state.gate.store(GateStatus::rollback_failed, std::memory_order_release);
        return false;
    }
}

void set_main_mission_profile_provider(std::uintptr_t base, LocalQuestStateLookup lookup,
    LocalQuestCompletionSave save) noexcept {
    auto& state = main_mission_override_state();
    const auto gate = state.gate.load(std::memory_order_acquire);
    if (state.base != base || (gate != GateStatus::prepared && gate != GateStatus::active &&
        gate != GateStatus::restoring && gate != GateStatus::route_required)) return;
    state.save_completion.store(save, std::memory_order_release);
    state.profile_lookup.store(lookup, std::memory_order_release);
}

void reset_local_mission_hydration(std::string_view id) noexcept {
    auto& state = main_mission_override_state();
    const auto index = progression_quest_index(id);
    if (index == override_quest_count) return;
    std::lock_guard lock(state.lease_mutex);
    state.profile_resets[index] = {};
}

bool arm_main_mission_override(
    std::uintptr_t base, bool authored_offline_route) noexcept {
    PreserveError preserve;
    auto& state = main_mission_override_state();
    try {
        std::lock_guard lock(state.initialization_mutex);
        const auto gate = state.gate.load(std::memory_order_acquire);
        if (state.base != base ||
            (gate != GateStatus::prepared && gate != GateStatus::route_required &&
             gate != GateStatus::restoring && gate != GateStatus::active))
            return false;
        if (!authored_offline_route) {
            if (gate == GateStatus::active) {
                ++state.generation;
                std::lock_guard lease_lock(state.lease_mutex);
                state.gate.store(pending_lease_count(state)
                        ? GateStatus::restoring : GateStatus::prepared,
                    std::memory_order_release);
            }
            state.authored_offline_route.store(false, std::memory_order_release);
            if (state.gate.load(std::memory_order_acquire) == GateStatus::prepared)
                state.gate.store(GateStatus::route_required, std::memory_order_release);
            return false;
        }
        if (gate != GateStatus::active) ++state.generation;
        state.authored_offline_route.store(true, std::memory_order_release);
        state.gate.store(GateStatus::active, std::memory_order_release);
        return true;
    } catch (...) {
        return false;
    }
}

void restore_main_mission_override() noexcept {
    PreserveError preserve;
    auto& state = main_mission_override_state();
    try {
        std::lock_guard lock(state.initialization_mutex);
        const auto gate = state.gate.load(std::memory_order_acquire);
        if (gate == GateStatus::active) {
            ++state.generation;
            std::lock_guard lease_lock(state.lease_mutex);
            state.gate.store(pending_lease_count(state)
                    ? GateStatus::restoring : GateStatus::prepared,
                std::memory_order_release);
        } else if (gate == GateStatus::route_required) {
            state.gate.store(GateStatus::prepared, std::memory_order_release);
        }
        state.authored_offline_route.store(false, std::memory_order_release);
        thread_lookup_tag = {};
    } catch (...) {
    }
}

} // namespace dingosdk
