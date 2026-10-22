#pragma once
#include <cstdint>
#include "bmx_getup_observation.h"
#include "bmx_getup_callback_audit.h"
#include "Extension/BMX/bmx_native_motion.h"
namespace dingosdk::bmx::intent_observer {
struct Status {
    bool requested{},initialized{},capturing{},written{};
    std::uint64_t begin_forwarded{},update_forwarded{},samples{},filtered{},unreadable{},after_capacity{};
    const char* issue{"not_requested"};
};
// Exact private-root opt-in. Activation additionally requires the separately
// explicit RESKATE_BMX_INTENT_ACTIVATION diagnostic flag.
bool initialize(std::uintptr_t base) noexcept;
// Explicit native BMX development launch only; passive capture remains off.
// Installs the same exact adapters, with per-crash bounded admission.
bool enable_riding_falls(std::uintptr_t base) noexcept;
// Cached bootstrap opt-in only; ordinary play can skip canonical frame reads.
bool requested() noexcept;
// Regular client tick only, with its independently fresh canonical NativeFrame.
// A match is observed identity correlation, not permission to invoke a setter.
Status poll(const NativeFrame& canonical,bool force_write=false) noexcept;
struct BailProbeStatus {
    bool available{true};
    bool armed{},completed{},context_verified{},owner_changed{};
    std::uint64_t callbacks{},matches{},ends{},started_ms{};
    const char* issue{"Not requested."};
    bool activation_enabled{},intent_written{},intent_reset{},reset_pending{},prior_false{},true_returned{},
        true_readback{},reset_readback{},native_cleared{},owner_expired{};
    std::uint64_t true_serial{},reset_serial{},true_calls{},reset_calls{},getter_calls{},asset_lookup_calls{};
    std::uint64_t generation{};
};
// Private diagnostic opt-in or explicit isolated riding-fall adapter. Each
// generation permits one typed Bool pulse through a fresh canonical Update.
// Rearm requires completed/cancelled prior work and verified reset after entry.
// No captured node/property pointer is invoked later; End is observation only.
bool arm_bail_probe(std::uintptr_t base,const NativeFrame& owner) noexcept;
void cancel_bail_probe() noexcept;
BailProbeStatus bail_probe_status() noexcept;
using GetupBoolObservation=getup_observation::BoolObservation;
struct GetupObservationStatus {
    bool available{true},active{},completed{},owner_changed{};
    std::uint64_t generation{},entity_handle{},serial{},observed_ms{},callbacks{},samples{},getter_calls{},
        asset_lookup_calls{},failures{},contention_drops{};
    std::uintptr_t entity{};
    double simulation_time{};
    GetupBoolObservation getup,is_getup_allowed;
    const char* issue{"Not requested."};
};
// Passive diagnostics only. Requires the isolated development RideTrace opt-in.
// Each value is read through a freshly qualified canonical Bool callback. A
// successful native getter can still return an asset default: slot presence is
// explicitly unknown until a separate read-only slot contract is established.
bool begin_getup_observation(std::uintptr_t base,const NativeFrame& owner) noexcept;
GetupObservationStatus getup_observation_status() noexcept;
struct GetupObservationTrace {
    bool available{true};
    GetupObservationStatus status;
    unsigned count{};
    std::array<GetupObservationStatus,getup_observation::capacity> samples;
};
// Bounded copy for an off-ride report flush, never a native callback or per-tick
// operation. Rows retain their own source clocks rather than a caller's clock.
GetupObservationTrace getup_observation_trace() noexcept;
void cancel_getup_observation() noexcept;
// One explicit lab experiment per process. This requests the authored GetUp
// Bool only; native guards may decline it. It does not establish a physical
// button mapping or restore an originally absent native binding slot.
struct GetupActionRequest {
    std::uint64_t fall_request_id{},fall_generation{};
    std::uintptr_t world{},native_state{};
    std::uint32_t body_index{0xffffffffU},body_generation{};
    std::uint64_t body_token{};
};
struct GetupActionStatus {
    bool available{true},armed{},completed{},expired{},cancelled{},owner_changed{},owner_expired{};
    bool prior_false{},true_returned{},true_readback{},intent_written{},intent_reset{},reset_readback{},
        reset_pending{},native_cleared{},allowed_before_action{},binding_present_known{},binding_present{};
    std::uintptr_t client{},entity{},core{},context{},rig{};
    std::uint64_t generation{},entity_handle{},started_ms{},requested_serial{},allowed_serial{},true_serial{},reset_serial{},
        true_calls{},reset_calls{},getter_calls{},asset_lookup_calls{},callbacks{};
    double requested_native_time{},allowed_native_time{};
    GetupActionRequest request;
    const char* issue{"Not requested."};
};
bool arm_getup_action(std::uintptr_t base,const NativeFrame&,const GetupActionRequest&) noexcept;
GetupActionStatus getup_action_status() noexcept;
// Removes start permission; a possible write still owes a fresh next-clock
// reset, independently of cancellation, observation expiry or ride shutdown.
void cancel_getup_action() noexcept;
struct GetupCallbackAuditStatus {
    bool available{true},armed{},active{},completed{},cancelled{},owner_changed{};
    std::uintptr_t client{},entity{},core{},context{},rig{};
    std::uint64_t generation{},entity_handle{},requested_serial{},started_ms{},deadline_ms{},callbacks{},candidates{},
        getter_calls{},asset_lookup_calls{},overflow_drops{},contention_drops{},rejected{};
    double requested_native_time{};
    GetupActionRequest request;
};
struct GetupCallbackAuditRow {
    std::uint64_t ordinal{},serial{},observed_ms{},entity_handle{};
    double native_time{};
    std::uintptr_t event_params{},node{},root{},data{},binding{},context_wrapper{};
    std::array<std::uint8_t,16> data_guid{},input_guid{},mirror_guid{};
    std::array<std::uint8_t,2> update_after{};
    bool clock_available{},owner_verified{},data_guid_available{},input_guid_available{},mirror_guid_available{};
    GetupBoolObservation getup,is_getup_allowed;
    getup_callback_audit::Dispatch dispatch;
    getup_callback_audit::Issue issue=getup_callback_audit::Issue::callback_unqualified;
};
struct GetupCallbackAuditTrace {
    bool available{true};
    GetupCallbackAuditStatus status;
    unsigned count{};
    std::array<GetupCallbackAuditRow,getup_callback_audit::capacity> samples;
};
// Getter-only post-action-observer evidence. No diagnostic value changes pulse
// eligibility or native input. Capacity/lock losses explicitly limit coverage.
bool begin_getup_callback_audit(std::uintptr_t base,const NativeFrame&,const GetupActionRequest&) noexcept;
// Caller owns the large buffer (normally allocated on the heap by report IO).
// No by-value return or large aggregate temporary is created by this adapter.
bool copy_getup_callback_audit_trace(GetupCallbackAuditTrace& out) noexcept;
void cancel_getup_callback_audit() noexcept;
}
