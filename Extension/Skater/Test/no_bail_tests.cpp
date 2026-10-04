// Exercise the actual hook callbacks with synthetic local ownership and native relays.
// No game files, injection, or instruction patching are needed by this test.
#include "../no_bail.cpp"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace dingosdk {
HookStatus WINAPI hook_prepare(void*, void*, void**) { return HookUnsupportedFunction; }
HookStatus WINAPI hook_enable(void*) { return HookUnsupportedFunction; }
HookStatus WINAPI hook_remove(void*) { return HookNotFound; }
}

namespace {
using namespace dingosdk;
struct Buffer {
    std::vector<unsigned char> bytes;
    explicit Buffer(std::size_t size) : bytes(size) {}
    std::uintptr_t address() { return reinterpret_cast<std::uintptr_t>(bytes.data()); }
    template<class T> void set(std::size_t offset, T value) { std::memcpy(bytes.data() + offset, &value, sizeof(value)); }
    template<class T> T get(std::size_t offset) { T value{}; std::memcpy(&value, bytes.data() + offset, sizeof(value)); return value; }
};
unsigned assertions{}, recovery_calls{}, rig_calls{}, publish_calls{}, resets{};
unsigned change_calls{}, import_calls{}, on_board_calls{};
std::uint32_t changed_state{};
std::uint8_t imported_board_seen{};
std::uint32_t next_state{};
std::uint8_t rig_wipeout_seen{}, rig_other_seen{};
LONG rig_flags_seen{};
std::uintptr_t rig_motion_seen{};
bool skeleton_wipeout_seen{};
void check(bool condition, const char* message) {
    ++assertions;
    if (!condition) throw std::runtime_error(message);
}
bool native_recovery(std::uintptr_t) { ++recovery_calls; return true; }
std::uint32_t native_choose(std::uintptr_t, std::uint32_t) { return next_state; }
void native_reset(std::uintptr_t causes) {
    ++resets;
    *reinterpret_cast<std::uint32_t*>(causes + 0xe0) = 0;
}
void native_rig(std::uintptr_t, std::uintptr_t animation, std::uintptr_t, std::uintptr_t motion) {
    ++rig_calls;
    rig_wipeout_seen = *reinterpret_cast<std::uint8_t*>(animation + rig_wipeout_offset);
    rig_other_seen = *reinterpret_cast<std::uint8_t*>(animation + 0x52);
    rig_flags_seen = *reinterpret_cast<LONG*>(animation + rig_failure_flags_offset);
    rig_motion_seen = motion;
}
void native_publish(std::uintptr_t core) {
    ++publish_calls;
    const auto output = pointer(pointer(core, 0x3b8), 0x38);
    for (auto offset : animation_failure_output_offsets)
        *reinterpret_cast<std::uint8_t*>(output + offset) = 1;
}
void native_skeleton(std::uintptr_t, float, bool wipeout) { skeleton_wipeout_seen = wipeout; }
void native_change(std::uintptr_t core, std::uint32_t requested) {
    ++change_calls;
    changed_state = requested;
    const auto context = pointer(core, 0x3c0);
    *reinterpret_cast<std::uint32_t*>(context + physics_state_offset) = requested;
    *reinterpret_cast<std::uint32_t*>(context + 0x1418) = (requested / 100) * 100;
}
bool native_on_board(std::uintptr_t) { ++on_board_calls; return false; }
void native_import(std::uintptr_t core) {
    ++import_calls;
    const auto animation = pointer(core, rig_animation_offset);
    const auto context = pointer(core, 0x3c0);
    imported_board_seen = *reinterpret_cast<std::uint8_t*>(animation + animation_on_board_offset);
    // Model feedback changing while native import runs. The callback must
    // restore the invariant after importing, and preserve unrelated flags.
    *reinterpret_cast<std::uint8_t*>(animation + animation_on_board_offset) = 0;
    *reinterpret_cast<LONG*>(context + impact_request_offset) &= ~animation_on_board_mask;
    *reinterpret_cast<LONG*>(context + animation_request_offset) |= dismount_request_mask | runout_feedback_mask;
}

struct Fixture {
    void* image{};
    Buffer client{0xd0}, engine_context{0x200}, manager{0x4d8}, players{8}, player{0xc0}, entity{0x800};
    Buffer handle{8}, collection{8}, component{0x78}, core{0x448}, context{0x1900}, selector{0x50};
    Buffer causes{0xe8}, rig{0x4640}, animation{0x450}, bundle{0x48}, output{0xa8};
    Fixture() {
        image = VirtualAlloc(nullptr, supported_build::game_image_size, MEM_RESERVE, PAGE_READWRITE);
        if (!image) throw std::runtime_error("Reserve synthetic image failed");
        const auto base = reinterpret_cast<std::uintptr_t>(image);
        const auto offset_address = base + addr::engine::context_player_manager_offset;
        if (!VirtualAlloc(reinterpret_cast<void*>(offset_address & ~std::uintptr_t{0xfff}), 0x1000,
                          MEM_COMMIT, PAGE_READWRITE)) throw std::runtime_error("Commit synthetic global failed");
        *reinterpret_cast<unsigned*>(offset_address) = 0x100;
        client.set(0, base + addr::engine::client_vtable);
        client.set(8, engine_context.address());
        client.set<unsigned>(0xc4, 13);
        engine_context.set(0x100, manager.address());
        manager.set(0, base + addr::engine::local_player_manager_vtable);
        manager.set(0x4c8, players.address());
        manager.set(0x4d0, players.address() + 8);
        players.set(0, player.address());
        player.set(0, base + addr::engine::local_player_vtable);
        player.set<std::uint8_t>(0x45, 1);
        player.set(0x78, engine_context.address());
        player.set(0xb0, handle.address());
        player.set(0xb8, entity.address());
        entity.set(0, base + addr::engine::skater_entity_vtable);
        entity.set(0x20, engine_context.address());
        entity.set(0x70, collection.address());
        entity.set(0xf8, player.address());
        entity.set(0x628, component.address());
        handle.set(0, entity.address() + 8);
        collection.set(0, entity.address());
        component.set(0, base + addr::engine::skater_component_vtable);
        component.set(0x18, collection.address());
        component.set(0x70, core.address());
        core.set(0, base + bail_core_vtable);
        core.set(0x3b8, bundle.address());
        core.set(0x3c0, context.address());
        core.set(rig_animation_offset, animation.address());
        core.set(0x428, causes.address());
        core.set(0x438, rig.address());
        core.set(0x440, selector.address());
        selector.set(8, context.address());
        causes.set(0x20, context.address());
        rig.set(0, context.address());
        rig.set(0x4630, core.address());
        bundle.set(0x38, output.address());
        auto& p = protection();
        p.base = base;
        p.recovery_original = native_recovery;
        p.rig_original = native_rig;
        p.publish_original = native_publish;
        p.choose_original = native_choose;
        p.skeleton_original = native_skeleton;
        p.reset_causes = native_reset;
        p.change_original = native_change;
        p.import_original = native_import;
        p.on_board_original = native_on_board;
        p.ready.store(true);
        state(100);
    }
    ~Fixture() {
        clear_no_bail();
        protection().ready.store(false);
        VirtualFree(image, 0, MEM_RELEASE);
    }
    void state(std::uint32_t value) { context.set(physics_state_offset, value); }
    void manual(bool enabled = true) {
        check(update_no_bail(client.address(), entity.address(), enabled, false, 0), "Local fixture must resolve");
    }
    void failures() {
        animation.set<std::uint8_t>(rig_wipeout_offset, 1);
        animation.set<std::uint8_t>(0x52, 0x5a);
        animation.set<LONG>(rig_failure_flags_offset, 0x31);
        context.set<LONG>(impact_request_offset, impact_request_mask | 0x400);
        context.set<LONG>(animation_request_offset, runout_feedback_mask | animation_request_mask | 0x180);
        causes.set<std::uint32_t>(0xe0, 7);
        output.set<std::uint8_t>(0x89, 0x5a);
    }
    void update_rig(std::uintptr_t physics_context = 0) {
        rig_update(rig.address(), animation.address(), physics_context ? physics_context : context.address(), 0x123456);
    }
};
}

