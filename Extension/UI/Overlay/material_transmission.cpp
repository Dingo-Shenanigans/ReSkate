#include "material_transmission.h"
#include "Engine/Core/Log/logging.h"
#include "material_mask_formats.h"
#include "material_transmission_hlsl.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <d3dcompiler.h>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <wrl/client.h>
namespace dingosdk::overlay
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr GUID base_interface{0xadec44e2, 0x61f0, 0x45c3, {0xad, 0x9f, 0x1b, 0x37, 0x37, 0x92, 0x84, 0xff}};
template <class T> ComPtr<T> plain(T *object)
{
    ComPtr<T> result = object;
    if (!object)
        return result;
    ComPtr<IUnknown> under;
    if (SUCCEEDED(object->QueryInterface(base_interface, reinterpret_cast<void **>(under.GetAddressOf()))))
    {
        ComPtr<T> native;
        if (SUCCEEDED(under.As(&native)))
            result = native;
    }
    return result;
}
ComPtr<IUnknown> identity(IUnknown *object)
{
    ComPtr<IUnknown> result;
    if (object)
        (void)object->QueryInterface(IID_PPV_ARGS(&result));
    return result;
}
bool same_device(ID3D12Device *a, ID3D12Device *b)
{
    auto x = plain(a), y = plain(b);
    return identity(x.Get()).Get() == identity(y.Get()).Get();
}
void check(HRESULT hr)
{
    if (FAILED(hr))
        throw std::runtime_error("D3D12 transmission operation failed");
}
void say(const char *text)
{
    logging::write(logging::Level::info, logging::Channel::graphics, text);
}
bool complete(ID3D12Fence *fence, UINT64 value)
{
    if (!value)
        return true;
    if (!fence)
        return false;
    const auto done = fence->GetCompletedValue();
    return done != ~UINT64{} && done >= value;
}
void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after, UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
{
    if (before == after)
        return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {resource, subresource, before, after};
    list->ResourceBarrier(1, &b);
}
ComPtr<ID3D12Resource> make_resource(ID3D12Device *device, D3D12_RESOURCE_DESC desc, D3D12_HEAP_TYPE type,
                                     D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    ComPtr<ID3D12Resource> resource;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource)));
    return resource;
}
D3D12_RESOURCE_DESC buffer_desc(UINT64 bytes)
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}
D3D12_RESOURCE_DESC image_desc(UINT width, UINT height, DXGI_FORMAT format,
                               D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Flags = flags;
    return d;
}
struct Use
{
    ComPtr<ID3D12Fence> fence;
    UINT64 value{};
};
struct DepthDescriptor
{
    ComPtr<ID3D12Resource> resource;
    D3D12_DSV_FLAGS flags{};
};
struct DepthFrame
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Resource> source, buffer, texture;
    ComPtr<IUnknown> source_id;
    ComPtr<ID3D12Fence> fence;
    UINT64 generation{}, epoch{}, serial{};
    ID3D12CommandList *list{};
    bool recording{}, submitted{}, released{};
    UINT width{}, height{};
    D3D12_RESOURCE_STATES texture_state{D3D12_RESOURCE_STATE_COPY_DEST}, buffer_state{D3D12_RESOURCE_STATE_COPY_DEST};
    std::vector<Use> consumers;
};
struct MaskFrame
{
    ComPtr<ID3D12Device> device;
    std::array<ComPtr<ID3D12Resource>, 8> images;
    ComPtr<ID3D12DescriptorHeap> rtvs;
    std::array<DXGI_FORMAT, 8> formats{};
    UINT targets{}, width{}, height{};
    ComPtr<ID3D12Resource> depth;
    ComPtr<IUnknown> depth_id;
    D3D12_CPU_DESCRIPTOR_HANDLE source_colour{};
    bool readonly_depth{};
    ComPtr<ID3D12Fence> mask_fence, use_fence;
    UINT64 generation{}, epoch{}, serial{};
    ID3D12CommandList *list{};
    bool recording{}, submitted{}, released{}, used{}, abandoned{};
    std::array<float, 8> parameters{};
    D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_RENDER_TARGET};
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Resource> colour;
    ComPtr<ID3D12DescriptorHeap> srvs, output;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12RootSignature> root;
    DXGI_FORMAT colour_format{};
    D3D12_RESOURCE_STATES colour_state{D3D12_RESOURCE_STATE_COPY_DEST};
};
struct Transmission
{
    std::mutex mutex;
    std::atomic<bool> configured{};
    std::atomic<bool> failed{};
    UINT width{}, height{};
    UINT64 epoch{1}, serial{}, recent_mask_epoch{};
    std::array<MaskFrame, 2> masks;
    std::array<DepthFrame, 3> depths;
    std::map<SIZE_T, DepthDescriptor> descriptors;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    DXGI_FORMAT format{};
};
Transmission &state()
{
    static auto *value = new Transmission;
    return *value;
}
void remember_descriptor(Transmission &s, SIZE_T key, DepthDescriptor descriptor)
{
    // Keep resources alive while their descriptors are tracked, including heap
    // copies. Bound retained references as well as newly allocated render targets.
    const auto estimate = [](const DepthDescriptor &value) {
        const auto d = value.resource->GetDesc();
        return d.Width * d.Height * 8;
    };
    const auto bytes = estimate(descriptor);
    s.descriptors.erase(key);
    static std::atomic<unsigned> registered{};
    if (registered.fetch_add(1) < 8)
    {
        const auto shape = descriptor.resource->GetDesc();
        logging::printf(logging::Level::info, logging::Channel::graphics,
                        "Material transmission: registering DSV 0x%llx, %llux%u.", static_cast<unsigned long long>(key),
                        static_cast<unsigned long long>(shape.Width), shape.Height);
    }
    if (bytes > 256ull * 1024 * 1024)
        return;
    const auto retained = [&] {
        UINT64 total = bytes;
        std::set<ID3D12Resource *> counted{descriptor.resource.Get()};
        for (const auto &[unused, value] : s.descriptors)
        {
            (void)unused;
            if (counted.insert(value.resource.Get()).second)
                total += estimate(value);
        }
        return total;
    };
    while (!s.descriptors.empty() && (s.descriptors.size() >= 512 || retained() > 256ull * 1024 * 1024))
        s.descriptors.erase(s.descriptors.begin());
    s.descriptors.emplace(key, std::move(descriptor));
}
bool mask_free(const MaskFrame &f)
{
    return !f.recording || (f.released && (f.abandoned || complete(f.mask_fence.Get(), f.generation)) &&
                            (!f.used || complete(f.use_fence.Get(), f.generation)));
}
bool depth_free(const DepthFrame &f)
{
    if (!f.recording)
        return true;
    if (!f.released)
        return false;
    if (f.submitted && !complete(f.fence.Get(), f.generation))
        return false;
    for (const auto &use : f.consumers)
        if (!complete(use.fence.Get(), use.value))
            return false;
    return true;
}
void ensure_mask(MaskFrame &f, const MaterialMaskDraw &draw)
{
    if (f.device && !same_device(f.device.Get(), draw.device))
        throw std::runtime_error("mask device changed");
    bool rebuild = f.width != static_cast<UINT>(draw.viewport.Width) ||
                   f.height != static_cast<UINT>(draw.viewport.Height) || f.targets != draw.target_count ||
                   f.formats != draw.formats || !f.device || !same_device(f.device.Get(), draw.device);
    if (!rebuild)
        return;
    if (detail::material_mask_bytes(static_cast<UINT>(draw.viewport.Width), static_cast<UINT>(draw.viewport.Height)) >
        detail::material_mask_byte_limit)
        throw std::runtime_error("material surface exceeds the per-frame memory budget");
    f.images = {};
    f.rtvs.Reset();
    f.device = plain(draw.device);
    f.width = static_cast<UINT>(draw.viewport.Width);
    f.height = static_cast<UINT>(draw.viewport.Height);
    f.targets = draw.target_count;
    f.formats = draw.formats;
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap.NumDescriptors = f.targets;
    check(f.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&f.rtvs)));
    auto handle = f.rtvs->GetCPUDescriptorHandleForHeapStart();
    const auto stride = f.device->GetDescriptorHandleIncrementSize(heap.Type);
    for (UINT i = 0; i < f.targets; ++i)
    {
        auto desc = image_desc(f.width, f.height, i == 3 ? DXGI_FORMAT_R32G32B32A32_TYPELESS : f.formats[i],
                               D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        f.images[i] = make_resource(f.device.Get(), desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = f.formats[i];
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        auto h = handle;
        h.ptr += i * stride;
        f.device->CreateRenderTargetView(f.images[i].Get(), &view, h);
    }
    if (!f.mask_fence)
        check(f.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f.mask_fence)));
    if (!f.use_fence)
        check(f.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f.use_fence)));
    f.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
}
void ensure_depth(DepthFrame &f, ID3D12Device *device, UINT width, UINT height)
{
    if (f.device && !same_device(f.device.Get(), device))
        throw std::runtime_error("depth device changed");
    if (f.texture && f.width == width && f.height == height && same_device(f.device.Get(), device))
        return;
    f.device = plain(device);
    f.width = width;
    f.height = height;
    const UINT pitch = (width * 4 + 255) & ~255u;
    f.buffer = make_resource(f.device.Get(), buffer_desc(UINT64{pitch} * height), D3D12_HEAP_TYPE_DEFAULT,
                             D3D12_RESOURCE_STATE_COPY_DEST);
    f.texture = make_resource(f.device.Get(), image_desc(width, height, DXGI_FORMAT_R32_TYPELESS),
                              D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!f.fence)
        check(f.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f.fence)));
    f.texture_state = f.buffer_state = D3D12_RESOURCE_STATE_COPY_DEST;
}
void load_pipeline(Transmission &s, ID3D12Device *device, DXGI_FORMAT format)
{
    if (s.pipeline && s.format == format && same_device(s.device.Get(), device))
        return;
    const std::string_view source = detail::material_transmission_hlsl;
    ComPtr<ID3DBlob> vs, ps, error;
    check(D3DCompile(source.data(), source.size(), "material_transmission", nullptr, nullptr, "vs_main", "vs_5_0",
                     D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &vs, &error));
    error.Reset();
    const auto compiled = D3DCompile(source.data(), source.size(), "material_transmission", nullptr, nullptr, "ps_main",
                                     "ps_5_0", D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &ps, &error);
    if (FAILED(compiled))
    {
        if (error)
            say(static_cast<const char *>(error->GetBufferPointer()));
        check(compiled);
    }
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 6, 0, 0, 0};
    std::array<D3D12_ROOT_PARAMETER, 2> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = {1, &range};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants = {0, 0, 8};
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC desc{2, parameters.data(), 1, &sampler,
                                   D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ComPtr<ID3DBlob> bytes;
    check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &bytes, &error));
    ComPtr<ID3D12RootSignature> root;
    check(device->CreateRootSignature(0, bytes->GetBufferPointer(), bytes->GetBufferSize(), IID_PPV_ARGS(&root)));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = root.Get();
    p.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    p.BlendState.RenderTarget[0].RenderTargetWriteMask = 15;
    p.SampleMask = UINT_MAX;
    p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    p.RasterizerState.DepthClipEnable = TRUE;
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.NumRenderTargets = 1;
    p.RTVFormats[0] = format;
    p.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pipeline;
    check(device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pipeline)));
    s.device = device;
    s.root = root;
    s.pipeline = pipeline;
    s.format = format;
}
void ensure_output(MaskFrame &f, ID3D12Device *device, ID3D12Resource *buffer)
{
    if (!f.allocator)
    {
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.allocator)));
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, f.allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&f.commands)));
        check(f.commands->Close());
    }
    const auto desc = buffer->GetDesc();
    if (!f.colour || f.colour_format != desc.Format || f.colour->GetDesc().Width != desc.Width ||
        f.colour->GetDesc().Height != desc.Height)
    {
        auto copy = desc;
        copy.Flags = D3D12_RESOURCE_FLAG_NONE;
        f.colour = make_resource(device, copy, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
        f.colour_format = desc.Format;
        f.colour_state = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    if (!f.srvs)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = 6;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&f.srvs)));
        heap = {};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = 1;
        check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&f.output)));
    }
}

} // namespace

