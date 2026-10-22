#include "no_bail.h"
#include "Extension/BoardBreak/board_break.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Game/Skater/protection_owner.h"
#include "Engine/Game/BMX/equipment_switch.h"
#include "Engine/Game/BMX/bmx_skeleton_phase_policy.h"
#include <array>
#include <atomic>
#include <intrin.h>

namespace dingosdk {
namespace {
using namespace addr::no_bail;
static_assert(bmx::natural_dismount_state(grounded_board_physics_state) &&
              bmx::natural_dismount_state(crouched_board_physics_state));
// Native cause recorder: RCX=collector, EDX=reason, XMM2=magnitude.
using RecordCause = void (*)(std::uintptr_t, std::int32_t, float);
using ChooseState = std::uint32_t (*)(std::uintptr_t, std::uint32_t);
using SkeletonResponse = void (*)(std::uintptr_t, float, bool);
using PublishAnimation = void (*)(std::uintptr_t);
using ResetCauses = void (*)(std::uintptr_t);
constexpr std::uintptr_t highest = memory::highest_user_address;
// The cause most real slams record (Board Break's logs: cause 6 at forces of 9.6 to 10.7): a wipeout
// asked for goes in as one of those.
constexpr std::int32_t wanted_wipeout_reason = 6;
// The hooks re-resolve the local skater's ownership (some 35 fields) on every
// protected physics step: guarded same-process copies, not a system call each.
template<class T> bool read(std::uintptr_t address, T& value) noexcept { return memory::peek(address, value); }
struct LastError {
    DWORD value = GetLastError();
    ~LastError() { SetLastError(value); }
};
std::uintptr_t pointer(std::uintptr_t object, std::uintptr_t offset = 0) noexcept {
    std::uintptr_t value{};
    if (object < 0x10000 || object > highest - offset || !read(object + offset, value) ||
        value < 0x10000 || value > highest - 0x1000100) return 0;
    return value;
}
struct Owner {
    std::uintptr_t client{}, entity{}, player{}, handle{}, component{}, core{}, context{}, selector{}, causes{}, rig{};
    bool operator==(const Owner&) const = default;
};
struct Lease {
    Owner owner;
    std::uint64_t manual_until{}, flight_until{};
    bool active(std::uint64_t now) const noexcept {
        return now < manual_until || now < flight_until;
    }
};
struct BoardLock {
    Owner owner;
    std::uint64_t until{};
};
struct Protection {
    std::uintptr_t base{};
    bool mount_test{}; // the selector's mount request test is the known one
    bool dismount_test{};
    bool board_mount_test{};
    BoardLock board;
    BoardLock bmx; // independent no-bail + mount-request hold; never changes teleport preference
    BoardLock dismount; // one-shot native request, consumed only by its owner's selector
    std::atomic<bool> dismount_pending{}; // ordinary selectors avoid an exclusive SDK lock
    BoardLock board_mount;
    std::atomic<bool> board_mount_pending{};
    RecordCause cause_original{};
    ChooseState choose_original{};
    SkeletonResponse skeleton_original{};
    PublishAnimation publish_original{};
    ResetCauses reset_causes{};
    std::atomic<bool> ready{};
    SRWLOCK lock = SRWLOCK_INIT;
    Lease lease;
    std::atomic<NoBailStepObserver> step_observer{};
    std::atomic<NoBailSkeletonObserver> skeleton_observer{};
    std::atomic<NoBailNativeObserver> native_observer{};
    std::atomic<std::uint64_t> skeleton_observation_sequence{};
    std::array<std::atomic<NoBailStepObserver>, no_bail_step_listener_slots> listeners{};
};
Protection& protection() { static auto* value = new Protection; return *value; }
struct StateWatch {
    std::atomic<std::uintptr_t> selector{}, causes{}; // the watched skater's selector and cause collector
    std::atomic<std::uint64_t> until{}, changes{}, wipeouts{};
    std::atomic<std::uint32_t> state{}, previous{};
    std::atomic<std::int64_t> since{}, previous_ticks{}; // performance-counter ticks
};
StateWatch& state_watch() { static auto* value = new StateWatch; return *value; }
// A wipeout asked for (no_bail_request_wipeout): the cause goes in at the watched selector's next pick.
struct WantedWipeout {
    std::atomic<float> magnitude{};
    std::atomic<std::uint64_t> until{};
};
WantedWipeout& wanted_wipeout() { static auto* value = new WantedWipeout; return *value; }

// Recheck live local ownership at use time. A retained physics address alone
// must never protect another skater after a respawn or level change.
bool resolve(std::uintptr_t client, std::uintptr_t entity, Owner& o) noexcept {
    const auto base = protection().base;
    unsigned state{}, manager_offset{};
    if (pointer(client) != base + addr::engine::client_vtable || !read(client + 0xc4, state) ||
        (state != 13 && state != 21) || pointer(entity) != base + addr::engine::skater_entity_vtable ||
        !read(base + addr::engine::context_player_manager_offset, manager_offset) || manager_offset > 0x1000000) return false;
    const auto context = pointer(client, 8);
    const auto manager = pointer(context, manager_offset);
    const auto begin = pointer(manager, 0x4c8), end = pointer(manager, 0x4d0);
    if (!context || pointer(entity, 0x20) != context || pointer(manager) != base + addr::engine::local_player_manager_vtable ||
        !begin || end != begin + 8) return false;
    o.player = pointer(begin);
    std::uint8_t local{}, remote{}, teleport{};
    if (pointer(o.player) != base + addr::engine::local_player_vtable || pointer(o.player, 0x78) != context ||
        !read(o.player + 0x45, local) || local != 1 || !read(o.player + 0x44, remote) || remote ||
        pointer(o.player, 0xb8) != entity || pointer(entity, 0xf8) != o.player ||
        !read(entity + 0x7e0, teleport) || teleport) return false;
    o.handle = pointer(o.player, 0xb0);
    const auto collection = pointer(entity, 0x70);
    o.component = pointer(entity, 0x628);
    o.core = pointer(o.component, 0x70);
    o.context = pointer(o.core, 0x3c0);
    o.selector = pointer(o.core, 0x440);
    o.causes = pointer(o.core, 0x428);
    o.rig = pointer(o.core, 0x438);
    if (pointer(o.handle) != entity + 8 || pointer(collection) != entity ||
        pointer(o.component) != base + addr::engine::skater_component_vtable || pointer(o.component, 0x18) != collection ||
        pointer(o.core) != base + bail_core_vtable || !o.context || !o.selector ||
        pointer(o.selector, 8) != o.context || pointer(o.causes, 0x20) != o.context || pointer(o.rig) != o.context || pointer(o.rig, 0x4630) != o.core)
        return false;
    o.client = client;
    o.entity = entity;
    return true;
}
bool protected_owner(std::uintptr_t object, std::uintptr_t Owner::* member, Owner* owner = nullptr) noexcept {
    auto& p = protection();
    if (!p.ready.load(std::memory_order_acquire)) return false;
    // The lock only copies SDK data, and is never held across native code or
    // memory reads. Hooks do not contend for the debug/camera action lock.
    AcquireSRWLockShared(&p.lock);
    const auto lease = p.lease;
    const auto bmx = p.bmx;
    ReleaseSRWLockShared(&p.lock);
    const auto now = GetTickCount64();
    return skater::validated_protection_owner(lease.owner, lease.active(now), object, member, resolve, owner) ||
        skater::validated_protection_owner(bmx.owner, now < bmx.until, object, member, resolve, owner);
}
// The published owner, its chain resolved again now; when `rig` is given, only if it is that owner's.
bool published_skater(std::uintptr_t rig, NoBailSkater& skater, bool nonblocking = false, std::uintptr_t* observed_client = nullptr) noexcept {
    if(observed_client)*observed_client=0;
    auto& p = protection();
    if (!p.ready.load(std::memory_order_acquire)) return false;
    if (nonblocking) {
        if (!TryAcquireSRWLockShared(&p.lock)) return false;
    } else AcquireSRWLockShared(&p.lock);
    const auto published = p.lease.owner;
    const auto bmx = p.bmx;
    ReleaseSRWLockShared(&p.lock);
    const auto accept = [&](const Owner& candidate) {
        Owner current;
        if (!candidate.entity || (rig && rig != candidate.rig) || !resolve(candidate.client, candidate.entity, current) ||
            current != candidate) return false;
        skater = {p.base, current.entity, current.core, current.context, current.rig};
        if(observed_client)*observed_client=current.client;
        return true;
    };
    return accept(published) || (GetTickCount64() < bmx.until && accept(bmx.owner));
}
bool cancel_request(std::uintptr_t context, std::uintptr_t offset, LONG mask) noexcept {
    const auto address = context + offset;
    if (context < 0x10000 || context > highest - offset - sizeof(LONG) ||
        (address & (alignof(LONG) - 1)) != 0) return false;
    // Consume only the identified request bit. Preserve unrelated native flags
    // even when another producer updates the same word.
    __try {
        _InterlockedAnd(reinterpret_cast<volatile LONG*>(address), ~mask);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool insert_animation_request(std::uintptr_t context, LONG mask, LONG& before) noexcept {
    const auto address = context + animation_request_offset;
    if (context < 0x10000 || context > highest - animation_request_offset - sizeof(LONG) ||
        (address & (alignof(LONG) - 1)) != 0) return false;
    __try {
        before = _InterlockedOr(reinterpret_cast<volatile LONG*>(address), mask);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void restore_animation_request(std::uintptr_t context, LONG mask, LONG before) noexcept {
    // Restore only a bit introduced here, and only if the whole native word is
    // still the one we inserted. A pre-existing request or changed native word
    // belongs to the game and is left alone; never overwrite unrelated flags.
    if (before & mask) return;
    const auto address = context + animation_request_offset;
    if (context < 0x10000 || context > highest - animation_request_offset - sizeof(LONG) ||
        (address & (alignof(LONG) - 1)) != 0) return;
    __try {
        (void)_InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(address), before,
            before | mask);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
struct DismountInjection {
    Owner owner;
    LONG before{};
    LONG mask{dismount_request_mask};
    bool inserted{};
};
DismountInjection begin_dismount(std::uintptr_t selector, std::uint32_t current) noexcept {
    LastError error;
    auto& p = protection();
    DismountInjection injection;
    if (!p.ready.load(std::memory_order_acquire) || !p.dismount_test ||
        !p.dismount_pending.load(std::memory_order_acquire)) return injection;
    const auto now = GetTickCount64();
    BoardLock request;
    AcquireSRWLockExclusive(&p.lock);
    if (now >= p.dismount.until) p.dismount = {};
    if (p.dismount.owner.selector == selector) {
        request = p.dismount;
        p.dismount = {};
    }
    p.dismount_pending.store(p.dismount.until != 0, std::memory_order_release);
    ReleaseSRWLockExclusive(&p.lock);
    if (!bmx::natural_dismount_state(current) ||
        !skater::validated_protection_owner(request.owner, now < request.until,
            selector, &Owner::selector, resolve, &injection.owner)) return injection;
    std::uint32_t state{};
    if (!read(injection.owner.context + 0x1414, state) ||
        !bmx::natural_dismount_selection(current, state)) return injection;
    injection.inserted = insert_animation_request(injection.owner.context, injection.mask, injection.before);
    return injection;
}
void end_dismount(const DismountInjection& injection) noexcept {
    LastError error;
    if (!injection.inserted || (injection.before & injection.mask)) return;
    Owner owner;
    if (resolve(injection.owner.client, injection.owner.entity, owner) && owner == injection.owner)
        restore_animation_request(owner.context, injection.mask, injection.before);
}
DismountInjection begin_board_mount(std::uintptr_t selector, std::uint32_t current) noexcept {
    LastError error;
    auto& p = protection();
    DismountInjection injection;
    injection.mask = mount_request_mask;
    if (!p.ready.load(std::memory_order_acquire) || !p.board_mount_test ||
        !p.board_mount_pending.load(std::memory_order_acquire)) return injection;
    const auto now = GetTickCount64();
    BoardLock request, board, bmx;
    AcquireSRWLockExclusive(&p.lock);
    if (now >= p.board_mount.until) p.board_mount = {};
    if (p.board_mount.owner.selector == selector) { request = p.board_mount; p.board_mount = {}; }
    p.board_mount_pending.store(p.board_mount.until != 0, std::memory_order_release);
    board = p.board; bmx = p.bmx;
    ReleaseSRWLockExclusive(&p.lock);
    if (current != offboard_physics_state ||
        !skater::validated_protection_owner(request.owner, now < request.until,
            selector, &Owner::selector, resolve, &injection.owner)) return injection;
    Owner held;
    if (skater::validated_protection_owner(board.owner, now < board.until, selector, &Owner::selector, resolve, &held) ||
        skater::validated_protection_owner(bmx.owner, now < bmx.until, selector, &Owner::selector, resolve, &held)) return injection;
    std::uint32_t state{};
    if (!read(injection.owner.context + 0x1414, state) || state != offboard_physics_state) return injection;
    injection.inserted = insert_animation_request(injection.owner.context, injection.mask, injection.before);
    return injection;
}
bool cancel_impact_request(std::uintptr_t context) noexcept {
    return cancel_request(context, impact_request_offset, impact_request_mask);
}
bool cancel_wipeout_requests(std::uintptr_t context) noexcept {
    return cancel_impact_request(context) &&
        cancel_request(context, animation_request_offset, animation_request_mask);
}
bool reset_pending_causes(std::uintptr_t causes) noexcept {
    const auto reset = protection().reset_causes;
    if (!reset) return false;
    __try {
        reset(causes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool filter_requests(std::uintptr_t object, std::uintptr_t Owner::* member) noexcept {
    LastError error;
    Owner owner;
    // Native collision/landing checks also write the collector inline. Reset
    // it at each consumer, after those writes, before either animation's bail
    // or recover/runout query. Filtering record_cause alone misses this path.
    return protected_owner(object, member, &owner) && cancel_wipeout_requests(owner.context) &&
        reset_pending_causes(owner.causes);
}
bool suppress_cause(std::uintptr_t causes, std::int32_t reason, std::uintptr_t caller) noexcept {
    LastError error;
    Owner owner;
    if (!protected_owner(causes, &Owner::causes, &owner)) return false;
    for (const auto& impact : impact_bail_calls) {
        if (caller == protection().base + impact.return_rva && reason == impact.reason)
            return cancel_impact_request(owner.context);
    }
    return true;
}
void record_cause(std::uintptr_t causes, std::int32_t reason, float magnitude) {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if (causes == state_watch().causes.load(std::memory_order_acquire)) board_break::note_cause(reason, magnitude);
    const bool protect = suppress_cause(causes, reason, caller);
    // Do not force the native recovery/stumble predicate (recovery_predicate). Its
    // result is also exported to animation at +0x9e, even without a collision.
    // Stop new causes before they reach either the wipeout or runout decision;
    // the native per-step reset still owns clearing the collector's history.
    if (!protect) protection().cause_original(causes, reason, magnitude);
}
// The local skater waiting on foot for its S.K.A.T.E. turn: its mount request goes.
void hold_off_board(std::uintptr_t selector, std::uint32_t current) noexcept {
    auto& p = protection();
    if (current != offboard_physics_state || !p.mount_test || !p.ready.load(std::memory_order_acquire)) return;
    AcquireSRWLockShared(&p.lock);
    const auto board = p.board;
    const auto bmx = p.bmx;
    ReleaseSRWLockShared(&p.lock);
    Owner current_owner;
    const auto now = GetTickCount64();
    if (!skater::validated_protection_owner(board.owner, now < board.until, selector, &Owner::selector, resolve, &current_owner) &&
        !skater::validated_protection_owner(bmx.owner, now < bmx.until, selector, &Owner::selector, resolve, &current_owner)) return;
    (void)cancel_request(current_owner.context, animation_request_offset, mount_request_mask);
}
std::uint32_t choose_state(std::uintptr_t selector, std::uint32_t current) {
    const auto native_observer=protection().native_observer.load(std::memory_order_acquire);
    std::uint64_t native_cookie{};
    if(native_observer){LastError error;native_cookie=native_observer(
        {NoBailNativePhase::selector_before_sdk,selector,current,0},0);}
    {
        LastError error;
        hold_off_board(selector, current);
    }
    // A wipeout asked for (no_bail_request_wipeout): its cause goes in as the game's contact tests
    // record theirs, and the pick is Wipeout, as some of those tests return it directly.
    bool force_wipeout{};
    if (auto& wanted = wanted_wipeout(); wanted.until.load(std::memory_order_acquire) &&
        selector == state_watch().selector.load(std::memory_order_acquire)) {
        LastError error;
        if (const auto until = wanted.until.exchange(0, std::memory_order_acq_rel); GetTickCount64() < until)
            if (const auto causes = state_watch().causes.load(std::memory_order_acquire); causes && !protected_owner(selector, &Owner::selector)) {
                record_cause(causes, wanted_wipeout_reason, wanted.magnitude.load(std::memory_order_relaxed));
                force_wipeout = true;
            }
    }
    // Clear shared requests before selection so native ground/air/walking
    // transitions can still run. Some contact tests return Wipeout directly;
    // retain the current state only for that result, never ordinary Offboard.
    const bool filtered = filter_requests(selector, &Owner::selector);
    const auto dismount = begin_dismount(selector, current);
    const auto mount = begin_board_mount(selector, current);
    auto next = protection().choose_original(selector, current);
    end_dismount(mount);
    end_dismount(dismount);
    if(native_observer){LastError error;(void)native_observer(
        {NoBailNativePhase::selector_after_restore,selector,current,next},native_cookie);}
    LastError error;
    if (force_wipeout && next != wipeout_physics_state) {
        logging::log(logging::Level::debug, logging::Channel::skater, "No Bail: a wipeout was asked for (state {} would have been {}).", current, next);
        next = wipeout_physics_state;
    }
    const auto chosen = filtered && next == wipeout_physics_state && protected_owner(selector, &Owner::selector) ? current : next;
    auto& w = state_watch();
    if (selector == w.selector.load(std::memory_order_acquire) && GetTickCount64() < w.until.load(std::memory_order_acquire)) {
        if (const auto before = w.state.exchange(chosen, std::memory_order_acq_rel); before != chosen) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            w.previous.store(before, std::memory_order_relaxed);
            w.previous_ticks.store(now.QuadPart - w.since.exchange(now.QuadPart, std::memory_order_relaxed), std::memory_order_relaxed);
            w.changes.fetch_add(1, std::memory_order_relaxed);
            if (chosen == wipeout_physics_state) {
                // Before the count: the tick that sees the new wipeout finds its cause there.
                board_break::note_wipeout(w.causes.load(std::memory_order_acquire));
                w.wipeouts.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    return chosen;
}
void skeleton_response(std::uintptr_t rig, float seconds, bool wipeout) {
    const bool incoming_third = wipeout;
    // The state post-update can raise another request after the selector ran.
    // Filter at this consumer, then let native constraints and recovery run.
    if (filter_requests(rig, &Owner::rig)) wipeout = false;
    const auto observer = protection().step_observer.load(std::memory_order_acquire);
    std::array<NoBailStepObserver, no_bail_step_listener_slots> listeners{};
    bool any = observer != nullptr;
    for (std::size_t i = 0; i < listeners.size(); ++i) {
        listeners[i] = protection().listeners[i].load(std::memory_order_acquire);
        any = any || listeners[i];
    }
    if (any) {
        LastError error;
        NoBailSkater skater;
        if (published_skater(rig, skater)) {
            if (observer) observer(skater, seconds, wipeout);
            for (const auto listener : listeners)
                if (listener) listener(skater, seconds, wipeout);
        }
    }
    // Disabled seam adds no owner reads and preserves original dispatch.
    const auto phase = protection().skeleton_observer.load(std::memory_order_acquire);
    if (!phase) { protection().skeleton_original(rig, seconds, wipeout); return; }
    NoBailSkeletonEvent event;
    bmx::skeleton_phase_probe::detail::invoke_observed(rig, seconds, wipeout, protection().skeleton_original,
        [&]() noexcept {
            event.edge = NoBailSkeletonEdge::before_original;
            event.invocation = protection().skeleton_observation_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
            event.rig_argument = rig;
            event.seconds = seconds;
            event.incoming_third = incoming_third;
            event.effective_third = wipeout;
            event.owner_available = published_skater(rig, event.owner, true);
            return phase(event, 0);
        },
        [&](std::uint64_t cookie) noexcept {
            event.edge = NoBailSkeletonEdge::original_returned;
            event.owner = {};
            event.owner_available = published_skater(rig, event.owner, true);
            (void)phase(event, cookie);
        },
        []() noexcept { return GetLastError(); },
        [](DWORD value) noexcept { SetLastError(value); });
}
bool clear_contact_output(std::uintptr_t contacts) noexcept {
    if (contacts < 0x10000 || contacts > highest - body_contact_output_offset) return false;
    __try {
        _InterlockedExchange8(reinterpret_cast<volatile char*>(contacts + body_contact_output_offset), 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void filter_contact_animation(std::uintptr_t core) noexcept {
    LastError error;
    Owner owner;
    std::uint32_t state{};
    if (!protected_owner(core, &Owner::core, &owner) || !read(owner.context + 0x1414, state)) return;
    // Ground/air and grind state families. Offboard, mounting, handplants and
    // existing ragdolls retain their native contact reporting and recovery.
    if (!((state >= 100 && state < 300) || (state >= 400 && state < 500))) return;
    const auto contacts = pointer(pointer(core, 0x3b8), 0x30);
    std::uint8_t contact{};
    if (!contacts || !read(contacts + body_contact_output_offset, contact) || contact > 1) return;
    // Animation gets this sensitive-body hit independently of the ordinary
    // wipeout output and cause collector. Filter it after native publication,
    // before the animation contact context copies it. Keep the general contact
    // latch, collision timers, per-bone records, impulses and physics state.
    if (clear_contact_output(contacts))
        (void)cancel_request(owner.context, body_contact_context_offset, body_contact_context_mask);
}
void publish_animation(std::uintptr_t core) {
    // Animation also reads requests without consulting the cause collector.
    // Clear flags and pending causes before native publication, including
    // its early recovery query and cause export. Leave the native query result
    // unchanged: forcing it true used to put the skater into a stumbling state.
    (void)filter_requests(core, &Owner::core);
    const auto native_observer=protection().native_observer.load(std::memory_order_acquire);
    std::uint64_t native_cookie{};
    if(native_observer){LastError error;native_cookie=native_observer(
        {NoBailNativePhase::publisher_before_original,core,0,0},0);}
    protection().publish_original(core);
    if(native_observer){LastError error;(void)native_observer(
        {NoBailNativePhase::publisher_original_returned,core,0,0},native_cookie);}
    filter_contact_animation(core);
}
bool compatible(std::uintptr_t base) noexcept {
    if (base < 0x10000 || base > highest - supported_build::game_image_size) return false;
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!read(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x100000 ||
        !read(base + dos.e_lfanew, nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.SizeOfImage != supported_build::game_image_size)
        return false;
    std::array<unsigned char, reset_bail_causes_code.size()> reset_code{};
    if (!read(base + reset_bail_causes_rva, reset_code) || reset_code != reset_bail_causes_code) return false;
    for (const auto& contract : {record_bail_cause_contract, choose_physics_state_contract,
            bail_animation_caller_contract, bail_state_caller_contract, bail_skeleton_contract, bail_publish_contract}) {
        std::array<unsigned char, 32> actual{};
        if (!read(base + contract.rva, actual) || actual != contract.bytes) return false;
    }
    for (const auto& contract : bail_consumer_contracts) {
        std::array<unsigned char, 32> actual{};
        if (!read(base + contract.rva, actual) || actual != contract.bytes) return false;
    }
    for (const auto& contract : body_contact_contracts) {
        std::array<unsigned char, 32> actual{};
        if (!read(base + contract.rva, actual) || actual != contract.bytes) return false;
    }
    for (const auto& impact : impact_bail_calls) {
        std::array<unsigned char, 32> actual{};
        if (!read(base + impact.caller.rva, actual) || actual != impact.caller.bytes) return false;
    }
    return true;
}
}
bool start_no_bail(std::uintptr_t base) noexcept {
    LastError error;
    try {
        auto& p = protection();
        if (p.ready.load()) return p.base == base;
        if (!compatible(base)) {
            logging::write(logging::Level::warning, logging::Channel::skater,
                "No Bail is unavailable: native bail contract did not match.");
            return false;
        }
        p.base = base;
        p.reset_causes = reinterpret_cast<ResetCauses>(base + reset_bail_causes_rva);
        std::array<unsigned char, 32> mount_test{};
        p.mount_test = read(base + mount_request_test_contract.rva, mount_test) &&
                       mount_test == mount_request_test_contract.bytes;
        p.dismount_test = true;
        for (const auto& contract : dismount_request_contracts) {
            std::array<unsigned char, 32> actual{};
            if (!read(base + contract.rva, actual) || actual != contract.bytes) {
                p.dismount_test = false;
                break;
            }
        }
        p.board_mount_test = p.mount_test && p.dismount_test;
        for (const auto& contract : board_mount_request_contracts) {
            std::array<unsigned char, 32> actual{};
            if (!read(base + contract.rva, actual) || actual != contract.bytes) {
                p.board_mount_test = false;
                break;
            }
        }
        const std::array targets{
            reinterpret_cast<void*>(base + record_bail_cause_contract.rva),
            reinterpret_cast<void*>(base + choose_physics_state_contract.rva),
            reinterpret_cast<void*>(base + bail_skeleton_contract.rva),
            reinterpret_cast<void*>(base + bail_publish_contract.rva)};
        const std::array replacements{
            reinterpret_cast<void*>(&record_cause), reinterpret_cast<void*>(&choose_state),
            reinterpret_cast<void*>(&skeleton_response), reinterpret_cast<void*>(&publish_animation)};
        std::array<void*, 4> originals{};
        auto status = HookOk;
        std::size_t prepared{};
        for (; prepared < targets.size(); ++prepared) {
            status = hook_prepare(targets[prepared], replacements[prepared], &originals[prepared]);
            if (status != HookOk) break;
            if (!originals[prepared]) { ++prepared; status = HookUnsupportedFunction; break; }
        }
        if (status == HookOk) {
            // Publish every relay before enabling any target.
            p.cause_original = reinterpret_cast<RecordCause>(originals[0]);
            p.choose_original = reinterpret_cast<ChooseState>(originals[1]);
            p.skeleton_original = reinterpret_cast<SkeletonResponse>(originals[2]);
            p.publish_original = reinterpret_cast<PublishAnimation>(originals[3]);
            for (auto target : targets) {
                status = hook_enable(target);
                if (status != HookOk) break;
            }
            if (status == HookOk) {
                p.ready.store(true, std::memory_order_release);
                logging::write(logging::Level::info, logging::Channel::skater,
                    "No Bail ready: collision and landing causes filtered before physics/animation consume them; noclip is protected.");
                return true;
            }
        }
        // Published relays remain callable even if Detours reports an uncertain
        // attach result. With ready=false any remaining hook simply forwards.
        logging::log(logging::Level::warning, logging::Channel::skater,
            "No Bail hook setup failed (status {}); protected noclip is unavailable.", static_cast<LONG>(status));
        while (prepared) (void)hook_remove(targets[--prepared]);
    } catch (...) {}
    return false;
}
bool no_bail_available() noexcept { return protection().ready.load(std::memory_order_acquire); }
bool update_no_bail(std::uintptr_t client, std::uintptr_t entity, bool manual,
    bool flying, std::uint64_t flight_expires) noexcept {
    LastError error;
    auto& p = protection();
    Lease next;
    const bool available = p.ready.load(std::memory_order_acquire) && resolve(client, entity, next.owner);
    if (available) {
        const auto now = GetTickCount64();
        next.manual_until = manual ? now + 500 : 0;
        next.flight_until = flying ? flight_expires : 0;
    }
    AcquireSRWLockExclusive(&p.lock);
    p.lease = available ? next : Lease{};
    ReleaseSRWLockExclusive(&p.lock);
    return available;
}
bool update_bmx_protection(std::uintptr_t client, std::uintptr_t entity, bool active) noexcept {
    LastError error;
    auto& p = protection();
    BoardLock next;
    bool accepted = !active;
    if (active && p.ready.load(std::memory_order_acquire) && p.mount_test && resolve(client, entity, next.owner)) {
        std::uint32_t physics{};
        if (read(next.owner.context + 0x1414, physics) && physics == offboard_physics_state) {
            next.until = GetTickCount64() + 250;
            accepted = true;
        }
    }
    // A failed refresh clears this feature's lease immediately. No previous owner's
    // protection survives loading/replacement; other protection slots are untouched.
    AcquireSRWLockExclusive(&p.lock);
    p.bmx = accepted && active ? next : BoardLock{};
    ReleaseSRWLockExclusive(&p.lock);
    return accepted;
}
bool request_bmx_dismount(std::uintptr_t client, std::uintptr_t entity) noexcept {
    LastError error;
    auto& p = protection();
    BoardLock next;
    bool accepted{};
    if (p.ready.load(std::memory_order_acquire) && p.dismount_test && resolve(client, entity, next.owner)) {
        std::uint32_t state{};
        if (read(next.owner.context + 0x1414, state) && bmx::natural_dismount_state(state)) {
            next.until = GetTickCount64() + 250;
            accepted = true;
        }
    }
    AcquireSRWLockExclusive(&p.lock);
    p.dismount = accepted ? next : BoardLock{};
    p.dismount_pending.store(accepted, std::memory_order_release);
    ReleaseSRWLockExclusive(&p.lock);
    return accepted;
}
void cancel_bmx_dismount() noexcept {
    auto& p = protection();
    AcquireSRWLockExclusive(&p.lock);
    p.dismount = {};
    p.dismount_pending.store(false, std::memory_order_release);
    ReleaseSRWLockExclusive(&p.lock);
}
bool request_bmx_board_mount(std::uintptr_t client, std::uintptr_t entity) noexcept {
    LastError error;
    auto& p = protection();
    BoardLock next;
    bool accepted{};
    if (p.ready.load(std::memory_order_acquire) && p.board_mount_test && resolve(client, entity, next.owner)) {
        std::uint32_t state{};
        if (read(next.owner.context + 0x1414, state) && state == offboard_physics_state) {
            next.until = GetTickCount64() + 250;
            accepted = true;
        }
    }
    AcquireSRWLockExclusive(&p.lock);
    // Do not queue through another feature's on-foot lock, including a still
    // live BMX lease. The selector independently rechecks the same precedence.
    const auto now = GetTickCount64();
    if ((p.board.owner.entity == entity && now < p.board.until) ||
        (p.bmx.owner.entity == entity && now < p.bmx.until)) accepted = false;
    p.board_mount = accepted ? next : BoardLock{};
    p.board_mount_pending.store(accepted, std::memory_order_release);
    ReleaseSRWLockExclusive(&p.lock);
    return accepted;
}
void cancel_bmx_board_mount() noexcept {
    auto& p = protection();
    AcquireSRWLockExclusive(&p.lock);
    p.board_mount = {};
    p.board_mount_pending.store(false, std::memory_order_release);
    ReleaseSRWLockExclusive(&p.lock);
}
bool set_teleport_on_board(std::uintptr_t component) noexcept {
    __try {
        *reinterpret_cast<volatile std::uint8_t*>(component + 0xc0) = 1;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void update_board_lock(std::uintptr_t client, std::uintptr_t entity, bool locked) noexcept {
    LastError error;
    auto& p = protection();
    if (!p.ready.load(std::memory_order_acquire)) return;
    static bool rearm{}; // client tick only
    if (locked) {
        // The owner does not resolve while a teleport is under way: the last one stands
        // until it expires.
        BoardLock next;
        if (resolve(client, entity, next.owner)) {
            next.until = GetTickCount64() + 500;
            AcquireSRWLockExclusive(&p.lock);
            p.board = next;
            ReleaseSRWLockExclusive(&p.lock);
        }
        rearm = true;
        return;
    }
    AcquireSRWLockExclusive(&p.lock);
    p.board = {};
    ReleaseSRWLockExclusive(&p.lock);
    // Released: teleports that keep the skater's own choice (the SDK's /tp) put it on the
    // board again. Once the skater resolves, outside a teleport.
    Owner owner;
    if (rearm && resolve(client, entity, owner) && set_teleport_on_board(owner.component)) rearm = false;
}
void clear_no_bail() noexcept {
    auto& p = protection();
    AcquireSRWLockExclusive(&p.lock);
    p.lease = {};
    ReleaseSRWLockExclusive(&p.lock);
}
bool no_bail_skater(NoBailSkater& skater) noexcept { return published_skater(0, skater); }
bool inspect_no_bail_owner(std::uintptr_t client, std::uintptr_t entity,
    NoBailSkater& skater, bool& protected_now) noexcept {
    LastError error;
    skater = {}; protected_now = false;
    auto& p = protection();
    Owner first, last;
    if (!p.ready.load(std::memory_order_acquire) || !resolve(client, entity, first)) return false;
    const bool held = protected_owner(first.core, &Owner::core);
    if (!resolve(client, entity, last) || first != last) return false;
    skater = {p.base, last.entity, last.core, last.context, last.rig};
    protected_now = held;
    return true;
}
bool inspect_bmx_switch_admission_owner(std::uintptr_t client,std::uintptr_t entity,
    NoBailSkater& skater,BmxSwitchAdmissionProtection& observed) noexcept {
    LastError error;skater={};observed={};auto& p=protection();Owner first,last;
    if(!p.ready.load(std::memory_order_acquire)||!resolve(client,entity,first))return false;
    AcquireSRWLockShared(&p.lock);const auto lease=p.lease;const auto bmx=p.bmx;const auto board=p.board;ReleaseSRWLockShared(&p.lock);
    const auto sampled=GetTickCount64();
    if(!resolve(client,entity,last)||first!=last)return false;
    AcquireSRWLockShared(&p.lock);
    const bool stable=p.lease.owner==lease.owner&&p.lease.manual_until==lease.manual_until&&p.lease.flight_until==lease.flight_until&&
        p.bmx.owner==bmx.owner&&p.bmx.until==bmx.until&&p.board.owner==board.owner&&p.board.until==board.until;
    ReleaseSRWLockShared(&p.lock);const auto verified=GetTickCount64();
    if(!p.ready.load(std::memory_order_acquire)||!stable||verified<sampled)return false;
    skater={p.base,last.entity,last.core,last.context,last.rig};
    observed={first==bmx.owner&&verified<bmx.until,sampled<lease.manual_until,sampled<lease.flight_until,sampled<board.until,
        sampled,verified,bmx.until,lease.manual_until,lease.flight_until,board.until};
    return true;
}
bool add_no_bail_skeleton_observer(NoBailSkeletonObserver observer) noexcept {
    if (!observer) return false;
    NoBailSkeletonObserver empty{};
    return protection().skeleton_observer.compare_exchange_strong(empty, observer, std::memory_order_acq_rel) || empty == observer;
}
void remove_no_bail_skeleton_observer(NoBailSkeletonObserver observer) noexcept {
    if (!observer) return;
    protection().skeleton_observer.compare_exchange_strong(observer, nullptr, std::memory_order_acq_rel);
}
bool try_no_bail_skater(std::uintptr_t rig, NoBailSkater& skater) noexcept {
    LastError error;
    skater = {};
    return published_skater(rig, skater, true);
}
bool try_no_bail_skater(std::uintptr_t rig,NoBailSkater& skater,std::uintptr_t& observed_client) noexcept {
    LastError error;skater={};observed_client=0;
    return published_skater(rig,skater,true,&observed_client);
}
void set_no_bail_step_observer(NoBailStepObserver observer) noexcept {
    protection().step_observer.store(observer, std::memory_order_release);
}
void no_bail_request_wipeout(float magnitude) noexcept {
    auto& wanted = wanted_wipeout();
    wanted.magnitude.store(magnitude, std::memory_order_relaxed);
    wanted.until.store(GetTickCount64() + 250, std::memory_order_release);
}
bool add_no_bail_step_listener(NoBailStepObserver listener) noexcept {
    if (!listener) return false;
    auto& slots = protection().listeners;
    for (auto& slot : slots)
        if (slot.load(std::memory_order_acquire) == listener) return true;
    for (auto& slot : slots) {
        NoBailStepObserver empty{};
        if (slot.compare_exchange_strong(empty, listener, std::memory_order_acq_rel)) return true;
    }
    return false;
}
void remove_no_bail_step_listener(NoBailStepObserver listener) noexcept {
    if (!listener) return;
    for (auto& slot : protection().listeners) {
        auto expected = listener;
        slot.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    }
}
void watch_physics_state(std::uintptr_t client, std::uintptr_t entity) noexcept {
    auto& w = state_watch();
    Owner owner;
    if (!protection().ready.load(std::memory_order_acquire) || !resolve(client, entity, owner)) return;
    if (w.selector.exchange(owner.selector, std::memory_order_acq_rel) != owner.selector)
        w.state.store(0, std::memory_order_release);
    w.causes.store(owner.causes, std::memory_order_release);
    w.until.store(GetTickCount64() + 500, std::memory_order_release);
}
PhysicsStateWatch watched_physics_state() noexcept {
    auto& w = state_watch();
    PhysicsStateWatch result;
    result.valid = protection().ready.load(std::memory_order_acquire) && w.selector.load(std::memory_order_acquire) &&
        GetTickCount64() < w.until.load(std::memory_order_acquire);
    result.state = w.state.load(std::memory_order_acquire);
    result.previous = w.previous.load(std::memory_order_relaxed);
    static const double frequency = [] { LARGE_INTEGER value{}; QueryPerformanceFrequency(&value); return static_cast<double>(value.QuadPart); }();
    result.previous_seconds = static_cast<float>(static_cast<double>(w.previous_ticks.load(std::memory_order_relaxed)) / frequency);
    result.changes = w.changes.load(std::memory_order_relaxed);
    result.wipeouts = w.wipeouts.load(std::memory_order_relaxed);
    return result;
}
void clear_no_bail_flight() noexcept {
    auto& p = protection();
    AcquireSRWLockExclusive(&p.lock);
    p.lease.flight_until = 0;
    ReleaseSRWLockExclusive(&p.lock);
}
}

namespace dingosdk {
bool add_no_bail_native_observer(NoBailNativeObserver observer) noexcept {
    LastError error;if(!observer)return false;
    auto expected=static_cast<NoBailNativeObserver>(nullptr);
    return protection().native_observer.compare_exchange_strong(expected,observer,std::memory_order_acq_rel)||expected==observer;
}
void remove_no_bail_native_observer(NoBailNativeObserver observer) noexcept {
    LastError error;if(observer)(void)protection().native_observer.compare_exchange_strong(observer,nullptr,std::memory_order_acq_rel);
}
}
