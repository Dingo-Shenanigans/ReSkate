#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <span>

namespace dingosdk::server {
// Skateboard check for one player. A player's game says where its skateboard is, and one left
// lying can be anywhere, so a hacked game can say its board is wherever another player is and
// keep it on them. Nothing a real board does looks like that: one away from its skater lies
// where it stopped. This flags a board that is away from its own skater and stays beside one
// other player while that player travels.
class BoardCheck {
  public:
    using Position = std::array<float, 3>;
    struct Other { std::uint64_t id{}; Position position{}; };
    static constexpr float away = 3.f;          // further than this from its skater: not ridden, held or just dropped
    static constexpr float beside = 1.5f;       // within this of another player
    static constexpr float travel = 8.f;        // that player has gone this far with it beside them...
    static constexpr std::uint64_t held_us = 3000000;   // ...for at least this long
    static constexpr std::uint64_t slack_us = 500000;   // a lost pose or two does not start it over
    static constexpr std::uint64_t clear_us = 30000000; // flagged until it has behaved for this long

    // One pose of the player: their skater, their board (null when they have none out) and the
    // other players on the map. Returns true when the flag changed (see flagged()).
    bool sample(std::uint64_t now, const Position &skater, const Position *board, std::span<const Other> others) {
        const Other *with{};
        if (board && apart(*board, skater) > away * away)
            for (const auto &other : others)
                if (apart(*board, other.position) <= beside * beside && (!with || other.id == following_)) with = &other;
        if (with && with->id == following_ && now - seen_ <= slack_us) {
            seen_ = now;
            // How far they have carried it: summed step by step, so circling counts as travel
            // and standing still with it lying beside them does not.
            const float step = std::sqrt(apart(with->position, last_));
            if (step >= 0.05f) { travelled_ += step; last_ = with->position; }
        } else if (with) {
            following_ = with->id;
            since_ = seen_ = now;
            last_ = with->position;
            travelled_ = 0;
        } else if (now - seen_ > slack_us) {
            following_ = 0;
            travelled_ = 0;
        }
        const bool pinned = following_ && travelled_ >= travel && now - since_ >= held_us;
        if (pinned) {
            behaved_since_ = 0;
            if (flagged_) return false;
            flagged_ = true;
            victim_ = following_;
            return true;
        }
        if (!flagged_) return false;
        if (following_) { behaved_since_ = 0; return false; } // still beside someone: not yet behaving
        if (!behaved_since_) behaved_since_ = now;
        if (now - behaved_since_ < clear_us) return false;
        flagged_ = false;
        return true;
    }
    bool flagged() const { return flagged_; }
    std::uint64_t victim() const { return victim_; } // the player it was on when flagged
    void restart() { *this = {}; }

  private:
    static float apart(const Position &a, const Position &b) {
        float squared{};
        for (unsigned i = 0; i < 3; ++i) squared += (a[i] - b[i]) * (a[i] - b[i]);
        return squared;
    }
    std::uint64_t following_{}, victim_{}, since_{}, seen_{}, behaved_since_{};
    Position last_{};
    float travelled_{};
    bool flagged_{};
};
} // namespace dingosdk::server
