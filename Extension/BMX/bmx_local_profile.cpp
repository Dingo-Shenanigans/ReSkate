#include "bmx_local_profile.h"
#include "bmx_local_profile_guard.h"
#include "bmx_local_profile_path.h"
#include "bmx_ecs_diagnostics_guard.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/launcher_support.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Core/Platform/user_data_paths.h"
#include "Engine/Game/Build/image_identity.h"
#include <Windows.h>
#include <intrin.h>
#include <array>
#include <atomic>
#include <mutex>
#include <sstream>

namespace dingosdk::bmx {
namespace {
// This native predicate is bool(). The two inspected callers both
// leave their real settings query in RCX. Preserve that register through the
// trampoline and use it only to restrict the offline adaptation to group zero.
using Gate = bool (__fastcall*)(std::uintptr_t);
using Configure = std::uint32_t (__fastcall*)(std::uintptr_t, std::uintptr_t);
constexpr std::uintptr_t gate_rva = 0x5c6700;
constexpr std::uintptr_t configure_rva = 0x5493080;
constexpr std::array<std::uint8_t, 19> configure_prefix{
    0x40,0x53,0x48,0x81,0xec,0x40,0x01,0x00,0x00,0x48,
    0x8b,0x05,0x30,0x13,0xd3,0x01,0x48,0x33,0xc4};
constexpr std::array<std::uint8_t, 20> gate_prefix{
    0x48,0x89,0x5c,0x24,0x18,0x56,0x48,0x83,0xec,0x20,
    0x48,0x89,0x7c,0x24,0x30,0xe8,0xac,0x5d,0x2e,0x01};
struct Call { std::uintptr_t instruction; std::array<std::uint8_t, 5> bytes; };
constexpr std::array<Call, 2> calls{{
    {0x5d2450, {0xe8,0xab,0x42,0xff,0xff}}, // ordinary query update
    {0x5cc638, {0xe8,0xc3,0xa0,0xff,0xff}}, // actual list/read request
}};
struct State {
    std::atomic<Gate> original{};
    std::atomic<Configure> configure{};
    std::atomic<std::uintptr_t> base{};
    std::atomic<bool> ready{};
    std::atomic<const char*> status{"not_requested"};
    std::atomic<const char*> user_status{"unobserved"};
    std::atomic<const char*> path_status{"unobserved"};
    std::atomic<std::uint64_t> invocations{}, native_allowed{}, adapted{}, rejected{};
    std::atomic<std::uint64_t> paths_allowed{}, paths_rejected{};
    std::atomic<std::uint32_t> configure_result{0xffffffffU};
    std::filesystem::path root;
    std::mutex mutex;
    bool attempted{};
};
State& state() { static auto* value = new State; return *value; }
struct PreserveError { DWORD value{GetLastError()}; ~PreserveError() { SetLastError(value); } };
bool read(std::uintptr_t p, void* out, std::size_t n) noexcept { return memory::peek_bytes(p, out, n); }
template<std::size_t N>
bool matches(std::uintptr_t p, const std::array<std::uint8_t, N>& expected) noexcept {
    std::array<std::uint8_t, N> actual{};
    return memory::peek(p, actual) && actual == expected;
}
std::uint32_t __fastcall isolated_storage(std::uintptr_t platform, std::uintptr_t configuration) {
    const auto incoming_error = GetLastError();
    auto& s = state();
    const auto reject = [&](const char* reason) {
        s.path_status.store(reason, std::memory_order_relaxed);
        s.paths_rejected.fetch_add(1, std::memory_order_relaxed);
        try { logging::log(logging::Level::warning, logging::Channel::runtime,
            "BMX isolated storage configuration rejected: {}", reason); } catch (...) {}
        SetLastError(incoming_error);
        return std::uint32_t{11}; // native invalid-path result, handled by native storage
    };
    try {
        const auto base = s.base.load(std::memory_order_acquire);
        const auto path = local_profile_path::inspect_storage_path(base, platform, configuration, read);
        if (!path.coherent) return reject(path.status);
        std::array<wchar_t, 261> wide{};
        const auto wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.text.data(),
            static_cast<int>(path.length), wide.data(), static_cast<int>(wide.size()));
        if (!wide_length) return reject("invalid_path_encoding");
        const std::filesystem::path supplied(std::wstring(wide.data(), static_cast<std::size_t>(wide_length)));
        if (!supplied.is_absolute()) return reject("relative_path");
        if (!user_data::detail::within(supplied.lexically_normal(), s.root)) return reject("outside_test_root");
        std::error_code error;
        const auto resolved = std::filesystem::weakly_canonical(supplied, error);
        if (error || resolved.empty() || !user_data::detail::within(resolved, s.root)) return reject("outside_test_root");
        const auto again = local_profile_path::inspect_storage_path(base, platform, configuration, read);
        if (!again.coherent || again.path_pointer != path.path_pointer || again.length != path.length || again.text != path.text)
            return reject("configuration_changed");
    } catch (...) { return reject("path_validation_failed"); }
    s.path_status.store("isolated", std::memory_order_relaxed);
    s.paths_allowed.fetch_add(1, std::memory_order_relaxed);
    try { logging::write(logging::Level::info, logging::Channel::runtime,
        "BMX native storage path verified inside the isolated test profile."); } catch (...) {}
    SetLastError(incoming_error);
    const auto result = s.configure.load(std::memory_order_acquire)(platform, configuration);
    const auto native_error = GetLastError();
    s.configure_result.store(result, std::memory_order_relaxed);
    try { logging::log(logging::Level::info, logging::Channel::runtime,
        "BMX native local storage configuration returned {}.", result); } catch (...) {}
    SetLastError(native_error);
    return result;
}
bool __fastcall local_gate(std::uintptr_t query) {
    const auto incoming_error = GetLastError();
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    auto& s = state();
    const auto original = s.original.load(std::memory_order_acquire);
    s.invocations.fetch_add(1, std::memory_order_relaxed);
    SetLastError(incoming_error);
    const bool native = original(query);
    const auto native_error = GetLastError();
    bool result = native;
    if (native) s.native_allowed.fetch_add(1, std::memory_order_relaxed);
    else if (s.ready.load(std::memory_order_acquire)) {
        const auto base = s.base.load(std::memory_order_acquire);
        bool known_caller{};
        for (const auto& call : calls) known_caller = known_caller || caller == base + call.instruction + 5;
        if (known_caller) {
            const auto primary = ecs_detail::inspect_profile_query(base + 0x71eff00, read);
            const auto native_query = ecs_detail::inspect_native_profile_query(base, primary, read);
            const auto user = local_profile_guard::inspect_primary_user(base, read);
            const auto again = ecs_detail::inspect_profile_query(base + 0x71eff00, read);
            s.user_status.store(user.status, std::memory_order_relaxed);
            // Never manufacture a user, storage handle, request, or completion.
            // The existing native update performs real local I/O and dispatches
            // its normal success, missing-file, and error callbacks afterwards.
            result = primary.coherent && primary.object == query && native_query.coherent && user.eligible &&
                again.coherent && again.manager == primary.manager && again.begin == primary.begin &&
                again.end == primary.end && again.object == query && again.vtable == primary.vtable &&
                again.function == primary.function;
            (result ? s.adapted : s.rejected).fetch_add(1, std::memory_order_relaxed);
        }
    }
    SetLastError(native_error);
    return result;
}
}

