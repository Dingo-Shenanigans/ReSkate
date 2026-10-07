#pragma once
#include "Extension/Profile/runtime_internal.h"
#include "Extension/World/local_world_controls.h"

namespace dingosdk::profile_runtime {
struct GraphicsLease { bool owned{}; std::uint8_t original{}, applied{}; bool original_flag{}; };

struct GraphicsFilmic {
    std::uintptr_t component{}, data{}, parent{};
    std::array<GraphicsLease, 2> fields{}; // vignette, chromatic aberration
};

struct GraphicsRuntime {
    using Construct = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t);
    using Destroy = std::uintptr_t (*)(std::uintptr_t, unsigned);
    using Property = void (*)(std::uintptr_t, const EnvironmentChange*);
    using Lookup = std::uintptr_t (*)(std::uintptr_t, const void*);
    using VideoSetup = void (*)(const std::uintptr_t*, std::uintptr_t, std::uintptr_t);
    Construct construct{}; Destroy destroy{}; Property property{}; Lookup lookup{};
    VideoSetup video_setup{};
    std::atomic<std::uint32_t> disabled_video_effects{};
    std::atomic<std::uint64_t> video_observed_at{};
    std::recursive_mutex mutex;
    std::atomic<bool> active{};
    GraphicsControlsModel model;
    std::map<std::uintptr_t, GraphicsFilmic> nodes;
    std::uintptr_t settings{};
    std::array<GraphicsLease, 2> settings_fields{};
    DWORD thread{};
    std::uint64_t last_update{};
    // VR turns vignette and chromatic aberration off while it runs, without
    // touching the saved choices (they come back when VR stops).
    bool vr_override{};
};

GraphicsRuntime& graphics_runtime();

std::uint32_t graphics_video_mask(const GraphicsControls& choices);

// The choices in effect: the saved ones, with the VR override applied.
GraphicsControls graphics_effective_choices(const GraphicsRuntime& r);
// Engine thread: VR running (true) or not; re-applies only on a change.
void set_graphics_vr_override(bool active) noexcept;

void graphics_video_setup(const std::uintptr_t* params, std::uintptr_t context, std::uintptr_t output);

bool graphics_filmic_identity(const GraphicsFilmic& n);

std::uintptr_t graphics_filmic_construct(std::uintptr_t factory, std::uintptr_t info);

std::uintptr_t graphics_filmic_destroy(std::uintptr_t component, unsigned flags);

bool graphics_write_byte(std::uintptr_t address, std::uint8_t value);

bool graphics_settings_identity(std::uintptr_t object);

bool graphics_settings_update();

bool graphics_filmic_update(GraphicsFilmic& n, unsigned field);

void update_graphics_controls();

void start_graphics_controls() noexcept;
}
