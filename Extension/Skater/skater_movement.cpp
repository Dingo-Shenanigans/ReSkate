#include "skater_movement.h"
#include <Windows.h>
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Game/Build/20260929/offboard_flight.h"
#include "Engine/Game/Build/20260929/skater_movement.h"
#include "Engine/Game/Build/20260929/skater_state.h"
#include "Extension/Multiplayer/Hud/game_ui_state.h"
#include "Extension/Objects/ParkEditor/park_editor_runtime.h"
#include "Extension/Skater/client_source_spawn.h"
#include "Extension/Skater/no_bail.h"
#include "Extension/UI/Overlay/overlay.h"
#include <algorithm>
#include <atomic>
#include <cmath>

namespace dingosdk::skater_movement {
namespace {
std::atomic<std::uintptr_t> client_now{};
bool sane(const std::array<float, 3>& v) {
    return std::all_of(v.begin(), v.end(), [](float value) { return std::isfinite(value) && std::abs(value) <= 100; });
}
}
void set_client(std::uintptr_t client) noexcept { client_now.store(client, std::memory_order_relaxed); }
std::optional<plugins::SkaterMovement> read(std::uintptr_t base) {
    namespace at = addr::skater_movement;
    namespace flight = addr::offboard_flight;
    namespace sk = addr::skater_state;
    NoBailSkater player;
    if (!base || !no_bail_skater(player))
        return std::nullopt;
    plugins::SkaterMovement result;
    if (!memory::peek(player.context + at::context_velocity, result.velocity) || !sane(result.velocity))
        return std::nullopt;
    std::uintptr_t trick{}, offboard{}, parent{}, type{}, context{}, board{}, rig{}, active{};
    std::uint8_t ragdoll{}, air{};
    std::uint32_t physics_state{};
    if (!memory::peek(player.context + sk::physics_state_offset, physics_state))
        return std::nullopt;
    result.physics_state = physics_state;
    result.on_foot = physics_state == addr::no_bail::offboard_physics_state;
    // Grounded on foot: not ragdolled or in the air, and the off-board machine is the skater's own and
    // walking.
    const bool offboard_ready =
        memory::peek(player.core + sk::trick_state_offset, trick) && memory::peek(trick + sk::offboard_state_offset, offboard) && offboard;
    result.grounded = result.on_foot && offboard_ready && memory::peek(offboard + sk::ragdoll_offset, ragdoll) && !ragdoll &&
                      memory::peek(offboard + sk::in_the_air_offset, air) && !air &&
                      memory::peek(player.core + at::skater_parent, parent) && memory::peek(parent, type) &&
                      type == player.base + flight::offboard_flight_vtable && memory::peek(parent + at::state_context, context) &&
                      context == player.context && memory::peek(parent + at::state_board, board) &&
                      memory::peek(player.core + at::skater_board, type) && board == type &&
                      memory::peek(parent + at::state_rig, rig) && rig == player.rig &&
                      memory::peek(parent + at::active_state, active) && active == parent + flight::offboard_flight_states[0].offset &&
                      memory::peek(active, type) && type == player.base + flight::offboard_flight_states[0].vtable_rva &&
                      memory::peek(active + at::walking_velocity, result.walking_velocity);
    if (!sane(result.walking_velocity)) {
        result.grounded = false;
        result.walking_velocity = {};
    }
    DWORD foreground_process{};
    GetWindowThreadProcessId(GetForegroundWindow(), &foreground_process);
    result.focused = foreground_process == GetCurrentProcessId() && !client_free_camera_active() &&
                     !multiplayer::sample_game_ui_state(base).in_menu && !profile_runtime::park_editor_owns_placements();
    ControllerInput pad;
    DingoSDKOverlayReadControllerInput(&pad);
    result.device = pad.device;
    if (result.focused) {
        overlay::FlightInput movement;
        DingoSDKOverlayReadFlightInput(&movement, true, true);
        std::array<float, 16> camera{};
        const float amount = (std::min)(1.f, std::hypot(movement.right, movement.forward));
        if (movement.active && amount > .05f && read_local_camera_transform(base, client_now.load(std::memory_order_relaxed), camera)) {
            const std::array<float, 3> direction{camera[0] * movement.right - camera[8] * movement.forward, 0,
                                                 camera[2] * movement.right - camera[10] * movement.forward};
            const float length = std::hypot(direction[0], direction[2]);
            if (length > .01f)
                result.stick = {direction[0] * amount / length, 0, direction[2] * amount / length};
        }
    }
    return result;
}
}
