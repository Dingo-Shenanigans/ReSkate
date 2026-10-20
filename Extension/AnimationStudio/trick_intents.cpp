#include "trick_intents.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/20260929/game_state_values.h"
#include "Engine/Game/Build/addresses.h"
#include <windows.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

namespace dingosdk::anim_studio::trick_intents {
namespace {
namespace values = addr::game_state_values;
// The setters' result, whatever it is, goes back to their callers untouched.
using SetFloat = std::uintptr_t (*)(std::uintptr_t setter, std::uintptr_t instance, float value);
using SetInt = std::uintptr_t (*)(std::uintptr_t setter, std::uintptr_t instance, std::int32_t value);

// A value is known by its descriptor and the descriptor's name pointer together, so an address the game frees and
// gives to another value does not answer for it, and a value whose descriptor moves is found again by its name.
std::uintptr_t name_pointer(std::uintptr_t descriptor) noexcept {
    std::uintptr_t name_at{};
    return memory::peek(descriptor + values::descriptor_name, name_at) ? name_at : 0;
}
bool is_named(std::uintptr_t name_at, std::string_view wanted) noexcept {
    std::array<char, 64> name{};
    return name_at && memory::peek_cstring(name_at, name.data(), name.size()) >= 0 && std::string_view(name.data()) == wanted;
}
struct KnownValue {
    std::string_view name;
    std::atomic<std::uintptr_t> descriptor{}, name_at{};
};
// The values one setter carries that are watched, with the descriptors already found to be none of them, so a name is
// read about once per value the graph writes, not once per write.
template <std::size_t Count> struct Watched {
    std::array<KnownValue, Count> known;
    std::array<std::atomic<std::uint64_t>, 1024> none_of_these{};

    // The index in `known` of the value written through `descriptor`, or -1. Hot: called for every write it is asked about.
    int find(std::uintptr_t descriptor) noexcept {
        const auto name_at = name_pointer(descriptor);
        if (!name_at) return -1;
        for (std::size_t i = 0; i < Count; ++i)
            if (known[i].descriptor.load(std::memory_order_relaxed) == descriptor && known[i].name_at.load(std::memory_order_relaxed) == name_at)
                return static_cast<int>(i);
        const auto identity = descriptor ^ (name_at * 0x9e3779b97f4a7c15ull);
        auto& slot = none_of_these[(descriptor >> 4) & (none_of_these.size() - 1)];
        if (slot.load(std::memory_order_relaxed) == identity) return -1;
        for (std::size_t i = 0; i < Count; ++i)
            if (is_named(name_at, known[i].name)) {
                known[i].descriptor.store(descriptor, std::memory_order_relaxed);
                known[i].name_at.store(name_at, std::memory_order_relaxed);
                return static_cast<int>(i);
            }
        slot.store(identity, std::memory_order_relaxed);
        return -1;
    }
};

// The float values a take scales and records, in this order.
enum : std::size_t { strength_index, flip_speed_index };
Watched<2> floats{{{{values::jump_strength_name}, {values::flip_speed_name}}}};
Watched<1> ints{{{{values::pop_height_name}}}};

struct History {
    std::atomic<float> scale{1.0f};
    // The values written in the take, each change once; more than fit are dropped. Only told in the log.
    std::array<std::atomic<float>, 64> values{};
    std::atomic<std::uint32_t> count{};
};
std::array<History, 2> histories;
std::atomic<SetFloat> original_set_float{};
std::atomic<SetInt> original_set_int{};
// While a take runs: GetTickCount64 at which its scaling stops even if nothing ends the take (0: no take).
std::atomic<std::uint64_t> take_until{};
std::atomic<PopHeight> take_grade{}, latest_grade{};
std::atomic<std::uint64_t> latest_grade_ms{};

bool in_take(std::uint64_t now) noexcept {
    const auto until = take_until.load(std::memory_order_acquire);
    return until && now < until;
}
void remember(History& history, float value) noexcept {
    const auto count = history.count.load(std::memory_order_relaxed);
    if (count >= history.values.size() || (count && history.values[count - 1].load(std::memory_order_relaxed) == value)) return;
    history.values[count].store(value, std::memory_order_relaxed);
    history.count.store(count + 1, std::memory_order_release);
}
std::vector<float> values_of(const History& history) {
    std::vector<float> list;
    const auto count = std::min<std::uint32_t>(history.count.load(std::memory_order_acquire), static_cast<std::uint32_t>(history.values.size()));
    for (std::uint32_t i = 0; i < count; ++i) list.push_back(history.values[i].load(std::memory_order_relaxed));
    return list;
}

std::uintptr_t set_float_hook(std::uintptr_t setter, std::uintptr_t instance, float value) {
    // Outside a take this is two loads. A value of 0 is the graph clearing it after the trick: left as it is.
    if (take_until.load(std::memory_order_relaxed) && value > 0 && in_take(GetTickCount64())) {
        if (const auto index = floats.find(setter - values::descriptor_to_setter); index >= 0) {
            auto& history = histories[static_cast<std::size_t>(index)];
            remember(history, value);
            value *= history.scale.load(std::memory_order_relaxed);
        }
    }
    return original_set_float.load(std::memory_order_acquire)(setter, instance, value);
}
std::uintptr_t set_int_hook(std::uintptr_t setter, std::uintptr_t instance, std::int32_t value) {
    if (value > 0 && ints.find(setter - values::descriptor_to_setter) == 0) {
        const auto now = GetTickCount64();
        latest_grade.store(value, std::memory_order_relaxed);
        latest_grade_ms.store(now, std::memory_order_release);
        if (in_take(now)) take_grade.store(value, std::memory_order_relaxed);
    }
    return original_set_int.load(std::memory_order_acquire)(setter, instance, value);
}

// The empty string, or why the setter could not be hooked. The original is published before the hook is live, so
// the first call through it forwards at once.
template <class Original>
std::string hook_setter(std::uintptr_t base, const game::build::Fingerprint& setter, void* replacement, std::atomic<Original>& original) {
    std::array<unsigned char, 32> bytes{};
    if (!base) return "the game is not loaded";
    if (!memory::peek(base + setter.rva, bytes) || bytes != setter.bytes) return "the game's value setter is not the one this was written for";
    const auto target = reinterpret_cast<void*>(base + setter.rva);
    void* relay{};
    if (const auto status = hook_prepare(target, replacement, &relay); status != HookOk)
        return std::string("the value setter could not be hooked: ") + hook_status_string(status);
    original.store(reinterpret_cast<Original>(relay), std::memory_order_release);
    if (const auto status = hook_enable(target); status != HookOk) return std::string("the value setter could not be hooked: ") + hook_status_string(status);
    return {};
}
} // namespace

std::string_view pop_name(PopHeight height) noexcept { return height == 1 ? "low" : height == 2 ? "mid" : height == 3 ? "high" : ""; }
PopHeight pop_height_of(std::string_view name) noexcept { return name == "low" ? 1 : name == "mid" ? 2 : name == "high" ? 3 : 0; }

bool install_grading(std::uintptr_t base, std::string& why) {
    // Tried once: the answer, and why not, stay for the rest of the game.
    static const std::string failure = hook_setter(base, values::int_setter, reinterpret_cast<void*>(&set_int_hook), original_set_int);
    why = failure;
    return failure.empty();
}
bool install_scaling(std::uintptr_t base, std::string& why) {
    if (!install_grading(base, why)) return false;
    static const std::string failure = hook_setter(base, values::float_setter, reinterpret_cast<void*>(&set_float_hook), original_set_float);
    why = failure;
    return failure.empty();
}
void begin_take(float strength_scale, float flip_speed_scale, std::uint64_t lasts_ms) noexcept {
    histories[strength_index].scale.store(strength_scale > 0 ? strength_scale : 1.0f, std::memory_order_relaxed);
    histories[flip_speed_index].scale.store(flip_speed_scale > 0 ? flip_speed_scale : 1.0f, std::memory_order_relaxed);
    for (auto& history : histories) history.count.store(0, std::memory_order_relaxed);
    take_grade.store(0, std::memory_order_relaxed);
    take_until.store(GetTickCount64() + lasts_ms, std::memory_order_release);
}
void end_take() noexcept {
    take_until.store(0, std::memory_order_release);
    for (auto& history : histories) history.scale.store(1.0f, std::memory_order_relaxed);
}
Written written() { return {values_of(histories[strength_index]), values_of(histories[flip_speed_index])}; }
PopHeight take_pop_height() noexcept { return take_grade.load(std::memory_order_relaxed); }
PopHeight pop_height_since(std::uint64_t since_ms) noexcept {
    if (latest_grade_ms.load(std::memory_order_acquire) < since_ms) return 0;
    return latest_grade.load(std::memory_order_relaxed);
}
} // namespace dingosdk::anim_studio::trick_intents
