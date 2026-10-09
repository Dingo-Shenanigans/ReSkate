#include "preset_lookup_guard.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/native_cosmetics.h"
#include <atomic>
#include <mutex>
#include <set>
#include <string>

namespace dingosdk::preset_lookup_guard {
namespace {
namespace native = addr::native_cosmetics;
using Lookup = void **(*)(void **out, const char *name, void *table, std::uint8_t flag, void *context);
std::atomic<Lookup> original{};

void **lookup(void **out, const char *name, void *table, std::uint8_t flag, void *context) {
    if (table) return original.load()(out, name, table, flag, context);
    // The callers take an empty result as "not found", as for an empty name.
    *out = nullptr;
    static std::mutex logged_mutex;
    static std::set<std::string> logged;
    std::string text;
    if (name) {
        char buffer[160]{};
        memory::read_bytes(reinterpret_cast<std::uintptr_t>(name), buffer, sizeof(buffer) - 1);
        text = buffer;
    }
    std::lock_guard lock(logged_mutex);
    if (logged.size() < 64 && logged.insert(text).second)
        logging::log(logging::Level::warning, logging::Channel::customization,
                     "A cosmetic preset could not be found ({}); it is left out instead of loading.",
                     text.empty() ? "unnamed" : text);
    return out;
}
// item:addEsShaderPreset(material, preset) with a preset that is not there: nothing is added,
// and the script goes on as it does after any other call.
using ScriptFunction = int (*)(void *state);
std::atomic<ScriptFunction> original_add_preset{};
std::uintptr_t image{};
int add_preset(void *state) {
    const auto reference = reinterpret_cast<void *(*)(void *, int)>(image + native::script_preset_reference)(state, 3);
    if (reference && reinterpret_cast<void *(*)(void *, int)>(image + native::referenced_preset)(reference, 0))
        return original_add_preset.load()(state);
    static std::atomic<unsigned> seen{};
    const auto count = seen.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 5 || count % 500 == 0)
        logging::log(logging::Level::warning, logging::Channel::customization,
                     "An item's shader preset was not loaded when its script used it ({} so far); the item is shown without it.",
                     count);
    return 0;
}
template<std::size_t Size> bool matches(std::uintptr_t address, const std::array<unsigned char, Size> &prefix) {
    std::array<unsigned char, Size> bytes{};
    return memory::read_bytes(address, bytes.data(), bytes.size()) && bytes == prefix;
}
bool start_add_preset(std::uintptr_t base) {
    if (!matches(base + native::add_shader_preset, native::add_shader_preset_prefix) ||
        !matches(base + native::script_preset_reference, native::script_preset_reference_prefix) ||
        !matches(base + native::referenced_preset, native::referenced_preset_prefix))
        return false;
    image = base;
    auto *target = reinterpret_cast<void *>(base + native::add_shader_preset);
    void *previous{};
    if (hook_prepare(target, reinterpret_cast<void *>(&add_preset), &previous) != HookOk) return false;
    original_add_preset = reinterpret_cast<ScriptFunction>(previous);
    if (hook_enable(target) != HookOk) {
        hook_remove(target);
        return false;
    }
    return true;
}
} // namespace

bool start(std::uintptr_t base) noexcept {
    try {
        std::array<unsigned char, native::named_lookup_prefix.size()> bytes{};
        if (!base || !memory::read_bytes(base + native::named_lookup, bytes.data(), bytes.size()) ||
            bytes != native::named_lookup_prefix)
            return false;
        auto *target = reinterpret_cast<void *>(base + native::named_lookup);
        void *previous{};
        if (hook_prepare(target, reinterpret_cast<void *>(&lookup), &previous) != HookOk) return false;
        original = reinterpret_cast<Lookup>(previous);
        if (hook_enable(target) != HookOk) {
            hook_remove(target);
            return false;
        }
        return start_add_preset(base);
    } catch (...) {
        return false;
    }
}
} // namespace dingosdk::preset_lookup_guard
