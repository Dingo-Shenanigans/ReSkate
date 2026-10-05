// Depth stereo, step 1: find the game's scene depth buffer. D3D12CreateDevice is
// hooked at startup; on the game's device, CreateDepthStencilView (descriptor ->
// resource) and ClearDepthStencilView (clears per depth surface, clear value)
// are hooked. These are the runtime's own functions, so calls that pass through
// Streamline or ReShade wrappers are seen too.
#include "vr.h"
#include "vr_stereo.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"

#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace dingosdk::vr {
namespace {
using Microsoft::WRL::ComPtr;
using CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using CreateDsv = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*,
    D3D12_CPU_DESCRIPTOR_HANDLE);
using ClearDsv = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT,
    UINT8, UINT, const D3D12_RECT*);

std::atomic<CreateDevice> original_create_device{};
std::atomic<CreateDsv> original_create_dsv{};
std::atomic<ClearDsv> original_clear_dsv{};
std::atomic<bool> device_hooked{};

struct Surface {
    ID3D12Resource* resource = nullptr; // Not owned: identity and description only.
    std::uint64_t width = 0;
    std::uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::uint64_t clears = 0;
    std::uint64_t last_clear = 0; // present count at its last clear
    float clear_depth = 0;
};
// Presents seen; a surface not cleared in the last few presents is gone (a map change
// recreates the depth targets) or unused, and is not picked.
std::atomic<std::uint64_t> present_count{0};
constexpr std::uint64_t recent_presents = 60;
struct Probe {
    std::mutex mutex;
    std::unordered_map<SIZE_T, ID3D12Resource*> views; // DSV descriptor -> resource
    std::unordered_map<ID3D12Resource*, Surface> surfaces;
};
Probe& probe() {
    static auto* value = new Probe;
    return *value;
}

void STDMETHODCALLTYPE create_dsv(ID3D12Device* device, ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
    D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    original_create_dsv.load(std::memory_order_acquire)(device, resource, desc, handle);
    if (!resource) return;
    try {
        const auto resource_desc = resource->GetDesc();
        auto& p = probe();
        std::lock_guard lock(p.mutex);
        p.views[handle.ptr] = resource;
        auto& surface = p.surfaces[resource];
        surface.resource = resource;
        surface.width = resource_desc.Width;
        surface.height = resource_desc.Height;
        surface.format = resource_desc.Format;
    } catch (...) {}
}

