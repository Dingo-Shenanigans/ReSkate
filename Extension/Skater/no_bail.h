#pragma once
#include <cstddef>
#include <cstdint>

namespace dingosdk {
bool start_no_bail(std::uintptr_t image_base) noexcept;
bool no_bail_available() noexcept;
// Publish from the validated local client tick. Returns owner availability even
// when both controls are off. Manual protection expires if ticks stop arriving.
bool update_no_bail(std::uintptr_t client, std::uintptr_t entity, bool manual,
    bool flying, std::uint64_t flight_expires) noexcept;
void clear_no_bail() noexcept;
// Separate mounted-BMX no-bail + on-foot board-lock lease. Refresh on the client thread
// every mounted tick; expires after 250 ms. Acquisition requires the known mount-request
// consumer and a currently owned native Offboard (504) skater. false only clears BMX;
// manual No Bail, noclip protection and the S.K.A.T.E. board lock remain independent.
bool update_bmx_protection(std::uintptr_t client, std::uintptr_t entity, bool active) noexcept;
// One-shot native dismount request, accepted only for the validated local skater
// in grounded on-board state 100 or ollie crouch 103 and the supported selector contract.
// Queued for its next selector call, expiring 250 ms unused. Acceptance does not
// mean the transition happened: wait for native Offboard 504 before BMX acquisition.
// Other native requests retain their priority; no physics state is forced.
bool request_bmx_dismount(std::uintptr_t client, std::uintptr_t entity) noexcept;
// Cancel only an unused request, which has not touched native flags. The selector
// applies/restores its transient bit synchronously; changed native flags are left alone.
void cancel_bmx_dismount() noexcept;
// Reverse equipment switch: request the native on-foot mount (context bit7)
// from Offboard504. The selector retains its board/space/action checks and
// chooses the actual transition. Independent board locks take precedence.
bool request_bmx_board_mount(std::uintptr_t client, std::uintptr_t entity) noexcept;
void cancel_bmx_board_mount() noexcept;
// S.K.A.T.E.: while `locked`, the local skater cannot get back on the board once it is off
// (its mount request is dropped). Publish from the client tick; it expires if ticks stop.
// Releasing it also leaves the skater's teleport option on the board again, since a turn's
// teleport may have set it off (skater component +0xc0) and the SDK's own teleports keep it.
void update_board_lock(std::uintptr_t client, std::uintptr_t entity, bool locked) noexcept;
// The physics state the local skater's selector last chose, for the trainer (air time, bail
// markers). Publish the skater to watch from the client tick; it expires if ticks stop.
struct PhysicsStateWatch {
    bool valid{};
    std::uint32_t state{};
    std::uint32_t previous{}; // the state before this one
    float previous_seconds{}; // how long that one lasted
    std::uint64_t changes{}, wipeouts{}; // counted since the process started
};
void watch_physics_state(std::uintptr_t client, std::uintptr_t entity) noexcept;
PhysicsStateWatch watched_physics_state() noexcept;
// Stopping flight must not discard the independent manual preference.
void clear_no_bail_flight() noexcept;
// The local skater as No Bail follows it (update_no_bail publishes it), for a feature that reads it
// (Hall of Meat): any thread, its ownership chain resolved again now.
struct NoBailSkater {
    std::uintptr_t base{}, entity{}, core{}, context{}, rig{};
};
bool no_bail_skater(NoBailSkater& skater) noexcept;
// Read-only full local ownership/protection query for an owner-pinned handoff.
// false means unavailable/changed; protected_owner includes manual/flight/BMX
// leases and must be false before another feature requests a native bail.
bool inspect_no_bail_owner(std::uintptr_t client, std::uintptr_t entity,
    NoBailSkater& skater, bool& protected_owner) noexcept;
// Read-only PRE-PARK human BMX switch classification. Independent manual,
// flight and board locks stay authoritative. Never use this to admit a native
// input/bail after cleanup: inspect_no_bail_owner remains the strict gate there.
struct BmxSwitchAdmissionProtection {
    bool own_bmx{},manual{},flight{},board_lock{};
    std::uint64_t sampled_ms{},verified_ms{},bmx_until{},manual_until{},flight_until{},board_until{};
};
bool inspect_bmx_switch_admission_owner(std::uintptr_t client,std::uintptr_t entity,
    NoBailSkater& skater,BmxSwitchAdmissionProtection& protection) noexcept;
// Every physics step of that skater, whether or not No Bail protects it: No Bail's hook on the
// skeleton response runs once per step, right after the step's body contacts, and hands it on after
// its own filtering, so a wipeout it filtered never happened. `seconds` is the step's simulated time.
// One observer, nullptr for none; it runs on the physics thread.
using NoBailStepObserver = void (*)(const NoBailSkater& skater, float seconds, bool wipeout) noexcept;
void set_no_bail_step_observer(NoBailStepObserver observer) noexcept;
// Further listeners on the same steps (the grind exit control, grind_exit_control.h),
// independent of the observer above so none displaces Hall of Meat's. Same thread and timing. Adding one already there, or removing one that is not, does nothing;
// false when all slots are taken.
inline constexpr std::size_t no_bail_step_listener_slots = 4;
bool add_no_bail_step_listener(NoBailStepObserver listener) noexcept;
// Any thread: the watched local skater bails at its next physics step, as from a slam of
// `magnitude` (Board Break: a board that snaps on a rail throws the rider). The cause goes in just
// before the game's selector picks the next state, the way the game's own contact tests record
// theirs, so the game makes the wipeout itself; while No Bail protects the skater it is dropped like
// any other. Expires unused after a quarter of a second.
void no_bail_request_wipeout(float magnitude) noexcept;
void remove_no_bail_step_listener(NoBailStepObserver listener) noexcept;

// Diagnostic ordering seam. Existing step observers/listeners remain pre-original.
// `before_original` is AFTER those listeners; `original_returned` proves only that
// this invocation returned, not completion of asynchronous physics or any branch.
// The incoming third argument is retained before filtering; effective is the exact
// argument handed to the original. An observer must not change either argument.
enum class NoBailSkeletonEdge : std::uint8_t { before_original, original_returned };
struct NoBailSkeletonEvent {
    NoBailSkeletonEdge edge{};
    std::uint64_t invocation{};
    std::uintptr_t rig_argument{};
    NoBailSkater owner{};
    float seconds{};
    bool owner_available{}, incoming_third{}, effective_third{};
};
// Pre returns an opaque fixed-storage cookie; post receives the same cookie.
// Unavailable owner is reported without dereferencing the retained rig argument.
using NoBailSkeletonObserver = std::uint64_t (*)(const NoBailSkeletonEvent&, std::uint64_t cookie) noexcept;
// Independent one-slot registration; never replaces another diagnostic observer.
bool add_no_bail_skeleton_observer(NoBailSkeletonObserver observer) noexcept;
void remove_no_bail_skeleton_observer(NoBailSkeletonObserver observer) noexcept;
// Nonblocking SDK publication lock, then fresh full-chain resolution. False is
// unknown or changed, never permission to dereference a previously seen owner.
bool try_no_bail_skater(std::uintptr_t rig, NoBailSkater& skater) noexcept;
// Optional diagnostic client from the same freshly accepted published Owner;
// zero on unavailable. The existing overload and ownership checks are unchanged.
bool try_no_bail_skater(std::uintptr_t rig,NoBailSkater& skater,std::uintptr_t& observed_client) noexcept;
}

namespace dingosdk {
// Private passive diagnostic seam. Entry/exit are synchronous observations only;
// the original native call and every existing request/filter retain their order.
enum class NoBailNativePhase : std::uint8_t {
    selector_before_sdk, selector_after_restore, publisher_before_original, publisher_original_returned
};
struct NoBailNativeEvent {
    NoBailNativePhase phase{};std::uintptr_t argument{};
    std::uint32_t current{},native_result{};
};
using NoBailNativeObserver=std::uint64_t (*)(const NoBailNativeEvent&,std::uint64_t cookie) noexcept;
// One independent slot. Registration never replaces another observer.
bool add_no_bail_native_observer(NoBailNativeObserver observer) noexcept;
void remove_no_bail_native_observer(NoBailNativeObserver observer) noexcept;
}
