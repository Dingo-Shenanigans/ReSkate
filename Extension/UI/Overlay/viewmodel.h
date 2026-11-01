#pragma once
#include "viewmodel_model.h"
#include <array>
#include <cstdint>
#include <d3d12.h>
#include <dxgi.h>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// A viewmodel (reskate.viewmodel): a plugin's rigged, animated model (viewmodel_model.h) drawn in front
// of the camera over the game's finished frame, ahead of the overlay's own drawing, with a depth buffer
// of its own, as shooters draw the player's arms and gun. One at a time; the plugin host decides whose.
// Its lens is its own (not the game camera's), so it looks the same whatever the game's field of view.
namespace dingosdk::viewmodel {
struct Look {
    std::array<float, 3> offset{};       // metres, in the view (x right, y up, z forward)
    float yaw{}, pitch{}, roll{};         // degrees, the whole model turned (viewmodel_model.h placement)
    float fov{60};                        // the viewmodel's lens: vertical degrees
    std::array<float, 3> light{-0.35f, 0.75f, -0.55f}; // toward the light, in the view
    std::array<float, 3> light_colour{1.0f, 0.96f, 0.9f};
    std::array<float, 3> sky{0.55f, 0.62f, 0.75f}, ground{0.32f, 0.29f, 0.26f};
    float ambient{0.6f};                  // how strong the sky and ground are
    float brightness{1};                  // everything, after lighting
};

// ---- client thread ----------------------------------------------------------------------------------
// Starts reading the model (and its pictures, beside it) in the background, replacing any other.
void load(const std::filesystem::path& file);
// Forgets the model; nothing is drawn until another is loaded.
void unload();
enum class Status { none, loading, ready, failed };
struct Info {
    Status status{Status::none};
    std::string why;  // failed: what was wrong
    struct ClipInfo {
        std::string name;
        double duration{};
    };
    std::vector<ClipInfo> clips;
    std::vector<std::string> bones;
};
Info info();
// Drawn for the next 300 ms (set it every tick) with this look, once it is loaded.
void show(const Look& look) noexcept;
void hide() noexcept;
// Plays a clip by name (from the start), faded in over what played; empty when it played.
std::string play(std::string_view clip, const Animator::Play& play);
void stop() noexcept;
struct State {
    std::string clip;  // empty: nothing playing
    double time{};
    double duration{};
    bool done{};
};
State state();
// Where a bone is now in the view, with the look's placement: its position and axes (x, y, z of the
// bone), metres; nothing when there is no such bone or no model.
struct BonePose {
    std::array<float, 3> position{}, x{}, y{}, z{};
};
std::optional<BonePose> bone(std::string_view name);

// ---- render thread only (the overlay's present hook) --------------------------------------------------
// Something to draw or upload this frame. Cheap: once a frame.
bool pending() noexcept;
struct Target {
    ID3D12Device* device{};
    ID3D12GraphicsCommandList* commands{};
    ID3D12Resource* buffer{};     // the back buffer, in the PRESENT state; left in it
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    ID3D12DescriptorHeap* srvs{}; // the overlay's shader-visible heap (already set on the list)
    unsigned slot{};              // the first of `slots` heap slots the viewmodel's pictures use
    unsigned slots{};
    DXGI_FORMAT format{};
    int transfer{};               // the overlay's output transfer: 0 SDR, 1 scRGB, 2 HDR10
    float white_nits{80};
    std::uint64_t fence{};        // what the overlay's fence reaches when this frame's commands are done
    std::uint64_t (*gpu_done)(){}; // the fence's value the GPU has reached
};
// Records the viewmodel on the frame's command list, ahead of the screen look and ImGui's draws. False
// when nothing was drawn.
bool record(const Target& target) noexcept;
// The overlay is tearing its graphics down (a resize, a lost device, stop).
void release() noexcept;
} // namespace dingosdk::viewmodel
