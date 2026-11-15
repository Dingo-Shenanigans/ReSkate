#pragma once
#include <array>
#include <cstdint>
#include <d3d12.h>
#include <optional>
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
// - The world's colour: the full-size, floating-point picture the game finishes writing (a barrier from
//   being written to being read) last after the world's depth, in most of 30 frames: the lit, not yet
//   tone-mapped picture (the game lights the world in compute; no float target is bound with the depth).
// - When the world is finished: the batch of command lists that takes the screen from PRESENT to a render
//   target and reads that picture (the game's composite with its UI). The cover is drawn by a command list
//   of ours submitted just before that batch, so nothing of the game's own command lists, their state or
//   their order changes.
// All of it is seen through ResourceBarrier (the depth tap's hook) and ExecuteCommandLists. Anything not as
// expected (the colour's state unknown, the depth and colour not the same size, no depth readable as it is)
// draws nothing that frame. At worst the cover is late and drawn after the game has used the picture: then
// it is not seen.
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
    float fall{0.0f};       // 0..1: flakes of it falling in front of the world (snow, ash, petals), none to heavy
    float fall_speed{1.0f}; // 0.1..4: how fast it falls, 1 about 1.1 m/s
    float left{1.0f};       // 0..1: how much of it is left (melting), the same everywhere
    bool fall_stopping{}; // the fall stops: no new flakes, the last ones fall out
    // false: none of the cover where something is overhead (a roof, a bridge: indoors), and no flakes falling
    // under one (outside it still falls, seen from in). Found from the collision near the camera (measure_roofs).
    bool under_roofs{true};
    // true: light from the start (fewer flakes and samples), as it goes by itself on a GPU it takes too long on.
    bool light{};
    // A trail's tread (add_trail's tread) as grooves at their own places: each (centre, width) a share of the tread's
    // width from one edge, at most 8; the raised strips between are pressed darker, the grooves lighter. None: the
    // tread's lines evenly spaced (tread_lines).
    std::array<std::array<float, 2>, 8> tread_grooves{};
    int tread_groove_count{};
};

// A place kept clear of the cover: an upright cylinder `radius` wide standing on `foot` (the world),
// `height` tall. For the skater: none of the cover on the player.
struct Clear {
    std::array<float, 3> foot{};
    float radius{0.6f};
    float height{2.0f};
    // How much cover shows on what stands in it (the skater after rolling in it), 0 to 1: a patchy
    // dusting on every side. 0: none.
    float dusting{};
    // A board-shaped place (length > 0): `foot` its middle, `forward` and `up` its own axes (unit, square to
    // each other), `radius` its half width, `height` its thickness. In it, in its own plane, the cover is
    // `dusting` of what it would be and no trail is pressed into it: a deck riding low, which the upright
    // place round the skater takes for the ground.
    float length{};
    std::array<float, 3> forward{0, 0, 1}, up{0, 1, 0};
};

// Client thread. On until clear().
void set(const Settings& settings) noexcept;
void clear() noexcept;
// Client thread: a point on a trail through the cover (where a board goes, or a skater rolls): the cover
// is pressed down along the trail, `width` wide, and fills back in over `fade` seconds. At most 256 points
// are kept, the oldest dropped first; a point closer than a few centimetres to the last is not added.
// `tread` (metres, 0: none): the width of a tread pressed into it, whose `tread_lines` lines (lighter and
// pressed darker in turn, a lighter one in the middle) show along the trail.
// `starts`: the point begins a new trail, not joined to the point before (added however close to it).
void add_trail(const std::array<float, 3>& at, float width, float fade, float tread = 0, bool starts = false, int tread_lines = 13) noexcept;
// The highest collision surface straight down at (x, z) between the heights `top` and `bottom`, or nothing.
using GroundTop = std::optional<float> (*)(float x, float z, float top, float bottom);
// Client thread, every tick: while a cover is kept from under roofs, some of the cells round the camera are
// measured with `ground_top` (the roof map: how high the highest surface over each is).
void measure_roofs(GroundTop ground_top) noexcept;
// Client thread: up to three places kept clear for the next 300 ms (set them every tick); none ends it.
// The first one also measures the depth's scale on the ground round its foot, which the cylinders
// need; the scale is held from many frames, so they stay put while the foot is out of view.
void keep_clear(std::vector<Clear> places) noexcept;
bool on() noexcept;
// Client thread, every tick: whether the world is being played now (a level in play, no game menu
// open). The cover is drawn only once that has held for two seconds, never on the front page, in
// menus or while a level loads.
void allow(bool playing) noexcept;

// Render thread (the overlay), with its graphics: the device, the game's queue and where the hook would go (the
// queues' shared table, through the overlay's own). Nothing is hooked until a plugin first asks for a cover (set):
// then ExecuteCommandLists is hooked, in a HookBatch. The shaders are compiled (or read from their cache) on a
// thread of their own then too, or here already for a player whose cache holds an older version of them. A player
// whose plugins draw no cover has none of it.
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