int main() {
    try {
        Fixture f;
        f.manual(false);
        f.failures();
        check(recovery_predicate(f.core.address()), "Disabled mode must forward the recovery predicate");
        f.update_rig();
        check(rig_wipeout_seen == 1 && rig_flags_seen == 0x31, "Disabled mode must preserve rig failure inputs");
        publish_animation(f.core.address());
        check(f.output.get<std::uint8_t>(0x9e) == 1, "Disabled mode must preserve native runout publication");
        check(!physics_on_board(f.core.address()), "Disabled on-board query stays native");
        change_state(f.core.address(), offboard_physics_state);
        check(changed_state == offboard_physics_state, "Disabled mode forwards offboard state entry");
        f.state(100);
        import_animation(f.core.address());
        check(imported_board_seen == 0 && f.animation.get<std::uint8_t>(animation_on_board_offset) == 0,
            "Disabled mode must preserve offboard animation feedback");

        f.manual();
        for (const auto state : {100U, 200U, 201U, 400U, 406U, 600U, 603U}) {
            f.state(state);
            check(!recovery_predicate(f.core.address()), "Riding must reject independent runout decisions");
            f.failures();
            f.update_rig();
            check(rig_wipeout_seen == 0 && rig_flags_seen == 0x21, "Filter rig wipeout inputs before native consumption");
            check(rig_other_seen == 0x5a && rig_motion_seen == 0x123456, "Preserve other rig inputs and forward motion");
            publish_animation(f.core.address());
            for (auto offset : animation_failure_output_offsets)
                check(f.output.get<std::uint8_t>(offset) == 0, "Late native failure publication must be filtered");
            check(f.output.get<std::uint8_t>(0x89) == 0x5a, "Keep unrelated animation exports");
            check(f.context.get<LONG>(animation_request_offset) == mount_request_mask, "Strict mode blocks dismount while preserving mount");
            check(f.context.get<LONG>(impact_request_offset) == 0x400, "Keep unrelated physics flags");
            check(f.causes.get<std::uint32_t>(0xe0) == 0, "Clear inline causes before consumption");
        }
        // Automatic noclip protection remains the existing recovery-friendly
        // mode. Manual No Bail now intentionally blocks leaving the board.
        f.manual(false);
        check(update_no_bail(f.client.address(), f.entity.address(), false, true, GetTickCount64() + 500), "Flight owner resolves for recovery tests");
        for (const auto state : {300U, 500U, 504U, 700U}) {
            f.state(state);
            f.failures();
            check(recovery_predicate(f.core.address()), "Existing wipeouts and offboard recovery stay native");
            f.update_rig();
            check(rig_wipeout_seen == 1 && rig_flags_seen == 0x31, "Do not interrupt an existing offboard/wipeout rig");
            publish_animation(f.core.address());
            check(f.output.get<std::uint8_t>(0x9e) == 1, "Keep existing recovery exports");
        }
        f.state(100);
        f.manual();
        next_state = wipeout_physics_state;
        check(choose_state(f.selector.address(), 100) == 100, "Block direct physics wipeout before offboard remapping");
        next_state = offboard_physics_state;
        check(choose_state(f.selector.address(), 100) == 100, "Strict mode blocks voluntary dismount");
        skeleton_response(f.rig.address(), 0.016f, true);
        check(!skeleton_wipeout_seen, "Block post-update skeleton wipeout requests");

        f.failures();
        f.update_rig(f.context.address() + 8);
        check(rig_wipeout_seen == 1, "An unrelated animation context must forward unchanged");
        check(recovery_predicate(f.core.address() + 8), "Unrelated skaters must forward unchanged");
        f.player.set<std::uint8_t>(0x44, 1);
        check(recovery_predicate(f.core.address()), "Remote ownership must invalidate protection");
        f.player.set<std::uint8_t>(0x44, 0);
        f.component.set(0x70, f.core.address() + 8);
        check(recovery_predicate(f.core.address()), "Reused core addresses must not retain protection");
        f.component.set(0x70, f.core.address());

        f.manual(false);
        check(update_no_bail(f.client.address(), f.entity.address(), false, true, GetTickCount64() + 500), "Flight owner resolves");
        check(!recovery_predicate(f.core.address()), "Noclip also receives runout protection");
        clear_no_bail_flight();
        check(recovery_predicate(f.core.address()), "Stopping flight restores native behavior when manual mode is off");
        f.manual();
        check(update_no_bail(f.client.address(), f.entity.address(), true, true, GetTickCount64() + 500), "Combined owner resolves");
        clear_no_bail_flight();
        check(!recovery_predicate(f.core.address()), "Stopping flight preserves manual protection");
        protection().lease.manual_until = GetTickCount64();
        check(recovery_predicate(f.core.address()), "Expired leases restore native behavior");
        f.manual();
        clear_no_bail();
        check(recovery_predicate(f.core.address()), "Turning the toggle off restores native behavior");

        f.state(200);
        f.manual();
        check(physics_on_board(f.core.address()), "Strict mode reports PhysicsOnBoard to animation");
        for (const auto request : {0U, 300U, 400U, 406U, 500U, 501U, 504U, 600U, 603U, 700U, 702U, 0xffffffffU}) {
            next_state = request;
            check(choose_state(f.selector.address(), 200) == 200, "Every non-riding selector result must retain the board state");
            const auto before = change_calls;
            change_state(f.core.address(), request);
            check(change_calls == before && f.context.get<std::uint32_t>(physics_state_offset) == 200,
                "Reject direct state entry without exiting/re-entering the current riding state");
        }
        next_state = 100;
        check(choose_state(f.selector.address(), 200) == 100, "Strict mode permits native air-to-ground selection");
        change_state(f.core.address(), 100);
        check(changed_state == 100 && f.context.get<std::uint32_t>(0x1418) == 100,
            "Allowed native entry keeps state and category consistent");
        change_state(f.core.address(), 201);
        check(changed_state == 201, "Strict mode permits native ground-to-air entry");
        f.state(504); // A stale/current offboard value must not release the lease.
        f.manual();
        next_state = 504;
        check(choose_state(f.selector.address(), 504) == 201, "Lease renewal retains the last riding state after forced offboard feedback");
        change_state(f.core.address(), 504);
        check(changed_state == 201 && f.context.get<std::uint32_t>(physics_state_offset) == 201,
            "Unexpected offboard entry must use native entry to restore riding");
        f.failures();
        import_animation(f.core.address());
        check(imported_board_seen == 1 && f.animation.get<std::uint8_t>(animation_on_board_offset) == 1,
            "Enforce on-board animation feedback before and after import");
        check(f.context.get<LONG>(impact_request_offset) == (animation_on_board_mask | 0x400),
            "On-board physics feedback preserves unrelated flags");
        check(f.context.get<LONG>(animation_request_offset) == mount_request_mask,
            "Late native dismount/runout feedback cannot release the board");
        f.state(504);
        check(!recovery_predicate(f.core.address()) && physics_on_board(f.core.address()),
            "Unexpected offboard feedback cannot disable strict animation protection");
        f.manual(false);
        check(!physics_on_board(f.core.address()), "Toggle off restores the native on-board query");
        change_state(f.core.address(), 504);
        check(changed_state == 504, "Toggle off permits dismount immediately");
        check(update_no_bail(f.client.address(), f.entity.address(), false, true, GetTickCount64() + 500), "Offboard flight owner resolves");
        check(!physics_on_board(f.core.address()), "Flight-only protection must not force a walking skater onto the board");
        next_state = 603;
        check(choose_state(f.selector.address(), 504) == 603, "Flight-only protection must preserve plant state selection");
        f.state(100);
        f.manual();
        protection().lease.manual_until = GetTickCount64();
        check(!physics_on_board(f.core.address()), "Expired manual lease releases strict mode even when flight protection remains");
        next_state = 504;
        check(choose_state(f.selector.address(), 100) == 504, "Expired manual lease restores offboard selection");
        f.manual();
        f.player.set<std::uint8_t>(0x44, 1);
        check(!physics_on_board(f.core.address()), "Remote ownership must not receive a forced on-board answer");
        change_state(f.core.address(), 504);
        check(changed_state == 504, "Invalidated ownership must forward state entry");
        f.player.set<std::uint8_t>(0x44, 0);
        clear_no_bail();
        check(change_calls && import_calls && on_board_calls, "All strict-mode native relays were exercised");
        check(!clear_animation_failures(0) && !clear_failure_exports(0), "Invalid buffers must fail without an access violation");
        check(recovery_calls && rig_calls && publish_calls && resets, "All native relays were exercised");
        std::cout << "No Bail: " << assertions << " assertions passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
