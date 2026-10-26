#pragma once
#include "material_transmission.h"
#include <d3d12.h>
namespace dingosdk::overlay
{
// Enabled only by validated declarations in the launch catalogue.
// Replays a byte-for-byte matched draw into private targets; never replaces the native draw.
void start_material_capture() noexcept;
void install_material_capture(ID3D12Device *, ID3D12GraphicsCommandList *, ID3D12CommandQueue *) noexcept;
void observe_material_depth_barriers(ID3D12GraphicsCommandList *, UINT, const D3D12_RESOURCE_BARRIER *,
                                     MaterialBarrierFn) noexcept;
void present_material_transmission_guarded(ID3D12Device *, ID3D12CommandQueue *, ID3D12Resource *,
                                           int transfer) noexcept;
} // namespace dingosdk::overlay
