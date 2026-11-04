#include "offline_boot.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/Build/image_identity.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/global_offline.h"
#include "Engine/Core/Platform/memory.h"

#include <Windows.h>
#include "Engine/Core/Hooks/hooks.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <intrin.h>
#include <mutex>
#include <sstream>

namespace dingosdk {
namespace {

using OnlineEnabledFunction = std::uint8_t (*)();
using ImageVerifier = bool (*)(std::uintptr_t) noexcept;
using OptInReader = bool (*)() noexcept;
using TargetCallerClassifier = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t) noexcept;

namespace offline = addr::global_offline;
constexpr std::uintptr_t function_rva = offline::online_enabled;
constexpr std::uintptr_t backend_singleton_rva = addr::engine::backend_services;
constexpr std::uintptr_t backend_predicate_return_rva = offline::backend_predicate_return;
constexpr std::uintptr_t async_prepare_character_return_rva =
    offline::async_prepare_character_return;
constexpr std::uintptr_t character_readiness_return_rva = offline::character_readiness_return;
// The stable getIsOnlineEnabled callable GUID selects this invocation adapter.
// Its CALL enters a tail-JMP thunk, so the leaf sees the adapter return address.
constexpr std::uintptr_t flow_adapter_return_rva = offline::flow_adapter_return;
constexpr std::uintptr_t post_spawn_progression_return_rva =
    offline::post_spawn_progression_return;
constexpr std::uintptr_t image_size = supported_build::game_image_size;
// The launcher's switch, and the name older launchers set (offline_boot.h).
constexpr wchar_t opt_in_name[] = L"RESKATE_GLOBAL_OFFLINE";
constexpr wchar_t old_opt_in_name[] = L"RESKATE_DEBUG_TEST_GLOBAL_OFFLINE";

static_assert(function_rva + 16 <= image_size,
    "Shared OnlineEnabled fingerprint must remain inside the inspected image");
static_assert(backend_singleton_rva + sizeof(std::uintptr_t) <= image_size,
    "Backend singleton must remain inside the inspected image");

// Detours hook service needs the INT3 padding in the fingerprint because the
// native function body itself is only three bytes.
constexpr auto function_fingerprint = offline::online_enabled_prefix;

enum class GateStatus : unsigned char {
    not_attempted,
    opted_out,
    image_mismatch,
    fingerprint_mismatch,
    backend_probe_failed,
    activation_too_late,
    create_failed,
    enable_failed,
    enable_failed_disable_failed,
    too_late_disable_failed,
    activating,
    active
};

bool environment_opted_in() noexcept;
std::uintptr_t exact_target_caller(
    std::uintptr_t return_address, std::uintptr_t base) noexcept;

struct OfflineBootState {
    std::atomic_bool local_profile_events{};
    std::atomic<LocalObjectBrowserReady> local_object_browser{};
    std::uintptr_t base{};
    std::atomic<OnlineEnabledFunction> original{};
    std::atomic<GateStatus> gate{GateStatus::not_attempted};
    std::atomic<std::uint64_t> invocations{};
    std::atomic<std::uint64_t> returned_false{};
    std::atomic<std::uint64_t> native_true{};
    std::atomic<std::uint64_t> native_false{};
    std::atomic<std::uint64_t> native_exceptions{};
    std::atomic<std::uint64_t> preactive_forwards{};
    std::atomic<std::uint64_t> activating_invocations{};
    std::atomic<std::uint64_t> backend_predicate_invocations{};
    std::atomic<std::uint64_t> async_prepare_character_invocations{};
    std::atomic<std::uint64_t> character_readiness_invocations{};
    std::atomic<std::uint64_t> flow_adapter_invocations{};
    std::atomic<std::uint64_t> post_spawn_progression_invocations{};
    std::atomic<std::uint64_t> unmatched_caller_forwards{};
    std::atomic<bool> backend_readable_before_enable{};
    std::atomic<bool> backend_present_before_enable{};
    std::atomic<bool> backend_readable_after_enable{};
    std::atomic<bool> backend_present_after_enable{};
    std::mutex initialization_mutex;
    ImageVerifier verify_image{&supported_build::running_image_matches};
    OptInReader read_opt_in{&environment_opted_in};
    TargetCallerClassifier classify_target_caller{&exact_target_caller};
    bool attempted{};
};

struct PreserveError {
    DWORD value{GetLastError()};
    ~PreserveError() { SetLastError(value); }
};

OfflineBootState& offline_boot_state() {
    static auto* value = new OfflineBootState;
    return *value;
}

bool environment_one(const wchar_t* name) noexcept {
    std::array<wchar_t, 2> value{};
    const auto length = GetEnvironmentVariableW(name, value.data(),
        static_cast<DWORD>(value.size()));
    return length == 1 && value[0] == L'1';
}

bool environment_opted_in() noexcept {
    return environment_one(opt_in_name) || environment_one(old_opt_in_name);
}

bool fingerprint_matches(std::uintptr_t base) noexcept {
    std::array<unsigned char, function_fingerprint.size()> actual{};
    return memory::read(base + function_rva, actual) && actual == function_fingerprint;
}

bool backend_singleton(std::uintptr_t base, bool& present) noexcept {
    std::uintptr_t value{};
    if (!memory::read(base + backend_singleton_rva, value)) return false;
    present = value != 0;
    return true;
}

std::uintptr_t exact_target_caller(
    std::uintptr_t return_address, std::uintptr_t base) noexcept {
    constexpr std::array target_return_rvas{
        backend_predicate_return_rva,
        async_prepare_character_return_rva,
        character_readiness_return_rva,
        flow_adapter_return_rva,
        post_spawn_progression_return_rva};
    for (const auto rva : target_return_rvas) {
        if (return_address == base + rva) return rva;
    }
    return 0;
}

const char* gate_name(GateStatus gate) noexcept {
    switch (gate) {
    case GateStatus::not_attempted: return "not_attempted";
    case GateStatus::opted_out: return "opted_out";
    case GateStatus::image_mismatch: return "image_mismatch";
    case GateStatus::fingerprint_mismatch: return "fingerprint_mismatch";
    case GateStatus::backend_probe_failed: return "backend_probe_failed";
    case GateStatus::activation_too_late: return "activation_too_late";
    case GateStatus::create_failed: return "create_failed";
    case GateStatus::enable_failed: return "enable_failed";
    case GateStatus::enable_failed_disable_failed: return "enable_failed_disable_failed";
    case GateStatus::too_late_disable_failed: return "too_late_disable_failed";
    case GateStatus::activating: return "activating";
    case GateStatus::active: return "active";
    }
    return "unknown";
}

std::uint8_t online_enabled_function_hook() {
    const auto incoming_error = GetLastError();
    auto& state = offline_boot_state();
    const auto gate = state.gate.load(std::memory_order_acquire);
    const bool activating = gate == GateStatus::activating;
    const bool interception_enabled = activating || gate == GateStatus::active;
    // Reading the release-published gate before the trampoline also publishes
    // the preceding original-pointer and image-base stores.
    const auto original = state.original.load(std::memory_order_acquire);
    std::uintptr_t target_caller_rva{};
    bool replace_result{};
    if (interception_enabled) {
        ++state.invocations;
        if (activating) ++state.activating_invocations;
        target_caller_rva = state.classify_target_caller(
            reinterpret_cast<std::uintptr_t>(_ReturnAddress()), state.base);
        if (target_caller_rva == backend_predicate_return_rva) {
            ++state.backend_predicate_invocations;
            replace_result = true;
        }
        else if (target_caller_rva == async_prepare_character_return_rva) {
            ++state.async_prepare_character_invocations;
            replace_result = true;
        }
        else if (target_caller_rva == character_readiness_return_rva) {
            ++state.character_readiness_invocations;
            replace_result = true;
        }
        else if (target_caller_rva == flow_adapter_return_rva) {
            ++state.flow_adapter_invocations;
            const auto ready = state.local_object_browser.load(std::memory_order_acquire);
            replace_result = !ready || !ready();
        }
        else if (target_caller_rva == post_spawn_progression_return_rva) {
            ++state.post_spawn_progression_invocations;
            replace_result = !state.local_profile_events.load(std::memory_order_acquire);
        }
        else ++state.unmatched_caller_forwards;
    }
    else ++state.preactive_forwards;

    SetLastError(incoming_error);
    std::uint8_t native_result{};
    try {
        native_result = original();
    } catch (...) {
        const auto native_error = GetLastError();
        if (interception_enabled) ++state.native_exceptions;
        SetLastError(native_error);
        throw;
    }
    const auto native_error = GetLastError();
    if (interception_enabled) {
        if (native_result) ++state.native_true;
        else ++state.native_false;
    }
    if (replace_result) {
        ++state.returned_false;
        native_result = 0;
    }
    SetLastError(native_error);
    return native_result;
}

} // namespace

