#pragma once
#include "actor_control_policy.h"
#include <cstdint>
#include <limits>

namespace dingosdk::actor_context {
enum class Continuity : std::uint8_t { native_lifetime, observed_context };
constexpr bool valid(Continuity value) noexcept {
    return value == Continuity::native_lifetime || value == Continuity::observed_context;
}
constexpr std::uint64_t generation(Continuity value, std::uint64_t native, std::uint64_t context) noexcept {
    return value == Continuity::observed_context ? context : native;
}
constexpr bool advance(std::uint64_t& generation) noexcept {
    if (generation == std::numeric_limits<std::uint64_t>::max()) return false;
    ++generation; return true;
}
constexpr bool published(const actor_control::Snapshot& snapshot, std::uint64_t owner,
    actor_control::Ticket ticket) noexcept {
    return owner && ticket && snapshot.available && snapshot.owner == owner && snapshot.ticket == ticket &&
        snapshot.domain == actor_control::Domain::equipment && snapshot.phase == actor_control::Phase::published;
}
// The global destruction epoch brackets a current observation. It is not the
// durable identity of a local actor, and does not qualify child-object ABA.
template<class Binding> constexpr bool same_identity(const Binding& left, const Binding& right) noexcept {
    return left.owner == right.owner && left.game_context == right.game_context &&
        left.world == right.world && left.world_state == right.world_state && left.actor_epoch == right.actor_epoch;
}
template<class Binding> constexpr bool generation_changed(bool previous_ready, bool ready,
    const Binding& previous, const Binding& current) noexcept {
    return previous_ready != ready || !same_identity(previous, current);
}
constexpr bool same_observation(std::uint64_t before, std::uint64_t after) noexcept {
    return before && before == after;
}
template<class Binding> constexpr bool observed_identity(const Binding& expected, const Binding& current,
    std::uintptr_t watched_entity, std::uint64_t actor_epoch) noexcept {
    return expected.owner.entity && watched_entity == expected.owner.entity &&
        actor_epoch == expected.actor_epoch && same_identity(expected, current);
}
} // namespace dingosdk::actor_context