void set_material_transmission_enabled(bool enabled) noexcept
{
    state().configured = enabled;
}
bool material_transmission_configured() noexcept
{
    return state().configured.load();
}
bool material_transmission_requested() noexcept
{
    return state().configured.load() && !state().failed;
}
void material_depth_descriptor(ID3D12Resource *resource, const D3D12_DEPTH_STENCIL_VIEW_DESC *desc,
                               D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept
{
    try
    {
        auto &s = state();
        if (!s.configured.load())
            return;
        std::lock_guard lock(s.mutex);
        // A descriptor handle can be reused for a null/unsupported view. Drop its
        // old association even when the replacement cannot supply transmission.
        s.descriptors.erase(handle.ptr);
        if (!resource)
            return;
        const auto shape = resource->GetDesc();
        if (shape.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || shape.MipLevels != 1 ||
            shape.DepthOrArraySize != 1 || shape.SampleDesc.Count != 1)
            return;
        if (desc && desc->ViewDimension != D3D12_DSV_DIMENSION_TEXTURE2D)
            return;
        // Match the formats material_depth_barriers can actually copy. Retaining
        // D24/shadow views we cannot sample evicts the main depth before its draw.
        if (shape.Format != DXGI_FORMAT_R32_TYPELESS && shape.Format != DXGI_FORMAT_D32_FLOAT &&
            shape.Format != DXGI_FORMAT_R32G8X24_TYPELESS && shape.Format != DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
            return;
        if (shape.Width < 64 || shape.Width > 4096 || shape.Height < 64 || shape.Height > 2160)
            return;
        remember_descriptor(s, handle.ptr, {plain(resource), desc ? desc->Flags : D3D12_DSV_FLAG_NONE});
    }
    catch (...)
    {
    }
}
void material_copy_depth_descriptors(ID3D12Device *device, UINT count, D3D12_CPU_DESCRIPTOR_HANDLE destination,
                                     D3D12_CPU_DESCRIPTOR_HANDLE source) noexcept
{
    try
    {
        auto &s = state();
        if (!s.configured.load() || count > 8192)
            return;
        std::lock_guard lock(s.mutex);
        const auto stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        for (UINT i = 0; i < count; ++i)
        {
            const auto found = s.descriptors.find(source.ptr + SIZE_T{i} * stride);
            const auto target = destination.ptr + SIZE_T{i} * stride;
            if (found == s.descriptors.end())
                s.descriptors.erase(target);
            else
                remember_descriptor(s, target, found->second);
        }
    }
    catch (...)
    {
    }
}
void material_list_reset(ID3D12GraphicsCommandList *list) noexcept
{
    try
    {
        auto &s = state();
        if (!s.configured.load())
            return;
        std::lock_guard lock(s.mutex);
        for (auto &f : s.masks)
            if (f.recording && !f.submitted && f.list == list)
            {
                f.abandoned = true;
                f.released = true;
            }
        for (auto &f : s.depths)
            if (f.recording && !f.submitted && f.list == list)
                f.released = true;
    }
    catch (...)
    {
    }
}
void record_material_mask(const MaterialMaskDraw &draw) noexcept
{
    try
    {
        auto &s = state();
        if (!material_transmission_requested())
            return;
        std::lock_guard lock(s.mutex);
        if (draw.target_count != 4 || draw.viewport.TopLeftX != 0 || draw.viewport.TopLeftY != 0 ||
            draw.viewport.Width != static_cast<float>(s.width) || draw.viewport.Height != static_cast<float>(s.height))
            return;
        const auto descriptor = s.descriptors.find(draw.depth.ptr);
        if (descriptor == s.descriptors.end())
        {
            static bool said{};
            if (!said)
            {
                said = true;
                logging::printf(logging::Level::warning, logging::Channel::graphics,
                                "Material transmission: missing DSV 0x%llx for %.0fx%.0f; %zu descriptors retained.",
                                static_cast<unsigned long long>(draw.depth.ptr), draw.viewport.Width,
                                draw.viewport.Height, s.descriptors.size());
                unsigned shown{};
                for (const auto &[handle, value] : s.descriptors)
                {
                    const auto shape = value.resource->GetDesc();
                    logging::printf(logging::Level::info, logging::Channel::graphics,
                                    "Material transmission: retained DSV 0x%llx, %llux%u, format %u.",
                                    static_cast<unsigned long long>(handle),
                                    static_cast<unsigned long long>(shape.Width), shape.Height,
                                    static_cast<unsigned>(shape.Format));
                    if (++shown >= 12)
                        break;
                }
            }
            return;
        }
        auto depth = plain(descriptor->second.resource.Get());
        const auto depth_id = identity(depth.Get());
        MaskFrame *selected{};
        for (auto &f : s.masks)
            if (f.recording && !f.submitted && !f.released && f.list == draw.commands &&
                f.depth_id.Get() == depth_id.Get() && f.source_colour.ptr == draw.source_colour.ptr &&
                f.formats == draw.formats && f.targets == draw.target_count && f.parameters == draw.parameters)
            {
                selected = &f;
                break;
            }
        bool first = false;
        if (!selected)
        {
            for (auto &f : s.masks)
                if (mask_free(f))
                {
                    selected = &f;
                    break;
                }
            if (!selected)
                return;
            auto &f = *selected;
            ensure_mask(f, draw);
            ++f.generation;
            f.epoch = f.serial = 0;
            f.list = draw.commands;
            f.depth = depth;
            f.depth_id = depth_id;
            f.readonly_depth = (descriptor->second.flags & D3D12_DSV_FLAG_READ_ONLY_DEPTH) != 0;
            f.source_colour = draw.source_colour;
            f.parameters = draw.parameters;
            f.recording = true;
            f.submitted = f.released = f.used = f.abandoned = false;
            first = true;
        }
        auto &f = *selected;
        if (first)
        {
            for (UINT i = 0; i < f.targets; ++i)
                transition(draw.commands, f.images[i].Get(), f.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
            const auto stride = f.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            auto handle = f.rtvs->GetCPUDescriptorHandleForHeapStart();
            const float zero[4]{};
            for (UINT i = 0; i < f.targets; ++i)
            {
                auto h = handle;
                h.ptr += i * stride;
                draw.commands->ClearRenderTargetView(h, zero, 0, nullptr);
            }
        }
        draw.replay(f.rtvs->GetCPUDescriptorHandleForHeapStart());
        s.recent_mask_epoch = s.epoch;
        static bool said{};
        if (!said)
        {
            said = true;
            say("Material transmission: material surface mask recorded for a presentation frame.");
        }
    }
    catch (...)
    {
        state().failed = true;
        say("Material transmission: mask recording failed; native picture retained.");
    }
}
void material_depth_barriers(ID3D12GraphicsCommandList *list, UINT count, const D3D12_RESOURCE_BARRIER *barriers,
                             MaterialBarrierFn original) noexcept
{
    try
    {
        auto &s = state();
        if (!material_transmission_requested() || !list || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
            return;
        std::lock_guard lock(s.mutex);
        if (!s.recent_mask_epoch || s.epoch > s.recent_mask_epoch + 2 || !s.width || !s.height)
            return;
        for (UINT index = 0; index < count; ++index)
        {
            const auto &b = barriers[index];
            if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || b.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE ||
                !b.Transition.pResource || !(b.Transition.StateBefore & D3D12_RESOURCE_STATE_DEPTH_WRITE) ||
                !(b.Transition.StateAfter &
                  (D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)) ||
                (b.Transition.Subresource != 0 && b.Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES))
                continue;
            const auto desc = b.Transition.pResource->GetDesc();
            if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 ||
                desc.MipLevels != 1 || desc.DepthOrArraySize != 1 || desc.Width < s.width ||
                desc.Width > s.width + 256 || desc.Height < s.height || desc.Height > s.height + 256 ||
                desc.Width > 4096 || desc.Height > 2160)
                continue;
            if (desc.Format != DXGI_FORMAT_R32_TYPELESS && desc.Format != DXGI_FORMAT_D32_FLOAT &&
                desc.Format != DXGI_FORMAT_R32G8X24_TYPELESS && desc.Format != DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
                continue;
            DepthFrame *selected{};
            for (auto &f : s.depths)
                if (depth_free(f))
                {
                    selected = &f;
                    break;
                }
            if (!selected)
                continue;
            auto &f = *selected;
            ComPtr<ID3D12Device> device;
            check(list->GetDevice(IID_PPV_ARGS(&device)));
            ensure_depth(f, device.Get(), static_cast<UINT>(desc.Width), desc.Height);
            ++f.generation;
            f.source = b.Transition.pResource;
            f.source_id = identity(plain(f.source.Get()).Get());
            f.list = list;
            f.recording = true;
            f.submitted = f.released = false;
            f.epoch = f.serial = 0;
            f.consumers.clear();
            D3D12_RESOURCE_BARRIER source{};
            source.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            source.Transition = b.Transition;
            source.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            original(list, 1, &source);
            transition(list, f.buffer.Get(), f.buffer_state, D3D12_RESOURCE_STATE_COPY_DEST);
            transition(list, f.texture.Get(), f.texture_state, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION target{}, from{};
            target.pResource = f.buffer.Get();
            target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            target.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_TYPELESS, f.width, f.height, 1,
                                                (f.width * 4 + 255) & ~255u};
            from.pResource = f.source.Get();
            from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&target, 0, 0, 0, &from, nullptr);
            transition(list, f.buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            from = target;
            target = {};
            target.pResource = f.texture.Get();
            target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&target, 0, 0, 0, &from, nullptr);
            transition(list, f.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            std::swap(source.Transition.StateBefore, source.Transition.StateAfter);
            original(list, 1, &source);
            static bool said{};
            if (!said)
            {
                said = true;
                say("Material transmission: completed opaque depth copied on its native command list.");
            }
        }
    }
    catch (...)
    {
        state().failed = true;
        say("Material transmission: depth recording failed; native picture retained.");
    }
}
void material_queue_submission(ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists) noexcept
{
    try
    {
        auto &s = state();
        if (!s.configured.load() || s.failed)
            return;
        std::lock_guard lock(s.mutex);
        for (UINT i = 0; i < count; ++i)
        {
            const auto serial = ++s.serial;
            for (auto &f : s.masks)
                if (f.recording && !f.submitted && !f.released && f.list == lists[i])
                {
                    check(queue->Signal(f.mask_fence.Get(), f.generation));
                    f.submitted = true;
                    f.epoch = s.epoch;
                    f.serial = serial;
                    f.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
                }
            for (auto &f : s.depths)
                if (f.recording && !f.submitted && !f.released && f.list == lists[i])
                {
                    check(queue->Signal(f.fence.Get(), f.generation));
                    f.submitted = true;
                    f.epoch = s.epoch;
                    f.serial = serial;
                    f.texture_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                    f.buffer_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
                }
        }
    }
    catch (...)
    {
        state().failed = true;
        say("Material transmission: source submission fence failed.");
    }
}
void present_material_transmission(ID3D12Device *device_arg, ID3D12CommandQueue *queue_arg, ID3D12Resource *buffer_arg,
                                   int transfer) noexcept
{
    try
    {
        auto &s = state();
        if (!s.configured.load())
            return;
        std::lock_guard lock(s.mutex);
        auto device = plain(device_arg);
        auto queue = plain(queue_arg);
        auto buffer = plain(buffer_arg);
        if (!device || !queue || !buffer)
            return;
        const auto shape = buffer->GetDesc();
        if (shape.Width > 4096 || shape.Height > 2160 || shape.SampleDesc.Count != 1)
            return;
        s.width = static_cast<UINT>(shape.Width);
        s.height = shape.Height;
        const auto retire = [&] {
            for (auto &f : s.masks)
                if (f.submitted && f.epoch == s.epoch)
                    f.released = true;
            for (auto &f : s.depths)
                if (f.submitted && f.epoch == s.epoch)
                    f.released = true;
            ++s.epoch;
        };
        if (!s.configured.load() || s.failed)
        {
            retire();
            return;
        }
        if ((transfer != 0 ||
             (shape.Format != DXGI_FORMAT_R10G10B10A2_UNORM && shape.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
              shape.Format != DXGI_FORMAT_B8G8R8A8_UNORM)))
        {
            static bool said{};
            if (!said)
            {
                said = true;
                say("Material transmission: SDR output is required; native HDR picture retained.");
            }
            retire();
            return;
        }
        std::vector<MaskFrame *> masks;
        for (auto &f : s.masks)
            if (f.recording && f.submitted && !f.released && f.epoch == s.epoch && f.width == s.width &&
                f.height == s.height && same_device(f.device.Get(), device.Get()))
                masks.push_back(&f);
        std::sort(masks.begin(), masks.end(), [](const auto *a, const auto *b) { return a->serial < b->serial; });
        for (auto *pointer : masks)
        {
            auto &f = *pointer;
            DepthFrame *selected{};
            for (auto &d : s.depths)
                if (d.recording && d.submitted && d.epoch == s.epoch && d.source_id.Get() == f.depth_id.Get() &&
                    (f.readonly_depth || d.serial >= f.serial) && (!selected || d.serial > selected->serial))
                    selected = &d;
            if (!selected)
            {
                static bool said{};
                if (!said)
                {
                    said = true;
                    say("Material transmission: waiting for completed depth from the mask's view and frame.");
                }
                continue;
            }
            auto &depth = *selected;
            if (!same_device(depth.device.Get(), device.Get()))
                continue;
            load_pipeline(s, device.Get(), shape.Format);
            ensure_output(f, device.Get(), buffer.Get());
            check(f.allocator->Reset());
            check(f.commands->Reset(f.allocator.Get(), nullptr));
            auto *list = f.commands.Get();
            for (UINT i = 0; i < f.targets; ++i)
                transition(list, f.images[i].Get(), f.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            transition(list, buffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(list, f.colour.Get(), f.colour_state, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyResource(f.colour.Get(), buffer.Get());
            transition(list, f.colour.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            transition(list, buffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
            {
                std::array<ID3D12Resource *, 6> inputs{f.colour.Get(),    depth.texture.Get(), f.images[0].Get(),
                                                       f.images[1].Get(), f.images[2].Get(),   f.images[3].Get()};
                const auto stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                auto handle = f.srvs->GetCPUDescriptorHandleForHeapStart();
                for (UINT i = 0; i < 6; ++i)
                {
                    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
                    view.Format = i == 0   ? shape.Format
                                  : i == 1 ? DXGI_FORMAT_R32_FLOAT
                                  : i == 5 ? DXGI_FORMAT_R32G32B32A32_UINT
                                           : f.formats[i - 2];
                    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    view.Texture2D.MipLevels = 1;
                    auto h = handle;
                    h.ptr += i * stride;
                    device->CreateShaderResourceView(inputs[i], &view, h);
                }
                device->CreateRenderTargetView(buffer.Get(), nullptr, f.output->GetCPUDescriptorHandleForHeapStart());
                ID3D12DescriptorHeap *heaps[]{f.srvs.Get()};
                list->SetDescriptorHeaps(1, heaps);
                list->SetGraphicsRootSignature(s.root.Get());
                list->SetGraphicsRootDescriptorTable(0, f.srvs->GetGPUDescriptorHandleForHeapStart());
                list->SetGraphicsRoot32BitConstants(1, 8, f.parameters.data(), 0);
                list->SetPipelineState(s.pipeline.Get());
                list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                D3D12_VIEWPORT viewport{0, 0, static_cast<float>(s.width), static_cast<float>(s.height), 0, 1};
                D3D12_RECT scissor{0, 0, static_cast<LONG>(s.width), static_cast<LONG>(s.height)};
                list->RSSetViewports(1, &viewport);
                list->RSSetScissorRects(1, &scissor);
                transition(list, buffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
                auto output = f.output->GetCPUDescriptorHandleForHeapStart();
                list->OMSetRenderTargets(1, &output, FALSE, nullptr);
                list->DrawInstanced(3, 1, 0, 0);
                transition(list, buffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
                f.root = s.root;
                f.pipeline = s.pipeline;
            }
            check(list->Close());
            depth.consumers.reserve(3);
            depth.consumers.push_back({f.use_fence, f.generation});
            check(queue->Wait(f.mask_fence.Get(), f.generation));
            check(queue->Wait(depth.fence.Get(), depth.generation));
            ID3D12CommandList *lists[]{list};
            queue->ExecuteCommandLists(1, lists);
            check(queue->Signal(f.use_fence.Get(), f.generation));
            f.used = true;
            f.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            f.colour_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            f.released = true;
            static bool said{};
            if (!said)
            {
                said = true;
                say("Material transmission: material transmission compositor submitted with same-frame native inputs.");
            }
        }
        retire();
    }
    catch (...)
    {
        state().failed = true;
        say("Material transmission: presentation failed; effect disabled.");
    }
}
} // namespace dingosdk::overlay
