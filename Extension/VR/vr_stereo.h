#pragma once
// Depth stereo: both eyes from one rendered frame and its depth. The game renders
// from between the eyes; each eye's image is the frame shifted per pixel by its
// disparity (eye offset over depth), searched backwards so nearer surfaces win
// and holes take the background. Present thread only.
#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <string>
#include <unordered_map>

namespace dingosdk::vr::detail {
struct StereoParams {
    // Horizontal shift in UV units per unit of (reversed-Z) depth, and its cap.
    float scale = 0;
    float max_shift = 0.04f;
    // Holes the centre view never saw: background (farthest surface) or the near edge stretched.
    bool background_fill = true;
    float vignette = 0; // comfort vignette (0 none)
};

// The scene depth copy (vr_depth.cpp): typeless, COPY_DEST between uses; null until copied.
Microsoft::WRL::ComPtr<ID3D12Resource> depth_texture();

class DepthStereo {
public:
    bool create(ID3D12Device* device, std::string& error);
    void destroy() noexcept;
    bool ready() const noexcept { return device_ != nullptr; }
    // Copies this frame's colour (`frame` in PRESENT, returned to it) for the next
    // render: the depth copy taken at the next clear belongs to this frame.
    bool keep(ID3D12CommandQueue* queue, ID3D12Resource* frame, std::string& error);
    bool has_kept() const noexcept { return kept_ != nullptr; }
    // Snapshots the kept frame's depth: `depth` is the typeless depth copy (COPY_DEST,
    // kept), taken at the clear right after that frame. Call once per kept frame.
    // `widening`: pixels (0-3) the near depth is widened by, so soft edges move with their object.
    bool keep_depth(ID3D12CommandQueue* queue, ID3D12Resource* depth, int widening, std::string& error);
    bool has_kept_depth() const noexcept { return kept_depth_ != nullptr; }
    // Renders the kept colour and depth into `targets` (left, right; RENDER_TARGET, kept).
    // raw: no sRGB decode.
    bool render(ID3D12CommandQueue* queue, const std::array<ID3D12Resource*, 2>& targets, DXGI_FORMAT target_format,
        const StereoParams& params, bool raw, std::string& error);
    bool wait_idle(DWORD timeout_ms) noexcept;

private:
    static constexpr UINT slots = 3;
    bool begin(std::string& error);
    bool submit(ID3D12CommandQueue* queue, std::string& error);
    ID3D12PipelineState* pipeline(DXGI_FORMAT format, std::string& error, bool dilate = false);
    bool wait(UINT64 value, DWORD timeout_ms) noexcept;

    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3DBlob> vertex_, pixel_, dilate_;
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
    // PIXEL_SHADER_RESOURCE between uses; widened_ is the near-dilated depth the eyes read.
    Microsoft::WRL::ComPtr<ID3D12Resource> kept_, kept_depth_, widened_;
    DXGI_FORMAT kept_format_ = DXGI_FORMAT_UNKNOWN;
};
}
