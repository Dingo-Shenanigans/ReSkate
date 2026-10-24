#pragma once
namespace dingosdk {
// Some stock worlds do not appear in the supplemental sublevel registries.
// An exact destination plus an initialized, controllable local world is an
// independent completion signal. Merely reaching state 13 (the splash also
// uses it), or retaining an actor from a different map, is insufficient.
inline constexpr bool playable_load_complete(bool transitioned, unsigned state,
        bool exact_destination, bool context_ready, bool players_available,
        unsigned local_players, unsigned controllables) noexcept {
    return transitioned && (state == 13 || state == 21) && exact_destination &&
        context_ready && players_available && local_players == 1 && controllables == 1;
}
}
