#pragma once
#include <array>
#include <d3d12.h>
#include <filesystem>
#include <functional>
namespace dingosdk::overlay
{
using MaterialBarrierFn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, const D3D12_RESOURCE_BARRIER *);
struct MaterialMaskDraw
{
    ID3D12Device *device{};
    ID3D12GraphicsCommandList *commands{};
    UINT target_count{};
    std::array<DXGI_FORMAT, 8> formats{};
    D3D12_VIEWPORT viewport{};
    D3D12_CPU_DESCRIPTOR_HANDLE depth{}, source_colour{};
    std::array<float, 8> parameters{20, 4, 2, 1.52f, 12, .0006f, 1, 1};
    std::function<void(D3D12_CPU_DESCRIPTOR_HANDLE)> replay;
};
void set_material_transmission_enabled(bool enabled) noexcept;
bool material_transmission_configured() noexcept;
bool material_transmission_requested() noexcept;
void record_material_mask(const MaterialMaskDraw &draw) noexcept;
void material_depth_descriptor(ID3D12Resource *, const D3D12_DEPTH_STENCIL_VIEW_DESC *,
                               D3D12_CPU_DESCRIPTOR_HANDLE) noexcept;
void material_copy_depth_descriptors(ID3D12Device *, UINT, D3D12_CPU_DESCRIPTOR_HANDLE,
                                     D3D12_CPU_DESCRIPTOR_HANDLE) noexcept;
void material_list_reset(ID3D12GraphicsCommandList *) noexcept;
void material_queue_submission(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *) noexcept;
void material_depth_barriers(ID3D12GraphicsCommandList *, UINT, const D3D12_RESOURCE_BARRIER *,
                             MaterialBarrierFn) noexcept;
void present_material_transmission(ID3D12Device *, ID3D12CommandQueue *, ID3D12Resource *, int transfer) noexcept;
} // namespace dingosdk::overlay
