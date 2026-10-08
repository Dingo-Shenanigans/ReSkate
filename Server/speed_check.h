#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>

namespace dingosdk::server {
// Game-speed check for one player: a speedhack (Cheat Engine and the like) speeds up the game's
// clock, which the player's pose timestamps come from, so the player's clock runs ahead of the
// server's. The gap between arrival and send time (network delay plus a fixed clock offset) then
// shrinks steadily instead of only jittering. Only its smallest value per block is used: delay
// spikes add to it, they never subtract, so lag cannot look like speed.
class SpeedCheck {
  public:
    static constexpr std::uint64_t block_us = 2000000;   // one minimum per block
    static constexpr std::uint64_t window_us = 20000000; // speed measured across this
    static constexpr std::uint64_t gap_us = 2500000;     // longer without poses (loading): start over
    // Half as fast again and more is flagged. A little under 1.5, since a game at exactly 1.5x
    // measures a hair either side of it; nothing but a speed hack comes near.
    static constexpr double limit = 1.45;
    static constexpr unsigned strikes_needed = 3;        // consecutive fast measurements (one per block)

    // One pose: the sender's timestamp and when it arrived, both in microseconds. Returns true
    // when this sample completes a measurement (see speed()).
    bool sample(std::uint64_t sent, std::uint64_t arrived) {
        // A reordered or repeated pose says nothing new; a clock that jumped back (a restarted game)
        // or a long pause (loading) starts over.
        if (started_ && sent <= last_sent_ && last_sent_ - sent < gap_us && arrived >= last_arrived_) return false;
        if (!started_ || sent < last_sent_ || arrived < last_arrived_ || arrived - last_arrived_ > gap_us) {
            restart();
            started_ = true;
        }
        last_sent_ = sent;
        last_arrived_ = arrived;
        const auto offset = static_cast<std::int64_t>(arrived) - static_cast<std::int64_t>(sent);
        if (blocks_.empty() || arrived - blocks_.back().start >= block_us) {
            const bool measured = measure();
            blocks_.push_back({arrived, offset});
            while (blocks_.size() > 1 && arrived - blocks_[1].start >= window_us) blocks_.pop_front();
            return measured;
        }
        blocks_.back().min_offset = std::min(blocks_.back().min_offset, offset);
        return false;
    }
    // The last measured speed (1 = real time) and whether it has been too fast for long enough.
    double speed() const { return speed_; }
    bool flagged() const { return strikes_ >= strikes_needed; }
    void restart() {
        blocks_.clear();
        speed_ = 1.0;
        strikes_ = 0;
        started_ = false;
    }

  private:
    struct Block { std::uint64_t start{}; std::int64_t min_offset{}; };
    std::deque<Block> blocks_;
    std::uint64_t last_sent_{}, last_arrived_{};
    double speed_ = 1.0;
    unsigned strikes_{};
    bool started_{};

    // The completed blocks span the window: compare the first block's minimum with the last one's.
    bool measure() {
        if (blocks_.size() < 2) return false;
        const auto &first = blocks_.front(), &last = blocks_.back();
        const auto span = last.start - first.start;
        if (span + block_us < window_us) return false;
        // Sent time advanced by (arrival span + how much the gap shrank).
        speed_ = 1.0 + static_cast<double>(first.min_offset - last.min_offset) / static_cast<double>(span);
        strikes_ = speed_ >= limit ? strikes_ + 1 : 0;
        return true;
    }
};
} // namespace dingosdk::server
