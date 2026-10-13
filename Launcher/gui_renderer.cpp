#include "gui_renderer.h"

#include "Engine/Core/Log/logging.h"

#include <dxgi1_6.h>

#include <backends/imgui_impl_dx12.h>
#include <wincodec.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

namespace dingosdk::launcher_gui::detail {

bool Renderer::init(HWND window) {
    UINT factory_flags = 0;
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory)))) {
        logging::write(logging::Level::error, logging::Channel::launcher, "Launcher renderer: no DXGI factory.");
        return false;
    }
    bool software = false;
    // Ask for the best card rather than whichever DXGI lists first. Where
    // onboard graphics sit beside a real card, the default adapter is usually
    // the onboard one, and its Direct3D 12 driver can be too old to draw the
    // window at all while the card that would have worked goes unused.
    if (ComPtr<IDXGIFactory6> ranked; SUCCEEDED(factory.As(&ranked))) {
        for (UINT index = 0; !device_; ++index) {
            ComPtr<IDXGIAdapter1> candidate;
            if (FAILED(ranked->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                    IID_PPV_ARGS(&candidate)))) break;
            DXGI_ADAPTER_DESC1 description{};
            // WARP is the last resort below, not a candidate here.
            if (FAILED(candidate->GetDesc1(&description)) || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                continue;
            D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
        }
    }
    if (!device_ && FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)))) {
        ComPtr<IDXGIAdapter> warp;
        if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
            FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)))) {
            logging::write(logging::Level::error, logging::Channel::launcher,
                "Launcher renderer: no Direct3D 12 device, not even the software one.");
            return false;
        }
        software = true;
    }
    {
        // Which card drew the window, so a blank one can be told apart from a
        // driver problem, a software fallback or a remote session.
        DXGI_ADAPTER_DESC1 description{};
        ComPtr<IDXGIAdapter1> adapter;
        if (SUCCEEDED(factory->EnumAdapterByLuid(device_->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) &&
            SUCCEEDED(adapter->GetDesc1(&description)))
            logging::write(logging::Level::info, logging::Channel::launcher,
                std::wstring(L"Launcher renderer: ") + description.Description +
                (software ? L" (software fallback)" : L"") + L", " +
                std::to_wstring(description.DedicatedVideoMemory / (1024 * 1024)) + L" MB dedicated");
        else
            logging::write(logging::Level::warning, logging::Channel::launcher,
                software ? "Launcher renderer: software fallback, adapter unidentified."
                         : "Launcher renderer: adapter unidentified.");
    }
    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, frame_count, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 1};
    D3D12_DESCRIPTOR_HEAP_DESC srv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1 + max_textures,
        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 1};
    if (FAILED(device_->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap_))) ||
        FAILED(device_->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&srv_heap_))) ||
        FAILED(device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_)))) return false;
    for (auto& frame : frames_)
        if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator)))) return false;
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
        logging::write(logging::Level::error, logging::Channel::launcher,
            "Launcher renderer: the Direct3D 12 backend did not start.");
        return false;
    }
    // The backend builds its pipeline and font texture on the first frame and
    // ignores whether that worked, so a driver that cannot make one draws
    // nothing at all and the window shows only its clear colour -- a black
    // window, with a log that says everything succeeded. Build it here, where
    // the answer can be acted on.
    if (!ImGui_ImplDX12_CreateDeviceObjects()) {
        logging::write(logging::Level::error, logging::Channel::launcher,
            "Launcher renderer: this driver cannot create the Direct3D 12 pipeline the window draws with.");
        return false;
    }
    if (!ImGui_ImplDX12_HasOutputTransfer())
        logging::write(logging::Level::info, logging::Channel::launcher,
            "Launcher renderer: drawing with ImGui's stock shader; this system could not build the HDR-aware one.");
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
    constexpr float clear[4]{0.035f, 0.043f, 0.059f, 1.0f};
    list_->ClearRenderTargetView(targets_[back].handle, clear, 0, nullptr);
    list_->OMSetRenderTargets(1, &targets_[back].handle, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[]{srv_heap_.Get()};
    list_->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), list_.Get());
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list_->ResourceBarrier(1, &barrier);
    list_->Close();
    ID3D12CommandList* lists[]{list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    swap_->Present(1, 0);
    queue_->Signal(fence_.Get(), ++fence_value_);
    frame.fence_value = fence_value_;
    ++frame_index_;
}

