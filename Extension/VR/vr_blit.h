#pragma once
// Copies the game's back buffer into an OpenXR swapchain image with a
// full-screen triangle, converting the gamma-encoded (or scRGB) back buffer to
// the linear values an sRGB render target expects. Present thread only.
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <string>
#include <unordered_map>

namespace dingosdk::vr::detail {
class Blitter {
public:
    Blitter() = default;
    Blitter(const Blitter&) = delete;
    Blitter& operator=(const Blitter&) = delete;
    ~Blitter() = default;

    bool create(ID3D12Device* device, std::string& error);
    // Waits for submitted copies, then releases every object.
    void destroy() noexcept;
    bool ready() const noexcept { return device_ != nullptr; }
    static bool supported_source(DXGI_FORMAT format) noexcept;
    // `source` must be in D3D12_RESOURCE_STATE_PRESENT and is returned to it;
    // `target` must be in D3D12_RESOURCE_STATE_RENDER_TARGET and stays there.
    // `u_offset`/`u_scale` select a horizontal window of the source (0/1: all of it);
    // `vignette` darkens the edges (comfort vignette, 0 none).
    bool copy(ID3D12CommandQueue* queue, ID3D12Resource* source, ID3D12Resource* target, DXGI_FORMAT target_format,
        std::string& error, float u_offset = 0, float u_scale = 1, float vignette = 0);
    // True once all submitted copies have finished (or after `timeout_ms`, false).
    bool wait_idle(DWORD timeout_ms) noexcept;

private:
    static constexpr UINT slots = 3;
    ID3D12PipelineState* pipeline(DXGI_FORMAT format, std::string& error);
    bool wait(UINT64 value, DWORD timeout_ms) noexcept;

    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3DBlob> vertex_, pixel_;
    std::unordered_map<int, Microsoft::WRL::ComPtr<ID3D12PipelineState>> pipelines_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvs_, rtvs_;
    UINT srv_stride_ = 0, rtv_stride_ = 0;
    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, slots> allocators_;
    std::array<UINT64, slots> slot_fences_{};
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    HANDLE event_ = nullptr;
    UINT64 next_fence_ = 0;
    UINT slot_ = 0;
};
}
