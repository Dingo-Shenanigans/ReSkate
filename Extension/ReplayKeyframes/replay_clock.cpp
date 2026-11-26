#include "replay_clock.h"
#include <algorithm>
#include <cmath>

namespace dingosdk::replay_keyframes {
float share_of(const Timeline& timeline, double clock) noexcept {
    if (timeline.chunks.empty())
        return timeline.length > 0 ? static_cast<float>((clock - timeline.start) / timeline.length) : -1.0f;
    // (Within a frame of a chunk's ends counts as in it.)
    constexpr double slack = 1.0 / 60.0;
    for (const auto& c : timeline.chunks)
        if (clock >= c.clock_from - slack && clock <= c.clock_to + slack) {
            const double span = c.clock_to - c.clock_from;
            const double along = span > 1e-9 ? std::clamp((clock - c.clock_from) / span, 0.0, 1.0) : 0.0;
            const float whole = c.from + static_cast<float>(along) * (c.to - c.from);
            // On the part the bar shows (off it, as nowhere: trimmed away).
            const float shown = timeline.shown_to - timeline.shown_from;
            if (shown <= 1e-6f) return whole;
            const float on_bar = (whole - timeline.shown_from) / shown;
            constexpr float edge = 1e-3f;
            return on_bar < -edge || on_bar > 1.0f + edge ? -1.0f : std::clamp(on_bar, 0.0f, 1.0f);
        }
    return -1.0f;
}

double clock_at_share(const Timeline& timeline, double share) noexcept {
    if (timeline.chunks.empty()) return timeline.start + share * timeline.length;
    const Chunk* last = &timeline.chunks.front();
    for (const auto& c : timeline.chunks) {
        if (share < c.from) return c.clock_from;
        if (share <= c.to) {
            const double span = static_cast<double>(c.to) - c.from;
            const double along = span > 1e-9 ? (share - c.from) / span : 0.0;
            return c.clock_from + along * (c.clock_to - c.clock_from);
        }
        last = &c;
    }
    return last->clock_to;
}

double time_by_frames(const Timeline& timeline, double clock, double along) noexcept {
    if (!timeline.known || timeline.length <= 1e-3) return clock + along;
    if (!timeline.chunks.empty()) {
        // The whole replay's seconds a share (its chunks' footage over the shares they fill), from the trim's start.
        double footage = 0, shares = 0;
        for (const auto& c : timeline.chunks) {
            footage += c.clock_to - c.clock_from;
            shares += static_cast<double>(c.to) - c.from;
        }
        if (footage > 1e-3 && shares > 1e-6) return clock_at_share(timeline, static_cast<double>(timeline.trim_from) + along * shares / footage);
    }
    // One stretch: the bar's start, the trim's start on it, and on from there.
    const double shown = static_cast<double>(timeline.shown_to) - timeline.shown_from;
    const double trim_at = shown > 1e-6 ? (static_cast<double>(timeline.trim_from) - timeline.shown_from) / shown : 0.0;
    return timeline.start + trim_at * timeline.length + along;
}

int next_streak(int streak, double last, double now) noexcept {
    if (!std::isfinite(now) || !std::isfinite(last) || now < last || now - last > 0.5) return 0;
    return streak < 1000000 ? streak + 1 : streak;
}
} // namespace dingosdk::replay_keyframes