bool initialize_local_profile(std::uintptr_t base) noexcept {
    PreserveError preserve;
    auto& s = state();
    try {
        std::lock_guard lock(s.mutex);
        if (s.attempted) return s.ready.load(std::memory_order_acquire) && s.base.load() == base;
        s.attempted = true;
        std::array<wchar_t, 2> flag{};
        if (GetEnvironmentVariableW(L"RESKATE_BMX_LOCAL_PROFILE", flag.data(), static_cast<DWORD>(flag.size())) != 1 || flag[0] != L'1') return false;
        const auto root = user_data::override_root();
        if (!launcher::offline_mode() || !user_data::development_requested() || !root) {
            s.status.store("requires_isolated_offline_launch"); return false;
        }
        if (!supported_build::running_image_matches(base) || !matches(base + gate_rva, gate_prefix) ||
            !matches(base + configure_rva, configure_prefix)) {
            s.status.store("image_or_gate_rejected"); return false;
        }
        for (const auto& call : calls) if (!matches(base + call.instruction, call.bytes)) {
            s.status.store("caller_rejected"); return false;
        }
        auto* target = reinterpret_cast<void*>(base + gate_rva);
        auto* storage_target = reinterpret_cast<void*>(base + configure_rva);
        void* relay{};
        if (hook_prepare(target, reinterpret_cast<void*>(&local_gate), &relay) != HookOk || !relay) {
            s.status.store("prepare_failed"); return false;
        }
        void* storage_relay{};
        if (hook_prepare(storage_target, reinterpret_cast<void*>(&isolated_storage), &storage_relay) != HookOk || !storage_relay) {
            (void)hook_remove(target);
            s.status.store("storage_prepare_failed"); return false;
        }
        s.root = *root;
        s.base.store(base, std::memory_order_release);
        s.original.store(reinterpret_cast<Gate>(relay), std::memory_order_release);
        s.configure.store(reinterpret_cast<Configure>(storage_relay), std::memory_order_release);
        // Publish before enabling: an enabled hook must always have a relay.
        s.ready.store(true, std::memory_order_release);
        if (hook_enable(storage_target) != HookOk || hook_enable(target) != HookOk) {
            s.ready.store(false, std::memory_order_release);
            (void)hook_remove(target);
            (void)hook_remove(storage_target);
            s.status.store("enable_failed"); return false;
        }
        s.status.store("ready"); return true;
    } catch (...) { s.status.store("initialization_failed"); return false; }
}

std::string local_profile_status() {
    PreserveError preserve;
    const auto& s = state();
    std::ostringstream out;
    out << "BMX isolated local profile: ready=" << s.ready.load() << "; status=" << s.status.load()
        << "; calls=" << s.invocations.load() << "; native_allowed=" << s.native_allowed.load()
        << "; adapted=" << s.adapted.load() << "; rejected=" << s.rejected.load();
    out << "; user=" << s.user_status.load();
    out << "; storage_paths_allowed=" << s.paths_allowed.load() << "; storage_paths_rejected=" << s.paths_rejected.load()
        << "; storage_path=" << s.path_status.load();
    out << "; native_configuration_result=" << s.configure_result.load();
    return out.str();
}
}
