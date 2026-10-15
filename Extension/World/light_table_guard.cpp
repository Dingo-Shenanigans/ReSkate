#include "light_table_guard.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/light_tables.h"
#include <array>
#include <atomic>
#include <utility>

namespace dingosdk::light_table_guard {
namespace {
namespace lt = addr::light_tables;
constexpr std::size_t kind_count = std::size(lt::kinds);
// The ID is taken a little after the factory is asked, and other lights may take IDs in between,
// so the last IDs are left free: a fresh one only below capacity - reserve, a recycled one up to
// capacity - reserve / 2.
constexpr std::uint32_t reserve = 256;
using Factory = void* (*)(void*, void*);
std::array<Factory, kind_count> originals{};
std::uintptr_t image{};
std::atomic<bool> installed{};
std::array<std::atomic<std::uint64_t>, kind_count> refused{};

bool room(std::size_t kind) noexcept {
    std::uintptr_t renderer{};
    if (!memory::peek(image + lt::renderer, renderer) || !renderer) return true;
    const auto allocator = renderer + lt::kinds[kind].manager + lt::allocator;
    std::uint64_t free_head{};
    std::uint32_t issued{};
    if (!memory::peek(allocator + lt::free_head, free_head) || !memory::peek(allocator + lt::issued, issued)) return true;
    if (issued < lt::capacity - reserve) return true;
    return static_cast<std::uint32_t>(free_head) != 0xffffffff && issued < lt::capacity - reserve / 2;
}
// Said for the first few of each kind, then every thousandth.
__declspec(noinline) void note_refused(std::size_t kind) noexcept {
    const auto count = refused[kind].fetch_add(1, std::memory_order_relaxed) + 1;
    if (count > 8 && count % 1000) return;
    try {
        logging::log(logging::Level::warning, logging::Channel::runtime,
            "Lights: the level has more {} lights than the game's table of them holds (8,192); this one is not made and "
            "is not drawn, where the game would have crashed ({} left out since the game started).",
            lt::kinds[kind].name, count);
    } catch (...) {}
}
template <std::size_t Kind> void* guarded(void* creator, void* info) {
    if (!room(Kind)) {
        note_refused(Kind);
        return nullptr; // (as the game's own factory does for a light it should not make)
    }
    return originals[Kind](creator, info);
}
template <std::size_t... Kinds> std::array<void*, kind_count> replacements(std::index_sequence<Kinds...>) {
    return {reinterpret_cast<void*>(&guarded<Kinds>)...};
}
bool matches(std::uintptr_t base, const game::build::Fingerprint& print) noexcept {
    std::array<unsigned char, 32> head{};
    return memory::peek(base + print.rva, head) && head == print.bytes;
}
} // namespace

bool install(std::uintptr_t base) noexcept {
    if (installed.exchange(true)) return originals[0] != nullptr;
    if (!base) return false;
    bool known = matches(base, lt::take_id);
    for (const auto& kind : lt::kinds) known = known && matches(base, kind.factory);
    if (!known) {
        logging::log(logging::Level::warning, logging::Channel::runtime,
            "Light table guard: this build's light code differs; it is left as it is.");
        return false;
    }
    image = base;
    const auto hooks = replacements(std::make_index_sequence<kind_count>{});
    const auto target = [base](std::size_t kind) { return reinterpret_cast<void*>(base + lt::kinds[kind].factory.rva); };
    // All six in one go, or none.
    std::size_t prepared{};
    auto status = HookOk;
    for (; prepared < kind_count; ++prepared) {
        status = hook_prepare(target(prepared), hooks[prepared], reinterpret_cast<void**>(&originals[prepared]));
        if (status != HookOk) break;
    }
    for (std::size_t kind = 0; kind < kind_count && status == HookOk; ++kind) status = hook_queue_enable(target(kind));
    if (status == HookOk) status = hook_apply_queued();
    if (status != HookOk) {
        for (std::size_t kind = 0; kind < prepared; ++kind) hook_remove(target(kind));
        originals = {};
        logging::log(logging::Level::warning, logging::Channel::runtime, "Light table guard: the game's light factories could not be hooked.");
        return false;
    }
    return true;
}
} // namespace dingosdk::light_table_guard
