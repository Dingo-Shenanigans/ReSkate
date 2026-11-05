#include "Engine/Core/Log/logging.h"
#include "Extension/Rendering/Capture/capture_readback.h"
#include "overlay_internal.h"
#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>

// Reads a presented frame back from the GPU for a screen capture (overlay.h, FrameGrabHost;
// reskate.capture), from Fit Check (pull request 326). The copy is made on the game's own queue right
// before the overlay draws, and waited for: a capture asks for a few hundred frames at most, and a
// frame's hitch then does not matter.
namespace dingosdk::overlay {
namespace {
std::atomic<const FrameGrabHost*> frame_grab_host{};
}
void set_frame_grab(const FrameGrabHost* host) noexcept { frame_grab_host.store(host); }
} // namespace dingosdk::overlay

namespace dingosdk::overlay::detail {
namespace {
// Render thread only (under the render mutex), like the rest of the overlay's graphics state.
struct Grab {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Resource> readback;
    UINT64 readback_size{};
    std::vector<std::uint8_t> converted; // 8-bit copy of a frame in a wider format
    DXGI_FORMAT warned{DXGI_FORMAT_UNKNOWN};
    unsigned timing_frames{};
    double gpu_ms{}, colour_ms{}, record_ms{};
};
Grab& grab() {
    static auto* value = new Grab;
    return *value;
} // process lifetime

const FrameGrabHost* host() { return frame_grab_host.load(); }
} // namespace

bool frame_grab_pending() {
    const auto* h = host();
    if (!h || !h->active)
        return false;
    try {
        return h->active();
    } catch (...) {
        return false;
    }
}

void release_frame_grab() {
    auto& g = grab();
    g.commands.Reset();
    g.allocator.Reset();
    g.readback.Reset();
    g.readback_size = 0;
    g.converted = {};
    g.timing_frames = 0;
    g.gpu_ms = g.colour_ms = g.record_ms = 0;
}

void grab_presented_frame(State& s, ID3D12Resource* buffer) {
    const auto* h = host();
    if (!h || !h->wants_frame || !h->take_frame || !buffer || !s.device || !s.queue || !s.fence)
        return;
    bool wanted = false;
    try {
        wanted = h->wants_frame();
    } catch (...) {
    }
    if (!wanted)
        return;
    auto& g = grab();
    const auto started = std::chrono::steady_clock::now();
    const auto description = buffer->GetDesc();
    const auto format = description.Format;
    const bool eight_bit = format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                           format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const bool ten_bit = format == DXGI_FORMAT_R10G10B10A2_UNORM;
    const bool half = format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (!eight_bit && !ten_bit && !half) {
        if (g.warned != format)
            logging::printf(logging::Level::warning, logging::Channel::graphics, "Capture: the backbuffer format %u cannot be read back.",
                            static_cast<unsigned>(format));
        g.warned = format;
        h->take_frame({});
        return;
    }
    if ((ten_bit || half) && g.warned != format) {
        // An HDR swapchain: the picture is converted, but its colours will not match what SDR shows.
        logging::printf(logging::Level::warning, logging::Channel::graphics,
                        "Capture: the game is presenting in HDR (format %u); captured colours are converted to SDR and may look off.",
                        static_cast<unsigned>(format));
        g.warned = format;
    }
    const auto source_width = static_cast<std::uint32_t>(description.Width), source_height = description.Height;
    auto region = h->region ? h->region(source_width, source_height) : GrabRegion{0, 0, source_width, source_height};
    if (region.x >= source_width || region.y >= source_height || !region.width || !region.height) {
        h->take_frame({});
        return;
    }
    region.width = std::min(region.width, source_width - region.x);
    region.height = std::min(region.height, source_height - region.y);
    auto copied_description = description;
    copied_description.Width = region.width;
    copied_description.Height = region.height;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT rows{};
    UINT64 row_bytes{}, total{};
    s.device->GetCopyableFootprints(&copied_description, 0, 1, 0, &layout, &rows, &row_bytes, &total);
    const auto fail = [&](const char* what) {
        logging::printf(logging::Level::warning, logging::Channel::graphics, "Capture: frame read-back failed at %s.", what);
        try {
            h->take_frame({});
        } catch (...) {
        }
    };
    if (!total || rows != region.height)
        return fail("its layout");
    if (!g.allocator && FAILED(s.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.allocator))))
        return fail("CreateCommandAllocator");
    if (!g.commands) {
        if (FAILED(s.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocator.Get(), nullptr, IID_PPV_ARGS(&g.commands))))
            return fail("CreateCommandList");
        g.commands->Close();
    }
    if (g.readback_size < total) {
        g.readback.Reset();
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bytes{};
        bytes.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bytes.Width = total;
        bytes.Height = 1;
        bytes.DepthOrArraySize = 1;
        bytes.MipLevels = 1;
        bytes.SampleDesc.Count = 1;
        bytes.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(s.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bytes, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&g.readback))))
            return fail("CreateCommittedResource");
        g.readback_size = total;
    }
    if (FAILED(g.allocator->Reset()) || FAILED(g.commands->Reset(g.allocator.Get(), nullptr)))
        return fail("Reset");
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = buffer;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g.commands->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION to{}, from{};
    to.pResource = g.readback.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = layout;
    from.pResource = buffer;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.SubresourceIndex = 0;
    const D3D12_BOX box{region.x, region.y, 0, region.x + region.width, region.y + region.height, 1};
    g.commands->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    g.commands->ResourceBarrier(1, &barrier);
    if (FAILED(g.commands->Close()))
        return fail("Close");
    ID3D12CommandList* lists[] = {g.commands.Get()};
    s.queue->ExecuteCommandLists(1, lists);
    const UINT64 signal = ++s.next_fence;
    if (FAILED(s.queue->Signal(s.fence.Get(), signal)) || !completed(signal, render_fence_timeout_ms))
        return fail("the GPU wait");
    void* mapped{};
    const D3D12_RANGE range{0, static_cast<SIZE_T>(total)};
    if (FAILED(g.readback->Map(0, &range, &mapped)) || !mapped)
        return fail("Map");
    const auto* pixels = static_cast<const std::uint8_t*>(mapped) + layout.Offset;
    const auto width = region.width, height = region.height;
    const auto colour_started = std::chrono::steady_clock::now();
    GrabbedFrame frame{pixels, width, height, layout.Footprint.RowPitch,
                       format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
    if (!eight_bit) {
        capture::readback_rgba(pixels, width, height, layout.Footprint.RowPitch,
                               ten_bit ? capture::ReadbackColour::rgb10 : capture::ReadbackColour::half_float, g.converted);
        frame = {g.converted.data(), width, height, width * 4, false};
    }
    frame.origin_x = region.x;
    frame.origin_y = region.y;
    frame.source_width = source_width;
    frame.source_height = source_height;
    // The depth of this frame (a capture cuts what it shows out by it), when there is one.
    std::uint32_t depth_width{}, depth_height{};
    frame.depth = depth_tap_latest(depth_width, depth_height, frame.depth_pitch);
    if (frame.depth && (depth_width != source_width || depth_height != source_height))
        frame.depth = nullptr;
    const auto record_started = std::chrono::steady_clock::now();
    try {
        h->take_frame(frame);
    } catch (...) {
    }
    const auto finished = std::chrono::steady_clock::now();
    g.gpu_ms += std::chrono::duration<double, std::milli>(colour_started - started).count();
    g.colour_ms += std::chrono::duration<double, std::milli>(record_started - colour_started).count();
    g.record_ms += std::chrono::duration<double, std::milli>(finished - record_started).count();
    if (++g.timing_frames >= 60) {
        logging::printf(logging::Level::info, logging::Channel::graphics,
                        "Capture frame timing (%ux%u crop from %ux%u, format %u): GPU readback %.2f ms, colour conversion %.2f ms, "
                        "recording %.2f ms (60 frames).",
                        width, height, source_width, source_height, static_cast<unsigned>(format), g.gpu_ms / 60, g.colour_ms / 60,
                        g.record_ms / 60);
        g.timing_frames = 0;
        g.gpu_ms = g.colour_ms = g.record_ms = 0;
    }
    depth_tap_done();
    const D3D12_RANGE nothing{};
    g.readback->Unmap(0, &nothing);
}
} // namespace dingosdk::overlay::detail