void set_local_object_browser_provider(std::uintptr_t base, LocalObjectBrowserReady ready) noexcept {
    PreserveError preserve;
    auto& state = offline_boot_state();
    if (state.base == base && state.gate.load(std::memory_order_acquire) == GateStatus::active)
        state.local_object_browser.store(ready, std::memory_order_release);
}

bool offline_boot_requested() noexcept {
    PreserveError preserve;
    return environment_opted_in();
}

bool start_offline_boot(std::uintptr_t base) noexcept {
    PreserveError preserve;
    auto& state = offline_boot_state();
    try {
        std::lock_guard lock(state.initialization_mutex);
        if (state.attempted)
            return state.base == base && state.gate.load(std::memory_order_acquire) == GateStatus::active;
        state.attempted = true;
        state.base = base;
        if (!state.read_opt_in()) {
            state.gate.store(GateStatus::opted_out, std::memory_order_release);
            return false;
        }
        if (!state.verify_image(base)) {
            state.gate.store(GateStatus::image_mismatch, std::memory_order_release);
            return false;
        }
        if (!fingerprint_matches(base)) {
            state.gate.store(GateStatus::fingerprint_mismatch, std::memory_order_release);
            return false;
        }

        bool backend_present{};
        const bool backend_readable = backend_singleton(base, backend_present);
        state.backend_readable_before_enable.store(backend_readable, std::memory_order_relaxed);
        state.backend_present_before_enable.store(backend_present, std::memory_order_relaxed);
        if (!backend_readable) {
            state.gate.store(GateStatus::backend_probe_failed, std::memory_order_release);
            return false;
        }
        if (backend_present) {
            state.gate.store(GateStatus::activation_too_late, std::memory_order_release);
            return false;
        }

        auto* target = reinterpret_cast<void*>(base + function_rva);
        void* trampoline{};
        const auto created = hook_prepare(target,
            reinterpret_cast<void*>(&online_enabled_function_hook), &trampoline);
        if (created != HookOk || !trampoline) {
            if (created == HookOk) hook_remove(target);
            state.gate.store(GateStatus::create_failed, std::memory_order_release);
            return false;
        }
        state.original.store(reinterpret_cast<OnlineEnabledFunction>(trampoline),
            std::memory_order_release);
        // Once executable redirection can become visible, every selected caller
        // must already receive false. This closes the post-enable/pre-active window.
        state.gate.store(GateStatus::activating, std::memory_order_release);
        if (hook_enable(target) != HookOk) {
            // Retain the trampoline allocation after an activation attempt so
            // a thread already forwarding through it cannot use freed code.
            state.gate.store(GateStatus::enable_failed, std::memory_order_release);
            const auto disabled = hook_disable(target);
            state.gate.store(disabled == HookOk || disabled == HookDisabled ?
                GateStatus::enable_failed : GateStatus::enable_failed_disable_failed,
                std::memory_order_release);
            return false;
        }

        backend_present = false;
        const bool backend_readable_after = backend_singleton(base, backend_present);
        state.backend_readable_after_enable.store(backend_readable_after,
            std::memory_order_relaxed);
        state.backend_present_after_enable.store(backend_present, std::memory_order_relaxed);
        const bool backend_call_intercepted =
            state.backend_predicate_invocations.load(std::memory_order_acquire) != 0;
        if (!backend_readable_after || (backend_present && !backend_call_intercepted) ||
            state.preactive_forwards.load(std::memory_order_acquire) != 0) {
            state.gate.store(backend_readable_after ? GateStatus::activation_too_late :
                GateStatus::backend_probe_failed, std::memory_order_release);
            const auto disabled = hook_disable(target);
            state.gate.store(disabled == HookOk || disabled == HookDisabled ?
                (backend_readable_after ? GateStatus::activation_too_late :
                    GateStatus::backend_probe_failed) : GateStatus::too_late_disable_failed,
                std::memory_order_release);
            return false;
        }
        state.gate.store(GateStatus::active, std::memory_order_release);
        return true;
    } catch (...) {
        return false;
    }
}

