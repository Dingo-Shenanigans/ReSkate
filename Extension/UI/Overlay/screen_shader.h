#pragma once
#include "screen_shader_file.h"
#include <array>
#include <cstdint>
#include <d3d12.h>
#include <dxgi.h>
#include <memory>
#include <string>
#include <string_view>

// A plugin's own pixel shader over the game's finished frame (reskate.screen.shader; screen_shader_file.h says what
// it may be and what it is given). Drawn as the screen look is (screen_look.h): the frame copied, then drawn back
// through the shader, ahead of the overlay's own drawing. One plugin's at a time.
//
// It runs on the GPU in the game's frame, so it is watched there: each frame it is drawn is timed on the GPU, and one
// that takes more than shader_budget_ms a frame on average (or shader_long_frames frames over shader_frame_most_ms
// in one stretch: one alone can be the game's window left and come back to) is turned off for the rest of the run, as is one that does not compile or that the driver refuses. The plugin is told why
// (set's answer). Nothing it draws is ever read back for the plugin: its only output is the picture.
namespace dingosdk::screen_shader {
inline constexpr double shader_budget_ms = 3.0;     // on average, over shader_budget_frames frames
inline constexpr unsigned shader_budget_frames = 30;
inline constexpr double shader_frame_most_ms = 50.0; // a long frame: left out of the average, and counted
inline constexpr unsigned shader_long_frames = 3;    // long frames in one stretch of shader_budget_frames that turn it off

// Client thread. The shader for the next 300 ms (set it every tick): `name` the plugin's file, `bytes` its content,
// checked by check_file as `kind`. Empty, or why it is not drawn (it did not compile, the driver refused it, it took
// too long on the GPU, another plugin's is on): the same file stays refused for the rest of the run.
std::string set(std::string_view owner, std::string_view name, std::shared_ptr<const std::string> bytes, Kind kind,
                const std::array<float, value_count> &values) noexcept;
// Ends the owner's shader (or anyone's, with no owner).
void clear(std::string_view owner = {}) noexcept;

// Render thread only (the overlay's present hook).
// A shader wants this frame drawn. Cheap: once a frame.
bool pending() noexcept;
struct Target {
    ID3D12Device *device{};
    ID3D12GraphicsCommandList *commands{};
    ID3D12Resource *buffer{};     // the back buffer, in the PRESENT state; left in it
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    ID3D12DescriptorHeap *srvs{}; // the overlay's shader-visible heap (already set on the list)
    unsigned slot{};              // the heap slot of the frame's copy
    DXGI_FORMAT format{};
    int transfer{};               // the overlay's output transfer: 0 SDR, 1 scRGB, 2 HDR10
    float white_nits{80};         // the overlay's white, in nits (80 for SDR)
    std::uint64_t timestamp_frequency{}; // the queue's ticks a second (0: the shader is not timed, and so not drawn)
    std::uint64_t fence{};        // what this frame's commands signal on the overlay's fence
    std::uint64_t (*gpu_done)(){}; // the overlay's fence's completed value
    bool (*wait_idle)(){};        // waits until the GPU has finished every frame submitted so far
};
// Records the shader on the frame's command list, ahead of ImGui's draws. False when nothing was drawn.
bool record(const Target &target) noexcept;
// The overlay is tearing its graphics down (a resize, a lost device, stop).
void release() noexcept;
} // namespace dingosdk::screen_shader
