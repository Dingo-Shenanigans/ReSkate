#pragma once
#include <set>
#include <string>
#include <string_view>

// Which challenges the local player has begun and not yet ended, from the game's own begin and end
// requests (local_challenge_runtime.cpp's challenge_request sees both, the local challenges' and those
// it hands back to the game unchanged). What runs only outside a challenge (a plugin's score line,
// score_line_hud.h) holds back while one is open. Pure, so it is tested on its own
// (Extension/Progression/Test/challenge_attempts_tests.cpp).
namespace dingosdk::profile_runtime {
class OpenAttempts {
public:
    void begin(std::string_view id) { open_.insert(std::string(id)); }
    // A begin whose id could not be read: it counts as open until any end comes.
    void begin_unknown() { open_.insert(std::string(unknown)); }
    // An end, with its id or (empty) one that could not be read; either way an unknown begin ends.
    void end(std::string_view id) {
        open_.erase(std::string(unknown));
        if (!id.empty()) open_.erase(std::string(id));
    }
    // A level left or a profile changed: what was open there has ended.
    void clear() { open_.clear(); }
    bool open() const { return !open_.empty(); }

private:
    // No challenge id holds a NUL, so this cannot be one.
    static constexpr std::string_view unknown{"\0unknown", 8};
    std::set<std::string, std::less<>> open_;
};

// Client and game threads (local_challenge_runtime.cpp, under its own lock). Whether a challenge the
// local player began is still open; also true while the game's begin and end requests are not watched
// (its hooks did not go in), since then it cannot be told.
bool local_challenge_attempt_open() noexcept;
// Leaving a level (states 3, 14, 22, 24, 25) ends every attempt there.
void challenge_attempts_before_level_transition(unsigned next) noexcept;
} // namespace dingosdk::profile_runtime
