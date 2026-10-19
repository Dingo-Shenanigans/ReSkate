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
// A preset whose parameters the game can apply: every value that is set has a type the game can
// read, as its type check (Skate.exe+0x3dc07e0) reads it: the TypeInfo, its data, a word at +4. A
// preset with one that is null or not an address at all crashed the game when the material was
// next applied, e.g. scrolling shirts onto a mod's (2026-10-17: Yudi-Yuddispack's
// philippleinshirt_ap, a type of 0x0c07010304040502). Catching that crash in this call is not
// enough: the preset is already on the material, and the game reads it again on its next update.
// (Not "a type in the game's image": many of the game's own presets have types built at run time,
// and leaving those out turned every item white.) `bad` is the key of the first parameter that fails.
bool preset_parameters_sound(void *preset, std::uint64_t &bad) {
    const auto at = reinterpret_cast<std::uintptr_t>(preset);
    std::uintptr_t parameters{};
    if (!memory::read_bytes(at + native::preset_parameters, &parameters, sizeof(parameters))) return false;
    if (!parameters) return true;
    std::uint32_t count{};
    if (!memory::read_bytes(parameters - 4, &count, sizeof(count))) return false;
    count &= 0x7fffffff;
    if (count > 4096) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto entry = parameters + i * native::preset_parameter_size;
        std::array<std::uintptr_t, 2> value{}; // the boxed value: TypeInfo*, data*
        if (!memory::read_bytes(entry, value.data(), sizeof(value))) return false;
        if (!value[1]) continue; // the game passes over an entry with no data
        std::uintptr_t type_data{};
        std::uint16_t flags{};
        if (!value[0] || !memory::read_bytes(value[0], &type_data, sizeof(type_data)) || !type_data ||
            !memory::read_bytes(type_data + 4, &flags, sizeof(flags))) {
            memory::read_bytes(entry + native::preset_parameter_key, &bad, sizeof(bad));
            return false;
        }
    }
    return true;
}
std::string preset_name(void *preset) {
    std::uintptr_t text{};
    if (!memory::read_bytes(reinterpret_cast<std::uintptr_t>(preset) + 0x18, &text, sizeof(text)) || !text) return {};
    char buffer[200]{};
    return memory::peek_cstring(text, buffer, sizeof(buffer) - 1) > 0 ? std::string(buffer) : std::string{};
}
int add_preset(void *state) {
    const auto reference = reinterpret_cast<void *(*)(void *, int)>(image + native::script_preset_reference)(state, 3);
    if (const auto preset = reference ? reinterpret_cast<void *(*)(void *, int)>(image + native::referenced_preset)(reference, 0) : nullptr) {
        std::uint64_t bad{};
        if (preset_parameters_sound(preset, bad)) return original_add_preset.load()(state);
        // Left out, as a preset that is not there: the item shows without it.
        static std::mutex refused_mutex;
        static std::set<std::string> refused;
        const auto name = preset_name(preset);
        std::lock_guard lock(refused_mutex);
        if (refused.size() < 64 && refused.insert(name).second)
            logging::log(logging::Level::warning, logging::Channel::customization,
                         "The shader preset {} is left out: its parameter 0x{:016x} has a type the game cannot read "
                         "(a mod's preset that is broken or made for another build). The item is shown without it.",
                         name.empty() ? "(unnamed)" : name, bad);
        return 0;
    }
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
