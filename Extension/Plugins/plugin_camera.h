#pragma once
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/client_source_spawn.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/UI/game_view.h"
#include <array>
#include <atomic>
#include <cmath>
#include <numbers>
#include <optional>

// World positions on screen, through the camera as it is now (as the nametags read it). The plugin host projects with it on the client thread (world_to_screen) and the
// overlay on the render thread (world shapes), each with the camera of its own moment.
namespace dingosdk::plugin_camera {
inline std::atomic<std::uintptr_t>& image_base() {
    static std::atomic<std::uintptr_t> value{};
    return value;
}

// The latest game view, with the camera's own matrix and field of view read again when the
// camera is one this build's types say it is: the game moves its camera after the update the view
// was taken in.
inline std::optional<GameView> camera_now() {
    auto view = latest_game_view();
    if (!view) return std::nullopt;
    const auto base = image_base().load(std::memory_order_relaxed);
    if (!base || !view->camera || (view->camera & 7)) return view;
    std::uintptr_t vtable{};
    if (!memory::peek(view->camera, vtable) ||
        (vtable != base + addr::engine::camera_vtable && vtable != base + addr::client_source_spawn::free_camera_vtable))
        return view;
    std::array<float, 16> world{};
    float fov{};
    if (!memory::peek(view->camera + 0x50, world) || !memory::peek(view->camera + 0xac, fov)) return view;
    if (!std::isfinite(fov) || fov <= 1 || fov >= 175) return view;
    for (const auto value : world)
        if (!std::isfinite(value) || std::abs(value) > 1e7f) return view;
    view->world = world;
    view->vertical_fov = fov;
    return view;
}

struct Projector {
    std::array<float, 16> m{};
    float focal{}, width{}, height{};
    // x, y in pixels and the depth in front of the camera, or empty behind it (or too close).
    std::optional<std::array<float, 3>> project(const std::array<float, 3>& p) const {
        const std::array<float, 3> d{p[0] - m[12], p[1] - m[13], p[2] - m[14]};
        const auto dot = [&](int row) { return d[0] * m[row] + d[1] * m[row + 1] + d[2] * m[row + 2]; };
        const float depth = -dot(8);
        if (depth < 0.05f) return std::nullopt;
        return std::array<float, 3>{width * 0.5f + dot(0) * focal / depth, height * 0.5f - dot(4) * focal / depth, depth};
    }
};
// The camera now, for a screen of this size; empty without a camera.
inline std::optional<Projector> projector(float width, float height) {
    const auto view = camera_now();
    if (!view || !(view->vertical_fov > 1 && view->vertical_fov < 175) || width <= 0 || height <= 0) return std::nullopt;
    Projector p;
    p.m = view->world;
    p.width = width;
    p.height = height;
    p.focal = height / (2.0f * std::tan(view->vertical_fov * std::numbers::pi_v<float> / 360.0f));
    return p;
}
}
