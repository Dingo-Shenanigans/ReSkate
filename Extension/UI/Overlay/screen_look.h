#pragma once
#include <array>
#include <d3d12.h>
#include <dxgi.h>
#include <string>

// A camcorder look over the game's finished frame (reskate.screen.look), made from the Filmer's VX1000
// look (neekgate/ReSkate, filmer-zee, filmer_look.cpp): the frame is copied and drawn back through one
// pixel shader (screen_look.hlsl) ahead of the overlay's own drawing. A fisheye adapter's bend and its
// dark hood edge, 4:3 bars, the camcorder's colour, standard-definition softness with its sharpening
// halo, interlaced fields, tape grain and its faults, an auto-iris, a hand that slaps the lens, and up
// to five lines of burnt-in text in the camcorder's own blocky characters.
namespace dingosdk::screen_look {
struct Settings {
    float bend{};        // 0..1: how much of the fisheye's bend; 0 none
    float fov{60};       // the game camera's vertical field of view (degrees), which the bend squeezes
    bool bars{};         // 4:3, black at the sides
    bool hood{};         // the hood's dark round edge in the corners
    bool grade{};        // the camcorder's colour: punchy, warm, a little crushed (SDR only)
    bool soft{};         // standard definition: soft, with a sharpening halo
    bool scanlines{};    // interlaced fields
    bool grain{};        // tape noise (SDR only)
    float age{};         // 0..1: how often and how badly the tape misbehaves
    bool dropouts{};     // now and then a white streak where the tape lost its coating
    bool head_switching{}; // the torn, noisy lines at the very bottom
    bool tracking{};     // a band of tearing rolls up the picture as the tape starts rolling
    bool smear{};        // reds trail to the right
    bool iris{};         // an auto-iris that lags the scene
    double rolling{-1};  // seconds since the tape started rolling (the tracking band); < 0 none
    // Burnt in: top left, top right, bottom left, then two lines bottom right; at most 12 characters
    // each, ASCII 32 to 95 (lower case is shown in capitals; '#' is the record dot, "[=]" a battery).
    // The right-hand lines are right-aligned.
    std::array<std::string, 5> text;
};
inline constexpr std::size_t text_columns = 12;

// Client thread. The look for the next 300 ms (set it every tick); clear() ends it.
void set(const Settings& settings) noexcept;
void clear() noexcept;
// A hand slaps the lens (about 0.6 s); mirrored every other time.
void slap() noexcept;

// Render thread only (the overlay's present hook).
// The look wants this frame drawn. Cheap: once a frame.
bool pending() noexcept;
struct Target {
    ID3D12Device* device{};
    ID3D12GraphicsCommandList* commands{};
    ID3D12Resource* buffer{};     // the back buffer, in the PRESENT state; left in it
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    ID3D12DescriptorHeap* srvs{}; // the overlay's shader-visible heap (already set on the list)
    unsigned slot{};              // the first of three heap slots the look uses (its copy, the auto-iris's two)
    DXGI_FORMAT format{};
    int transfer{};               // the overlay's output transfer: 0 SDR, 1 scRGB, 2 HDR10
    // Waits until the GPU has finished every frame submitted so far (a copy about to be replaced may
    // still be read). False when it could not.
    bool (*wait_idle)(){};
};
// Records the look on the frame's command list, ahead of ImGui's draws. False when nothing was drawn
// (the look is off, or this system cannot build it: said once in the log).
bool record(const Target& target) noexcept;
// The overlay is tearing its graphics down (a resize, a lost device, stop).
void release() noexcept;
} // namespace dingosdk::screen_look
