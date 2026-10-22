#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace dingosdk::skater {
inline constexpr std::uint32_t server_spawn_event_id = 0x0dc320de;
struct ServerSpawnEventLayout {
    std::uintptr_t server_pointer_address{}, server_vtable{}, spawner_vtable{}, player_vtable{};
};
struct ServerSpawnEventSample {
    std::uintptr_t server{}, context{};
    bool owned{}, event_readable{}, enabled_readable{}, pending_readable{};
    std::uint32_t event_id{}, pending{};
    std::uint8_t enabled{};
    // Raw payload observations, not the result of the native RTTI cast or
    // evidence that the player belongs to the current server's player manager.
    bool raw_weak_present{}, raw_weak_resolved{}, raw_player_type_matches{};
};
namespace server_spawn_event_detail {
inline constexpr std::uintptr_t highest = 0x00007fffffffffffULL;
template<class T, class Read>
bool get(Read& read, std::uintptr_t object, std::uintptr_t offset, T& value) noexcept {
    return object >= 0x10000 && offset <= highest - sizeof(T) &&
        object <= highest - sizeof(T) - offset && read(object + offset, &value, sizeof(T));
}
}
// Fixed-size, passive sampling for a native handler observer. No callbacks,
// allocation, native weak-reference retain/release, or native getters run.
// The reader must be nonthrowing and return false for unreadable memory.
template<class Read>
ServerSpawnEventSample sample_server_spawn_event(const ServerSpawnEventLayout& layout,
    std::uintptr_t source, std::uintptr_t event, Read&& read) noexcept {
    using server_spawn_event_detail::get;
    ServerSpawnEventSample result;
    std::uintptr_t server_type{}, source_type{}, source_context{};
    if (!layout.server_vtable || !layout.spawner_vtable ||
        !get(read, layout.server_pointer_address, 0, result.server) ||
        !get(read, result.server, 0, server_type) || server_type != layout.server_vtable ||
        !get(read, result.server, 8, result.context) || result.context < 0x10000 ||
        !get(read, source, 0, source_type) || source_type != layout.spawner_vtable ||
        !get(read, source, 0x20, source_context) || source_context != result.context)
        return result;
    result.event_readable = get(read, event, 8, result.event_id);
    result.enabled_readable = get(read, source, 0x250, result.enabled);
    std::array<std::uintptr_t, 3> bounds{};
    if (get(read, source, 0x378, bounds) && bounds[0] <= bounds[1] && bounds[1] <= bounds[2] &&
        bounds[2] <= server_spawn_event_detail::highest &&
        (!bounds[2] || bounds[0] >= 0x10000) &&
        bounds[0] % 8 == 0 && bounds[1] % 8 == 0 && bounds[2] % 8 == 0 &&
        (bounds[2] - bounds[0]) / 8 <= 128) {
        result.pending_readable = true;
        result.pending = static_cast<std::uint32_t>((bounds[1] - bounds[0]) / 8);
    }
    if (result.event_readable && result.event_id == server_spawn_event_id &&
        result.enabled_readable && result.enabled) {
        std::uintptr_t weak{}, weak_target{}, player_type{};
        result.raw_weak_present = get(read, event, 0x20, weak) && weak != 0;
        result.raw_weak_resolved = result.raw_weak_present && get(read, weak, 0, weak_target) && weak_target >= 0x10008;
        result.raw_player_type_matches = result.raw_weak_resolved && layout.player_vtable &&
            get(read, weak_target - 8, 0, player_type) && player_type == layout.player_vtable;
    }
    // A changing owner must not be published as a valid before/after sample.
    std::uintptr_t checked_server{}, checked_type{}, checked_context{}, checked_source_type{}, checked_source_context{};
    result.owned = get(read, layout.server_pointer_address, 0, checked_server) && checked_server == result.server &&
        get(read, result.server, 0, checked_type) && checked_type == server_type &&
        get(read, result.server, 8, checked_context) && checked_context == result.context &&
        get(read, source, 0, checked_source_type) && checked_source_type == source_type &&
        get(read, source, 0x20, checked_source_context) && checked_source_context == source_context;
    return result;
}

struct ServerSpawnEventCounts {
    std::uint64_t calls{}, owner_rejected{}, spawn{}, disabled{}, unreadable_gate{};
    std::uint64_t raw_weak_present{}, raw_weak_resolved{}, raw_player_type_matches{};
    std::uint64_t pending_increased{}, pending_added{}, pending_before_max{}, pending_after_max{}, post_owner_changed{};
};
class ServerSpawnEventCounters {
public:
    void before(const ServerSpawnEventSample& sample) noexcept {
        add(calls_);
        if (!sample.owned) { add(owner_rejected_); return; }
        if (!sample.event_readable || sample.event_id != server_spawn_event_id) return;
        add(spawn_);
        if (!sample.enabled_readable) add(unreadable_gate_);
        else if (!sample.enabled) add(disabled_);
        if (sample.raw_weak_present) add(raw_weak_present_);
        if (sample.raw_weak_resolved) add(raw_weak_resolved_);
        if (sample.raw_player_type_matches) add(raw_player_type_matches_);
        if (sample.pending_readable) maximum(pending_before_max_, sample.pending);
    }
    void after(const ServerSpawnEventSample& before_sample, const ServerSpawnEventSample& after_sample) noexcept {
        if (!before_sample.owned || !before_sample.event_readable || before_sample.event_id != server_spawn_event_id) return;
        if (!after_sample.owned || before_sample.server != after_sample.server || before_sample.context != after_sample.context) {
            add(post_owner_changed_); return;
        }
        if (after_sample.pending_readable) maximum(pending_after_max_, after_sample.pending);
        if (before_sample.pending_readable && after_sample.pending_readable && after_sample.pending > before_sample.pending) {
            add(pending_increased_); add(pending_added_, after_sample.pending - before_sample.pending);
        }
    }
    ServerSpawnEventCounts snapshot() const noexcept {
        return {get(calls_), get(owner_rejected_), get(spawn_), get(disabled_), get(unreadable_gate_),
            get(raw_weak_present_), get(raw_weak_resolved_), get(raw_player_type_matches_),
            get(pending_increased_), get(pending_added_), get(pending_before_max_), get(pending_after_max_), get(post_owner_changed_)};
    }
private:
    using Counter = std::atomic<std::uint64_t>;
    static void add(Counter& counter, std::uint64_t value = 1) noexcept { counter.fetch_add(value, std::memory_order_relaxed); }
    static std::uint64_t get(const Counter& counter) noexcept { return counter.load(std::memory_order_relaxed); }
    static void maximum(Counter& counter, std::uint64_t value) noexcept {
        auto previous = get(counter);
        while (value > previous && !counter.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {}
    }
    Counter calls_{}, owner_rejected_{}, spawn_{}, disabled_{}, unreadable_gate_{};
    Counter raw_weak_present_{}, raw_weak_resolved_{}, raw_player_type_matches_{};
    Counter pending_increased_{}, pending_added_{}, pending_before_max_{}, pending_after_max_{}, post_owner_changed_{};
};
}
