#pragma once
#include <cstdint>
#include <optional>
#include <string>

// skate.'s score HUD as values (ScoringHUDViewModel, read through the game's UI models), what
// trick_watch.h turns into tricks.
namespace dingosdk::tricks {
struct ScoreHud {
    bool action{}; // a trick is going on
    bool slam{};
    float line_score{}, multiplier{};
    // The sequence's header ("Kickflip", "Manual"...) and descriptor labels as the HUD shows them,
    // and its sequence state: empty, or 0, when this build's model does not have them readable.
    std::string header, descriptor;
    std::uint32_t sequence_state{};
};
// Whether the HUD names a manual going on (its header or descriptor says one).
bool names_manual(const ScoreHud& now);
// Client thread. Empty while there is no score HUD model (the menu, a level loading), or when this
// game build's model has none of the fields; the model is looked for at most once a second.
std::optional<ScoreHud> read_score_hud(std::uintptr_t image_base) noexcept;
}
