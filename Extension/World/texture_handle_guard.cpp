#include "texture_handle_guard.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/texture_handles.h"
#include <array>
#include <atomic>
#include <bit>

namespace dingosdk::texture_handle_guard {
namespace {
namespace th = addr::texture_handles;
using Redirect = std::uintptr_t* (*)(std::uintptr_t, std::uint16_t);
Redirect original{};
std::atomic<bool> installed{};
std::atomic<std::uint64_t> misses{};
// What both callers read when there is nothing: a holder with no texture in it, which sends them
// to the pool's default for the id.
std::uintptr_t no_texture{};

// Said for the first few, then each power of two: a stale handle is resolved on every frame it
// is drawn.
__declspec(noinline) void note(std::uint16_t id) noexcept {
    const auto count = misses.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count > 4 && !std::has_single_bit(count)) return;
    try {
        logging::log(logging::Level::warning, logging::Channel::runtime,
            "Textures: a texture handle ({:#x}) names a texture that is gone, where the game would have crashed; the default "
            "texture is drawn for it ({} time(s) since the game started). A mod's material that keeps a texture another "
            "bundle unloaded does this.", id, count);
    } catch (...) {}
}

std::uintptr_t* guarded(std::uintptr_t pool, std::uint16_t id) {
    if (auto* const holder = original(pool, id)) return holder;
    note(id);
    return &no_texture;
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