ImTextureID Renderer::upload_texture(const std::vector<unsigned char>& pixels, UINT width, UINT height) {
    auto free_slot = std::find(textures_.begin(), textures_.end(), nullptr);
    if (free_slot == textures_.end() && textures_.size() >= max_textures) return {};
    ComPtr<ID3D12Resource> texture;
    D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC texture_desc{};
    texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_desc.Width = width;
    texture_desc.Height = height;
    texture_desc.DepthOrArraySize = 1;
    texture_desc.MipLevels = 1;
    texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture_desc.SampleDesc.Count = 1;
    if (FAILED(device_->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)))) return {};
    const UINT pitch = (width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
    D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC buffer_desc{};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = static_cast<UINT64>(pitch) * height;
    buffer_desc.Height = buffer_desc.DepthOrArraySize = buffer_desc.MipLevels = 1;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> upload;
    if (FAILED(device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)))) return {};
    void* mapped{};
    D3D12_RANGE none{0, 0};
    if (FAILED(upload->Map(0, &none, &mapped))) return {};
    for (UINT row = 0; row < height; ++row)
        std::memcpy(static_cast<unsigned char*>(mapped) + static_cast<std::size_t>(row) * pitch,
            pixels.data() + static_cast<std::size_t>(row) * width * 4, static_cast<std::size_t>(width) * 4);
    upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION source{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    source.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, pitch};
    D3D12_TEXTURE_COPY_LOCATION target{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    auto& frame = frames_[0];
    wait(frame.fence_value);
    frame.allocator->Reset();
    list_->Reset(frame.allocator.Get(), nullptr);
    list_->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    list_->ResourceBarrier(1, &barrier);
    list_->Close();
    ID3D12CommandList* lists[]{list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    queue_->Signal(fence_.Get(), ++fence_value_);
    wait(fence_value_);

    const auto increment = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = srv_heap_->GetCPUDescriptorHandleForHeapStart();
    auto gpu = srv_heap_->GetGPUDescriptorHandleForHeapStart();
    const auto slot = static_cast<UINT>(free_slot - textures_.begin()) + 1;
    cpu.ptr += static_cast<SIZE_T>(increment) * slot;
    gpu.ptr += static_cast<UINT64>(increment) * slot;
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(texture.Get(), &view, cpu);
    if (free_slot == textures_.end()) textures_.push_back(std::move(texture));
    else *free_slot = std::move(texture);
    return static_cast<ImTextureID>(gpu.ptr);
}

void Renderer::release_texture(ImTextureID id) {
    if (!id) return;
    const auto increment = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto start = srv_heap_->GetGPUDescriptorHandleForHeapStart().ptr;
    const auto offset = static_cast<UINT64>(id) - start;
    if (static_cast<UINT64>(id) <= start || offset % increment) return;
    const auto index = static_cast<std::size_t>(offset / increment) - 1;
    if (index >= textures_.size() || !textures_[index]) return;
    // Frames in flight may still sample it; this is rare enough to wait for.
    queue_->Signal(fence_.Get(), ++fence_value_);
    wait(fence_value_);
    textures_[index].Reset();
}

void Renderer::shutdown() {
    if (queue_ && fence_) {
        queue_->Signal(fence_.Get(), ++fence_value_);
        wait(fence_value_);
    }
    ImGui_ImplDX12_Shutdown();
    if (waitable_) CloseHandle(waitable_);
    if (fence_event_) CloseHandle(fence_event_);
}

void Renderer::wait(UINT64 value) {
    if (!value || fence_->GetCompletedValue() >= value) return;
    fence_->SetEventOnCompletion(value, fence_event_);
    WaitForSingleObject(fence_event_, INFINITE);
}

bool decode_image(const std::vector<unsigned char>& bytes, ImVec2 cover, std::vector<unsigned char>& pixels,
                  UINT& width, UINT& height) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&width, &height)) || !width || !height)
        return false;
    ComPtr<IWICBitmapSource> source = frame;
    const float fit = std::max(cover.x / static_cast<float>(width), cover.y / static_cast<float>(height));
    if (fit < 1.0f) {
        ComPtr<IWICBitmapScaler> scaler;
        width = std::max(1u, static_cast<UINT>(static_cast<float>(width) * fit + 0.5f));
        height = std::max(1u, static_cast<UINT>(static_cast<float>(height) * fit + 0.5f));
        if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(frame.Get(), width, height, WICBitmapInterpolationModeHighQualityCubic))) return false;
        source = scaler;
    }
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
            WICBitmapPaletteTypeCustom))) return false;
    pixels.resize(static_cast<std::size_t>(width) * height * 4);
    return SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()));
}

std::vector<unsigned char> resource_bytes(const wchar_t* name) {
    const auto instance = GetModuleHandleW(nullptr);
    if (const auto resource = FindResourceW(instance, name, MAKEINTRESOURCEW(10) /* RT_RCDATA */))
        if (const auto loaded = LoadResource(instance, resource))
            if (const auto* data = static_cast<const unsigned char*>(LockResource(loaded)))
                return {data, data + SizeofResource(instance, resource)};
    return {};
}

std::vector<unsigned char> background_bytes(const std::filesystem::path& directory) {
    for (const auto* name : {L"ReSkateLauncher.background.jpg", L"ReSkateLauncher.background.png"}) {
        std::ifstream input(directory / name, std::ios::binary);
        if (input) return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }
    return resource_bytes(L"LAUNCHER_BACKGROUND");
}

} // namespace dingosdk::launcher_gui::detail
