#pragma once
#include "Engine/Game/Input/input_lease.h"
#include <cstdint>

// Passive observation of one selected raw XInput stream. This service never
// changes a report; its separate opt-in capture extension owns no native actor or clock.
namespace dingosdk::native_input_source {
bool lease_request(std::uint64_t owner, std::uint32_t user,
    input_lease::Handle& handle, input_lease::Reason& reason) noexcept;
bool lease_read(std::uint64_t owner, input_lease::Handle handle,
    input_lease::Snapshot& snapshot, input_lease::Reason& reason) noexcept;
bool lease_renew(std::uint64_t owner, input_lease::Handle handle,
    const input_lease::Snapshot& snapshot, input_lease::Reason& reason) noexcept;
bool lease_release(std::uint64_t owner, input_lease::Handle handle,
    input_lease::Reason& reason) noexcept;
bool lease_state(std::uint64_t owner, input_lease::Handle handle,
    input_lease::Receipt& receipt, input_lease::Reason& reason) noexcept;
bool lease_acknowledge(std::uint64_t owner, input_lease::Handle handle,
    input_lease::Reason& reason) noexcept;
// Any thread, idempotent. Revoke immediately; preserve an orphaned terminal
// reservation until its exact callbacks drain, then reclaim it automatically.
void lease_release_all(std::uint64_t owner) noexcept;

// Called before plugin dispatch on the verified client thread. Identifiers are
// equality-only internal context tokens; no native graph is read by this module.
void tick(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept;

// Existing XInput wrappers own the original call. The stack cookie is noncopyable;
// abandonment drains exactly its own reservation and cannot issue a sample.
class Poll final {
public:
    Poll() noexcept = default;
    ~Poll();
    Poll(const Poll&) = delete;
    Poll& operator=(const Poll&) = delete;
    Poll(Poll&&) = delete;
    Poll& operator=(Poll&&) = delete;
private:
    friend void begin(bool, std::uint32_t, Poll&) noexcept;
    friend void finish(Poll&, std::uint32_t, bool, std::uint32_t,
        const input_lease::Pad&, std::uint32_t, bool, std::uint32_t,
        const input_lease::Pad&) noexcept;
    friend std::uint32_t finish_capture(Poll&, std::uint32_t, bool, std::uint32_t,
        const input_lease::Pad&, std::uint32_t, bool, std::uint32_t,
        const input_lease::Pad&, std::uint64_t, void*, bool (*)(void*, std::uint32_t, const input_lease::Pad&) noexcept) noexcept;
    std::uint64_t ticket_{};
    std::uint32_t fallback_user_{4};
};
void begin(bool outer_game_poll, std::uint32_t user, Poll& poll) noexcept;
// Raw facts are copied immediately after original, returned facts after existing
// upstream transforms. Missing/failed/mismatched reports cannot seed a lease.
// This function does not dereference or mutate the game's output buffer.
void finish(Poll& poll, std::uint32_t original_result, bool raw_known,
    std::uint32_t raw_packet, const input_lease::Pad& raw,
    std::uint32_t returned_result, bool returned_known,
    std::uint32_t returned_packet, const input_lease::Pad& returned) noexcept;
}
