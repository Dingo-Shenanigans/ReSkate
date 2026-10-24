#pragma once

// When a trick lands and what it was worth, from the score HUD's values each tick (score_hud.h).
// Apart from the game, so its rules are tested alone. The start event is where a caller records
// where a trick began.
//
// Measured in play (a recording of the score HUD's values in a play session, 2026-10-14):
// ActionActive is on while a trick goes on; LineScore rises one tick after it goes off. LineScore
// looks like the line's points so far times the Multiplier (a rise in the multiplier also raises
// earlier tricks' share), so a trick's points are taken from the unscaled rise, at the new
// multiplier (not yet checked against the game's own trick list).
namespace dingosdk::tricks {
// The score HUD's values (ScoringHUDViewModel), and whether the skater is in a wipeout (physics
// states 300-399).
struct TrickSignals {
    bool action{};
    float line_score{}, multiplier{};
    bool slam{}, wipeout{};
};
struct TrickEvent {
    enum class Kind { none, started, landed, broken } kind{Kind::none};
    float points{};     // landed: the trick's points at the multiplier it raised the line to
    float multiplier{}; // landed: that multiplier
};
class TrickWatch {
public:
    // `wait_ticks`: how long after a trick ends its points may take to show in the line's score.
    explicit TrickWatch(int wait_ticks = 20) : wait_ticks_(wait_ticks) {}
    TrickEvent update(const TrickSignals& signals);
    bool in_trick() const noexcept { return action_; }
    // Waiting for the points of a trick that just ended.
    bool waiting() const noexcept { return waiting_ > 0; }

private:
    int wait_ticks_;
    bool seen_{}, action_{};
    int waiting_{};
    float score_{}, multiplier_{}; // the line's score and multiplier before the trick
};
}
