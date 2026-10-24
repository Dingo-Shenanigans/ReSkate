#include "physics_wake_guard.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/physics_wake.h"
#include <array>
#include <atomic>

namespace dingosdk::physics_wake_guard {
namespace {
namespace pw = addr::physics_wake;
using Wake = void (*)(std::uintptr_t);
Wake original{};
std::atomic<bool> installed{};
std::atomic<std::uint32_t> most{};
std::atomic<std::uint64_t> widened{};

// Said once: with the owner pool raised (Extension/World/physics_world_size.cpp) every pass of every
// session is widened, so the line says how the pass is sized, not a crash each time (55a7342's logs
// had it thousands of times per session; 2026-10-24).
__declspec(noinline) void note(std::uint32_t owners, std::uint32_t limit) noexcept {
    if (widened.fetch_add(1, std::memory_order_relaxed) != 0) return;
    try {
        logging::log(logging::Level::info, logging::Channel::runtime,
            "Physics: the wake pass makes room for the world's {} physics owners (the game's own size is {}, below the "
            "raised owner pool).", owners, limit);
    } catch (...) {}
}

// The world's owner count, 0 when it cannot be read (the game's own size is then kept).
std::uint32_t owners_of(std::uintptr_t system) noexcept {
    std::uintptr_t world{}, begin{}, end{};
    if (!memory::peek(system + pw::world_offset, world) || !world ||
        !memory::peek(world + pw::owner_generations_begin, begin) ||
        !memory::peek(world + pw::owner_generations_end, end) || end < begin || (end - begin) % 4)
        return 0;
    const auto count = (end - begin) / 4;
    return count > 0x7fffffc0u ? 0 : static_cast<std::uint32_t>(count);
}

void guarded(std::uintptr_t system) {
    auto* const limit = system ? reinterpret_cast<std::uint32_t*>(system + pw::owner_limit_offset) : nullptr;
    const auto owners = system ? owners_of(system) : 0;
    if (!limit || owners <= *limit) return original(system);
    // The pass runs on the entity update's thread and is the one reader of the limit, which is
    // put back as soon as it returns.
    const auto stock = *limit;
    *limit = owners;
    if (owners > most.load(std::memory_order_relaxed)) most.store(owners, std::memory_order_relaxed);
    note(owners, stock);
    original(system);
    *limit = stock;
}
} // namespace

bool install(std::uintptr_t base) noexcept {
    if (installed.exchange(true)) return original != nullptr;
    if (!base) return false;
    std::array<unsigned char, 32> head{};
    if (!memory::peek(base + pw::wake.rva, head) || head != pw::wake.bytes) {
        logging::log(logging::Level::warning, logging::Channel::runtime,
            "Physics wake guard: this build's physics code differs; it is left as it is.");
        return false;
    }
    const auto target = reinterpret_cast<void*>(base + pw::wake.rva);
    if (hook_prepare(target, reinterpret_cast<void*>(&guarded), reinterpret_cast<void**>(&original)) != HookOk ||
        hook_enable(target) != HookOk) {
        original = nullptr;
        logging::log(logging::Level::warning, logging::Channel::runtime, "Physics wake guard: the game's function could not be hooked.");
        return false;
    }
    return true;
}

std::uint32_t widest() noexcept { return most.load(std::memory_order_relaxed); }
} // namespace dingosdk::physics_wake_guard
