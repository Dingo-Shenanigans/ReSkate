#include "Engine/Game/Skater/actor_control_policy.h"
#include <array>
#include <barrier>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <type_traits>

namespace c = dingosdk::actor_control;
namespace {
unsigned checks{};
void require(bool condition, const char* why) {
    ++checks;
    if (!condition) { std::cerr << "actor control: " << why << '\n'; std::exit(1); }
}
void equipment_and_follow_share_one_gate() {
    c::Arbiter gate; c::Ticket equipment, follow;
    require(gate.publication_allowed(), "idle permits legacy publication");
    require(!gate.reserve(0, c::Domain::equipment, equipment), "zero VM rejected");
    require(!gate.reserve(7, static_cast<c::Domain>(99), equipment), "unknown domain rejected");
    require(gate.reserve(7, c::Domain::equipment, equipment), "equipment reserves before native admission");
    require(!gate.publication_allowed(), "admission already blocks legacy publication");
    require(!gate.reserve(7, c::Domain::follow, follow) && !follow, "same VM follow cannot overwrite action admission");
    require(!gate.reserve(8, c::Domain::follow, follow), "other VM cannot overwrite admission");
    require(!gate.retire(7, equipment) && !gate.revoke(7, equipment), "observer cleanup cannot clear admitting action");
    require(!gate.publish(8, equipment) && !gate.abandon(8, equipment), "cross VM admission mutation rejected");
    require(gate.owns(7, equipment, c::Domain::equipment) && !gate.owns(7, equipment, c::Domain::follow), "domain is part of ownership");
    require(gate.publish(7, equipment), "original admission publishes");
    require(!gate.abandon(7, equipment), "published claim is not abandoned as an admission");
    require(gate.revoke(7, equipment) && gate.retire(7, equipment), "terminal equipment retires exact generation");
    require(gate.reserve(8, c::Domain::follow, follow) && follow.generation > equipment.generation, "follow gets later generation");
    require(!gate.publish(7, equipment) && !gate.abandon(7, equipment) && !gate.revoke(7, equipment) && !gate.retire(7, equipment),
        "stale equipment cleanup cannot affect subsequent follow");
    require(!gate.publication_allowed() && gate.owns(8, follow, c::Domain::follow), "follow admission remains reserved");
    require(gate.abandon(8, follow) && gate.publication_allowed(), "failed follow admission explicitly abandons own token");
}
void release_requires_original_drain_and_body_park() {
    c::Arbiter gate; c::Ticket ticket, other;
    c::Call original, overlapping;
    require(gate.reserve(17, c::Domain::follow, ticket) && gate.publish(17, ticket), "follow admitted");
    require(!gate.enter(18, ticket, original) && !original.active(), "wrong VM cannot enter callback");
    require(gate.enter(17, ticket, original) && gate.enter(17, ticket, overlapping), "two original invocations enter");
    require(!gate.enter(17, ticket, original), "same cookie cannot enter twice");
    const auto required = c::Barrier::body_parked;
    require(!gate.revoke(17, ticket) && !gate.revoke(17, ticket, c::Barrier::native_consumed) &&
        !gate.revoke(17, ticket, static_cast<c::Barrier>(4)),
        "follow cannot omit body park or request unknown proof bits");
    require(gate.revoke(17, ticket, required), "release stops new entries before waiting");
    c::Call late;
    require(!gate.enter(17, ticket, late) && !late.active(), "revoked generation cannot enter new native call");
    require(!gate.retire(17, ticket), "pending proofs and originals hold publication gate");
    require(!gate.satisfy(17, ticket, c::Barrier::native_consumed), "native history consumption is not a prepaid or fabricated SDK proof");
    require(!gate.retire(17, ticket), "missing body park and native calls retain ownership");
    require(gate.leave(original) && !original.active(), "one original return leaves once");
    require(!gate.leave(original) && gate.snapshot().in_flight == 1, "duplicate return cannot subtract another original");
    require(gate.satisfy(17, ticket, c::Barrier::body_parked), "independent body park proof recorded");
    require(!gate.retire(17, ticket), "positive body park cannot bypass outstanding native call");
    require(!gate.reserve(19, c::Domain::equipment, other), "other VM stays blocked through release barrier");
    require(gate.leave(overlapping) && gate.retire(17, ticket), "only all conditions retire");
    require(gate.publication_allowed(), "exact SDK drain and park open legacy gate");
    require(gate.reserve(19, c::Domain::equipment, other), "later native action admitted");
    require(!gate.satisfy(17, ticket, required) && !gate.retire(17, ticket), "old proof cannot retire new action");
    require(gate.abandon(19, other), "later action can abandon");
}
void stronger_follow_keeps_its_explicit_native_barrier() {
    c::Arbiter gate; c::Ticket ticket;
    c::Call original;
    constexpr auto both = c::Barrier::native_consumed | c::Barrier::body_parked;
    require(gate.reserve(57, c::Domain::follow, ticket) && gate.publish(57, ticket), "stronger follow admitted");
    require(gate.enter(57, ticket, original), "stronger follow original entered");
    require(gate.revoke(57, ticket, both), "explicit stronger native/body contract remains supported");
    const auto initial = gate.snapshot();
    require(initial.available && initial.required == both && initial.pending == both && initial.in_flight == 1,
        "stronger follow retains both actual pending barriers and original");
    require(!gate.revoke(57, ticket, c::Barrier::body_parked), "repeated revoke cannot weaken an explicit stronger contract");
    require(gate.revoke(57, ticket, both), "same stronger revoke is idempotent");
    require(gate.satisfy(57, ticket, c::Barrier::body_parked) && !gate.retire(57, ticket), "body park cannot bypass stronger native debt or original");
    require(gate.snapshot().pending == c::Barrier::native_consumed, "only explicitly supplied body proof cleared");
    require(gate.leave(original) && !gate.retire(57, ticket), "drain alone cannot erase explicitly required native proof");
    require(!gate.satisfy(58, ticket, c::Barrier::native_consumed), "foreign owner cannot discharge stronger native proof");
    require(gate.satisfy(57, ticket, c::Barrier::native_consumed) && gate.retire(57, ticket), "both explicit stronger proofs and drain retire exact claim");
    c::Ticket reverse;
    require(gate.reserve(57, c::Domain::follow, reverse) && reverse.generation > ticket.generation && gate.publish(57, reverse), "stronger proof test uses later exact generation");
    require(gate.revoke(57, reverse, both), "second stronger contract accepted");
    require(gate.satisfy(57, reverse, c::Barrier::native_consumed) && !gate.retire(57, reverse), "native proof alone cannot replace mandatory body park");
    require(gate.snapshot().pending == c::Barrier::body_parked, "stronger pending body proof remains exact");
    require(gate.satisfy(57, reverse, c::Barrier::body_parked) && gate.retire(57, reverse), "stronger proofs can arrive in either order");
}
void proof_scope_and_raii() {
    c::Arbiter gate, wrong_gate; c::Ticket ticket;
    require(gate.reserve(27, c::Domain::follow, ticket), "scope admission");
    require(!gate.satisfy(27, ticket, c::Barrier::native_consumed), "proof cannot be prepaid during admission");
    require(gate.publish(27, ticket), "scope publishes");
    require(!gate.satisfy(27, ticket, c::Barrier::native_consumed), "proof cannot be prepaid before revoke");
    {
        c::Call native;
        require(gate.enter(27, ticket, native), "RAII native call");
        require(!wrong_gate.leave(native) && native.active(), "wrong arbiter cannot consume cookie");
        const auto required = c::Barrier::body_parked;
        require(gate.revoke(27, ticket, required), "follow requires its body-park release barrier");
        require(!gate.revoke(27, ticket, c::Barrier::none), "required proofs cannot be weakened on repeated revoke");
        require(gate.revoke(27, ticket, required), "same revoke is idempotent");
        require(!gate.satisfy(28, ticket, c::Barrier::body_parked), "wrong VM cannot satisfy barrier");
        require(!gate.satisfy(27, ticket, static_cast<c::Barrier>(4)), "invalid proof cannot corrupt claim word");
        require(!gate.satisfy(27, ticket, c::Barrier::none), "empty proof is not success");
    }
    require(gate.snapshot().in_flight == 0, "original scope return decrements exact native generation");
    require(!gate.retire(27, ticket), "scope return alone is no body park proof");
    require(!gate.satisfy(27, ticket, c::Barrier::native_consumed), "SDK completion does not claim native history retirement");
    require(gate.satisfy(27, ticket, c::Barrier::body_parked) && gate.retire(27, ticket), "actual call drain and positive body park retire SDK ownership");
}
void capacity_and_generation_exhaustion() {
    c::Arbiter gate; c::Ticket ticket;
    std::array<c::Call, c::Arbiter::maximum_in_flight> calls;
    require(gate.reserve(37, c::Domain::follow, ticket) && gate.publish(37, ticket), "capacity claim");
    for (auto& call : calls) require(gate.enter(37, ticket, call), "bounded native slot enters");
    c::Call excess;
    require(!gate.enter(37, ticket, excess) && !excess.active(), "callback count cannot overflow into generation");
    require(gate.snapshot().in_flight == c::Arbiter::maximum_in_flight, "callback saturation preserves exact count");
    const auto required = c::Barrier::body_parked;
    require(gate.revoke(37, ticket, required), "saturated claim can revoke");
    for (auto& call : calls) require(gate.leave(call), "bounded slot returns");
    require(gate.satisfy(37, ticket, required) && gate.retire(37, ticket), "all saturated calls and release proofs retire");
    c::Arbiter final(c::Arbiter::maximum_generation); c::Ticket last, never;
    require(final.reserve(37, c::Domain::equipment, last) && last.generation == c::Arbiter::maximum_generation, "last representable token");
    require(final.abandon(37, last), "last admission retires");
    require(!final.reserve(37, c::Domain::follow, never) && !never, "token exhaustion fails without wrapping");
    require(final.publication_allowed() && !final.publish(37, last), "exhausted service neither blocks legacy nor revives stale token");
    c::Arbiter invalid(0);
    require(!invalid.reserve(37, c::Domain::equipment, never), "zero seed cannot create zero identity");
}
void simultaneous_domains_contend_on_actual_core() {
    for (unsigned round = 0; round < 64; ++round) {
        c::Arbiter gate;
        std::array<c::Ticket, 2> tickets{};
        std::array<bool, 2> accepted{};
        std::barrier start(3), finished(3);
        auto run = [&](unsigned index) {
            start.arrive_and_wait();
            accepted[index] = gate.reserve(47 + index, index == 0 ? c::Domain::equipment : c::Domain::follow, tickets[index]);
            finished.arrive_and_wait();
        };
        std::thread equipment(run, 0), follow(run, 1);
        start.arrive_and_wait(); finished.arrive_and_wait();
        equipment.join(); follow.join();
        require(accepted[0] != accepted[1], "concurrent equipment/follow have exactly one winner");
        const unsigned winner = accepted[0] ? 0 : 1;
        require(!tickets[1 - winner] && !gate.publication_allowed(), "loser gets no token and cannot open gate");
        require(gate.abandon(47 + winner, tickets[winner]) && gate.publication_allowed(), "only admission winner retires");
    }
}
void contended_scope_returns_preserve_exact_drain() {
    c::Arbiter gate; c::Ticket ticket;
    constexpr std::uint64_t owner = 67;
    constexpr unsigned churn_calls = 128, workers = 16;
    constexpr auto required = c::Barrier::native_consumed | c::Barrier::body_parked;
    require(gate.reserve(owner, c::Domain::follow, ticket) && gate.publish(owner, ticket), "contended follow publishes");
    {
        c::Call retained;
        require(gate.enter(owner, ticket, retained), "one exact cookie retains the generation throughout contention");
        unsigned admitted{};
        std::barrier churn_start(2), churn_done(2);
        std::thread churn([&] {
            churn_start.arrive_and_wait();
            for (unsigned i = 0; i < churn_calls; ++i) {
                c::Call original;
                if (gate.enter(owner, ticket, original)) ++admitted;
                // Each successful iteration destroys a different admitted cookie.
            }
            churn_done.arrive_and_wait();
        });
        churn_start.arrive_and_wait();
        churn_done.arrive_and_wait();
        churn.join();
        require(admitted == churn_calls && gate.snapshot().in_flight == 1,
            "more than 64 competing enter/return pairs leave exactly the retained cookie");

        std::array<bool, workers> entered{};
        std::array<std::thread, workers> originals;
        std::barrier entered_all(workers + 1), return_all(workers + 1), returned_all(workers + 1);
        for (unsigned i = 0; i < workers; ++i) {
            originals[i] = std::thread([&, i] {
                {
                    c::Call original;
                    entered[i] = gate.enter(owner, ticket, original);
                    entered_all.arrive_and_wait();
                    return_all.arrive_and_wait();
                }
                returned_all.arrive_and_wait();
            });
        }
        entered_all.arrive_and_wait();
        for (const auto accepted : entered) require(accepted, "every contending original owns its own cookie");
        require(gate.snapshot().in_flight == workers + 1, "synchronized originals contribute exact independent counts");
        require(gate.revoke(owner, ticket, required), "revocation retains all active cookies and explicit barriers");
        require(gate.satisfy(owner, ticket, c::Barrier::body_parked), "body proof precedes concurrent scope returns");
        require(!gate.retire(owner, ticket), "pending native proof and original scopes prevent retirement");
        return_all.arrive_and_wait();
        // Race the other barrier's update with actual scope destruction. Its
        // bounded refusal is permitted; the exact callback decrements are not.
        const bool proof_during_drain = gate.satisfy(owner, ticket, c::Barrier::native_consumed);
        returned_all.arrive_and_wait();
        for (auto& original : originals) original.join();
        require(proof_during_drain || gate.satisfy(owner, ticket, c::Barrier::native_consumed), "native proof remains satisfiable after contention");
        const auto after = gate.snapshot();
        require(after.available && after.ticket == ticket && after.owner == owner && after.phase == c::Phase::revoking &&
            after.in_flight == 1 && after.required == required && after.pending == c::Barrier::none,
            "concurrent destructors remove only their counts without losing the retained cookie or barrier state");
        require(!gate.retire(owner, ticket), "resolved barriers cannot retire the still-live exact cookie");
    }
    require(gate.snapshot().in_flight == 0 && gate.retire(owner, ticket), "the final destructor guarantees actual drain and permits retirement");
    c::Ticket replacement;
    require(gate.reserve(owner + 1, c::Domain::equipment, replacement) && replacement.generation > ticket.generation,
        "drained ownership admits a distinct replacement generation");
    require(!gate.retire(owner, ticket) && !gate.satisfy(owner, ticket, c::Barrier::native_consumed) &&
        gate.owns(owner + 1, replacement, c::Domain::equipment), "old cleanup cannot mutate the replacement after exact drain");
    require(gate.abandon(owner + 1, replacement) && gate.publication_allowed(), "no phantom callback remains to trap future writers");
}
static_assert(!std::is_copy_constructible_v<c::Call> && !std::is_move_constructible_v<c::Call>);
static_assert(!std::is_copy_constructible_v<c::Arbiter> && !std::is_move_constructible_v<c::Arbiter>);
}
int main() {
    equipment_and_follow_share_one_gate();
    release_requires_original_drain_and_body_park();
    stronger_follow_keeps_its_explicit_native_barrier();
    proof_scope_and_raii();
    capacity_and_generation_exhaustion();
    simultaneous_domains_contend_on_actual_core();
    contended_scope_returns_preserve_exact_drain();
    std::cout << "actor control: " << checks << " checks passed\n";
}
