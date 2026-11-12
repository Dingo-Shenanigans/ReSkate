#pragma once
#include <array>
#include <cstdint>
#include <d3d12.h>
#include <vector>

// A cover over the world's upward-facing surfaces (reskate.effects.cover): snow, sand, dust, moss,
// whatever a plugin makes of its colour. It is drawn into the game's own lit picture of the world,
// before the game's post-processing and its UI, so menus, the HUD and the overlay are never under it
// and the game's grading, bloom and anti-aliasing treat it as part of the world.
//
// How it finds that moment, from what D3D12 shows of the game's frame (no game addresses):
// - The world's depth: the main depth buffer as the depth tap finds it (depth_tap.cpp: a
//   screen-shaped depth buffer that goes from being written to being read). It is read where it is,
//   by our list, and only while the game itself has left it readable by pixel shaders: nothing is
//   recorded on the game's command lists (a copy of the depth put on them hung the GPU, 2026-11-10).
// - The world's colour: the render target bound together with that depth buffer (OMSetRenderTargets)
//   whose format is a floating-point one: the lit, not yet tone-mapped picture. The G-buffer's
//   targets are not floating point and the UI never draws with the world's depth bound, so neither is
//   taken for it.
// - When the world is finished: the first batch of command lists the game submits to its direct
//   queue, after one in which the world was drawn, with no world drawing in it. The cover is drawn by
//   a command list of ours submitted just before that batch, so nothing of the game's own command
//   lists, their state or their order changes.
// Anything not as expected (the colour's state unknown, the depth and colour not the same size, no
// depth readable as it is, render passes instead of OMSetRenderTargets) draws nothing that frame. At
// worst the cover is late and drawn after the game has used the picture: then it is not seen.
// Also nothing while the colour is half-way through a split barrier, has memory aliased onto it,
// or is in an unordered-access state. If the game touches the colour from a command list that is
// not a direct one (async compute, which could run beside our list), the cover stops for good.
//
// The surfaces' direction comes from the depth (each pixel's neighbours) and the camera
// (plugin_camera.h); the game's depth is reversed (1 near, 0 far: capture_image.h), so the sky (0) is
// never covered.
namespace dingosdk::world_cover {
struct Settings {
    std::array<float, 3> colour{0.92f, 0.95f, 1.0f}; // the cover's own colour, 0..1 each
    float amount{1.0f};     // 0..1: how much of a fully covered surface it hides
    float facing{0.55f};    // 0..1: how upward a surface must face to be covered (1 only flat ground)
    float softness{0.15f};  // 0..0.5: how gradually the cover thins out below `facing`
    float brightness{1.6f}; // 0.25..4: the cover's lit brightness against what it covers
    float breakup{0.35f};   // 0..1: how patchy its edge is (world-space noise)
    float sparkle{0.0f};    // 0..1: glints, as fresh snow has
    float snowfall{0.0f};   // 0..1: snow falling in front of the world, none to heavy
    float fall_speed{1.0f}; // 0.1..4: how fast it falls, 1 about 1.1 m/s
    float left{1.0f};       // 0..1: how much of it is left (melting), the same everywhere
    bool snowfall_stopping{}; // the snow stops falling: no new flakes, the last ones fall out
};

// A place kept clear of the cover: an upright cylinder `radius` wide standing on `foot` (the world),
// `height` tall. For the skater: no snow on the player.
struct Clear {
    std::array<float, 3> foot{};
    float radius{0.6f};
    float height{2.0f};
    // A board-shaped place (length > 0): `foot` its middle, `forward` and `up` its own axes (unit, square to
    // each other), `radius` its half width, `height` its thickness. Everything in it is clear, the ground
    // included: it stands for something lying on the ground (a deck), which hides the ground under it.
    float length{};
    std::array<float, 3> forward{0, 0, 1}, up{0, 1, 0};
};

// Client thread. On until clear().
void set(const Settings& settings) noexcept;
void clear() noexcept;
// Client thread: up to three places kept clear for the next 300 ms (set them every tick); none ends it.
// The first one also measures the depth's scale on the ground round its foot, which the cylinders
// need; the scale is held from many frames, so they stay put while the foot is out of view.
void keep_clear(std::vector<Clear> places) noexcept;
bool on() noexcept;
// Client thread, every tick: whether the world is being played now (a level in play, no game menu
// open). The cover is drawn only once that has held for two seconds, never on the front page, in
// menus or while a level loads.
void allow(bool playing) noexcept;

// Render thread (the overlay): its hooks on the game's device, its queue and the command lists'
// shared table (through the overlay's own list), installed once with the overlay's graphics.
void install(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list) noexcept;
// Each present, before the overlay draws: a new frame of the game's begins after it. `width` and
// `height` are the back buffer's.
void present(std::uint32_t width, std::uint32_t height) noexcept;
// The overlay is tearing its graphics down (a resize, a lost device, stop): the cover's own
// resources go, and it draws nothing until the next install.
void release() noexcept;

// From the depth tap's ResourceBarrier hook (depth_tap.cpp), for each barrier the game records,
// before the game's own barrier is recorded. Only watches: it records nothing.
using BarrierFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
void on_barrier(ID3D12GraphicsCommandList* list, const D3D12_RESOURCE_BARRIER& barrier, BarrierFn original) noexcept;
}
