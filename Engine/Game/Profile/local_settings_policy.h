#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <string_view>

namespace dingosdk::local_settings_policy {
enum class Phase : std::uint8_t { inactive, installing, active, revoked, failed };
struct StartupRequest {
    std::wstring_view opt_in;
    bool offline_requested{}, offline_active{}, redirect_checked{}, development_root_selected{};
};
constexpr bool selected(const StartupRequest& request) noexcept {
    return request.opt_in == L"1" && request.offline_requested && request.offline_active &&
        request.redirect_checked && request.development_root_selected;
}

// Image identity and instruction bytes are checked during installation. This
// compares only return addresses from that approved table, without wrapping
// either addition at a malformed image base.
template<class Calls>
bool approved_caller(std::uintptr_t base, std::uintptr_t caller, const Calls& calls) noexcept {
    if (!base || !caller) return false;
    constexpr auto maximum = (std::numeric_limits<std::uintptr_t>::max)();
    for (const auto& call : calls) {
        const auto size = static_cast<std::uintptr_t>(call.bytes.size());
        if (!size || call.instruction > maximum - base || size > maximum - base - call.instruction) continue;
        if (caller == base + call.instruction + size) return true;
    }
    return false;
}

// Authority belongs to one process initialization. Neither a later environment
// change nor another initialization call can revive failed or revoked hooks.
class Authority {
public:
    Phase phase() const noexcept { return phase_.load(std::memory_order_acquire); }
    bool active() const noexcept { return phase() == Phase::active; }
    bool begin(const StartupRequest& request) noexcept {
        const bool accepted = selected(request);
        auto expected = Phase::inactive;
        return phase_.compare_exchange_strong(expected, accepted ? Phase::installing : Phase::failed,
            std::memory_order_acq_rel, std::memory_order_acquire) && accepted;
    }
    // The Boot owner supplies this only after both verified hooks committed.
    // A revocation while installation is in progress wins over publication.
    bool publish(bool hook_pair_verified) noexcept {
        auto expected = Phase::installing;
        return phase_.compare_exchange_strong(expected, hook_pair_verified ? Phase::active : Phase::failed,
            std::memory_order_acq_rel, std::memory_order_acquire) && hook_pair_verified;
    }
    void fail() noexcept { finish(Phase::failed); }
    void revoke() noexcept { finish(Phase::revoked); }
    // Callback use only. The caller obtains fresh launch, redirect and root
    // selection facts; this policy does not supply an I/O destination.
    bool check(bool selection_current) noexcept {
        if (!selection_current) revoke();
        return selection_current && active();
    }
private:
    void finish(Phase terminal) noexcept {
        auto observed = phase();
        while (observed != Phase::revoked && observed != Phase::failed &&
            !phase_.compare_exchange_weak(observed, terminal,
                std::memory_order_acq_rel, std::memory_order_acquire)) {}
    }
    std::atomic<Phase> phase_{Phase::inactive};
};

struct PrepareResult { bool prepared{}, verified{}; };
enum class InstallFailure : std::uint8_t { none, prepare, enable, commit };
struct InstallResult {
    InstallFailure failure{InstallFailure::none};
    bool cleanup_complete{true};
    unsigned prepared{};
    bool verified() const noexcept {
        return failure == InstallFailure::none && cleanup_complete && prepared == 2;
    }
};

// A successful prepare owns a target even when its relay was unusable. A
// refused prepare owns nothing; it must not remove another owner's hook.
// Enable queues only. Boot commits that queue before publishing authority.
template<class Prepare, class Enable, class Commit, class Remove>
InstallResult install_pair(Prepare&& prepare, Enable&& enable, Commit&& commit, Remove&& remove) noexcept {
    InstallResult result;
    std::array<bool, 2> owned{};
    const auto rollback = [&]() noexcept {
        for (unsigned remaining = 2; remaining; --remaining) {
            const auto index = remaining - 1;
            if (!owned[index]) continue;
            try {
                if (!remove(index)) result.cleanup_complete = false;
            } catch (...) { result.cleanup_complete = false; }
        }
    };
    for (unsigned index = 0; index < 2; ++index) {
        PrepareResult target;
        try { target = prepare(index); }
        catch (...) {
            // A throwing prepare supplied no ownership receipt. Cleanup of
            // that attempted target is unknown, even if earlier removal works.
            result.failure = InstallFailure::prepare;
            result.cleanup_complete = false;
            rollback();
            return result;
        }
        owned[index] = target.prepared;
        if (target.prepared) ++result.prepared;
        if (!target.prepared || !target.verified) {
            result.failure = InstallFailure::prepare;
            rollback();
            return result;
        }
    }
    for (unsigned index = 0; index < 2; ++index) {
        bool queued{};
        try { queued = enable(index); } catch (...) {}
        if (!queued) {
            result.failure = InstallFailure::enable;
            rollback();
            return result;
        }
    }
    bool committed{};
    try { committed = commit(); } catch (...) {}
    if (!committed) {
        result.failure = InstallFailure::commit;
        rollback();
    }
    return result;
}

namespace detail {
// Error accessors must not throw. They are injected so the same composition
// runs with Win32 error state in Boot and controlled error state in tests.
template<class SetError> struct RestoreError {
    SetError& set_error;
    std::uint32_t error;
    ~RestoreError() noexcept { set_error(error); }
};
}

// The native predicate always runs once, first. A true result keeps its normal
// behavior without inspecting adaptation witnesses. The callback supplies real
// independently read query/user facts, not an invented ownership-join proof.
template<class Native, class Qualified, class GetError, class SetError>
bool gate(Authority& authority, Native&& native, Qualified&& qualified, GetError&& get_error, SetError&& set_error) {
    const bool allowed = native();
    detail::RestoreError<SetError> restore{set_error, static_cast<std::uint32_t>(get_error())};
    if (allowed) return true;
    if (!authority.active()) return false;
    try { return qualified() && authority.active(); }
    catch (...) { return false; }
}

// Validation precedes native configuration because it may perform real I/O.
// Rejection preserves the incoming error; acceptance gives native code that
// same incoming error and preserves its own exact result/error afterward.
template<class Native, class Validate, class GetError, class SetError>
std::uint32_t configure_storage(Authority& authority, Native&& native, Validate&& validate,
    GetError&& get_error, SetError&& set_error, std::uint32_t invalid_result) {
    detail::RestoreError<SetError> restore{set_error, static_cast<std::uint32_t>(get_error())};
    if (!authority.active()) return invalid_result;
    try {
        if (!validate() || !authority.active()) return invalid_result;
    } catch (...) { return invalid_result; }
    set_error(restore.error);
    try {
        const auto result = static_cast<std::uint32_t>(native());
        restore.error = static_cast<std::uint32_t>(get_error());
        return result;
    } catch (...) {
        restore.error = static_cast<std::uint32_t>(get_error());
        throw;
    }
}
} // namespace dingosdk::local_settings_policy
