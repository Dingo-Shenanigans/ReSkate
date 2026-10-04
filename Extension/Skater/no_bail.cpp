#include "no_bail.h"
#include "hall_of_meat.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Extension/UI/Overlay/overlay.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <intrin.h>

namespace dingosdk {
namespace {
using namespace addr::no_bail;
// Native cause recorder: RCX=collector, EDX=reason, XMM2=magnitude.
using RecordCause = void (*)(std::uintptr_t, std::int32_t, float);
using ChooseState = std::uint32_t (*)(std::uintptr_t, std::uint32_t);
using SkeletonResponse = void (*)(std::uintptr_t, float, bool);
using PublishAnimation = void (*)(std::uintptr_t);
using ResetCauses = void (*)(std::uintptr_t);
constexpr std::uintptr_t highest = memory::highest_user_address;
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
    std::uint64_t manual_until{}, flight_until{}, refreshed_at{};
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
    BoardLock board;
    RecordCause cause_original{};
    ChooseState choose_original{};
    SkeletonResponse skeleton_original{};
    PublishAnimation publish_original{};
    ResetCauses reset_causes{};
    std::atomic<bool> ready{};
    SRWLOCK lock = SRWLOCK_INIT;
    Lease lease;
};
Protection& protection() { static auto* value = new Protection; return *value; }

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
// The hooks also serve Hall of Meat, which observes the local skater with or
// without an active protection lease: same ownership validation as
// protected_owner, minus the lease's expiry requirement. Mid-ragdoll the
// client's state can leave the resolver's on-board pair, so a lease refreshed
// this tick still owns the object it names -- a respawn or level change makes
// new objects, whose addresses no longer match the retained owner anyway.
bool observed_owner(std::uintptr_t object, std::uintptr_t Owner::* member, Owner* owner = nullptr) noexcept {
    auto& p = protection();
    if (!p.ready.load(std::memory_order_acquire)) return false;
    AcquireSRWLockShared(&p.lock);
    const auto lease = p.lease;
    ReleaseSRWLockShared(&p.lock);
    if (object != lease.owner.*member || GetTickCount64() - lease.refreshed_at > 500) return false;
    Owner current;
    if (!resolve(lease.owner.client, lease.owner.entity, current) || current != lease.owner) {
        if (owner) *owner = lease.owner;
        return true;
    }
    if (owner) *owner = current;
    return true;
}
bool protected_owner(std::uintptr_t object, std::uintptr_t Owner::* member, Owner* owner = nullptr) noexcept {
    auto& p = protection();
    if (!p.ready.load(std::memory_order_acquire)) return false;
    // The lock only copies SDK data, and is never held across native code or
    // memory reads. Hooks do not contend for the debug/camera action lock.
    AcquireSRWLockShared(&p.lock);
    const auto lease = p.lease;
    ReleaseSRWLockShared(&p.lock);
    if (!lease.active(GetTickCount64()) || object != lease.owner.*member) return false;
    Owner current;
    if (!resolve(lease.owner.client, lease.owner.entity, current) || current != lease.owner) return false;
    if (owner) *owner = current;
    return true;
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
    // Hall of Meat keeps the local skater's impact feed. The collector holds
    // every skater's causes, so ownership decides which are ours to show.
    {
        LastError error;
        if (observed_owner(causes, &Owner::causes)) hall_of_meat::observe_cause(reason, magnitude);
    }
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
    ReleaseSRWLockShared(&p.lock);
    if (GetTickCount64() >= board.until || selector != board.owner.selector) return;
    Owner current_owner;
    if (!resolve(board.owner.client, board.owner.entity, current_owner) || current_owner != board.owner) return;
    (void)cancel_request(current_owner.context, animation_request_offset, mount_request_mask);
}
std::uint32_t choose_state(std::uintptr_t selector, std::uint32_t current) {
    {
        LastError error;
        hold_off_board(selector, current);
    }
    // Clear shared requests before selection so native ground/air/walking
    // transitions can still run. Some contact tests return Wipeout directly;
    // retain the current state only for that result, never ordinary Offboard.
    const bool filtered = filter_requests(selector, &Owner::selector);
    const auto next = protection().choose_original(selector, current);
    LastError error;
    return filtered && next == wipeout_physics_state && protected_owner(selector, &Owner::selector) ? current : next;
}
// The wipeout step: hand Hall of Meat the causes recorded around it and the
// per-bone contact report (contacts+0xfb0..0xfbd, fourteen configured body
// bones; the sensitive-body flag at +0xfb8 sits among them), then announce
// it. The overlay notice is safe from any game thread.
void note_local_wipeout(const Owner& owner) noexcept {
    std::array<std::uint8_t, hall_of_meat::bone_contact_count> bones{};
    constexpr std::uintptr_t first_bone_flag = body_contact_output_offset - 8;
    const auto contacts = pointer(pointer(owner.core, 0x3b8), 0x30);
    if (contacts)
        for (std::size_t index = 0; index < bones.size(); ++index) {
            std::uint8_t flag{};
            if (read(contacts + first_bone_flag + index, flag) && flag <= 1) bones[index] = flag;
        }
    hall_of_meat::Bail bail;
    if (!hall_of_meat::observe_wipeout(bones, &bail)) return; // the ragdoll's follow-up step
    logging::log(logging::Level::info, logging::Channel::skater,
        "Hall of Meat: wipeout recorded ({} cause(s), impact {:.1f}, body contact {})",
        bail.impact_count, bail.magnitude, bail.body_contact ? "yes" : "no");
    char text[96];
    std::snprintf(text, sizeof(text), "Wipeout at impact %.1f, %u bone contact(s)",
        bail.magnitude, static_cast<unsigned>(std::count(bones.begin(), bones.end(), 1)));
    overlay::notify(overlay::NoticeLevel::info, "Hall of Meat", text);
}
void skeleton_response(std::uintptr_t rig, float seconds, bool wipeout) {
    // The state post-update can raise another request after the selector ran.
    // Filter at this consumer, then let native constraints and recovery run.
    if (filter_requests(rig, &Owner::rig)) wipeout = false;
    // A filtered wipeout never happened, so it feeds no injuries.
    if (wipeout) {
        Owner owner;
        if (observed_owner(rig, &Owner::rig, &owner)) {
            LastError error;
            note_local_wipeout(owner);
        }
    }
    protection().skeleton_original(rig, seconds, wipeout);
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
    protection().publish_original(core);
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
        next.refreshed_at = now;
    }
    AcquireSRWLockExclusive(&p.lock);
    p.lease = available ? next : Lease{};
    ReleaseSRWLockExclusive(&p.lock);
    return available;
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
void clear_no_bail_flight() noexcept {
    auto& p = protection();
    AcquireSRWLockExclusive(&p.lock);
    p.lease.flight_until = 0;
    ReleaseSRWLockExclusive(&p.lock);
}
}
