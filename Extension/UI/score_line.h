#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A plugin's own line in skate.'s score HUD (reskate.hud.score_line), display only: the parts that
// need no game, so they are tested on their own (Extension/UI/Test/score_line_tests.cpp).
// score_line_hud.h shows the line with them.
namespace dingosdk::score_line {
// What a plugin may pass: the trick list keeps at most 5 rows on this build (the widget draws the
// newest 3), and a name is at most 63 bytes.
inline constexpr std::size_t most_tricks = 5;
inline constexpr std::size_t most_trick_bytes = 63;
inline constexpr double most_points = 999'999'999;
inline constexpr double least_multiplier = 1, most_multiplier = 99;
// The line shows while its plugin sets it again at least this often (one call a tick is the use).
inline constexpr double lease_ms = 500;
// skate.'s own line keeps a plugin's down until this long after all its signals went idle.
inline constexpr double game_hold_ms = 500;

struct Line {
    std::vector<std::string> tricks; // the line's trick names, newest last
    double points{};                 // the current action's points: the big number beside the ring
    double line{};                   // what the line has banked: "LINE: N" while above 0
    float multiplier{1};             // the ring's number
    float timer{1};                  // the ring's time left, 0 to 1
    // The game's look for a line that has ended: the ring, number and rows go and "LINE: N" stays.
    bool closed{};
    bool operator==(const Line&) const = default;
};

// A challenge begin is already watched by hooks, so it must hold the display back at once. Only
// the native activity getter is cached; an idle result must never hide a later challenge begin.
class ActivityCheck {
public:
    template <class Read> bool running(bool challenge_open, std::uint64_t now_ms, Read&& read_activity) {
        if (challenge_open) return true;
        if (read_at_ && now_ms - *read_at_ < 250) return active_;
        active_ = std::forward<Read>(read_activity)();
        read_at_ = now_ms;
        return active_;
    }

private:
    std::optional<std::uint64_t> read_at_;
    bool active_{};
};

// One plugin's line at a time, held while it sets it again within lease_ms.
class LineOwner {
public:
    // `who` shows `line`, or (nullopt) clears its own. "" or why not: another plugin's line is up.
    std::string ask(std::string_view who, std::optional<Line> line, double now_ms) {
        if (!line) {
            if (owner_ == who) clear();
            return {};
        }
        if (!owner_.empty() && owner_ != who && live(now_ms)) return owner_ + " shows its line there";
        owner_ = std::string(who);
        line_ = std::move(line);
        asked_ms_ = now_ms;
        return {};
    }
    void release(std::string_view who) {
        if (owner_ == who) clear();
    }
    // The line to show now, or null once its plugin has not set it for lease_ms (it is then let go).
    const Line* current(double now_ms) {
        if (!live(now_ms)) {
            clear();
            return nullptr;
        }
        return &*line_;
    }
    const std::string& owner() const { return owner_; }

private:
    bool live(double now_ms) const { return line_ && now_ms - asked_ms_ <= lease_ms; }
    void clear() {
        owner_.clear();
        line_.reset();
    }
    std::string owner_;
    std::optional<Line> line_;
    double asked_ms_{};
};

// skate.'s own line, from its ScoringHUDViewModel root and its item in DpadScoringStack.
struct GameSignals {
    bool action{};                                // ActionActive: an action goes on
    float multiplier{}, line_score{}, line_shown{}; // Multiplier, LineScore and the "LINE: N" number
    bool item{};                                  // its ScoringHUD_Widget item is in DpadScoringStack
};
// Whether skate.'s own line is up, so a plugin's yields to it. Measured on this build (scoremap1,
// scoremap4): a line starts on its first scored action with ActionActive, the first row and the big
// number on one tick and its item in the stack 15-17 ms later; Multiplier reads 0 at rest; "LINE: N"
// stays 2.0 s after the line and the item leaves 0.3 s after that. A plugin's line goes on the tick
// any of these comes on and stays down until game_hold_ms after all are idle. SequenceState,
// HudWidgetActive and LineTimer are left out (they move on the same ticks, and hud.corner replaces
// the first two while it hides the corner), and so is IsSlam (it latched true after one on-foot move).
class GameLine {
public:
    void observe(const GameSignals& s, double now_ms) {
        const auto on = signal_of(s);
        if (on.empty()) return;
        seen_ = true;
        last_on_ms_ = now_ms;
        why_ = on;
    }
    bool running(double now_ms) const { return seen_ && now_ms - last_on_ms_ < game_hold_ms; }
    // The signal last seen on, with its value ("ActionActive", "LINE 22"), for a log line.
    const std::string& why() const { return why_; }
    void reset() { *this = {}; }

private:
    static std::string number(const char* name, float value) {
        char text[48]{};
        std::snprintf(text, sizeof text, "%s %g", name, static_cast<double>(value));
        return text;
    }
    static std::string signal_of(const GameSignals& s) {
        if (s.action) return "ActionActive";
        if (s.multiplier > 0.01f) return number("Multiplier", s.multiplier);
        if (s.line_score > 0.5f) return number("LineScore", s.line_score);
        if (s.line_shown > 0.5f) return number("LINE", s.line_shown);
        if (s.item) return "its score item";
        return {};
    }
    bool seen_{};
    double last_on_ms_{};
    std::string why_;
};

// Whether an item of ours may still be in the game's stack, so its take-out is never dropped: from
// the first insert (or try at one) until a read of that registry's stack finds none of ours. A tick
// that fails, or does not find the stack, calls nothing and so leaves it to be done again.
class ItemPresence {
public:
    void inserting() { must_scan_ = true; }
    void scanned(bool found) { must_scan_ = found; }
    // A new UI model registry: our item went with the old one.
    void registry_changed() { must_scan_ = false; }
    bool must_scan() const { return must_scan_; }

private:
    bool must_scan_{};
};

// What of skate.'s own line is still in its root, which our model is made as a copy of. A copy while
// any of it is on would carry the game's rows (stack items bound to row models of the game's) into
// ours, and emptying them would run their destructors on ours: the scorep2 launch copied the root
// only with all of these off. After a level load GameLine has seen nothing yet, so this is what keeps
// that first copy clean.
struct GameRoot {
    bool hud_active{};              // HudWidgetActive
    float line_timer{};             // LineTimer
    unsigned trick_rows{}, second_rows{}; // items in its trick list and its second stack (emptied 150 ms after a line)
    bool item{};                    // its score item in DpadScoringStack (out 2.3 s after a line)
};
// "" when the root may be copied, else what is on, for a log line.
inline std::string root_busy(const GameRoot& root) {
    std::string on;
    const auto add = [&on](const char* what) { on += on.empty() ? what : std::string(", ") + what; };
    if (root.hud_active) add("HudWidgetActive");
    if (root.line_timer > 0) add("LineTimer above 0");
    if (root.trick_rows) add("rows in its trick list");
    if (root.second_rows) add("rows in its second stack");
    if (root.item) add("its score item");
    return on;
}

// Whether a line's rows start again from an empty list. A line that opens after none was shown, or
// after a closed one, is a new line: the game empties its own list about 150 ms after each line ends,
// so the rows of a plugin's last line never carry into its next, even under the same names.
class LineStart {
public:
    // Each tick a line is shown; true when its rows start again.
    bool shown(bool closed) {
        const bool starts = !open_ && !closed;
        open_ = !closed;
        return starts;
    }
    // The line was taken out or let go.
    void out() { open_ = false; }

private:
    bool open_{};
};

// The game's root among the registry's roots of its type: the first that is not ours (`own`, 0 none).
// The SDK's readers (score_hud.cpp, hud_corner.cpp) use it so they never read or hold a plugin's line.
template <class Root> const Root* pick_game_root(const std::vector<Root>& roots, std::uint64_t own) {
    for (const auto& root : roots)
        if (root.model.handle && root.model.handle != own) return &root;
    return nullptr;
}

// How many of `wanted`'s rows (newest first) are new: the rest are `shown`'s newest, kept as they are
// so only a new row comes in, as the game's list does. All of them when the two do not line up.
inline std::size_t fresh_rows(const std::vector<std::string>& shown, const std::vector<std::string>& wanted) {
    for (std::size_t fresh = 0; fresh < wanted.size(); ++fresh) {
        const auto kept = wanted.size() - fresh;
        if (kept <= shown.size() && std::equal(wanted.begin() + static_cast<std::ptrdiff_t>(fresh), wanted.end(), shown.begin())) return fresh;
    }
    return wanted.size();
}

// What a line writes into the SDK's own ScoringHUDViewModel, as the game's own line looks.
struct Look {
    bool hud_active{}; // HudWidgetActive: the ring, the big number and the rows
    bool ring_on{}, line_on{};
    float line_score{}, multiplier{}, timer{}, fill{};
    std::string points, multiplier_text;
    std::vector<std::string> rows; // newest first, at most most_tricks
    bool operator==(const Look&) const = default;
};
// The ring's number as the game writes it: "1.5", "2" (tenths, no ".0").
inline std::string multiplier_text(float multiplier) {
    char text[16]{};
    std::snprintf(text, sizeof text, "%.1f", std::round(static_cast<double>(multiplier) * 10) / 10);
    std::string out(text);
    if (out.size() > 2 && out.ends_with(".0")) out.resize(out.size() - 2);
    return out;
}
// `fill_points`: the points that fill the ring (the build's action_fill_points).
inline Look look_of(const Line& line, float fill_points) {
    Look look;
    look.hud_active = !line.closed;
    look.ring_on = !line.closed;
    look.line_on = !line.closed && line.line > 0;
    look.line_score = static_cast<float>(line.line);
    look.multiplier = line.multiplier;
    look.timer = line.timer;
    look.fill = fill_points > 0 ? static_cast<float>(std::min(1.0, line.points / fill_points)) : 0.f;
    look.points = std::to_string(static_cast<std::int64_t>(line.points));
    look.multiplier_text = multiplier_text(line.multiplier);
    const auto count = std::min(line.tricks.size(), most_tricks);
    for (std::size_t i = 0; i < count; ++i) look.rows.push_back(line.tricks[line.tricks.size() - 1 - i]);
    return look;
}
} // namespace dingosdk::score_line
