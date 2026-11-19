#pragma once
#include "screen_shader_file.h"
#include <array>
#include <cstdint>
#include <d3d12.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// A plugin's own pixel shader drawn into the game's lit picture of the world (reskate.world.shader): before the
// game's post-processing and its UI, so menus, the HUD and the overlay are never under it, and the game's grading,
// bloom and anti-aliasing treat what it draws as part of the world. With the world's depth and the camera it was
// drawn with, a shader can find where each pixel is in the world and which way its surface faces: snow, sand or
// moss on the ground, fog, rain, marks pressed into the ground.
//
// How it finds that moment, from what D3D12 shows of the game's frame (no game addresses):
// - The world's depth: the main depth buffer as the depth tap finds it (depth_tap.cpp: a screen-shaped depth buffer
//   that goes from being written to being read), copied by a list of ours as soon as the game has finished it.
//   Nothing is recorded on the game's command lists.
// - The world's colour: the full-size, floating-point picture the game finishes writing last after the world's depth,
//   in most of 30 frames: the lit, not yet tone-mapped picture.
// - When the world is finished: the batch of command lists that takes the screen from PRESENT to a render target and
//   reads that picture (the game's composite with its UI). The shader is drawn by a command list of ours submitted
//   just before that batch, so nothing of the game's own command lists, their state or their order changes.
// All of it is seen through ResourceBarrier (the depth tap's hook) and ExecuteCommandLists, which is hooked (in a
// HookBatch) only when a plugin first asks for a world shader. Anything not as expected draws nothing that frame.
//
// The shader. It may be HLSL source (.hlsl, compiled by ReSkate with `prelude` in front of it, entry `main`:
// plugin_shader_compile.h says with what), or a compiled pixel shader container (.cso or .dxbc: DXBC; .dxil: DXIL, signed), as screen_shader_file.h
// checks. Its bindings, which ReSkate gives it and nothing else:
//   b0  the cbuffer in `prelude` (the camera the picture was drawn with, last frame's, the measured near distance,
//       an anchor, a clock, the plugin's 48 values, and the picture's brightness eased);
//   t0  a copy of the world's picture before the shader (float, linear, not tone-mapped);
//   t1  the world's depth, copied (R32_FLOAT, reversed: 1 at the near plane, 0 far away, 0 the sky; a pixel's
//       distance along the view is near / depth);
//   t2  the plugin's data (shader_data): StructuredBuffer<float4>, reskate_clock.z of them;
//   t3  the ground heights round the camera (when asked for: reskate_clock.w is 1): heights_layout says how;
//   s0  linear filtering, clamped; s1 the nearest pixel, clamped.
// Its input: SV_Position (pixels) and TEXCOORD0 (0..1 across, y down); its output one float4 at SV_Target0, which
// replaces the pixel of the world's picture (alpha is not blended).
//
// It runs on the GPU in the game's frame, so it is watched there: each frame is timed, and one that takes more than
// world_budget_ms a frame on average (or world_long_frames frames over world_frame_most_ms in one stretch: one alone
// can be the game's window left and come back to) is turned off for the rest of the run, as is one that does not
// compile or that the driver refuses. Nothing it draws is read back for the plugin.
namespace dingosdk::world_shader {
inline constexpr double world_budget_ms = 12.0;     // on average, over world_budget_frames frames
inline constexpr unsigned world_budget_frames = 120;
inline constexpr double world_frame_most_ms = 100.0; // a long frame: left out of the average, and counted
inline constexpr unsigned world_long_frames = 3;     // long frames in one stretch of world_budget_frames that turn it off
inline constexpr std::size_t most_data_bytes = screen_shader::world_data_most_bytes; // the plugin's data: 4096 float4s

// In front of a plugin's HLSL source. A compiled shader declares the same itself (the bindings are what count).
inline constexpr const char* prelude = R"hlsl(
cbuffer ReSkateWorld : register(b0) {
    float4 reskate_right_focal;  // the camera the picture was drawn with: its right (world), its focal length (pixels)
    float4 reskate_up_width;     // its up, the picture's width (pixels)
    float4 reskate_back_height;  // its back (it looks along -back), the picture's height
    float4 reskate_eye_near;     // where it is, the near distance the depth is over (0: not measured yet)
    float4 reskate_last_right;   // last frame's camera, the same way: w 1 when it was drawn last frame too
    float4 reskate_last_up;
    float4 reskate_last_back;
    float4 reskate_last_eye;
    float4 reskate_anchor;       // shader_anchor's point (the near distance is measured on it), w 1 while it is set
    float4 reskate_clock;        // seconds (from 0 again every 1000), seconds since the last frame, the plugin's
                                 // data's float4s (t2), 1 while the ground heights (t3) are measured
    float4 reskate_values[12];   // the plugin's (reskate.world.shader's values, four to a float4)
    float4 reskate_light;        // the picture's brightness (its luminance, averaged over 32 points), eased over about half
                                 // a second; this frame's own; 1 once measured; the open sky's (taken once, then eased over
                                 // about 30 s, kept while none is in view, taken afresh as the world's depth is made anew),
                                 // 0 until seen
};
Texture2D<float4> reskate_picture : register(t0);
Texture2D<float> reskate_depth : register(t1);
StructuredBuffer<float4> reskate_data : register(t2);
StructuredBuffer<float4> reskate_heights : register(t3);
SamplerState reskate_linear : register(s0);
SamplerState reskate_point : register(s1);
#line 1
)hlsl";

