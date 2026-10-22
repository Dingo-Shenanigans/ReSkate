#pragma once
#include "bmx_native_motion.h"
#include <cstdint>

namespace dingosdk::bmx::bail {
struct Status {
    bool ready{},pending{},consumed{},ragdoll_observed{},expired{},owner_changed{},protected_owner{};
    std::uint64_t requests{},native_calls{},request_id{},requested_ms{},consumed_ms{},observed_ms{};
    std::uintptr_t entity{};
    const char* issue{"Not initialized."}; // static lifetime; snapshot has no retained native references
    bool native_ragdoll_mode{};
    std::uint32_t native_offboard_flags{};
    bool intent_armed{},intent_written{},intent_reset{},context_verified{};
    std::uint64_t intent_callbacks{},intent_native_calls{};
    bool activation_enabled{},intent_reset_pending{},intent_prior_false{},intent_true_returned{},intent_true_readback{},
        intent_reset_readback{},intent_native_cleared{},intent_owner_expired{};
    std::uint64_t intent_true_serial{},intent_reset_serial{},intent_true_calls{},intent_reset_calls{};
    std::uint64_t intent_generation{};
    bool intent_status_available{true};
};
// Registers the existing verified local skater physics handoff listener.
bool initialize(std::uintptr_t base) noexcept;
// Client thread, only AFTER releasing chassis forces, input, rider pose/motion
// and the BMX protection lease. Existing manual/flight protection takes priority.
// Animated riders use the explicitly enabled typed RequestWipeout pulse at a
// freshly canonical authored callback. Already physical rigs use the verified
// short native Offboard leaf. Native update owns fall entry and recovery.
bool request(std::uintptr_t base,const NativeFrame& owner) noexcept;
void cancel() noexcept;
Status status() noexcept;
struct ObservationStatus {
    bool active{},completed{},written{},owner_changed{},native_ragdoll_mode_seen{},ragdoll_seen{};
    std::uint64_t samples{},started_ms{},completed_ms{};
    const char* issue{"Not requested."};
};
// Passive, once per process: exact chassis-probe profile and explicit
// RESKATE_BMX_NATIVE_FALL_OBSERVER=1. Starts only from an unprotected native
// board100 owner; it never requests a fall or changes native state.
bool begin_observation(std::uintptr_t base,const NativeFrame& owner) noexcept;
// Main/client thread: expires and writes the bounded six-second sample report.
// Call while observing even if the native owner disappears (no native reads here).
ObservationStatus poll_observation() noexcept;
}
