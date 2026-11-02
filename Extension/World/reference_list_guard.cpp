#include "reference_list_guard.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/reference_lists.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <mutex>
#include <set>
#include <string>

namespace dingosdk::reference_list_guard {
namespace {
namespace rl = addr::reference_lists;
using Register = void (*)(std::uintptr_t registry, std::uintptr_t* assets, std::uint8_t flag);
Register original_named{};
Register original_assets{};
std::atomic<bool> installed{};
std::atomic<bool> hooked{};

// Moves an array's references that are not empty down over the empty ones and lowers its count.
// The number taken out, 0 when the array cannot be read or written.
std::uint32_t drop_empty(std::uintptr_t array) noexcept {
    __try {
        auto& count = *reinterpret_cast<std::uint32_t*>(array - 4);
        const auto size = count & ~rl::array_count_flag;
        auto* const entries = reinterpret_cast<std::uintptr_t*>(array);
        std::uint32_t kept = 0;
        for (std::uint32_t i = 0; i < size; ++i)
            if (entries[i] & ~rl::pointer_flag) entries[kept++] = entries[i];
        if (kept == size) return 0;
        count = (count & rl::array_count_flag) | kept;
        return size - kept;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Said once for each asset, with its name, so a mod's broken asset can be found.
void note(std::uintptr_t asset, std::uint32_t dropped) noexcept {
    try {
        std::string name = "(unnamed)";
        std::uintptr_t text{};
        char buffer[200]{};
        if (memory::peek(asset + rl::asset_name, text) && memory::peek_cstring(text, buffer, sizeof(buffer) - 1) > 0) name = buffer;
        static std::mutex mutex;
        static std::set<std::string> said;
        std::lock_guard lock(mutex);
        if (said.size() >= 64 || !said.insert(name).second) return;
        logging::log(logging::Level::warning, logging::Channel::runtime,
            "Level load: the asset {} lists {} reference(s) that lead nowhere, where the game would have crashed; they are "
            "left out. A mod's asset that names something the game does not have does this.", name, dropped);
    } catch (...) {}
}

void tidy(std::uintptr_t* assets) noexcept {
    std::uintptr_t list{};
    std::uint32_t count{};
    if (!assets || !memory::peek(reinterpret_cast<std::uintptr_t>(assets), list) || !list ||
        !memory::peek(list - 4, count)) return;
    count &= ~rl::array_count_flag;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uintptr_t asset{}, references{};
        if (!memory::peek(list + i * sizeof(std::uintptr_t), asset)) return;
        asset &= ~rl::pointer_flag;
        if (!asset || !memory::peek(asset + rl::asset_references, references) || !references) continue;
        if (const auto dropped = drop_empty(references)) note(asset, dropped);
    }
}

void registered_named(std::uintptr_t registry, std::uintptr_t* assets, std::uint8_t flag) {
    tidy(assets);
    original_named(registry, assets, flag);
}
void registered_assets(std::uintptr_t registry, std::uintptr_t* assets, std::uint8_t flag) {
    tidy(assets);
    original_assets(registry, assets, flag);
}

bool hook(std::uintptr_t base, const game::build::Fingerprint& contract, void* detour, Register& original) noexcept {
    std::array<unsigned char, 32> head{};
    if (!memory::peek(base + contract.rva, head) || head != contract.bytes) return false;
    const auto target = reinterpret_cast<void*>(base + contract.rva);
    if (hook_prepare(target, detour, reinterpret_cast<void**>(&original)) != HookOk || hook_enable(target) != HookOk) {
        original = nullptr;
        return false;
    }
    return true;
}
} // namespace

bool install(std::uintptr_t base) noexcept {
    if (installed.exchange(true)) return hooked.load();
    if (!base) return false;
    // Each loader on its own: one that differs leaves the other guarded.
    const bool named = hook(base, rl::register_named, reinterpret_cast<void*>(&registered_named), original_named);
    const bool assets = hook(base, rl::register_assets, reinterpret_cast<void*>(&registered_assets), original_assets);
    if (!named || !assets)
        logging::log(logging::Level::warning, logging::Channel::runtime,
            "Reference list guard: this build's level load code differs or could not be hooked ({} {}, {} {}); it is left as it is.",
            "0x557c40", named ? "hooked" : "not hooked", "0x891c80", assets ? "hooked" : "not hooked");
    hooked.store(named && assets);
    return named && assets;
}
} // namespace dingosdk::reference_list_guard
