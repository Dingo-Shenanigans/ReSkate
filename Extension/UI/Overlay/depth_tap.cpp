#include "overlay_internal.h"
#include "Engine/Core/Log/logging.h"
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

// The game's depth for screen captures (reskate.capture). ID3D12GraphicsCommandList::ResourceBarrier
// (slot 26) is hooked once, when the overlay starts (through its own command list: every list of
// the runtime shares it). Unarmed, the hook only passes the call on. In development builds the
// depth probe (depth_probe.cpp, `capture depth`) also looks at every barrier while it is armed.
namespace dingosdk::overlay {
namespace {
using detail::ResourceBarrierFn;
using detail::depth_capable;
std::atomic<ResourceBarrierFn> original_barrier{};

// The depth tap: while a capture records (reskate.capture), the game's main depth buffer (the screen's size, the
// first one each frame that goes from being written to being read, found with `capture depth`,
// 2026-10-21) is copied each frame into one of a few readback buffers, with a number after it so
// that the copy the GPU has made can be told from one still waiting.
struct TapSlot {
    Microsoft::WRL::ComPtr<ID3D12Resource> readback, marker;
    std::uint32_t* marker_cpu{};
    std::uint32_t seq{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT64 total{};
};
struct Tap {
    std::mutex mutex;
    std::atomic<bool> on{};
    std::vector<TapSlot> slots;
    std::size_t next{};
    std::uint32_t seq{};
    std::uint64_t epoch{}, copied_epoch{~std::uint64_t{0}};
    std::uint32_t width{}, height{};
    UINT64 slot_size{};
    unsigned idle{};               // presents since a grab last wanted it
    TapSlot* mapped{};             // the copy handed out by depth_tap_latest
    bool said{};
};
Tap& tap() { static auto* value = new Tap; return *value; }

constexpr std::size_t tap_slots = 4;

void tap_barrier(ID3D12GraphicsCommandList* list, const D3D12_RESOURCE_BARRIER& b, ResourceBarrierFn original) {
    auto& t = tap();
    if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !b.Transition.pResource) return;
    if (!(b.Transition.StateBefore & D3D12_RESOURCE_STATE_DEPTH_WRITE)) return;
    constexpr auto read = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (!(b.Transition.StateAfter & read)) return;
    if (b.Flags & D3D12_RESOURCE_BARRIER_FLAG_END_ONLY) return;
    if (b.Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES && b.Transition.Subresource != 0) return;
    std::lock_guard lock(t.mutex);
    if (t.slots.empty() || t.copied_epoch == t.epoch) return;
    const auto desc = b.Transition.pResource->GetDesc();
    if (!depth_capable(desc) || desc.Width != t.width || desc.Height != t.height || desc.DepthOrArraySize != 1) return;
    if (desc.Format != DXGI_FORMAT_R32G8X24_TYPELESS && desc.Format != DXGI_FORMAT_D32_FLOAT_S8X24_UINT &&
        desc.Format != DXGI_FORMAT_R32_TYPELESS && desc.Format != DXGI_FORMAT_D32_FLOAT)
        return;
    if (list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) return;
    auto& slot = t.slots[t.next];
    t.next = (t.next + 1) % t.slots.size();
    slot.layout = {};
    slot.layout.Footprint.Format = DXGI_FORMAT_R32_TYPELESS;
    slot.layout.Footprint.Width = static_cast<UINT>(desc.Width);
    slot.layout.Footprint.Height = desc.Height;
    slot.layout.Footprint.Depth = 1;
    slot.layout.Footprint.RowPitch = (static_cast<UINT>(desc.Width) * 4 + 255) / 256 * 256;
    slot.total = std::uint64_t{slot.layout.Footprint.RowPitch} * desc.Height;
    if (slot.total + 4 > t.slot_size) return;
    D3D12_RESOURCE_BARRIER ours[2]{};
    ours[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    ours[0].Transition.pResource = b.Transition.pResource;
    ours[0].Transition.Subresource = b.Transition.Subresource;
    ours[0].Transition.StateBefore = b.Transition.StateBefore;
    ours[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ours[1] = ours[0];
    std::swap(ours[1].Transition.StateBefore, ours[1].Transition.StateAfter);
    original(list, 1, &ours[0]);
    D3D12_TEXTURE_COPY_LOCATION to{}, from{};
    to.pResource = slot.readback.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = slot.layout;
    from.pResource = b.Transition.pResource;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.SubresourceIndex = 0;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    original(list, 1, &ours[1]);
    // The number, after the copy on the same list: when it is in the readback buffer, so is the copy.
    slot.seq = ++t.seq;
    *slot.marker_cpu = slot.seq;
    list->CopyBufferRegion(slot.readback.Get(), slot.total, slot.marker.Get(), 0, 4);
    t.copied_epoch = t.epoch;
}

void STDMETHODCALLTYPE hooked_barrier(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
    const auto original = original_barrier.load();
    if (tap().on.load(std::memory_order_relaxed) && list && barriers) {
        try {
            for (UINT i = 0; i < count; ++i) tap_barrier(list, barriers[i], original);
        } catch (...) {}
    }
#if defined(DINGOSDK_DEV_TOOLS)
    detail::depth_probe_barrier(list, count, barriers, original);
#endif
    original(list, count, barriers);
}
} // namespace

namespace detail {
bool depth_capable(const D3D12_RESOURCE_DESC& d) {
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) && d.SampleDesc.Count == 1;
}

void install_depth_tap(ID3D12GraphicsCommandList* list) noexcept {
    static std::atomic<bool> installed{};
    if (!list || installed.exchange(true)) return;
    auto* const table = *reinterpret_cast<void***>(list);
    void* original{};
    const auto prepared = hook_prepare(table[26], reinterpret_cast<void*>(&hooked_barrier), &original);
    const auto prepare_failure = hook_last_failure();
    if (prepared != HookOk) {
        logging::printf(logging::Level::warning, logging::Channel::graphics,
            "Capture depth: ResourceBarrier hook_prepare failed at %p: %s (%ld); operation=%s error=%ld thread=%lu.",
            table[26], hook_status_string(prepared), static_cast<long>(prepared), prepare_failure.operation,
            prepare_failure.error, prepare_failure.thread_id);
        return;
    }
    original_barrier.store(reinterpret_cast<ResourceBarrierFn>(original));
    const auto enabled = hook_enable(table[26]);
    const auto enable_failure = hook_last_failure();
    if (enabled != HookOk) {
        original_barrier.store(nullptr);
        logging::printf(logging::Level::warning, logging::Channel::graphics,
            "Capture depth: ResourceBarrier hook_enable failed at %p: %s (%ld); operation=%s error=%ld thread=%lu.",
            table[26], hook_status_string(enabled), static_cast<long>(enabled), enable_failure.operation,
            enable_failure.error, enable_failure.thread_id);
    } else {
        logging::printf(logging::Level::info, logging::Channel::graphics,
            "Capture depth: ResourceBarrier hook installed at %p.", table[26]);
    }
}

bool depth_barrier_hooked() noexcept { return original_barrier.load() != nullptr; }

bool depth_tap_pending() {
    auto& t = tap();
    std::lock_guard lock(t.mutex);
    return !t.slots.empty(); // presents until its buffers are let go
}

void depth_tap_present(State& s, std::uint32_t width, std::uint32_t height, bool wanted) {
    auto& t = tap();
    if (!original_barrier.load()) return;
    std::lock_guard lock(t.mutex);
    ++t.epoch;
    if (!wanted) {
        t.on.store(false);
        // Let go a few frames after the last grab: the GPU may still be copying into them.
        if (!t.slots.empty() && ++t.idle > 8) t.slots.clear();
        return;
    }
    t.idle = 0;
    if (t.slots.empty() || t.width != width || t.height != height) {
        t.on.store(false);
        t.slots.clear();
        t.width = width, t.height = height;
        t.slot_size = ((std::uint64_t{width} * 4 + 255) / 256 * 256) * height + 256;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bytes{};
        bytes.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bytes.Width = t.slot_size;
        bytes.Height = 1;
        bytes.DepthOrArraySize = 1;
        bytes.MipLevels = 1;
        bytes.SampleDesc.Count = 1;
        bytes.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES upload{};
        upload.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC marker_desc = bytes;
        marker_desc.Width = 256;
        for (std::size_t i = 0; i < tap_slots; ++i) {
            TapSlot slot;
            void* cpu{};
            if (FAILED(s.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bytes, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&slot.readback))) ||
                FAILED(s.device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &marker_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&slot.marker))) ||
                FAILED(slot.marker->Map(0, nullptr, &cpu)) || !cpu) {
                if (!t.said)
                    logging::printf(logging::Level::warning, logging::Channel::graphics,
                                    "Capture depth: no buffers for the depth copies; cutting out by colour.");
                t.said = true;
                t.slots.clear();
                return;
            }
            slot.marker_cpu = static_cast<std::uint32_t*>(cpu);
            *slot.marker_cpu = 0;
            t.slots.push_back(std::move(slot));
        }
        t.next = 0;
    }
    t.on.store(true);
}

