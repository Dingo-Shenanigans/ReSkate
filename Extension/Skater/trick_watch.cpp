#include "trick_watch.h"
#include <algorithm>
#include <utility>

namespace dingosdk::tricks {
namespace {
// The line's points before the multiplier.
float unscaled(float score, float multiplier) { return multiplier > 0.01f ? score / multiplier : score; }
}

TrickEvent TrickWatch::update(const TrickSignals& in) {
    if (!std::exchange(seen_, true)) {
        score_ = in.line_score;
        multiplier_ = in.multiplier;
    }
    // A slam or a wipeout: skate. drops the line, and the trick going on scores nothing.
    if (in.slam || in.wipeout) {
        const bool had = action_ || waiting_;
        action_ = false;
        waiting_ = 0;
        score_ = in.line_score;
        multiplier_ = in.multiplier;
        return had ? TrickEvent{TrickEvent::Kind::broken} : TrickEvent{};
    }
    if (in.action) {
        if (action_) return {};
        // A trick starts (a previous one whose points never came scored nothing).
        action_ = true;
        waiting_ = 0;
        return {TrickEvent::Kind::started};
    }
    if (action_) { // the trick ended this tick: its points show in the line's score shortly
        action_ = false;
        waiting_ = wait_ticks_;
    }
    if (waiting_ > 0) {
        if (in.line_score > score_ + 0.5f) {
            const float scale = in.multiplier > 0.01f ? in.multiplier : 1.0f;
            const float points = std::max(0.0f, (unscaled(in.line_score, in.multiplier) - unscaled(score_, multiplier_)) * scale);
            score_ = in.line_score;
            multiplier_ = in.multiplier;
            waiting_ = 0;
            return {TrickEvent::Kind::landed, points, scale};
        }
        if (--waiting_ > 0) return {};
    }
    // No trick pending: the line's score follows whatever else moves it (its end resets it to 0).
    score_ = in.line_score;
    multiplier_ = in.multiplier;
    return {};
}
}
