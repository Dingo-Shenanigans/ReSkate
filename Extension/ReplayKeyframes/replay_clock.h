#pragma once
#include <vector>

// The replay editor's clock and timeline, apart from the game (replay_editor.h reads them from it): the replay's footage
// is strung as chunks on its clock, each filling a share of the editor's whole timeline; the editor's bar shows the whole
// replay, or once it is trimmed, the trimmed part.
namespace dingosdk::replay_keyframes {
struct Chunk {
    double clock_from{}, clock_to{}; // its footage on the replay's clock
    float from{}, to{};              // the share of the whole replay (0 to 1) it fills
    bool operator==(const Chunk&) const = default;
};
// The editor's timeline as it maps to the clock.
struct Timeline {
    std::vector<Chunk> chunks;     // none: the timeline is one stretch of the clock from `start`
    double start{}, length{};      // the bar: where it starts on the clock, and its length in seconds
    float trim_from{}, trim_to{1}; // what an export films: the trim, shares of the whole replay (as the chunks')
    float shown_from{}, shown_to{1}; // the part of the whole replay the bar shows (trimmed: the trim)
    bool known{};
};
// The share of the bar (0 to 1) a time on the clock is at: through its chunk when the chunks are known (-1 when it is in
// none of them, or off the part the bar shows), else as one stretch from the bar's start.
float share_of(const Timeline& timeline, double clock) noexcept;
// The time on the clock a share (0 to 1) of the whole replay is at, through its chunks (one stretch without them); before
// the first chunk its start, past the last its end.
double clock_at_share(const Timeline& timeline, double share) noexcept;
// Where an export has got by its frames alone, `along` seconds of them in: from the trim's start, through the chunks; with
// no timeline known, on from the clock where it stood (`clock`).
double time_by_frames(const Timeline& timeline, double clock, double along) noexcept;
// The run of the camera keys' plays so far, one more: each no earlier than the one before and less than half a second on
// (as an export's frames are, slow motion or not), else a new run (0). An export's first play can be off its clip, and
// after an export the editor's playhead comes back: neither is a run.
int next_streak(int streak, double last, double now) noexcept;
} // namespace dingosdk::replay_keyframes