const std::uint8_t* depth_tap_latest(std::uint32_t& width, std::uint32_t& height, std::uint32_t& pitch) {
    auto& t = tap();
    std::lock_guard lock(t.mutex);
    t.mapped = nullptr;
    TapSlot* best{};
    for (auto& slot : t.slots) {
        if (!slot.seq || !slot.readback) continue;
        void* cpu{};
        const D3D12_RANGE tail{static_cast<SIZE_T>(slot.total), static_cast<SIZE_T>(slot.total + 4)};
        if (FAILED(slot.readback->Map(0, &tail, &cpu)) || !cpu) continue;
        std::uint32_t written{};
        std::memcpy(&written, static_cast<const std::uint8_t*>(cpu) + slot.total, 4);
        const D3D12_RANGE nothing{};
        slot.readback->Unmap(0, &nothing);
        if (written == slot.seq && (!best || slot.seq > best->seq)) best = &slot;
    }
    if (!best) return nullptr;
    void* cpu{};
    const D3D12_RANGE all{0, static_cast<SIZE_T>(best->total)};
    if (FAILED(best->readback->Map(0, &all, &cpu)) || !cpu) return nullptr;
    t.mapped = best;
    width = best->layout.Footprint.Width, height = best->layout.Footprint.Height, pitch = best->layout.Footprint.RowPitch;
    return static_cast<const std::uint8_t*>(cpu);
}

void depth_tap_done() {
    auto& t = tap();
    std::lock_guard lock(t.mutex);
    if (!t.mapped) return;
    const D3D12_RANGE nothing{};
    t.mapped->readback->Unmap(0, &nothing);
    t.mapped = nullptr;
}
} // namespace detail
} // namespace dingosdk::overlay
