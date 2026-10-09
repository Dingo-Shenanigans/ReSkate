#pragma once
#include <atomic>
#include <d3d12.h>

// Skate 3's Hall of Meat colour matrix (post_fx colour_matrix_hall_of_meat: half saturation, blue
// x1.2) applied to the game's own picture, in the overlay's DirectX 12 pass before the HUD is drawn:
// the overlay alone can only darken and tint, not take the colour out.
namespace dingosdk::overlay {
// 0..1, set by the Hall of Meat HUD each frame: how far the colour matrix applies.
std::atomic<float> &hom_colour_strength() noexcept;
// Records the pass into `commands`. `buffer` must be in the PRESENT state; on success it is left in
// RENDER_TARGET with `rtv` bound (the caller then draws the HUD and returns it to PRESENT). False when
// nothing was recorded (the caller does its own PRESENT -> RENDER_TARGET barrier then).
bool hom_grade_record(ID3D12Device *device, ID3D12GraphicsCommandList *commands, ID3D12Resource *buffer,
                      D3D12_CPU_DESCRIPTOR_HANDLE rtv, float strength) noexcept;
// Drops the pass's GPU objects (with the overlay's own graphics teardown).
void hom_grade_release() noexcept;
} // namespace dingosdk::overlay