// Step 2: the main depth's previous-frame contents, copied at its clear (plane 0).
struct DepthCopy {
    std::mutex mutex;
    ComPtr<ID3D12Resource> texture; // R32_TYPELESS; stays in COPY_DEST between uses.
    ComPtr<ID3D12Resource> staging; // a buffer with the depth plane's rows; stays in COPY_DEST
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    // Copies replaced after a size change, with the present count they may be released at:
    // the GPU can still be using them for a frame or two.
    std::vector<std::pair<std::uint64_t, ComPtr<ID3D12Resource>>> retired;
    std::atomic<ID3D12Resource*> source{};
    std::atomic<bool> enabled{};
    std::atomic<std::uint64_t> copies{};
};
DepthCopy& depth_copy() {
    static auto* value = new DepthCopy;
    return *value;
}
// The format the depth plane is copied into (and read as R32_FLOAT). D32_FLOAT_S8X24 keeps
// depth and stencil in separate planes: plane 0 is plain 32-bit depth, so it goes into
// R32_TYPELESS too. (An R32G8X24 copy receives the plane's 4-byte texels packed two to each
// 8-byte texel, and reads back as the scene squeezed twice side by side.)
DXGI_FORMAT typeless(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_TYPELESS;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
// Records the copy into the game's list just before it clears the depth: the
// clear requires DEPTH_WRITE, so that is the state here. Only plane 0 (depth), through a
// buffer: depth plane -> buffer rows -> R32 texture, the copies every format allows (a
// direct copy from a planar depth-stencil to another format is not one of them).
void copy_before_clear(ID3D12GraphicsCommandList* list, ID3D12Resource* depth) {
    auto& c = depth_copy();
    std::lock_guard lock(c.mutex);
    const auto desc = depth->GetDesc();
    const auto format = typeless(desc.Format);
    if (format == DXGI_FORMAT_UNKNOWN) return;
    if (!c.texture || !c.staging || c.texture->GetDesc().Width != desc.Width || c.texture->GetDesc().Height != desc.Height ||
        c.format != desc.Format) {
        // Keep the old copies alive for 8 presents: the GPU may still be reading them.
        const auto retire_at = present_count.load(std::memory_order_relaxed) + 8;
        if (c.texture) c.retired.emplace_back(retire_at, std::move(c.texture));
        if (c.staging) c.retired.emplace_back(retire_at, std::move(c.staging));
        c.texture.Reset();
        c.staging.Reset();
        ComPtr<ID3D12Device> device;
        if (FAILED(list->GetDevice(IID_PPV_ARGS(&device)))) return;
        // The depth plane's rows in the buffer: 4-byte texels, rows aligned to 256 bytes (the
        // footprint query rejects some depth-stencil descriptions, so it is laid out here).
        c.footprint = {};
        c.footprint.Footprint.Format = DXGI_FORMAT_R32_TYPELESS;
        c.footprint.Footprint.Width = static_cast<UINT>(desc.Width);
        c.footprint.Footprint.Height = desc.Height;
        c.footprint.Footprint.Depth = 1;
        c.footprint.Footprint.RowPitch = (static_cast<UINT>(desc.Width) * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
            ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
        const UINT64 total = static_cast<UINT64>(c.footprint.Footprint.RowPitch) * desc.Height;
        static bool reported = false;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = total;
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        auto copy_desc = desc;
        copy_desc.Format = format;
        copy_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        copy_desc.MipLevels = 1;
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        const HRESULT made_buffer = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&c.staging));
        const HRESULT made_texture = SUCCEEDED(made_buffer) ? device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &copy_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&c.texture)) : E_ABORT;
        if (FAILED(made_buffer) || FAILED(made_texture)) {
            if (!reported) {
                reported = true;
                logging::write(logging::Level::warning, logging::Channel::graphics,
                    std::format("VR: depth copy unavailable: buffer {:#x} ({} bytes), texture {:#x} ({}x{} format {}).",
                        static_cast<unsigned long>(made_buffer), total, static_cast<unsigned long>(made_texture), desc.Width, desc.Height,
                        static_cast<int>(copy_desc.Format)));
            }
            c.staging.Reset();
            c.texture.Reset();
            return;
        }
        c.format = desc.Format;
    }
    std::array<D3D12_RESOURCE_BARRIER, 2> barriers{};
    for (auto& barrier : barriers) barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition = {depth, 0, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, barriers.data());
    // Depth plane -> buffer, in the plane's own footprint.
    D3D12_TEXTURE_COPY_LOCATION rows{c.staging.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    rows.PlacedFootprint = c.footprint;
    D3D12_TEXTURE_COPY_LOCATION plane{depth, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    plane.SubresourceIndex = 0;
    list->CopyTextureRegion(&rows, 0, 0, 0, &plane, nullptr);
    barriers[0].Transition = {depth, 0, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE};
    barriers[1].Transition = {c.staging.Get(), 0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(2, barriers.data());
    // Buffer -> R32 texture: the same 4-byte rows, read as R32_TYPELESS.
    D3D12_TEXTURE_COPY_LOCATION source_rows = rows;
    D3D12_TEXTURE_COPY_LOCATION target{c.texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    target.SubresourceIndex = 0;
    list->CopyTextureRegion(&target, 0, 0, 0, &source_rows, nullptr);
    barriers[0].Transition = {c.staging.Get(), 0, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST};
    list->ResourceBarrier(1, barriers.data());
    c.copies.fetch_add(1, std::memory_order_relaxed);
}

void STDMETHODCALLTYPE clear_dsv(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE handle, D3D12_CLEAR_FLAGS flags,
    FLOAT depth, UINT8 stencil, UINT count, const D3D12_RECT* rects) {
    try {
        if (flags & D3D12_CLEAR_FLAG_DEPTH) {
            ID3D12Resource* resource = nullptr;
            {
                auto& p = probe();
                std::lock_guard lock(p.mutex);
                if (const auto view = p.views.find(handle.ptr); view != p.views.end())
                    if (const auto surface = p.surfaces.find(view->second); surface != p.surfaces.end()) {
                        ++surface->second.clears;
                        surface->second.last_clear = present_count.load(std::memory_order_relaxed);
                        surface->second.clear_depth = depth;
                        resource = view->second;
                    }
            }
            auto& c = depth_copy();
            if (resource && c.enabled.load(std::memory_order_acquire) && resource == c.source.load(std::memory_order_acquire))
                copy_before_clear(list, resource);
        }
    } catch (...) {}
    original_clear_dsv.load(std::memory_order_acquire)(list, handle, flags, depth, stencil, count, rects);
}

void hook_device(ID3D12Device* device) {
    if (device_hooked.exchange(true)) return;
    auto** table = *reinterpret_cast<void***>(device);
    void* trampoline = nullptr;
    if (hook_prepare(table[21], reinterpret_cast<void*>(&create_dsv), &trampoline) == HookOk && trampoline) {
        original_create_dsv.store(reinterpret_cast<CreateDsv>(trampoline), std::memory_order_release);
        (void)hook_enable(table[21]);
    }
    // The command list's functions: from a throwaway list on the same device.
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
        SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)))) {
        auto** list_table = *reinterpret_cast<void***>(list.Get());
        trampoline = nullptr;
        if (hook_prepare(list_table[47], reinterpret_cast<void*>(&clear_dsv), &trampoline) == HookOk && trampoline) {
            original_clear_dsv.store(reinterpret_cast<ClearDsv>(trampoline), std::memory_order_release);
            (void)hook_enable(list_table[47]);
        }
        list->Close();
    }
    logging::write(logging::Level::info, logging::Channel::graphics,
        std::format("VR: depth probe on the game's D3D12 device (depth views {}, depth clears {}).",
            original_create_dsv.load() ? "hooked" : "NOT hooked", original_clear_dsv.load() ? "hooked" : "NOT hooked"));
}

