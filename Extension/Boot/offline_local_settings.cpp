#include "offline_local_settings.h"
#include "local_settings_path.h"
#include "offline_boot.h"
#include "user_data_redirect.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/launcher_support.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Core/Platform/user_data_paths.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/image_identity.h"
#include "Engine/Game/Build/20260929/local_native_settings.h"
#include "Engine/Game/Profile/local_settings_guard.h"
#include "Engine/Game/Profile/local_settings_policy.h"

#include <Windows.h>
#include <intrin.h>
#include <array>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>

namespace dingosdk {
namespace {
namespace pins = addr::local_native_settings;
namespace guard = local_settings_guard;
namespace policy = local_settings_policy;

// The inspected bool() callers leave their query in RCX. Forward that same
// register and use it only at those exact call sites, never as a general ABI.
using Gate = bool (__fastcall*)(std::uintptr_t);
using Configure = std::uint32_t (__fastcall*)(std::uintptr_t, std::uintptr_t);
enum class Request { absent, selected, invalid };

struct PreserveError {
    DWORD value{GetLastError()};
    ~PreserveError() { SetLastError(value); }
};
struct State {
    std::mutex mutex;
    policy::Authority authority;
    std::atomic<Gate> original{};
    std::atomic<Configure> configure{};
    std::atomic<std::uintptr_t> base{};
    std::filesystem::path root;
    std::atomic<bool> admission_noted{}, storage_noted{}, rejection_noted{}, revocation_noted{};
    bool attempted{}, opted_out{};
};
State& state() { static auto* value = new State; return *value; }

Request requested() noexcept {
    std::array<wchar_t, 2> value{};
    SetLastError(ERROR_SUCCESS);
    const auto length = GetEnvironmentVariableW(L"RESKATE_OFFLINE_LOCAL_SETTINGS", value.data(),
        static_cast<DWORD>(value.size()));
    if (!length && GetLastError() == ERROR_ENVVAR_NOT_FOUND) return Request::absent;
    return length == 1 && value[0] == L'1' ? Request::selected : Request::invalid;
}

bool read(std::uintptr_t address, void* bytes, std::size_t size) noexcept {
    return memory::peek_bytes(address, bytes, size);
}
template<std::size_t Size>
bool matches(std::uintptr_t address, const std::array<std::uint8_t, Size>& expected) noexcept {
    std::array<std::uint8_t, Size> actual{};
    return memory::peek(address, actual) && actual == expected;
}
bool same_path(const std::filesystem::path& a, const std::filesystem::path& b) {
    return user_data::detail::within(a, b) && user_data::detail::within(b, a);
}
void note(std::atomic<bool>& once, logging::Level level, const char* message) noexcept {
    PreserveError preserve;
    if (once.exchange(true, std::memory_order_relaxed)) return;
    try { logging::write(level, logging::Channel::runtime, message); } catch (...) {}
}

bool selection_matches(const State& s) noexcept {
    try {
        const auto base = s.base.load(std::memory_order_acquire);
        const auto root = user_data::override_root();
        return requested() == Request::selected && launcher::offline_mode() &&
            offline_boot_active(base) && user_data_redirect_ready() &&
            user_data::development_requested() && root && same_path(*root, s.root) &&
            same_path(redirected_local_app_data().parent_path(), s.root);
    } catch (...) { return false; }
}

bool selection_current(State& s) noexcept {
    const auto allowed = s.authority.check(selection_matches(s));
    if (!allowed) note(s.revocation_noted, logging::Level::warning,
        "Offline local settings: selected launch authority was revoked; it cannot rearm in this process.");
    return allowed;
}

bool qualified_query(State& s, std::uintptr_t query, std::uintptr_t caller) noexcept {
    const auto base = s.base.load(std::memory_order_acquire);
    if (!policy::approved_caller(base, caller, pins::calls)) return false;
    if (!selection_current(s)) return false;
    const auto opening = guard::inspect_profile_query(base, pins::guard_layout, read);
    if (!opening.coherent || opening.object != query) return false;
    const auto typed = guard::inspect_native_profile_query(base, opening, pins::guard_layout, read);
    const auto user = guard::inspect_primary_user(base, pins::guard_layout, read);
    if (!typed.coherent || !user.coherent || !user.eligible) return false;
    const auto closing = guard::inspect_profile_query(base, pins::guard_layout, read);
    const auto user_again = guard::inspect_primary_user(base, pins::guard_layout, read);
    // These are two independently observed witnesses. Matching them does not
    // retain native objects or establish a query-to-user ownership relationship.
    return guard::observations_qualify(query, opening, typed, user, closing, user_again) && selection_current(s);
}

bool __fastcall local_gate(std::uintptr_t query) {
    auto& s = state();
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const auto original = s.original.load(std::memory_order_acquire);
    bool native{};
    const auto result = policy::gate(s.authority,
        [&] { native = original(query); return native; },
        [&] { return qualified_query(s, query, caller); },
        [] { return static_cast<std::uint32_t>(GetLastError()); },
        [](std::uint32_t error) { SetLastError(error); });
    if (result && !native) note(s.admission_noted, logging::Level::info,
        "Offline local settings: the verified primary query was admitted to native local I/O.");
    return result;
}

std::uint32_t __fastcall isolated_storage(std::uintptr_t platform, std::uintptr_t configuration) {
    auto& s = state();
    bool native_called{};
    const char* rejection = "launch_not_active";
    const auto result = policy::configure_storage(s.authority,
        [&] {
            native_called = true;
            return s.configure.load(std::memory_order_acquire)(platform, configuration);
        },
        [&] {
            if (!selection_current(s)) return false;
            const auto base = s.base.load(std::memory_order_acquire);
            const auto path = guard::inspect_storage_path(base, platform, configuration, pins::guard_layout, read);
            if (!path.coherent) { rejection = path.status; return false; }
            const auto checked = local_settings_path::validate(
                std::string_view(path.text.data(), path.length), s.root);
            if (checked != local_settings_path::Status::accepted) {
                rejection = local_settings_path::status_name(checked); return false;
            }
            const auto again = guard::inspect_storage_path(base, platform, configuration, pins::guard_layout, read);
            if (!again.coherent || again.path_pointer != path.path_pointer ||
                again.length != path.length || again.text != path.text) {
                rejection = "configuration_changed"; return false;
            }
            return selection_current(s);
        },
        [] { return static_cast<std::uint32_t>(GetLastError()); },
        [](std::uint32_t error) { SetLastError(error); }, pins::invalid_path_result);
    PreserveError preserve;
    try {
        if (native_called && !s.storage_noted.exchange(true, std::memory_order_relaxed))
            logging::log(logging::Level::info, logging::Channel::runtime,
                "Offline local settings: native storage configuration returned {}.", result);
        else if (!native_called && !s.rejection_noted.exchange(true, std::memory_order_relaxed))
            logging::log(logging::Level::warning, logging::Channel::runtime,
                "Offline local settings: storage configuration refused ({}).", rejection);
    } catch (...) {}
    return result;
}
} // namespace

bool start_offline_local_settings(std::uintptr_t base, std::string& error) noexcept {
    PreserveError preserve;
    auto& s = state();
    try {
        std::lock_guard lock(s.mutex);
        if (s.attempted) {
            if (s.opted_out && requested() == Request::absent) { error.clear(); return true; }
            if (s.authority.active() && s.base.load(std::memory_order_acquire) == base && selection_current(s)) {
                error.clear(); return true;
            }
            error = "Offline local settings cannot be selected or rearmed after the first initialization attempt.";
            return false;
        }
        s.attempted = true;
        const auto request = requested();
        if (request == Request::absent) { s.opted_out = true; error.clear(); return true; }
        const auto fail = [&](const char* reason) {
            s.authority.fail(); error = reason; return false;
        };
        if (request != Request::selected)
            return fail("RESKATE_OFFLINE_LOCAL_SETTINGS requires the exact value 1, or must be absent.");
        const auto root = user_data::override_root();
        const policy::StartupRequest admission{L"1", launcher::offline_mode(), offline_boot_active(base),
            user_data_redirect_ready(), user_data::development_requested() && root.has_value()};
        if (!s.authority.begin(admission))
            return fail("Offline local settings requires offline mode, active offline boot and a checked redirected development root.");
        if (!root || !same_path(redirected_local_app_data().parent_path(), *root))
            return fail("Offline local settings redirect does not match the selected development root.");
        if (!supported_build::running_image_matches(base) || !matches(base + pins::gate_rva, pins::gate_prefix) ||
            !matches(base + pins::configure_rva, pins::configure_prefix))
            return fail("Offline local settings refused the game image or native function bytes.");
        for (const auto& call : pins::calls)
            if (!matches(base + call.instruction, call.bytes))
                return fail("Offline local settings refused an unrecognized native caller.");
        std::uintptr_t manager{};
        if (!memory::peek(base + pins::profile_settings_manager, manager) || manager)
            return fail("Offline local settings must be installed before the native settings manager is created.");
        s.root = *root;
        s.base.store(base, std::memory_order_release);
        const std::array<void*, 2> targets{reinterpret_cast<void*>(base + pins::gate_rva),
            reinterpret_cast<void*>(base + pins::configure_rva)};
        const std::array<void*, 2> replacements{reinterpret_cast<void*>(&local_gate),
            reinterpret_cast<void*>(&isolated_storage)};
        const auto remove = [&](unsigned index) {
            const auto removed = hook_remove(targets[index]);
            return removed == HookOk || removed == HookNotFound;
        };
        policy::InstallResult installed;
        {
            HookBatch batch;
            // HookBatch alone reports queued success. Confirm the transaction
            // before any callback is allowed to adapt the native predicate.
            installed = policy::install_pair(
                [&](unsigned index) {
                    void* relay{};
                    const auto prepared = hook_prepare(targets[index], replacements[index], &relay) == HookOk;
                    if (prepared && relay) {
                        if (!index) s.original.store(reinterpret_cast<Gate>(relay), std::memory_order_release);
                        else s.configure.store(reinterpret_cast<Configure>(relay), std::memory_order_release);
                    }
                    return policy::PrepareResult{prepared, prepared && relay != nullptr};
                },
                [&](unsigned index) { return hook_enable(targets[index]) == HookOk; },
                [] { return hook_apply_queued() == HookOk; }, remove);
        }
        if (!installed.verified()) {
            if (!installed.cleanup_complete)
                return fail("Offline local settings hook installation and cleanup failed; the game must not resume.");
            switch (installed.failure) {
            case policy::InstallFailure::prepare: return fail("Offline local settings could not prepare both verified native hooks.");
            case policy::InstallFailure::enable: return fail("Offline local settings could not queue both native hooks.");
            case policy::InstallFailure::commit: return fail("Offline local settings native hook transaction failed.");
            case policy::InstallFailure::none: return fail("Offline local settings hook installation was incomplete.");
            }
        }
        if (!selection_matches(s)) s.authority.revoke();
        if (!s.authority.publish(installed.verified())) {
            const bool storage_removed = remove(1);
            const bool gate_removed = remove(0);
            return fail(storage_removed && gate_removed ? "Offline local settings authority changed during hook activation." :
                "Offline local settings authority changed and hook cleanup failed; the game must not resume.");
        }
        error.clear();
        logging::write(logging::Level::info, logging::Channel::runtime,
            "Offline local settings: installed for the selected checked development root.");
        return true;
    } catch (...) {
        s.authority.fail();
        error = "Offline local settings initialization failed; the game must not resume.";
        return false;
    }
}

} // namespace dingosdk
