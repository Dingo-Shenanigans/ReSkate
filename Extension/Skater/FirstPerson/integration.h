#pragma once
#include "Engine/Game/Skater/first_person_spring.h"
#include "Extension/UI/Overlay/overlay.h"
#include <optional>
#include <span>
#include "controls_context.h"
namespace dingosdk::first_person_integration {
using QueueCameraProfile = bool (*)(std::span<const overlay::DebugRequest>, char*, std::size_t);
void set_callbacks(const overlay::CallbacksV3& callbacks, QueueCameraProfile profile_queue) noexcept;
// Called only after ReSkate's existing overlay initializer reports ready.
void start_when_ready() noexcept;
bool ready() noexcept;
bool failed() noexcept;
// Native input leases must be refreshed each frame even when the camera is off.
void observe(const first_person::Settings& settings, float fov, bool active,
    bool available, bool requested, std::optional<bool> on_foot, std::uintptr_t client, const ControlsContext& context) noexcept;
// Existing native party/throwdown camera calls this on its engine-thread tick.
bool spectator_camera(std::uintptr_t base,std::uintptr_t client,std::uint64_t id,std::uint64_t epoch,
    std::uintptr_t entity,first_person::Matrix& pose,float& fov,bool ready) noexcept;
bool toggle_spectator() noexcept;
void observe_bindings(const ControllerBindingsModel& bindings) noexcept;
}
