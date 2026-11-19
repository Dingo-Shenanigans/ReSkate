#include "Engine/Core/Hooks/callback_listeners.h"
#include "Engine/Game/Skater/actor_clock.h"
#include "Engine/Game/Skater/actor_context.h"
#include "Engine/Game/Skater/actor_control_policy.h"
#include "Extension/Skater/actor_control.h"
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace context = dingosdk::actor_context;
namespace control = dingosdk::actor_control;
namespace clock_model = dingosdk::actor_clock;
namespace {
unsigned checks{};
void check(bool value, std::string_view why) {
    ++checks;
    if (!value) { std::cerr << "actor context: " << why << '\n'; std::exit(1); }
}

// Synthetic equality tokens only. The native owner is private to no_bail.cpp;
// these fields exercise the helper's whole-owner comparison without game reads.
struct Owner {
    std::uintptr_t client{}, entity{}, player{}, handle{}, component{}, core{}, context{}, selector{}, causes{}, rig{};
    bool operator==(const Owner&) const = default;
};
struct Binding {
    Owner owner;
    std::uintptr_t game_context{}, world{}, world_state{};
    std::uint64_t world_generation{}, lifetime_epoch{}, actor_epoch{};
    bool operator==(const Binding&) const = default;
};
const Binding first{{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, 11, 12, 13, 17, 1, 23};
using Clock = clock_model::Ledger<Binding>;
using Key = clock_model::Key<Binding>;
using Feed = clock_model::Feed<Binding>;

std::uint64_t idle(const dingosdk::CallbackEpoch& epoch) {
    std::uint64_t value{};
    check(epoch.idle(value) && value, "constructed destruction gate is idle");
    return value;
}
void scope_is_explicit_and_strict_by_default() {
    static_assert(dingosdk::actor_control_native::Actor{}.continuity == context::Continuity::native_lifetime);
    check(context::valid(context::Continuity::native_lifetime) && context::valid(context::Continuity::observed_context),
        "both named continuity contracts are valid");
    check(!context::valid(static_cast<context::Continuity>(2)) && !context::valid(static_cast<context::Continuity>(255)),
        "unknown continuity values must be refused by callers");
    check(context::generation(context::Continuity::native_lifetime, 31, 37) == 31,
        "strict contract selects the native-lifetime generation");
    check(context::generation(context::Continuity::observed_context, 31, 37) == 37,
        "observed contract explicitly selects the context generation");
}
void context_and_revision_counters_cannot_wrap() {
    std::uint64_t initial{};
    check(context::advance(initial) && initial == 1, "first explicit generation is nonzero");
    auto last = std::numeric_limits<std::uint64_t>::max() - 1;
    check(context::advance(last) && last == std::numeric_limits<std::uint64_t>::max(),
        "last representable context or revision value may be issued");
    check(!context::advance(last) && last == std::numeric_limits<std::uint64_t>::max(),
        "exhausted counter refuses without wrapping or modifying its final value");
    check(!context::advance(last), "another attempt cannot revive an exhausted generation");
}
void between_operations_is_distinct_from_an_overlapping_destruction() {
    dingosdk::CallbackEpoch gate;
    auto admitted = first; admitted.lifetime_epoch = idle(gate);
    gate.enter(); gate.leave();
    auto current = admitted; current.lifetime_epoch = idle(gate);
    check(!context::same_observation(admitted.lifetime_epoch, current.lifetime_epoch),
        "strict admission-global witness rejects even a completed intervening invocation");
    check(context::observed_identity(admitted, current, admitted.owner.entity, admitted.actor_epoch),
        "observed identity can remain equal after a completed invocation between operations");
    check(!context::generation_changed(true, true, admitted, current),
        "only an epoch delta cannot fabricate a new observed context generation");
    const auto operation = idle(gate);
    check(context::same_observation(operation, idle(gate)), "one unchanged current idle bracket qualifies");
    gate.enter();
    std::uint64_t active = 99;
    check(!gate.idle(active) && !active && !context::same_observation(operation, active),
        "in-flight destruction refuses the current operation");
    gate.enter(); gate.leave();
    check(!gate.idle(active) && !context::same_observation(operation, active),
        "one nested exit cannot qualify an outstanding destruction");
    gate.leave();
    check(!context::same_observation(operation, idle(gate)),
        "a destruction entirely inside the operation is detected after it returns");
    check(!context::same_observation(0, 0) && !context::same_observation(0, operation) &&
        !context::same_observation(operation, 0), "unknown endpoint epochs never form an observation");
}
void watched_actor_and_every_identity_field_are_required() {
    check(context::observed_identity(first, first, first.owner.entity, first.actor_epoch),
        "matching full owner and watched-root counter qualify the identity predicate");
    check(!context::observed_identity(first, first, 0, first.actor_epoch) &&
        !context::observed_identity(first, first, first.owner.entity + 1, first.actor_epoch),
        "an unset or different watched entity is not covered by an equal counter");
    check(!context::observed_identity(first, first, first.owner.entity, first.actor_epoch + 1),
        "same-address root reuse is rejected by its destruction counter");
    auto reused = first; ++reused.actor_epoch;
    check(!context::observed_identity(first, reused, first.owner.entity, first.actor_epoch),
        "a fresh capture cannot replace the admitted root generation");
    auto absent = first; absent.owner.entity = 0;
    check(!context::observed_identity(absent, absent, 0, absent.actor_epoch), "no actor cannot qualify itself");
    struct OwnerField { std::uintptr_t Owner::* member; const char* name; };
    const std::array fields{OwnerField{&Owner::client, "client"}, OwnerField{&Owner::entity, "entity"},
        OwnerField{&Owner::player, "player"}, OwnerField{&Owner::handle, "player handle"},
        OwnerField{&Owner::component, "component"}, OwnerField{&Owner::core, "core"},
        OwnerField{&Owner::context, "input context"}, OwnerField{&Owner::selector, "selector"},
        OwnerField{&Owner::causes, "causes"}, OwnerField{&Owner::rig, "rig"}};
    for (const auto& field : fields) {
        auto changed = first; ++(changed.owner.*field.member);
        check(!context::observed_identity(first, changed, first.owner.entity, first.actor_epoch),
            std::string("changed owner field refuses: ") + field.name);
        check(context::generation_changed(true, true, first, changed), "changed owner advances observed context identity");
    }
    const std::array worlds{&Binding::game_context, &Binding::world, &Binding::world_state};
    for (const auto member : worlds) {
        auto changed = first; ++(changed.*member);
        check(!context::observed_identity(first, changed, first.owner.entity, first.actor_epoch) &&
            context::generation_changed(true, true, first, changed), "changed game context/world/state refuses identity");
    }
    check(context::generation_changed(true, true, first, reused), "exact-root destruction changes the observed context");
}
void readiness_and_context_generations_do_not_rebase_a_lease() {
    check(context::generation_changed(true, false, first, first), "readiness loss invalidates the observed context");
    check(context::generation_changed(false, true, first, first), "readiness recovery is a new admission boundary");
    check(!context::generation_changed(false, false, first, first),
        "repeated unchanged unavailability does not fabricate context generations");
    auto advanced = first; ++advanced.world_generation; ++advanced.lifetime_epoch;
    check(context::same_identity(first, advanced), "transient epoch and selected generation are separate from pointer identity");
    check(context::generation(context::Continuity::observed_context, 101, first.world_generation) !=
        context::generation(context::Continuity::observed_context, 101, advanced.world_generation),
        "an actual context-generation change still refuses a saved observed lease");
    check(context::generation(context::Continuity::native_lifetime, 101, 17) !=
        context::generation(context::Continuity::native_lifetime, 102, 17),
        "strict callers retain native-generation invalidation");
}
void permitted_global_changes_preserve_the_exact_clock_key_and_time() {
    dingosdk::CallbackEpoch gate;
    auto binding = first; binding.lifetime_epoch = idle(gate);
    const Key key{7, 11, 19, binding};
    Clock clock; Feed feed; clock_model::Sample before, after;
    check(clock.enable(key) && clock.begin(key, feed) && clock.publish(feed, .01, 100) && clock.read(key, 100, before),
        "first actually published observation establishes a clock sample");
    gate.enter(); gate.leave();
    auto current = binding; current.lifetime_epoch = idle(gate);
    check(context::observed_identity(binding, current, binding.owner.entity, binding.actor_epoch),
        "a separate completed invocation does not change the observed identity");
    check(clock.begin(key, feed) && clock.publish(feed, .02, 120) && clock.read(key, 120, after),
        "the original immutable key continues without another enable or reseed");
    check(after.serial > before.serial && after.seconds == .01 + .02 && after.observed_ms == 120 &&
        key.actor.lifetime_epoch == binding.lifetime_epoch, "serial/time advance without rebasing the admitted key");
    auto rebased = key; rebased.actor = current;
    check(!clock.read(rebased, 120, after) && !clock.begin(rebased, feed) && !clock.enable(rebased),
        "fresh global capture cannot be copied into an existing clock key");
    check(clock.read(key, 120, after) && after.seconds == .01 + .02, "rejected rekey leaves the real sample intact");
}
void rejected_candidate_clock_publications_cannot_leak() {
    dingosdk::CallbackEpoch gate;
    const Key key{7, 11, 19, first};
    Clock clock; Feed feed; clock_model::Sample sample;
    check(clock.enable(key) && clock.begin(key, feed) && clock.publish(feed, .01, 100), "baseline clock is real");
    const auto before = idle(gate);
    auto candidate = clock;
    check(candidate.begin(key, feed) && candidate.publish(feed, .02, 120), "candidate uses the actual clock ledger");
    gate.enter(); gate.leave();
    const auto after = idle(gate);
    check(!context::same_observation(before, after), "closing bracket rejects the candidate");
    // This is the constructed commit boundary, not a native binding test. A
    // failed global bracket must discard the candidate, never expose its time.
    if (context::same_observation(before, after)) clock = candidate;
    check(clock.read(key, 120, sample) && sample.serial == 1 && sample.seconds == .01 && sample.observed_ms == 100,
        "later idle reads cannot see rejected candidate serial/time");
    const auto fresh = idle(gate);
    candidate = clock;
    check(candidate.begin(key, feed) && candidate.publish(feed, .03, 130) &&
        context::same_observation(fresh, idle(gate)), "a separately qualified candidate has a stable bracket");
    clock = candidate;
    check(clock.read(key, 130, sample) && sample.serial == 2 && sample.seconds == .01 + .03,
        "only committed observations accumulate time; missing observations are not synthesized");
}
void old_tickets_and_callbacks_cannot_cross_to_a_new_claim() {
    control::Arbiter arbiter; control::Ticket old_ticket, next_ticket;
    control::Call active, late;
    check(arbiter.reserve(7, control::Domain::equipment, old_ticket) && arbiter.publish(7, old_ticket), "first claim publishes");
    const Key old_key{7, 11, old_ticket.generation, first};
    Clock clock; Feed old_feed; clock_model::Sample sample;
    check(clock.enable(old_key) && clock.begin(old_key, old_feed) && arbiter.enter(7, old_ticket, active),
        "original callback retains the exact old claim and clock feed");
    check(arbiter.revoke(7, old_ticket) && !arbiter.enter(7, old_ticket, late) && !arbiter.retire(7, old_ticket),
        "revocation refuses new calls and cannot retire an in-flight original");
    check(!arbiter.reserve(8, control::Domain::equipment, next_ticket), "another owner cannot replace an in-flight claim");
    check(clock.disable(old_key) && !clock.publish(old_feed, .01, 100), "late old feed is refused after clock disable");
    check(arbiter.leave(active) && arbiter.retire(7, old_ticket) &&
        arbiter.reserve(8, control::Domain::equipment, next_ticket) && arbiter.publish(8, next_ticket),
        "only drained retirement permits a later claim");
    const Key next_key{8, 12, next_ticket.generation, first};
    check(next_ticket.generation > old_ticket.generation && clock.enable(next_key), "new claim has its own monotonic generation");
    check(!arbiter.enter(7, old_ticket, late) && !arbiter.revoke(7, old_ticket) && !arbiter.retire(7, old_ticket),
        "old claim operations cannot mutate the new owner");
    check(!clock.publish(old_feed, .01, 110) && !clock.disable(old_key) && !clock.enable(old_key) &&
        !clock.read(next_key, 110, sample), "old callbacks cannot publish, disable, re-enable or seed the new clock");
    Feed next_feed;
    check(clock.begin(next_key, next_feed) && clock.publish(next_feed, .02, 120) && clock.read(next_key, 120, sample) &&
        sample.seconds == .02, "the new claim starts only with its own qualified observation");
}
void cleanup_ownership_does_not_grant_published_clock_authority() {
    control::Arbiter arbiter; control::Ticket ticket, next;
    control::Call active;
    check(arbiter.reserve(7, control::Domain::equipment, ticket), "equipment claim reserves");
    check(!context::published(arbiter.snapshot(), 7, ticket), "admission is not published clock authority");
    check(arbiter.publish(7, ticket) && context::published(arbiter.snapshot(), 7, ticket),
        "exact published equipment snapshot grants clock authority");
    const auto published = arbiter.snapshot();
    check(!context::published(published, 0, ticket) && !context::published(published, 7, {}) &&
        !context::published(published, 8, ticket) && !context::published(published, 7, {ticket.generation + 1}),
        "zero or wrong owner and zero or wrong ticket cannot use a published snapshot");
    auto unknown = published; unknown.available = false;
    check(!context::published(unknown, 7, ticket) && !context::published({}, 7, ticket),
        "unavailable or unknown snapshots never grant clock authority");
    control::Arbiter follow; control::Ticket follow_ticket;
    check(follow.reserve(7, control::Domain::follow, follow_ticket) && follow.publish(7, follow_ticket) &&
        !context::published(follow.snapshot(), 7, follow_ticket), "a real published follow claim is the wrong clock domain");
    check(arbiter.enter(7, ticket, active) && arbiter.revoke(7, ticket), "active original remains retained during revocation");
    check(arbiter.owns(7, ticket, control::Domain::equipment) && active.active() &&
        arbiter.snapshot().phase == control::Phase::revoking, "revoking claim still owns its cleanup duty and original call");
    check(!context::published(arbiter.snapshot(), 7, ticket) && !arbiter.retire(7, ticket),
        "retained cleanup ownership cannot authorize clock work or bypass an in-flight call");
    check(arbiter.leave(active) && arbiter.retire(7, ticket) &&
        !context::published(arbiter.snapshot(), 7, ticket), "drained retirement remains unavailable to the old ticket");
    check(arbiter.reserve(7, control::Domain::equipment, next) && next.generation > ticket.generation &&
        arbiter.publish(7, next), "same owner may later publish a different generation");
    check(!context::published(arbiter.snapshot(), 7, ticket) && context::published(arbiter.snapshot(), 7, next),
        "an old ticket cannot borrow the same owner's later published authority");
}
} // namespace
int main() {
    scope_is_explicit_and_strict_by_default();
    context_and_revision_counters_cannot_wrap();
    between_operations_is_distinct_from_an_overlapping_destruction();
    watched_actor_and_every_identity_field_are_required();
    readiness_and_context_generations_do_not_rebase_a_lease();
    permitted_global_changes_preserve_the_exact_clock_key_and_time();
    rejected_candidate_clock_publications_cannot_leak();
    old_tickets_and_callbacks_cannot_cross_to_a_new_claim();
    cleanup_ownership_does_not_grant_published_clock_authority();
    std::cout << "actor_context: 9 cases, " << checks << " checks, native_calls=0\n";
}
