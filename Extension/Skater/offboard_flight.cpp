#include "offboard_flight.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Game/Build/20260929/offboard_flight.h"
#include <algorithm>
#include <cmath>

namespace dingosdk {
namespace {
using namespace addr::offboard_flight;
struct FlightLastError {
    DWORD value = GetLastError();
    ~FlightLastError() { SetLastError(value); }
};
bool object(std::uintptr_t p) noexcept {
    return p >= 0x10000 && p <= memory::highest_user_address - supported_build::game_image_size;
}
std::uintptr_t link(std::uintptr_t p, std::uintptr_t offset = 0) noexcept {
    std::uintptr_t value{};
    return object(p) && memory::read(p + offset, value) ? value : 0;
}
}
bool offboard_flight_compatible(std::uintptr_t base) noexcept {
    FlightLastError error;
    if (!object(base)) return false;
    for (const auto& contract : offboard_owner_contracts) {
        std::array<unsigned char, 32> bytes{};
        if (!memory::read(base + contract.rva, bytes) || bytes != contract.bytes) return false;
    }
    for (const auto& state : offboard_flight_states) {
        std::array<unsigned char, 32> bytes{};
        if (link(base + state.vtable_rva, 0x18) != base + state.update_rva ||
            !memory::read(base + state.velocity_write.rva, bytes) || bytes != state.velocity_write.bytes) return false;
    }
    for (const auto& state : offboard_ragdoll_states)
        if (link(base + state[1], 0x18) != base + state[2]) return false;
    std::array<unsigned char, 32> ragdoll_bytes{};
    if (!memory::read(base + ragdoll_velocity_write.rva, ragdoll_bytes) ||
        ragdoll_bytes != ragdoll_velocity_write.bytes) return false;
    return true;
}
bool scale_offboard_up_velocity(std::uintptr_t base, std::uintptr_t core, std::uintptr_t context,
    std::uintptr_t rig, float factor, float* before) noexcept {
    FlightLastError error;
    if (before) *before = 0;
    if (!object(base) || !object(context) || !object(rig) || !std::isfinite(factor) || factor <= 0 || factor > 20 ||
        link(core) != base + addr::no_bail::bail_core_vtable || link(core, 0x3c0) != context || link(core, 0x438) != rig) return false;
    const auto parent = link(core, 0x3b0);
    if (link(parent) != base + offboard_flight_vtable || link(parent, 8) != context) return false;
    const auto active = link(parent, 0x48);
    for (const auto& state : offboard_flight_states) {
        if (active != parent + state.offset) continue;
        std::array<float, 3> velocity{};
        if (link(active) != base + state.vtable_rva || !memory::read(active + 0x10, velocity)) return false;
        if (before) *before = velocity[1];
        if (!std::isfinite(velocity[1]) || velocity[1] < 0.5f) return false; // not a jump (yet)
        velocity[1] *= factor;
        return sync_offboard_flight_velocity(base, core, context, rig, velocity);
    }
    return false;
}
bool ensure_offboard_up_velocity(std::uintptr_t base, std::uintptr_t core, std::uintptr_t context,
    std::uintptr_t rig, float minimum_up_speed) noexcept {
    FlightLastError error;
    if (!object(base) || !object(context) || !object(rig) || !std::isfinite(minimum_up_speed) ||
        minimum_up_speed <= 0 || minimum_up_speed > 100000 ||
        link(core) != base + addr::no_bail::bail_core_vtable || link(core, 0x3c0) != context || link(core, 0x438) != rig)
        return false;
    const auto parent = link(core, 0x3b0);
    if (link(parent) != base + offboard_flight_vtable || link(parent, 8) != context ||
        link(parent, 0x10) != link(core, 0x430) || link(parent, 0x18) != rig ||
        link(rig) != context || link(rig, 0x4630) != core) return false;
    const auto active = link(parent, 0x48);
    for (const auto& state : offboard_flight_states) {
        if (active != parent + state.offset) continue;
        std::array<float, 3> velocity{};
        if (link(active) != base + state.vtable_rva || !memory::read(active + 0x10, velocity)) return false;
        velocity[1] = std::max(velocity[1], minimum_up_speed);
        return sync_offboard_flight_velocity(base, core, context, rig, velocity);
    }
    for (const auto& state : offboard_ragdoll_states) {
        if (active != parent + state[0]) continue;
        if (link(active) != base + state[1]) return false;
        float vertical{};
        const auto address = active + 0x14;
        if (!memory::read(address, vertical) || !std::isfinite(vertical) || std::abs(vertical) > 100000)
            return false;
        // Replace a fall with the positive launch speed, rather than adding
        // an impulse to a cached negative velocity. Preserve horizontal motion.
        const float launched = std::max(vertical, minimum_up_speed);
        MEMORY_BASIC_INFORMATION page{};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &page, sizeof(page)) ||
            page.State != MEM_COMMIT || (page.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(page.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)))
            return false;
        SIZE_T written{};
        return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), &launched,
            sizeof(launched), &written) && written == sizeof(launched) &&
            memory::read(address, vertical) && vertical == launched;
    }
    // Scripted animation/trajectory states have different motion rules.
    return true;
}
bool sync_offboard_flight_velocity(std::uintptr_t base, std::uintptr_t core,
    std::uintptr_t context, std::uintptr_t rig, const std::array<float, 3>& velocity) noexcept {
    FlightLastError error;
    if (!object(base) || !object(context) || !object(rig) || link(core) != base + addr::no_bail::bail_core_vtable ||
        link(core, 0x3c0) != context || link(core, 0x438) != rig ||
        link(rig) != context || link(rig, 0x4630) != core) return false;
    const auto parent = link(core, 0x3b0);
    const auto board = link(core, 0x430);
    // The native constructor receives (context, board wrapper, rig wrapper).
    // Parent+10 is the board wrapper, not a backlink to the physics core.
    if (!object(board) || link(parent) != base + offboard_flight_vtable || link(parent, 8) != context ||
        link(parent, 0x10) != board || link(parent, 0x18) != rig) return false;
    const auto active = link(parent, 0x48);
    for (const auto& state : offboard_flight_states) {
        if (active != parent + state.offset) continue;
        if (link(active) != base + state.vtable_rva) return false;
        std::array<float, 3> previous{};
        if (!memory::read(active + 0x10, previous)) return false;
        for (std::size_t i = 0; i < velocity.size(); ++i)
            if (!std::isfinite(velocity[i]) || std::abs(velocity[i]) > 6000 ||
                !std::isfinite(previous[i]) || std::abs(previous[i]) > 100000) return false;
        MEMORY_BASIC_INFORMATION page{};
        const auto address = active + 0x10;
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &page, sizeof(page)) ||
            page.State != MEM_COMMIT || (page.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(page.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)) ||
            page.RegionSize < sizeof(velocity) ||
            address - reinterpret_cast<std::uintptr_t>(page.BaseAddress) > page.RegionSize - sizeof(velocity)) return false;
        // The caller is inside this substate's native update, after gravity was
        // integrated. Match the flight target's velocity on every call, including
        // duplicate motion calls in one step. No saved pointer or exit-time write.
        SIZE_T written{};
        return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), &velocity,
            sizeof(velocity), &written) && written == sizeof(velocity) &&
            memory::read(address, previous) && previous == velocity;
    }
    // Animation/trajectory/ragdoll states have their own motion rules. They do
    // not use these accumulators; retain their existing flight target behavior.
    return true;
}
}
