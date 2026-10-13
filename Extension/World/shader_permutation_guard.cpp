#include "shader_permutation_guard.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/shader_permutations.h"
#include <array>
#include <atomic>

namespace dingosdk::shader_permutation_guard {
namespace {
namespace sp = addr::shader_permutations;
using Prepare = void (*)(std::uintptr_t, void*, void*, std::uint8_t, std::uint8_t);
Prepare original{};
std::atomic<bool> installed{};
std::atomic<std::uint64_t> skips{};

// Whether every permutation the game is about to use is there. The game reads the first one
// whatever the array's length, so an array without storage counts as missing too. This is
// asked for every shader of every view: two reads and a short walk, nothing more.
bool complete(std::uintptr_t shader) noexcept {
    if (!shader) return true; // (not this guard's: the game's own fault, left as it is)
    const auto *const begin = *reinterpret_cast<const std::uintptr_t *const *>(shader + sp::permutations_begin);
    const auto *const end = *reinterpret_cast<const std::uintptr_t *const *>(shader + sp::permutations_end);
    if (!begin || !*begin) return false;
    if (end < begin || end - begin > 4096) return true;
    for (const auto *at = begin; at != end; ++at)
        if (!*at) return false;
    return true;
}
// Said for the first few, then every thousandth: a shader that is passed over is passed over
// on every pass.
__declspec(noinline) void note_skip(std::uintptr_t shader) noexcept {
    const auto count = skips.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count > 8 && count % 1000) return;
    try {
        logging::log(logging::Level::warning, logging::Channel::runtime,
            "Shaders: a shader ({:#x}) has a compiled permutation missing, where the game would have crashed; it is not "
            "prepared, and what uses it may not be drawn ({} time(s) since the game started). A mod's material that names a "
            "shader the game could not build does this.", shader, count);
    } catch (...) {}
}
void guarded(std::uintptr_t shader, void* renderer, void* state, std::uint8_t fourth, std::uint8_t fifth) {
    if (!complete(shader)) {
        note_skip(shader);
        return;
    }
    original(shader, renderer, state, fourth, fifth);
}
} // namespace

bool install(std::uintptr_t base) noexcept {
    if (installed.exchange(true)) return original != nullptr;
    if (!base) return false;
    std::array<unsigned char, 32> head{};
    if (!memory::peek(base + sp::prepare.rva, head) || head != sp::prepare.bytes) {
        logging::log(logging::Level::warning, logging::Channel::runtime,
            "Shader permutation guard: this build's shader code differs; it is left as it is.");
        return false;
    }
    const auto target = reinterpret_cast<void*>(base + sp::prepare.rva);
    if (hook_prepare(target, reinterpret_cast<void*>(&guarded), reinterpret_cast<void**>(&original)) != HookOk ||
        hook_enable(target) != HookOk) {
        original = nullptr;
        logging::log(logging::Level::warning, logging::Channel::runtime, "Shader permutation guard: the game's function could not be hooked.");
        return false;
    }
    return true;
}

std::uint64_t skipped() noexcept { return skips.load(std::memory_order_relaxed); }
} // namespace dingosdk::shader_permutation_guard
