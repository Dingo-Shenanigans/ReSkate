#pragma once
#include <atomic>
#include <set>
#include <string>
#include <string_view>
#include <utility>

// Which challenges the local player has begun and not yet ended, from the game's own begin and end
// requests (local_challenge_runtime.cpp's challenge_request sees both, the local challenges' and those
// it hands back to the game unchanged). What runs only outside a challenge (a plugin's score line,
// score_line_hud.h) holds back while one is open. Pure, so it is tested on its own
// (Extension/Progression/Test/challenge_attempts_tests.cpp).
namespace dingosdk::profile_runtime {
// Prepared originals survive an initialization failure. Only the completed hook installation,
// published by the profile runtime's active flag, makes those originals evidence of a watch.
inline bool challenge_requests_watched(bool profile_active, bool begin_prepared, bool end_prepared) noexcept {
    return profile_active && begin_prepared && end_prepared;
}

// Tracker allocation or locking may fail before an OpenAttempts can be reached. Keep that unknown
// without allocating, until the next successful level clear. A later ordinary observation cannot
// silently turn a lost begin into a claim that no challenge is open.
class AttemptFallback {
public:
    template <class Observe> void guard(Observe&& observe) noexcept {
        try { std::forward<Observe>(observe)(); }
        catch (...) { unknown_.store(true, std::memory_order_release); }
    }
    bool unknown() const noexcept { return unknown_.load(std::memory_order_acquire); }
    void cleared() noexcept { unknown_.store(false, std::memory_order_release); }

private:
    std::atomic<bool> unknown_{};
};

class OpenAttempts {
public:
    void begin(std::string_view id) { open_.insert(std::string(id)); }
    // A begin whose id could not be read: it counts as open until any end comes.
    void begin_unknown() noexcept { unknown_ = true; }
    // An end, with its id or (empty) one that could not be read; either way an unknown begin ends.
    void end(std::string_view id) {
        unknown_ = false;
        // Finding by string_view avoids allocating just to retire an observed attempt.
        if (!id.empty()) {
            const auto found = open_.find(id);
            if (found != open_.end()) open_.erase(found);
        }
    }
    // Read/identifier or set allocation failures lose an observation. The unknown flag itself never
    // allocates, so that loss holds the display back until an observed end or a level clear.
    template <class ReadId> void observe(bool ending, ReadId&& read_id) noexcept {
        try {
            std::string id;
            const bool known = std::forward<ReadId>(read_id)(id) && !id.empty();
            if (ending) end(known ? std::string_view(id) : std::string_view{});
            else if (known) begin(id);
            else begin_unknown();
        } catch (...) { begin_unknown(); }
    }
    // A level left or a profile changed: what was open there has ended.
    void clear() noexcept { open_.clear(); unknown_ = false; }
    bool open() const { return unknown_ || !open_.empty(); }

private:
    bool unknown_{};
    std::set<std::string, std::less<>> open_;
};

// Client and game threads (local_challenge_runtime.cpp, under its own lock). Whether a challenge the
// local player began is still open; also true while the game's begin and end requests are not watched
// (its hooks did not go in), or an observation failed, since then it cannot be told.
bool local_challenge_attempt_open() noexcept;
// Leaving a level (states 3, 14, 22, 24, 25) ends every attempt there.
void challenge_attempts_before_level_transition(unsigned next) noexcept;
} // namespace dingosdk::profile_runtime
