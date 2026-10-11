#pragma once
#include <cstdint>
#include <array>
#include <string>
#include "Extension/UI/Overlay/overlay.h"

namespace dingosdk {
// Binds the native camera functions the debug controls use. Installs no hooks.
void initialize_client_source_spawn(std::uintptr_t base);
// Installs the exact camera-reset handler guard; forwarding is unchanged until a Noclip request is queued.
bool start_client_noclip_velocity(std::uintptr_t base) noexcept;
// Trainer: at the local skater's next physics update, multiply its upward velocity by
// `factor` if it is off the board and rising (a hippy jump that has just started). One request
// at a time; it expires after 150 ms.
bool queue_jump_scale(std::uintptr_t client, std::uintptr_t entity, float factor) noexcept;
// What the last request did: 0 pending or none, 2 scaled, -1 the skater was not rising yet,
// -2 nothing to scale (on the board, or a state that keeps no velocity), -3 it failed.
struct JumpScaleResult {
    int outcome{};
    float up_speed{}; // before scaling
};
JumpScaleResult take_jump_scale_result() noexcept;
// Trainer: the local skater's push speed as a multiple of the game's own (1 leaves the skater
// alone); `stock` is the game's top pushing speed in m/s. `cruise` above 0 is auto push: a
// rolling skater that is not braking gains speed up to it (m/s). Publish every client tick: it
// expires after 500 ms.
void set_push_speed(std::uintptr_t client, std::uintptr_t entity, float factor, float stock, float cruise) noexcept;
// Engine-thread-only interactive controls. Presentation callbacks only queue requests.
overlay::DebugModel on_client_debug_tick(std::uintptr_t base, std::uintptr_t client,
    bool can_control, bool camera_phase_observed, const overlay::DebugRequest* request = nullptr,
    const overlay::FlightInput* flight_input = nullptr);
bool restore_client_debug(std::uintptr_t base, std::uintptr_t client, bool camera_phase_observed);
bool client_free_camera_active() noexcept;
// Native camera lease for the party's Spectate action. Null restores only the
// camera acquired by this function; it never changes another debug camera. A positive
// `fov` is applied to the spectate camera while it is held; its own comes back after.
bool update_party_camera(std::uintptr_t base, std::uintptr_t client, bool ready, bool phase,
    const std::array<float, 16>* transform, std::string& detail, float fov = 0) noexcept;
bool read_local_camera_transform(std::uintptr_t base, std::uintptr_t client, std::array<float, 16>& transform) noexcept;
// Publishes the local view's camera (Engine/Game/UI/game_view.h) for things drawn over the
// world. Client thread, every tick while something needs it.
bool publish_local_camera_view(std::uintptr_t base, std::uintptr_t client) noexcept;
}
