#pragma once
#include <chrono>
#include <cstdint>
#include <optional>

// S.K.A.T.E. with no repeated tricks: a trick that was already set in this game does not
// count as a set. The setter's landed attempt is turned into a missed set, which the
// game shows as a failed set, and the round rule then passes the set to the next player.
//
// ServerSkateThrowdown's attempt handler (graph 0xc4a37359) finds the setter with one
// ArrayIndexOf (pc 0x60c), after copying the attempt's WasSuccessful into local L811 and
// its trick record into L378, and before branching on L811 (pc 0x690). That one call is
// pointed at a wrapper that compares the record with the tricks already set in the event
// and clears L811 for a repeat. Every machine's server runs the handler for every attempt,
// its own and the relayed ones, in the same order, so every machine keeps the same
// history and reaches the same outcome.
namespace dingosdk::multiplayer {
// Called by the expression pump after the handler graph has run (server realm): patches
// each loaded copy once, recording the set its first unpatched run made.
bool skate_attempt_prepare(std::uintptr_t base,std::uintptr_t vm) noexcept;
void skate_attempt_ran(std::uintptr_t base, std::uintptr_t vm) noexcept;
// The last set refused as a repeat, for the HUD: whose it was and when.
struct SkateRepeat {
    std::uint32_t player{};
    std::chrono::steady_clock::time_point at{};
};
std::optional<SkateRepeat> skate_last_repeat() noexcept;
} // namespace dingosdk::multiplayer
