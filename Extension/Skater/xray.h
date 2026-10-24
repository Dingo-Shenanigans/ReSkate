#pragma once
#include "xray_skeleton.h"
#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

// The local skater's x-ray, for plugins (reskate.skater.xray, the draw handler's d:skeleton): skate.'s
// own skeleton mesh (xray_skeleton.h) posed by the renderer's skinning matrices as it drew the skater
// (xray_render.h), seen by the camera of that same picture. The overlay draws it, each body in the
// colour a plugin gives it (Extension/UI/Overlay/xray_overlay.cpp).
//
// The renderer's two hooks are on while any plugin keeps the x-ray ready (off, the renderer never
// calls them), and the picture is read only while one draws it.
namespace dingosdk::xray {
// Requires the validated build: prepares the renderer's hooks, which patches nothing.
bool start(std::uintptr_t image_base) noexcept;
bool available() noexcept;
// Client thread: a plugin wants the x-ray ready (true) or no longer (false). The mesh is read in the
// background the first time.
void prepare(std::string_view owner, bool on);
// Client thread, every tick: the hooks on while any plugin keeps it ready; the picture read while it
// was drawn lately.
void tick() noexcept;

// The skeleton as the renderer last drew the skater, in world space, with that picture's camera.
struct Pose {
    std::array<float, 16> camera{}; // world matrix: right, up, back, position rows
    float vertical_fov{};           // degrees
    std::vector<game::Vec3> positions, normals; // one per vertex
    std::shared_ptr<const SkeletonMesh> mesh;   // its triangles, and each vertex's body
};
// Render thread, as a plugin draws it: false while there is no picture of the skater.
bool latest(Pose& pose);
// Client thread: the local skater not drawn on this screen while any plugin asks (reskate.skater.hide),
// through the same renderer hook. False when the renderer's hooks are unavailable.
bool hide(std::string_view owner, bool on);
}
