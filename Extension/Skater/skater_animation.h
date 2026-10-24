#pragma once
#include "Extension/AnimationStudio/anim_core.h"
#include "Extension/Skater/arm_reach.h"
#include "Extension/Skater/carry.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Clips on the local skater's body, for plugins (reskate.animation): Animation Studio's pose
// override (PR 297) without the studio. A clip (anim_core.h's, recorded from the game) replaces the
// body, Hips and every joint below it, blended in and out; the placement joints the game moves the
// skater with are never written, and the board follows the clip only when it recorded one. Other
// players see what is drawn here (the local pose filter). Nothing is hooked until a clip is asked for.
//
// The pose is read on the thread that evaluates animation and written when it goes to the renderer
// (the body physics rebuild the buffer in between); the client thread only hands over what to play.
namespace dingosdk::skater_animation {
using anim_studio::Clip;

// A clip from now on, in place: `speed` times as fast, blended in and out over `blend` seconds,
// once or over and over.
struct Play {
    std::shared_ptr<const Clip> clip;
    float speed{1}, blend{0.15f};
    bool loop{};
    float at{};     // seconds into the clip it starts from
    bool paused{};  // held at `at` until resumed (seek, pause)
    // On the skater's floor (PR 297's studio): the clip's rise and turn since its first frame go onto
    // the body, and the whole clip moves up or down so its board's lowest point lies on the floor
    // before the takeoff and after the landing, with a jump as long as its air between them for a clip
    // that does not rise by itself (the game's own trick clips stay in place); on the ground its board
    // lies under the feet. Without it the body hangs from where the skater is, as the clip has it.
    bool floor{};
};
// A clip the game's own trick plays as, from its takeoff: the clip's air (its takeoff to its
// landing) stretched to the trick's. `trick` is the game's name ("kickflip"); `pop` "", "low",
// "mid" or "high".
struct Replacement {
    std::string trick, pop;
    std::shared_ptr<const Clip> clip;
    float speed{1};
};

// Client thread throughout.
void play(std::optional<Play> clip);
bool playing() noexcept; // a clip of play() has not finished
// The clip of play(), moved to `seconds` (clamped to it), held there or playing on from it.
void seek(float seconds);
void pause(bool paused);
// Where in its clip play() is now, in seconds; empty without one.
std::optional<float> play_time();
void set_replacements(std::vector<Replacement> list);
// Records the pose for `seconds` (at most 10); the clip comes from take_recording once done.
bool record(float seconds);
bool recording() noexcept;
std::shared_ptr<const Clip> take_recording();
// Who sees a pose, a carry or a reach: everyone (the pose drawn here and sent), only the other players
// (sent, not drawn: you look through a camera your skater is seen holding), or only you (drawn, not sent).
enum class Shown : std::uint8_t { everyone, others, me };
// An arm reaching (arm_reach.h), after any clip and carry; none ends it. It holds for 0.3 s: set it
// every tick. Unlike clips it is allowed in a throwdown.
void reach(std::optional<arm_reach::Goal> goal, Shown shown = Shown::everyone);
// Something held out in the hands (carry.h), after any pose and before any reach; none ends it. It
// holds for 0.3 s: set it every tick. `push_straightens`: riding, the knees straighten (most of the
// crouch let go) while a foot is down pushing.
void carry(std::optional<carry::Goal> goal, bool push_straightens = false, Shown shown = Shown::everyone);
// Joints turned (in their own frame) and scaled on top of the animation and any clip, before any
// carry and reach; `weight` 0..1 of the way. Joints 0 and 1 (the skater's world placement) are left
// alone. It holds for 0.3 s: set it every tick; empty ends it.
struct PoseOffset {
    std::uint16_t joint{};
    anim_studio::Quat rotation{0, 0, 0, 1};
    float scale{1};
    anim_studio::Vec3 offset{}; // added to its position, in its parent's frame
};
void pose(std::vector<PoseOffset> offsets, float weight, Shown shown = Shown::everyone);
// Where a joint (of the 395) was in the world in the last pose evaluated, after any clip and reach;
// empty without a skater. Asking keeps that joint read for two seconds, so the first ask comes back
// empty; at most 32 joints are read at once.
std::optional<anim_studio::Joint> joint(std::uint16_t index);
// The hand joint's (joint()).
std::optional<arm_reach::Placed> hand(arm_reach::Hand side);
// The game menu's character room (Extension/World/character_room.h). While the walk looks for it,
// watch_room has the room's own skater (the menu's) looked for; room_skater_seen says it was drawn in
// the room just now. show_in_room draws the local skater (and its board, under a clip that has one) on
// the room's floor, facing as the menu's skater did, as high above the floor as he is above his own
// ground; the room's own skater and board are hidden meanwhile and come back after. Only what is
// drawn moves: the skater stays where he is, and other players are not sent the room. Any thread.
void watch_room(bool on) noexcept;
bool room_skater_seen() noexcept;
void show_in_room(bool on) noexcept;
// Whether the local skater is drawn in the room now. The game takes the room's floor, far under where he
// is, for a fall and bails him (client_debug.cpp guards him with No Bail meanwhile).
bool shown_in_room() noexcept;
// The game's name for the trick the skater is doing now, or empty.
std::string current_trick() noexcept;
// Each client tick: the local skater's animation component (0 for none), and whether clips may be
// written now (not while a throwdown or a S.K.A.T.E. game runs).
void tick(std::uintptr_t base, std::uintptr_t component, bool allowed);
std::string status();
}