void set_local_profile_event_provider(std::uintptr_t base, bool active) noexcept {
    auto& state = offline_boot_state();
    if (state.base == base) state.local_profile_events.store(active, std::memory_order_release);
}

OfflineBootObservation offline_boot_observation() {
    PreserveError preserve;
    auto& state = offline_boot_state();
    const auto gate = state.gate.load(std::memory_order_acquire);
    const auto invocations = state.invocations.load(std::memory_order_relaxed);
    const auto returned_false = state.returned_false.load(std::memory_order_relaxed);
    const auto native_true = state.native_true.load(std::memory_order_relaxed);
    const auto native_false = state.native_false.load(std::memory_order_relaxed);
    const auto exceptions = state.native_exceptions.load(std::memory_order_relaxed);
    const auto preactive = state.preactive_forwards.load(std::memory_order_relaxed);
    const auto activating_invocations =
        state.activating_invocations.load(std::memory_order_relaxed);
    const auto backend_predicate_invocations =
        state.backend_predicate_invocations.load(std::memory_order_relaxed);
    const auto async_prepare_character_invocations =
        state.async_prepare_character_invocations.load(std::memory_order_relaxed);
    const auto character_readiness_invocations =
        state.character_readiness_invocations.load(std::memory_order_relaxed);
    const auto flow_adapter_invocations =
        state.flow_adapter_invocations.load(std::memory_order_relaxed);
    const auto post_spawn_progression_invocations =
        state.post_spawn_progression_invocations.load(std::memory_order_relaxed);
    const auto unmatched_caller_forwards =
        state.unmatched_caller_forwards.load(std::memory_order_relaxed);
    const auto backend_readable_before =
        state.backend_readable_before_enable.load(std::memory_order_relaxed);
    const auto backend_present_before =
        state.backend_present_before_enable.load(std::memory_order_relaxed);
    const auto backend_readable_after =
        state.backend_readable_after_enable.load(std::memory_order_relaxed);
    const auto backend_present_after =
        state.backend_present_after_enable.load(std::memory_order_relaxed);

    std::ostringstream json;
    json << "{\"event\":\"global_offline_observation\",\"gate\":\""
         << gate_name(gate) << "\",\"opted_in\":"
         << (gate != GateStatus::not_attempted && gate != GateStatus::opted_out ? "true" : "false")
         << ",\"function_rva\":\"0x" << std::hex << function_rva << std::dec
         << "\",\"invocations\":" << invocations
         << ",\"returned_false\":" << returned_false
         << ",\"native_true\":" << native_true << ",\"native_false\":" << native_false
         << ",\"native_exceptions\":" << exceptions
         << ",\"preactive_forwards\":" << preactive
         << ",\"activating_invocations\":" << activating_invocations
         << ",\"backend_predicate_invocations\":" << backend_predicate_invocations
         << ",\"async_prepare_character_invocations\":"
         << async_prepare_character_invocations
         << ",\"character_readiness_invocations\":" << character_readiness_invocations
         << ",\"flow_adapter_invocations\":" << flow_adapter_invocations
         << ",\"post_spawn_progression_invocations\":"
         << post_spawn_progression_invocations
         << ",\"unmatched_caller_forwards\":" << unmatched_caller_forwards
         << std::hex
         << ",\"backend_predicate_return_rva\":\"0x" << backend_predicate_return_rva << '"'
         << ",\"async_prepare_character_return_rva\":\"0x" << async_prepare_character_return_rva << '"'
         << ",\"character_readiness_return_rva\":\"0x" << character_readiness_return_rva << '"'
         << ",\"flow_adapter_return_rva\":\"0x" << flow_adapter_return_rva << '"'
         << ",\"post_spawn_progression_return_rva\":\"0x" << post_spawn_progression_return_rva << '"'
         << ",\"backend_singleton_rva\":\"0x" << backend_singleton_rva << '"' << std::dec
         << ",\"backend_readable_before_enable\":"
         << (backend_readable_before ? "true" : "false")
         << ",\"backend_present_before_enable\":"
         << (backend_present_before ? "true" : "false")
         << ",\"backend_readable_after_enable\":"
         << (backend_readable_after ? "true" : "false")
         << ",\"backend_present_after_enable\":"
         << (backend_present_after ? "true" : "false") << '}';
    return {json.str()};
}

} // namespace dingosdk
