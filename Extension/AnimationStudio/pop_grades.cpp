#include "pop_grades.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/20260929/game_state_values.h"
#include "Engine/Game/Build/addresses.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <string>

namespace dingosdk::anim_studio::pop_grades {
namespace {
namespace values = addr::game_state_values;
// The setter's result, whatever it is, goes back to its callers untouched.
using SetInt = std::uintptr_t (*)(std::uintptr_t setter, std::uintptr_t instance, std::int32_t value);

std::atomic<SetInt> original_set_int{};
// While a take runs: GetTickCount64 at which it stops counting grades as its own (0: no take).
std::atomic<std::uint64_t> take_until{};
std::atomic<PopHeight> take_grade{}, latest_grade{};
std::atomic<std::uint64_t> latest_grade_ms{};

// PopHeight's descriptor is known by its address and its name pointer together, so an address the game frees and gives
// to another value does not answer for it, and a descriptor that moves is found again by its name.
std::atomic<std::uintptr_t> known_descriptor{}, known_name_at{};
// Descriptors (with their name pointers) already found not to be PopHeight, so a name is read about once per int value
// the graph writes, not once per write.
std::array<std::atomic<std::uint64_t>, 1024> not_pop_height{};

bool is_pop_height(std::uintptr_t descriptor) noexcept {
    std::uintptr_t name_at{};
    if (!memory::peek(descriptor + values::descriptor_name, name_at) || !name_at) return false;
    if (descriptor == known_descriptor.load(std::memory_order_relaxed) && name_at == known_name_at.load(std::memory_order_relaxed)) return true;
    const auto identity = descriptor ^ (name_at * 0x9e3779b97f4a7c15ull);
    auto& slot = not_pop_height[(descriptor >> 4) & (not_pop_height.size() - 1)];
    if (slot.load(std::memory_order_relaxed) == identity) return false;
    std::array<char, sizeof(values::pop_height_name)> name{};
    if (memory::peek_cstring(name_at, name.data(), name.size()) >= 0 && std::string_view(name.data()) == values::pop_height_name) {
        known_descriptor.store(descriptor, std::memory_order_relaxed);
        known_name_at.store(name_at, std::memory_order_relaxed);
        return true;
    }
    slot.store(identity, std::memory_order_relaxed);
    return false;
}
std::uintptr_t set_int_hook(std::uintptr_t setter, std::uintptr_t instance, std::int32_t value) {
    if (value > 0 && is_pop_height(setter - values::descriptor_to_setter)) {
        const auto now = GetTickCount64();
        latest_grade.store(value, std::memory_order_relaxed);
        latest_grade_ms.store(now, std::memory_order_release);
        if (const auto until = take_until.load(std::memory_order_acquire); until && now < until) take_grade.store(value, std::memory_order_relaxed);
    }
    return original_set_int.load(std::memory_order_acquire)(setter, instance, value);
}
// The empty string, or why the setter could not be hooked. The original is published before the hook is live, so the
// first call through it forwards at once.
std::string hook_setter(std::uintptr_t base) {
    std::array<unsigned char, 32> bytes{};
    if (!base) return "the game is not loaded";
    if (!memory::peek(base + values::int_setter.rva, bytes) || bytes != values::int_setter.bytes)
        return "the game's value setter is not the one this was written for";
    const auto target = reinterpret_cast<void*>(base + values::int_setter.rva);
    void* relay{};
    if (const auto status = hook_prepare(target, reinterpret_cast<void*>(&set_int_hook), &relay); status != HookOk)
        return std::string("the value setter could not be hooked: ") + hook_status_string(status);
    original_set_int.store(reinterpret_cast<SetInt>(relay), std::memory_order_release);
    if (const auto status = hook_enable(target); status != HookOk) return std::string("the value setter could not be hooked: ") + hook_status_string(status);
    return {};
}
} // namespace

std::string_view pop_name(PopHeight height) noexcept { return height == 1 ? "low" : height == 2 ? "mid" : height == 3 ? "high" : ""; }
PopHeight pop_height_of(std::string_view name) noexcept { return name == "low" ? 1 : name == "mid" ? 2 : name == "high" ? 3 : 0; }

bool install(std::uintptr_t base, std::string& why) {
    // Tried once: the answer, and why not, stay for the rest of the game.
    static const std::string failure = hook_setter(base);
    why = failure;
    return failure.empty();
}
void begin_take(std::uint64_t lasts_ms) noexcept {
    take_grade.store(0, std::memory_order_relaxed);
    take_until.store(GetTickCount64() + lasts_ms, std::memory_order_release);
}
void end_take() noexcept { take_until.store(0, std::memory_order_release); }
PopHeight take_pop_height() noexcept { return take_grade.load(std::memory_order_relaxed); }
PopHeight pop_height_since(std::uint64_t since_ms) noexcept {
    if (latest_grade_ms.load(std::memory_order_acquire) < since_ms) return 0;
    return latest_grade.load(std::memory_order_relaxed);
}
} // namespace dingosdk::anim_studio::pop_grades
