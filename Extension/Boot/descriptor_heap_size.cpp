#include "descriptor_heap_size.h"

#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Hooks/patches.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/Build/20260929/descriptor_heap_size.h"

#include <Windows.h>
#include <d3d12.h>
#include <array>
#include <atomic>
#include <cstring>

namespace dingosdk {
bool single_heap_layout = false;
namespace {
namespace contract = addr::descriptor_heap_size;

bool supported_image(std::uintptr_t base) noexcept {
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    return base && memory::read(base, dos) && dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0 &&
        dos.e_lfanew <= 0x1000 && memory::read(base + dos.e_lfanew, nt) && nt.Signature == IMAGE_NT_SIGNATURE &&
        nt.OptionalHeader.SizeOfImage == supported_build::game_image_size;
}

template<class T> T field(const void* object, std::size_t at) noexcept {
    T value{};
    std::memcpy(&value, static_cast<const std::uint8_t*>(object) + at, sizeof(value));
    return value;
}

// The shader-visible CBV/SRV/UAV heap's object, for the overflow check.
std::atomic<void*> frame_heap_object{};

// The renderer picks a frame's heap as frame % 3, but the single-heap object has one entry per
// vector. Fill each out to the region count so every frame gets the one heap.
using HeapConstructor = void* (*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
HeapConstructor original_constructor{};
void* heap_constructor(void* self, std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d, std::uint64_t e,
    std::uint64_t f, std::uint64_t g, std::uint64_t h, std::uint64_t i, std::uint64_t j) {
    void* const result = original_constructor(self, a, b, c, d, e, f, g, h, i, j);
    auto* const object = static_cast<std::uint8_t*>(self);
    auto** const heaps = reinterpret_cast<ID3D12DescriptorHeap***>(object + contract::heap_vectors[0]);
    if (heaps[1] == heaps[0] || !heaps[0][0]) return result;
    auto* const heap = heaps[0][0];
    const auto desc = heap->GetDesc();
    // Only the shader-visible CBV/SRV/UAV heap. The sampler and CPU staging heaps already use one
    // heap with a region per frame, as the game made them.
    if (desc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || !(desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)) return result;
    frame_heap_object.store(self);
    const auto regions = field<std::uint32_t>(object, contract::heap_regions);
    if (regions < 2 || regions > 4) return result;
    for (const auto at : contract::heap_vectors) {
        auto** const v = reinterpret_cast<std::uint64_t**>(object + at);
        if (v[1] - v[0] != 1 || v[2] - v[0] < regions) return result;
    }
    for (const auto at : contract::heap_vectors) {
        auto** const v = reinterpret_cast<std::uint64_t**>(object + at);
        for (std::uint32_t k = 1; k < regions; ++k) v[0][k] = v[0][0];
        v[1] = v[0] + regions;
    }
    for (std::uint32_t k = 1; k < regions; ++k) heap->AddRef();
    return result;
}

// The per-frame allocator has no bounds check, so a frame that outgrows its region writes into the
// next one (or past the heap). Said as the next frame begins, at 75%, 90% and each new overflow.
using BeginFrame = void (*)(void*);
BeginFrame original_begin_frame{};
std::uint32_t frame_peak{};
void begin_frame(void* self) {
    if (self == frame_heap_object.load(std::memory_order_relaxed)) {
        const auto per_frame = field<std::uint32_t>(self, contract::heap_per_frame);
        const auto start = field<std::uint32_t>(self, contract::heap_frame_start);
        const auto cursor = field<std::uint32_t>(self, contract::heap_cursor);
        const auto used = cursor > start ? cursor - start : 0;
        if (per_frame && used > frame_peak) {
            const auto share = [per_frame](std::uint64_t n) { return n * 10 >= per_frame * 9ull ? 2 : n * 4 >= per_frame * 3ull ? 1 : 0; };
            const bool crossed = share(used) > share(frame_peak);
            frame_peak = used;
            if (used > per_frame)
                logging::log(logging::Level::error, logging::Channel::graphics,
                    "Descriptor heap: a frame used {} descriptors, past its {}; the next frame's descriptors were overwritten.",
                    used, per_frame);
            else if (crossed)
                logging::log(logging::Level::warning, logging::Channel::graphics, "Descriptor heap: a frame used {} of its {} descriptors.",
                    used, per_frame);
        }
    }
    original_begin_frame(self);
}

template<std::size_t N> bool matches(std::uintptr_t address, const std::array<std::uint8_t, N>& expected) noexcept {
    std::array<std::uint8_t, N> actual{};
    return memory::read_bytes(address, actual.data(), actual.size()) && actual == expected;
}
bool hook(std::uintptr_t address, void* replacement, void** original) noexcept {
    auto* const target = reinterpret_cast<void*>(address);
    return hook_prepare(target, replacement, original) == HookOk && hook_enable(target) == HookOk;
}
std::array<std::uint8_t, contract::cap_load.size()> cap_code(std::uint32_t cap) noexcept {
    // The same divide, with the cap a constant instead of the setting (which is never set):
    //   div r9d / mov edi, eax / mov ecx, cap / or eax, -1 / nop
    std::array<std::uint8_t, contract::cap_load.size()> code{0x41, 0xf7, 0xf1, 0x8b, 0xf8, 0xb9, 0, 0, 0, 0, 0x83, 0xc8, 0xff, 0x90};
    std::memcpy(code.data() + 6, &cap, 4);
    return code;
}
} // namespace

bool start_descriptor_heap_size(std::uintptr_t base, std::string& error) {
    if (!supported_image(base) || !matches(base + contract::cap_load_rva, contract::cap_load) ||
        !matches(base + contract::total_cap_rva, contract::total_cap)) {
        error = "the renderer's descriptor heap setup does not match the supported game";
        return false;
    }
    // The hooks first: the one-heap layout is only patched in once they are in place.
    bool hooked = matches(base + contract::heap_constructor_rva, contract::heap_constructor) &&
        matches(base + contract::begin_frame_rva, contract::begin_frame);
    void* original{};
    if (hooked && (hooked = hook(base + contract::heap_constructor_rva, reinterpret_cast<void*>(&heap_constructor), &original)))
        original_constructor = reinterpret_cast<HeapConstructor>(original);
    if (hooked && (hooked = hook(base + contract::begin_frame_rva, reinterpret_cast<void*>(&begin_frame), &original)))
        original_begin_frame = reinterpret_cast<BeginFrame>(original);
    const bool single = hooked && matches(base + contract::layout_rva, contract::layout);
    // The total, a constant, then capped at 1,000,000 as before: mov ecx, total / nop (4 bytes).
    std::array<std::uint8_t, contract::total_cap.size()> total{0xb9, 0, 0, 0, 0, 0x0f, 0x1f, 0x40, 0x00};
    std::memcpy(total.data() + 1, &heap_descriptor_total, 4);
    const auto cap = cap_code(single ? frame_descriptor_cap : split_frame_descriptor_cap);
    if (patch_code("descriptor heap frame cap", base + contract::cap_load_rva, contract::cap_load, cap) != HookOk ||
        patch_code("descriptor heap total", base + contract::total_cap_rva, contract::total_cap, total) != HookOk) {
        (void)patch_code_restore(base + contract::cap_load_rva, contract::cap_load);
        (void)patch_code_restore(base + contract::total_cap_rva, contract::total_cap);
        error = "the heap sizes could not be patched";
        return false;
    }
    if (!single) return true;
    // Last, so a failure leaves the three-heap layout with its own cap: mov sil, 0 / mov r9d, ebx / jmp 0x0410eae8
    constexpr std::array<std::uint8_t, contract::layout.size()> layout{0x40, 0xb6, 0x00, 0x44, 0x8b, 0xcb, 0xeb, 0x25};
    if (patch_code("descriptor heap single-heap layout", base + contract::layout_rva, contract::layout, layout) == HookOk) {
        single_heap_layout = true;
        return true;
    }
    (void)patch_code_restore(base + contract::layout_rva, contract::layout);
    if (patch_code("descriptor heap frame cap", base + contract::cap_load_rva, cap, cap_code(split_frame_descriptor_cap)) != HookOk) {
        (void)patch_code_restore(base + contract::cap_load_rva, contract::cap_load);
        (void)patch_code_restore(base + contract::total_cap_rva, contract::total_cap);
        error = "the heap layout could not be patched";
        return false;
    }
    return true;
}
} // namespace dingosdk
