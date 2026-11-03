#include "grind_target.h"
#include "no_bail.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/grind_target.h"
#include "Engine/Game/Build/20260929/skater_state.h"
#include "Extension/UI/Overlay/overlay.h"
#include "Extension/UI/NativeMenu/throwdown_menu.h"
#include <Windows.h>
#include <atomic>
#include <cmath>
#include <mutex>

namespace dingosdk::grind_target {
namespace {
namespace layout = addr::grind_target;
using Select = void (*)(std::uintptr_t, std::uintptr_t);
struct State {
    std::mutex mutex; std::atomic<Select> original{}; std::atomic<bool> ready{};
    std::string owner; std::uint64_t expires{}; std::uintptr_t core{}, context{};
    int type{}, cached_type{}; bool negative{}, cached_negative{};
    std::uintptr_t cached_object{}; std::uint64_t cached_at{};
    std::array<std::uint8_t,5> cache{};
};
State& state() { static auto* value = new State; return *value; }
bool store(std::uintptr_t at, const void* bytes, std::size_t size) {
    SIZE_T count{};
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(at), bytes, size, &count) && count == size;
}
bool targeted(const NoBailSkater& skater, Target& value) {
    std::uintptr_t root{}, target{}, again{}; std::uint8_t active{};
    if (!memory::peek(skater.core + layout::root, root) || !memory::peek(root + layout::target, target) ||
        !memory::peek(target + layout::targeting, active) || active > 1 ||
        !memory::peek(target + layout::direction, value.direction) || !memory::peek(root + layout::target, again) || again != target) return false;
    for (const auto v : value.direction) if (!std::isfinite(v) || std::abs(v) > 100) return false;
    value.targeting = active != 0;
    return true;
}
void select(std::uintptr_t object, std::uintptr_t output) {
    auto& s = state(); const auto original = s.original.load(std::memory_order_acquire);
    const auto saved_error = GetLastError();
    std::unique_lock lock(s.mutex, std::try_to_lock);
    NoBailSkater skater{}; Target target{}; std::uintptr_t context{}; std::uint32_t physics{}, flags{};
    int old_type{}; float gate{}; std::array<std::uint8_t,5> old_cache{};
    const auto now = GetTickCount64();
    const bool eligible = lock.owns_lock() && !s.owner.empty() && now < s.expires &&
        !overlay::interface_open() && multiplayer::native_throwdown_gameplay_input_allowed() &&
        no_bail_skater(skater) && skater.core == s.core && skater.context == s.context &&
        memory::peek(object + layout::context, context) && context == skater.context &&
        memory::peek(context + addr::skater_state::physics_state_offset, physics) && physics == 201 &&
        targeted(skater, target) && target.targeting &&
        memory::peek(object + layout::request_gate, gate) && std::isfinite(gate) && gate < 5 &&
        memory::peek(context + layout::requested, old_type) && old_type >= 0 && old_type <= 7 &&
        memory::peek(context + layout::request_flags, flags) &&
        memory::peek(object + layout::alignment, old_cache) && old_cache[4] <= 1;
    if (!eligible) { SetLastError(saved_error); original(object, output); return; }
    if (s.cached_object != object || s.cached_type != s.type || s.cached_negative != s.negative ||
        now < s.cached_at || now - s.cached_at > 60) s.cache = {};
    s.cached_object = object; s.cached_type = s.type; s.cached_negative = s.negative; s.cached_at = now;
    const auto requested_flags = flags | layout::requested_mask;
    const bool applied = store(object + layout::alignment, s.cache.data(), s.cache.size()) &&
        store(context + layout::requested, &s.type, sizeof(s.type)) &&
        store(context + layout::request_flags, &requested_flags, sizeof(requested_flags));
    if (!applied) {
        store(object + layout::alignment, old_cache.data(), old_cache.size());
        store(context + layout::requested, &old_type, sizeof(old_type));
        store(context + layout::request_flags, &flags, sizeof(flags));
        s.owner.clear(); s.cached_at = 0;
    }
    SetLastError(saved_error); original(object, output); const auto returned_error = GetLastError();
    if (applied) {
        float angle{};
        if (!memory::peek(object + layout::alignment, s.cache) || s.cache[4] > 1 ||
            !memory::peek(object + layout::alignment, angle) || !std::isfinite(angle) || std::abs(angle) > 180) s.cached_at = 0;
        if (!store(object + layout::alignment, old_cache.data(), old_cache.size())) s.owner.clear();
        int after{}; std::uint32_t after_flags{};
        if (memory::peek(context + layout::requested, after) && after == s.type) {
            if (!store(context + layout::requested, &old_type, sizeof(old_type))) s.owner.clear();
            if (memory::peek(context + layout::request_flags, after_flags) && (after_flags & layout::requested_mask)) {
                after_flags = (after_flags & ~layout::requested_mask) | (flags & layout::requested_mask);
                if (!store(context + layout::request_flags, &after_flags, sizeof(after_flags))) s.owner.clear();
            }
        }
    }
    SetLastError(returned_error);
}
bool prepare(std::uintptr_t base) {
    auto& s = state(); if (s.ready.load(std::memory_order_acquire)) return true;
    std::array<std::uint8_t,layout::select_prefix.size()> actual{};
    if (!memory::peek(base + layout::select, actual) || actual != layout::select_prefix) return false;
    HookBatch batch;
    void* original{}; auto* target = reinterpret_cast<void*>(base + layout::select);
    if (hook_prepare(target, reinterpret_cast<void*>(&select), &original) != HookOk) return false;
    s.original.store(reinterpret_cast<Select>(original), std::memory_order_release);
    if (hook_enable(target) != HookOk) return false;
    s.ready.store(true, std::memory_order_release); return true;
}
}
std::optional<Target> read() {
    NoBailSkater skater{}; Target value{};
    if (!no_bail_skater(skater) || !targeted(skater, value)) return std::nullopt;
    value.available = state().ready.load(std::memory_order_acquire); return value;
}
std::string request(std::string_view owner, std::optional<int> type, bool negative) {
    auto& s = state();
    if (!type) { release(owner); return {}; }
    NoBailSkater skater{};
    if (!no_bail_skater(skater) || !prepare(skater.base)) return "rail candidate selection is unavailable";
    std::lock_guard lock(s.mutex); const auto now = GetTickCount64();
    if (!s.owner.empty() && s.owner != owner && now < s.expires) return s.owner + " selects rail targets";
    if (s.owner != owner || s.core != skater.core || s.context != skater.context) s.cached_at = 0;
    s.owner = owner; s.expires = now + 100; s.type = *type; s.negative = negative;
    s.core = skater.core; s.context = skater.context; return {};
}
void release(std::string_view owner) {
    auto& s = state(); std::lock_guard lock(s.mutex);
    if (s.owner == owner) { s.owner.clear(); s.cached_at = 0; }
}
}