// The ground heights (t3), measured round the camera with one straight-down ray a cell (the collision, the highest
// surface over the cell's middle, from 60 m over the camera to 60 m under it), a few hundred cells a second at most
// 1 ms a tick of the game's update thread: [0] = (1 when on, the near grid's cells across, its cell size, the far
// grid's cells across), [1] = (the far grid's cell size, the near grid's first cell x and z when its blocks were
// worked out, 0), then the near grid's cells, then the far grid's, a float4 each kept where its world position falls
// (x and z each modulo its cells across): (the highest surface, which cell it is: x, z, 1 once measured; -1e30 where
// nothing was hit), then for each 8x8 block of the near grid's cells the highest surface over it and the blocks round
// it, four to a float4 (1e30 while any of them is not measured).
struct HeightsLayout {
    int near_cells = 128;
    float near_size = 0.5f;  // 64 m across
    int far_cells = 128;
    float far_size = 1.5f;   // 192 m across
    int block = 8;
};
inline constexpr HeightsLayout heights_layout{};

using Kind = screen_shader::Kind;
inline constexpr std::size_t value_count = screen_shader::value_count;

// Client thread. The plugin's shader, on until clear(owner) (or the plugin goes): `name` its file, `bytes` its content
// as check_file found it to be `kind`, `values` its numbers, `heights` whether the ground heights are measured for it.
// Empty, or why it is not drawn (another plugin's is on; it did not compile, the driver refused it, or it took too long
// on the GPU, which keeps the same file off for the rest of the run).
std::string set(std::string_view owner, std::string_view name, std::shared_ptr<const std::string> bytes, Kind kind,
                const std::array<float, value_count>& values, bool heights) noexcept;
void clear(std::string_view owner = {}) noexcept;
// The owner's data (t2): float4s, at most most_data_bytes; kept until replaced. Empty, or why not.
std::string set_data(std::string_view owner, std::shared_ptr<const std::string> bytes) noexcept;
// Where the depth's near distance is measured (on screen, at a known place: the skater, standing), for the next 300 ms
// (set it every tick); none: not measured (the distance measured last is kept).
void set_anchor(std::string_view owner, std::optional<std::array<float, 3>> at) noexcept;
// The shader's clock (reskate_clock.x): seconds, from 0 again every 1000, as the frame drawn now has it. A plugin
// stamps its data with it (when a mark was made, when something began) for its shader to work out how long ago.
double clock_seconds() noexcept;
// The owner's shader's average time a frame on the GPU over its last frames (milliseconds), once known.
std::optional<double> cost_ms(std::string_view owner) noexcept;
bool on() noexcept;
// The highest collision surface straight down at (x, z) between the heights `top` and `bottom`, or nothing.
using GroundTop = std::optional<float> (*)(float x, float z, float top, float bottom);
// Client thread, every tick: while a world shader asks for the ground heights, some cells round the camera are measured.
void measure_heights(GroundTop ground_top) noexcept;
// Client thread, every tick: whether the world is being played now (a level in play, no game menu open). The shader
// is drawn only once that has held for two seconds, never on the front page, in menus or while a level loads. `in_menu`:
// a game menu is why not (the shader goes a quarter of a second later); otherwise (the game not ready a moment) it goes
// only after 2.5 s.
void allow(bool playing, bool in_menu = false) noexcept;

// Render thread (the overlay), with its graphics: the device, the game's queue and the queues' table (through the
// overlay's own). Nothing is hooked until a plugin first asks for a world shader.
void install(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list) noexcept;
// Each present, before the overlay draws: a new frame of the game's begins after it (the back buffer's size).
void present(std::uint32_t width, std::uint32_t height) noexcept;
// The overlay is tearing its graphics down (a resize, a lost device, stop).
void release() noexcept;
// From the depth tap's ResourceBarrier hook (depth_tap.cpp), for each barrier the game records, before the game's own
// barrier is recorded. Only watches: it records nothing.
using BarrierFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
void on_barrier(ID3D12GraphicsCommandList* list, const D3D12_RESOURCE_BARRIER& barrier, BarrierFn original) noexcept;
} // namespace dingosdk::world_shader
