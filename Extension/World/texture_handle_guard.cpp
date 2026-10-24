#include "texture_handle_guard.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/Build/20260929/texture_handles.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <limits>

namespace dingosdk::texture_handle_guard {
namespace {
namespace th = addr::texture_handles;
using Redirect = std::uintptr_t* (*)(std::uintptr_t, std::uint16_t);
Redirect original{};
std::atomic<bool> installed{};
std::atomic<std::uint64_t> misses{};
// What both callers read when there is nothing: a holder with no texture in it, which sends them
// to the entry's own texture, null for a dead entry (texture_handles.h).
std::uintptr_t no_texture{};
constexpr std::size_t stub_page_bytes = 0x1000;

// Said for the first few, then each power of two: a stale handle is resolved on every frame it
// is drawn.
__declspec(noinline) void note(std::uint16_t id) noexcept {
    const auto count = misses.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count > 4 && !std::has_single_bit(count)) return;
    try {
        logging::log(logging::Level::warning, logging::Channel::runtime,
            "Textures: a texture handle ({:#x}) names a texture that is gone, where the game would have crashed; nothing is "
            "drawn for it ({} time(s) since the game started). A mod's material that keeps a texture another "
            "bundle unloaded does this.", id, count);
    } catch (...) {}
}

std::uintptr_t* guarded(std::uintptr_t pool, std::uint16_t id) {
    if (auto* const holder = original(pool, id)) return holder;
    note(id);
    return &no_texture;
}

bool rel32(std::uintptr_t next_instruction, std::uintptr_t target, std::int32_t& displacement) noexcept {
    const auto delta = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(next_instruction);
    if (delta < std::numeric_limits<std::int32_t>::min() || delta > std::numeric_limits<std::int32_t>::max()) return false;
    displacement = static_cast<std::int32_t>(delta);
    return true;
}

bool write_code(std::uintptr_t address, const void* bytes, std::size_t size) noexcept {
    auto* const destination = reinterpret_cast<void*>(address);
    DWORD previous{};
    if (!VirtualProtect(destination, size, PAGE_EXECUTE_READWRITE, &previous)) return false;
    std::memcpy(destination, bytes, size);
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), destination, size) != FALSE;
    DWORD discarded{};
    return VirtualProtect(destination, size, previous, &discarded) != FALSE && flushed;
}

// The null the guard leads resolve to reaches the texture view cache, which asks the texture for its
// key before keeping the view it built (texture_handles.h). The stub skips the keeping for a null
// texture, so the cache hands back the empty view:
//   test r15, r15 / jz return / mov rbx, [cache] / add rbx, lock / jmp resume
bool skip_null_view_insert(std::uintptr_t base) noexcept {
    std::array<unsigned char, th::view_cache_insert.size()> actual{};
    if (!memory::peek(base + th::view_cache_insert_rva, actual) || actual != th::view_cache_insert) return false;
    auto* const stub = static_cast<std::uint8_t*>(memory::allocate_near_image(base, supported_build::game_image_size, stub_page_bytes));
    if (!stub) return false;
    const auto at = reinterpret_cast<std::uintptr_t>(stub);
    std::array<std::uint8_t, 28> code{0x4d, 0x85, 0xff, 0x0f, 0x84, 0, 0, 0, 0, 0x48, 0x8b, 0x1d, 0, 0, 0, 0,
        0x48, 0x81, 0xc3, 0, 0, 0, 0, 0xe9, 0, 0, 0, 0};
    std::memcpy(code.data() + 19, &th::view_cache_lock_offset, 4);
    std::int32_t leave{}, cache{}, resume{}, into{};
    if (!rel32(at + 9, base + th::view_cache_return_rva, leave) || !rel32(at + 16, base + th::view_cache, cache) ||
        !rel32(at + 28, base + th::view_cache_resume_rva, resume) || !rel32(base + th::view_cache_insert_rva + 5, at, into)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        return false;
    }
    std::memcpy(code.data() + 5, &leave, 4);
    std::memcpy(code.data() + 12, &cache, 4);
    std::memcpy(code.data() + 24, &resume, 4);
    std::memset(stub, 0xcc, stub_page_bytes);
    std::memcpy(stub, code.data(), code.size());
    DWORD previous{};
    if (!VirtualProtect(stub, stub_page_bytes, PAGE_EXECUTE_READ, &previous) ||
        !FlushInstructionCache(GetCurrentProcess(), stub, stub_page_bytes)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        return false;
    }
    std::array<std::uint8_t, th::view_cache_insert.size()> jump{0xe9};
    std::memcpy(jump.data() + 1, &into, 4);
    std::memset(jump.data() + 5, 0x90, jump.size() - 5);
    if (!write_code(base + th::view_cache_insert_rva, jump.data(), jump.size())) {
        (void)write_code(base + th::view_cache_insert_rva, th::view_cache_insert.data(), th::view_cache_insert.size());
        VirtualFree(stub, 0, MEM_RELEASE);
        return false;
    }
    return true;
}
} // namespace

bool install(std::uintptr_t base) noexcept {
    if (installed.exchange(true)) return original != nullptr;
    if (!base) return false;
    std::array<unsigned char, 32> head{};
    if (!memory::peek(base + th::redirect.rva, head) || head != th::redirect.bytes) {
        logging::log(logging::Level::warning, logging::Channel::runtime,
            "Texture handle guard: this build's texture code differs; it is left as it is.");
        return false;
    }
    // Without it the null only crashes a step later (Skate.exe+0x3e6ddc1), so the guard needs both.
    if (!skip_null_view_insert(base)) {
        logging::log(logging::Level::warning, logging::Channel::runtime,
            "Texture handle guard: the game's texture view cache differs or could not be patched; it is left as it is.");
        return false;
    }
    const auto target = reinterpret_cast<void*>(base + th::redirect.rva);
    if (hook_prepare(target, reinterpret_cast<void*>(&guarded), reinterpret_cast<void**>(&original)) != HookOk ||
        hook_enable(target) != HookOk) {
        original = nullptr;
        logging::log(logging::Level::warning, logging::Channel::runtime, "Texture handle guard: the game's function could not be hooked.");
        return false;
    }
    return true;
}

std::uint64_t stale() noexcept { return misses.load(std::memory_order_relaxed); }
} // namespace dingosdk::texture_handle_guard
