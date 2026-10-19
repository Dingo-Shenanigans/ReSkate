#include "mesh_settings_guard.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/mesh_settings.h"
#include <array>
#include <atomic>
#include <bit>
#include <cstring>

namespace dingosdk::mesh_settings_guard {
namespace {
namespace ms = addr::mesh_settings;
using ApplySettings = void (*)(std::byte*, void*);
ApplySettings original{};
std::uintptr_t empty{};
std::atomic<bool> installed{};
std::atomic<std::uint64_t> missing{}, stale{};

// (A count worth a line in the log: the first few, then each power of two.)
__declspec(noinline) void note(std::uint64_t count, std::uint64_t held, std::uintptr_t value) noexcept {
    if (count > 4 && !std::has_single_bit(count)) return;
    try {
        logging::log(logging::Level::info, logging::Channel::runtime,
            "Mesh settings: {} mesh(es) have none ({} held something the game would have freed on unload; this one {:#x}); they are "
            "given an empty string.", count, held, value);
    } catch (...) {}
}

void guarded(std::byte* mesh, void* settings) {
    // Only a mesh about to be marked as having settings it will not get: the string its unload
    // frees is made the empty one. A mesh that gets its settings has the string made with them.
    if (mesh && !settings && mesh[ms::applied_flag_offset] == std::byte{0}) {
        std::uintptr_t held{};
        std::memcpy(&held, mesh + ms::unload_string_offset, sizeof(held));
        std::memcpy(mesh + ms::unload_string_offset, &empty, sizeof(empty));
        const bool would_free = held != 0 && held != empty;
        const auto count = missing.fetch_add(1, std::memory_order_relaxed) + 1;
        const auto freed = stale.fetch_add(would_free ? 1 : 0, std::memory_order_relaxed) + (would_free ? 1 : 0);
        note(count, freed, held);
    }
    original(mesh, settings);
}
} // namespace

bool install(std::uintptr_t base) noexcept {
    if (installed.exchange(true)) return original != nullptr;
    if (!base) return false;
    std::array<unsigned char, 32> head{};
    if (!memory::peek(base + ms::apply_settings.rva, head) || head != ms::apply_settings.bytes) {
        logging::log(logging::Level::warning, logging::Channel::runtime,
            "Mesh settings guard: this build's mesh settings code differs; it is left as it is.");
        return false;
    }
    empty = base + ms::empty_string;
    const auto target = reinterpret_cast<void*>(base + ms::apply_settings.rva);
    if (hook_prepare(target, reinterpret_cast<void*>(&guarded), reinterpret_cast<void**>(&original)) != HookOk ||
        hook_enable(target) != HookOk) {
        original = nullptr;
        logging::log(logging::Level::warning, logging::Channel::runtime, "Mesh settings guard: the game's function could not be hooked.");
        return false;
    }
    return true;
}

std::uint64_t without_settings() noexcept { return missing.load(std::memory_order_relaxed); }
std::uint64_t would_have_freed() noexcept { return stale.load(std::memory_order_relaxed); }
} // namespace dingosdk::mesh_settings_guard
