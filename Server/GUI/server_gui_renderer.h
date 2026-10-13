#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <imgui.h>
#ifdef small
#undef small
#endif

#include <array>
#include <vector>

namespace dingosdk::server_gui {

using Microsoft::WRL::ComPtr;

constexpr UINT frame_count = 2;

class Renderer {
public:
    bool init(HWND window);
    void render();
    void resize(UINT width, UINT height);
    void shutdown();

private:
    struct Frame {
        ComPtr<ID3D12CommandAllocator> allocator;
        UINT64 fence_value{};
    };
    struct Target {
        ComPtr<ID3D12Resource> resource;
        D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    };

    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_, srv_heap_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> fence_;
    ComPtr<IDXGISwapChain3> swap_;
    std::array<Frame, frame_count> frames_;
    std::array<Target, frame_count> targets_;
    HANDLE fence_event_{};
    HANDLE waitable_{};
    UINT64 fence_value_{};
    UINT64 frame_index_{};

    void wait(UINT64 value);
};

} // namespace dingosdk::server_gui

