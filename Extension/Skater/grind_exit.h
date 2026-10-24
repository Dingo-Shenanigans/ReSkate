#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Which side the local skater pops out of a grind to, as a plugin asks (reskate.skater.grind_exit;
// the grind-exits mod picks it with LT / RT), and pad controls kept from the game meanwhile
// (reskate.input.hide). What decides the side, when and why, is the plugin's; this is what has to
// happen in the game's own threads.
//
// At the pop the game calls its jump-trajectory setter with the launch velocity; the trainer hooks
// that call (Extension/Trainer/trainer_jump.cpp). For a pop out of a grind (its call site, measured:
// Engine/Game/Build/20260929/grind_exit.h) only the sideways part of the launch is replaced, across
// the rail: the game's own sideways speed when it had one, mirrored to the chosen side, else
// `side_speed` (1.8 m/s, the game's own), scaled by the wish's strength. Along the rail, the pop's
// height, the trick, the flip and the body's rotation stay the game's.
//
// Toe and heel come from which foot leads (the game's stance flag, grind_exit.h, and the player's
// stance as the wish gives it) and the skater root's right axis: the toes face it when the right foot
// leads (measured). On slides, where toe and heel point along the rail, and without a stance, toe is
// the left of travel and heel the right.
//
// The pad: a hidden trigger reads as let go; the left stick's left/right, hidden "while grinding",
// reads as centred only while the skater grinds, so it reaches the game again at the pop.
namespace dingosdk::grind_exit {
// Requires the validated build and No Bail started.
bool start(std::uintptr_t image_base) noexcept;
bool available() noexcept;

struct Wish {
    enum class Side : std::uint8_t { toe, heel, left, right } side{Side::left};
    int stance{};          // 0 not set, 1 regular, 2 goofy: what toe and heel need
    float strength{1.0f};  // 0.1 to 1 of the full sideways speed
};
// Client thread: what the next pop out of a grind does (empty: the game's own exit). One plugin at a
// time: empty when taken, else why not.
std::string wish(std::string_view owner, std::optional<Wish> wish);
struct Hidden {
    bool left_trigger{}, right_trigger{}, left_x_grinding{};
    // The right stick (reads centred) and the stick clicks (read up), whatever the skater does.
    bool right_stick{}, left_stick_click{}, right_stick_click{};
    bool any() const noexcept { return left_trigger || right_trigger || left_x_grinding || right_stick || left_stick_click || right_stick_click; }
};
void hide(std::string_view owner, Hidden hidden);
// Whatever the plugin held.
void release(std::string_view owner);

// A pop out of a grind, as it went: whether the side was set, and why (or why not).
struct Pop {
    bool applied{};
    std::string reason;  // "applied", "kept", "none", "slow", "not_ours"
    std::string basis;   // "toe_heel", "slide", "no_stance", "no_facing", "travel"
    int toward{};        // +1 the left of travel, -1 the right, 0 the game's own
    float before{}, after{}; // the sideways speed, m/s (+ the left)
};
// Client thread: the pops since the last call, for the plugin that wished.
std::vector<Pop> take_pops(std::string_view owner);
// The skater is grinding now (as the physics steps last said).
bool grinding() noexcept;
// Client thread, every tick, with the local client: the launch hook made ready while used, the
// skater's facing sampled.
void tick(std::uintptr_t client) noexcept;
}
