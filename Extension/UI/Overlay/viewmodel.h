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
// Material pictures and a separate scene-depth SRV. Shared with the live overlay
// and graphics checks so neither can silently allocate a smaller descriptor table.
inline constexpr unsigned scene_descriptor_slots = max_materials * 3 + 5;
inline constexpr unsigned descriptor_slots = scene_descriptor_slots * 2;
// A second model is a world display: one named material receives the active
// scene, rendered locally. Its placement never changes the spectator's camera.
struct Display {
    std::string owner, material;
    std::array<float,3> position{},forward{0,0,1},up{0,1,0};
    std::vector<std::array<float,3>> depth_points;
    bool live{},scene_clear{};
};
void display(const std::filesystem::path& file, const std::optional<Display>& look);
std::optional<Display> display_now();
inline constexpr unsigned display_width=1024,display_height=768;
struct Inset {
    std::array<float,3> position{},forward{0,0,1},up{0,1,0};
    std::array<float,3> centre{1,1,0},radius{.2f,.2f,0},projection_offset{};
    float fov{60},background_alpha{.35f};
    std::vector<std::string> hidden_bones,hidden_materials;
};
struct Look {
    struct Reflection {
        struct PortalView { std::array<float,3> eye{}, forward{0,0,1}, up{0,1,0}, exit{}, normal{0,0,1}; };
        std::vector<PortalView> portal_views;
        std::array<float,3> position{},normal{0,1,0},tint{1,1,1};
        std::vector<std::string> parts;
        float time{},distortion{.01f},strength{.6f},texture_scale{1};
    };
    std::optional<Reflection> reflection;
    // Internal capture state, never a game-frame capture: only this owned model.
    std::optional<std::array<float,3>> clip_position;
    std::array<float,3> clip_normal{0,1,0};
    float projection_aspect{};
    std::optional<Inset> inset;
    // Internal secondary-view uniforms; plugins supply the bounded Inset above.
    std::array<float,4> clip_circle{};
    std::array<float,3> projection_shift{};
    std::array<float, 3> offset{};       // metres, in the view (x right, y up, z forward)
    float yaw{}, pitch{}, roll{};         // degrees, the whole model turned (viewmodel_model.h placement)
    float fov{60};                        // the viewmodel's lens: vertical degrees
    std::array<float, 3> light{-0.35f, 0.75f, -0.55f}; // toward the light, in the view
    std::array<float, 3> light_colour{1.0f, 0.96f, 0.9f};
    std::array<float, 3> sky{0.55f, 0.62f, 0.75f}, ground{0.32f, 0.29f, 0.26f};
    float ambient{0.6f};                  // how strong the sky and ground are
    float brightness{1};
    std::array<float,3> fog{}, fog_colour{};
    std::vector<std::string> fog_exempt_parts;
    std::array<float, 3> ambient_up{0,1,0};
        std::vector<std::pair<std::string, std::array<float, 12>>> transforms;
        std::vector<std::pair<std::string, float>> opacities;
        // Known visible surface points in view space, for matching the scene's depth projection.
        std::vector<std::array<float, 3>> depth_points;
        std::optional<std::array<float,12>> pose_transform;
        std::vector<std::pair<std::string,std::array<float,12>>> part_transforms;
        std::vector<std::string> hidden_parts;
        std::vector<std::pair<std::string,std::array<float,3>>> part_effects; // opacity, UV scroll x/y
        std::vector<std::pair<std::string,std::array<float,3>>> material_colours; // sRGB albedo multipliers
        std::optional<std::array<float,3>> view_position, backdrop;
        std::array<float,3> view_forward{0,0,1}, view_up{0,1,0};
        float view_fov{60};
        bool scene_depth{};
        bool scene_clear{};
        std::optional<std::array<float, 3>> contact_position;
        std::array<float, 3> contact_normal{0,1,0};
        float contact_margin{0.08f};
        std::optional<std::array<float, 3>> world_position;
        std::array<float, 3> world_forward{0,0,1}, world_up{0,1,0};                  // everything, after lighting
};

// ---- client thread ----------------------------------------------------------------------------------
// Starts reading the model (and its pictures, beside it) in the background, replacing any other.
void load(const std::filesystem::path& file, const std::vector<std::filesystem::path>& attachments = {});
// One replacement may prepare in the background while the active scene keeps
// drawing. Activation is refused until its complete GPU upload fence finishes.
void prepare(const std::filesystem::path& file, const std::vector<std::filesystem::path>& attachments = {});
std::string activate_prepared();
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
    bool render_ready{};
    Status prepared_status{Status::none};
    std::string prepared_why;
    bool prepared_ready{};
    Status display_status{Status::none};
    std::string display_why;
};
Info info();
// Drawn for the next 300 ms (set it every tick) with this look, once it is loaded.
void show(const Look& look) noexcept;
void hide() noexcept;
// Plays a clip by name (from the start), faded in over what played; empty when it played.
std::string play(std::string_view clip, const Animator::Play& play);
void stop() noexcept;
std::string seek(double seconds);
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
    void (*draw_surface_overlay)(const Target&){};
};
// Reuses the overlay's verified ResourceBarrier hook; no new game hook or address.
void capture_depth(ID3D12GraphicsCommandList* list, const D3D12_RESOURCE_BARRIER& barrier,
    void (STDMETHODCALLTYPE* original)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*)) noexcept;
// Records the viewmodel on the frame's command list, ahead of the screen look and ImGui's draws. False
// when nothing was drawn.
bool record(const Target& target) noexcept;
// The overlay is tearing its graphics down (a resize, a lost device, stop).
void release() noexcept;
} // namespace dingosdk::viewmodel
