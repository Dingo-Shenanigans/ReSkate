#include "server_gui_renderer.h"

#include <dxgi1_6.h>
#include <backends/imgui_impl_dx12.h>

#include <algorithm>
#include <cstdio>

namespace dingosdk::server_gui {

bool Renderer::init(HWND window) {
    UINT factory_flags = 0;
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory)))) {
        return false;
    }

    // Try high performance GPU first
    if (ComPtr<IDXGIFactory6> ranked; SUCCEEDED(factory.As(&ranked))) {
        for (UINT index = 0; !device_; ++index) {
            ComPtr<IDXGIAdapter1> candidate;
            if (FAILED(ranked->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                    IID_PPV_ARGS(&candidate)))) break;
            DXGI_ADAPTER_DESC1 description{};
            if (FAILED(candidate->GetDesc1(&description)) || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                continue;
            D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
        }
    }

    // Fallback to default device or WARP software device
    if (!device_ && FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)))) {
        ComPtr<IDXGIAdapter> warp;
        if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
            FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)))) {
            return false;
        }
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, frame_count, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 1};
    D3D12_DESCRIPTOR_HEAP_DESC srv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 64,
        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 1};

    if (FAILED(device_->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap_))) ||
        FAILED(device_->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&srv_heap_))) ||
        FAILED(device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_)))) return false;

    for (auto& frame : frames_) {
        if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator)))) return false;
    }

    if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames_[0].allocator.Get(), nullptr,
            IID_PPV_ARGS(&list_))) || FAILED(list_->Close()) ||
        FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) return false;

    fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event_) return false;

    DXGI_SWAP_CHAIN_DESC1 swap_desc{};
    swap_desc.BufferCount = frame_count;
    swap_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swap_desc.SampleDesc.Count = 1;
    swap_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    swap_desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    ComPtr<IDXGISwapChain1> swap;
    if (FAILED(factory->CreateSwapChainForHwnd(queue_.Get(), window, &swap_desc, nullptr, nullptr, &swap)) ||
        FAILED(swap.As(&swap_))) return false;

    factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
    swap_->SetMaximumFrameLatency(frame_count);
    waitable_ = swap_->GetFrameLatencyWaitableObject();

    const auto rtv_size = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT index = 0; index < frame_count; ++index) {
        targets_[index].handle = rtv;
        if (FAILED(swap_->GetBuffer(index, IID_PPV_ARGS(&targets_[index].resource)))) return false;
        device_->CreateRenderTargetView(targets_[index].resource.Get(), nullptr, rtv);
        rtv.ptr += rtv_size;
    }

    ImGui_ImplDX12_InitInfo info;
    info.Device = device_.Get();
    info.CommandQueue = queue_.Get();
    info.NumFramesInFlight = frame_count;
    info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    info.SrvDescriptorHeap = srv_heap_.Get();
    info.LegacySingleSrvCpuDescriptor = srv_heap_->GetCPUDescriptorHandleForHeapStart();
    info.LegacySingleSrvGpuDescriptor = srv_heap_->GetGPUDescriptorHandleForHeapStart();

    if (!ImGui_ImplDX12_Init(&info)) {
        return false;
    }
    if (!ImGui_ImplDX12_CreateDeviceObjects()) {
        return false;
    }
    return true;
}

void Renderer::render() {
    auto& frame = frames_[frame_index_ % frame_count];
    wait(frame.fence_value);
    if (waitable_) WaitForSingleObject(waitable_, 1000);

    const auto back = swap_->GetCurrentBackBufferIndex();
    frame.allocator->Reset();
    list_->Reset(frame.allocator.Get(), nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = targets_[back].resource.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    list_->ResourceBarrier(1, &barrier);

    // Dark slate background color matching ReSkate Trainer / Launcher
    constexpr float clear[4]{0.035f, 0.043f, 0.059f, 1.0f};
    list_->ClearRenderTargetView(targets_[back].handle, clear, 0, nullptr);
    list_->OMSetRenderTargets(1, &targets_[back].handle, FALSE, nullptr);

    ID3D12DescriptorHeap* heaps[]{srv_heap_.Get()};
    list_->SetDescriptorHeaps(1, heaps);

    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), list_.Get());

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    list_->ResourceBarrier(1, &barrier);
    list_->Close();

    ID3D12CommandList* lists[]{list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    swap_->Present(1, 0); // VSync enabled for smooth UI

    frame.fence_value = ++fence_value_;
    queue_->Signal(fence_.Get(), frame.fence_value);
    ++frame_index_;
}

void Renderer::resize(UINT width, UINT height) {
    if (!swap_ || width == 0 || height == 0) return;

    for (auto& frame : frames_) wait(frame.fence_value);
    for (auto& target : targets_) target.resource.Reset();

    swap_->ResizeBuffers(frame_count, width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);

    const auto rtv_size = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT index = 0; index < frame_count; ++index) {
        targets_[index].handle = rtv;
        swap_->GetBuffer(index, IID_PPV_ARGS(&targets_[index].resource));
        device_->CreateRenderTargetView(targets_[index].resource.Get(), nullptr, rtv);
        rtv.ptr += rtv_size;
    }
}

void Renderer::wait(UINT64 value) {
    if (fence_->GetCompletedValue() >= value) return;
    fence_->SetEventOnCompletion(value, fence_event_);
    WaitForSingleObject(fence_event_, INFINITE);
}

void Renderer::shutdown() {
    for (auto& frame : frames_) wait(frame.fence_value);
    ImGui_ImplDX12_Shutdown();
    for (auto& target : targets_) target.resource.Reset();
    if (swap_) swap_.Reset();
    if (waitable_) { CloseHandle(waitable_); waitable_ = nullptr; }
    if (fence_event_) { CloseHandle(fence_event_); fence_event_ = nullptr; }
    fence_.Reset();
    list_.Reset();
    for (auto& frame : frames_) frame.allocator.Reset();
    queue_.Reset();
    srv_heap_.Reset();
    rtv_heap_.Reset();
    device_.Reset();
}

} // namespace dingosdk::server_gui