HRESULT WINAPI create_device(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID riid, void** output) {
    const HRESULT result = original_create_device.load(std::memory_order_acquire)(adapter, level, riid, output);
    if (SUCCEEDED(result) && output && *output) {
        try {
            ComPtr<ID3D12Device> device;
            if (SUCCEEDED(static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&device)))) hook_device(device.Get());
        } catch (...) {}
    }
    return result;
}
}

void start_depth_probe() noexcept {
    try {
        const HMODULE module = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        auto* target = module ? reinterpret_cast<void*>(GetProcAddress(module, "D3D12CreateDevice")) : nullptr;
        void* trampoline = nullptr;
        if (target && hook_prepare(target, reinterpret_cast<void*>(&create_device), &trampoline) == HookOk && trampoline) {
            original_create_device.store(reinterpret_cast<CreateDevice>(trampoline), std::memory_order_release);
            if (hook_enable(target) == HookOk) return;
        }
        logging::write(logging::Level::warning, logging::Channel::graphics, "VR: depth probe could not hook D3D12CreateDevice.");
    } catch (...) {}
}


namespace detail {
ComPtr<ID3D12Resource> depth_texture() {
    auto& c = depth_copy();
    std::lock_guard lock(c.mutex);
    return c.texture;
}
}


void depth_on_present(std::uint64_t width, std::uint32_t height) noexcept {
    try {
        auto& c = depth_copy();
        const auto now = present_count.fetch_add(1, std::memory_order_relaxed) + 1;
        {
            // Release copies replaced by a size change once the GPU is done with them.
            std::lock_guard lock(c.mutex);
            std::erase_if(c.retired, [now](const auto& entry) { return entry.first <= now; });
        }
        // The scene depth: the largest D32 surface with the image's shape, cleared to 0
        // (reversed Z) in the last few presents, then the one cleared most. With upscaling or a resolution scale the
        // game renders (and its depth is) smaller than the image; the stereo pass samples
        // depth by screen position, so any scale works.
        {
            auto& p = probe();
            std::lock_guard lock(p.mutex);
            const Surface* best = nullptr;
            const double aspect = height ? static_cast<double>(width) / height : 0;
            for (const auto& [resource, surface] : p.surfaces) {
                if (!surface.height || surface.clear_depth != 0.0f || typeless(surface.format) == DXGI_FORMAT_UNKNOWN ||
                    now - surface.last_clear > recent_presents ||
                    surface.width > width || std::abs(static_cast<double>(surface.width) / surface.height - aspect) > 0.01 * aspect)
                    continue;
                const auto area = surface.width * surface.height;
                if (!best || area > best->width * best->height || (area == best->width * best->height && surface.clears > best->clears))
                    best = &surface;
            }
            c.source.store(best ? best->resource : nullptr, std::memory_order_release);
        }
    } catch (...) {}
}
void set_depth_copy(bool enabled) noexcept { depth_copy().enabled.store(enabled, std::memory_order_release); }
}
