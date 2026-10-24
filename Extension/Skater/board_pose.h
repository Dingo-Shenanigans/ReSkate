#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The skateboard's rig as plugins may change what is drawn of it (reskate.board), as one service. The board is a 16-bone rig: 0 its
// reference, 1 its place in the world, 2 the deck's mount (deck, trucks and wheels hang on it), 3
// the deck, 4 and 8 the trucks, 6, 7, 10, 11 the wheels, 12 to 15 a breakable deck's stretches. What
// is written goes into the pose as it goes to the renderer, every frame, and the next animation
// update writes the whole pose again: nothing of the board itself changes, only what is drawn. Bones
// 0 and 1 are never written (they place the board; clips of the skater move bone 1).
namespace dingosdk::board_pose {
using Vec3 = std::array<float, 3>;
using Quat = std::array<float, 4>; // x, y, z, w
struct Bone {
    Vec3 position{};
    Quat rotation{0, 0, 0, 1};
    Vec3 scale{1, 1, 1};
};
// How a set bone is meant: in its own parent's frame ("local", as the pose holds it), or in the
// world, for a bone that hangs on the board's place (bone 1): written relative to wherever the board
// is in that frame.
enum class Space { local, world };
inline constexpr int bones = 16, first_writable = 2;

// Client thread. One owner holds the local board's bones at a time; empty when taken, else why not.
std::string set(std::string_view owner, int bone, const Bone& value, Space space);
// `everyone`: hidden from the other players too (the board as you are sent: Extension/Multiplayer/
// Remote/native_skater.h's sent pose filter), not only on this screen.
std::string hide(std::string_view owner, int bone, bool everyone = false);
void clear(std::string_view owner, int bone = -1);
// These bones go out of sight on every board, other players' included (a breakable deck's
// stretches, which the game would draw as cracks).
void park(std::string_view owner, std::vector<int> list);
// How every board in the game is fitted, other players' included, for a deck model that is not the
// game's own (a breakable deck pack's): `plates` holds each truck's baseplate (its effector, bones 5
// and 9) on the deck as the truck steers, and `axle` sets the wheels as far apart as the deck's
// trucks have them (1 the game's own, 0.8 to 1.6). One owner at a time; empty: fitted, else why not.
// No value: that owner's fit ends.
struct Fit {
    bool plates{};
    float axle = 1.0f;
};
std::string fit(std::string_view owner, std::optional<Fit> value);
// A piece comes off the local board: these bones (hanging on bone 1) leave as one, from where they
// are, at `velocity` (m/s, world) turning at `spin` (rad/s, world), fall, land on `ground` (a world
// height) and lie there, until cleared.
std::string toss(std::string_view owner, std::vector<int> list, Vec3 velocity, Vec3 spin, float ground);
// The local board's bones as last drawn, in their parents' frames; empty without a board.
std::optional<std::array<Bone, bones>> read();
// Each client tick: the local skater's animation component (0 for none).
void tick(std::uintptr_t base, std::uintptr_t component) noexcept;
// From the render pose hook (native_skater.cpp), every skeleton handed to the renderer.
void on_render_pose(std::uintptr_t animation_interface) noexcept;
}
