#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>

// Board Break's rules and geometry, apart from the game: where a board breaks, what is left of it,
// and where each part of a broken board lies. Everything is measured on the deck, in the deck
// model's own axes: metres, y up, z along the board with the nose at +z.
namespace dingosdk::board_break {
// ---- rigid motions ----------------------------------------------------------------------------
// Quaternions are x, y, z, w (Hamilton), as the game's poses store them.
struct Vec {
    float x{}, y{}, z{};
};
struct Quat {
    float x{}, y{}, z{}, w{1.0f};
};
constexpr Vec operator+(Vec a, Vec b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
constexpr Vec operator-(Vec a, Vec b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
constexpr Vec operator*(Vec a, float s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
inline float length(Vec a) noexcept { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
constexpr Quat operator*(Quat a, Quat b) noexcept { // b first, then a
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
constexpr Quat conj(Quat q) noexcept { return {-q.x, -q.y, -q.z, q.w}; }
inline Quat unit(Quat q) noexcept {
    const auto n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return n > 1e-6f ? Quat{q.x / n, q.y / n, q.z / n, q.w / n} : Quat{};
}
inline Quat about(Vec axis, float radians) noexcept {
    const auto s = std::sin(radians / 2);
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(radians / 2)};
}
constexpr Vec turn(Quat q, Vec v) noexcept {
    const Vec u{q.x, q.y, q.z};
    const Vec c{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
    const Vec d{u.y * c.z - u.z * c.y, u.z * c.x - u.x * c.z, u.x * c.y - u.y * c.x};
    return v + c * (2 * q.w) + d * 2;
}
// Turned, then shifted.
struct Rigid {
    Quat facing{};
    Vec at{};
};

// ---- breaks -----------------------------------------------------------------------------------
// A deck breaks across one of three rows: just ahead of the front bolts, in the middle, just behind
// the back bolts. That cuts it into four stretches; each truck rides on the stretch its bolts are in.
enum Piece : std::uint32_t { nose = 1, front = 2, back = 4, tail = 8, front_truck = 16, back_truck = 32 };
constexpr std::uint32_t whole_board = nose | front | back | tail | front_truck | back_truck;
constexpr std::uint32_t stretches = nose | front | back | tail;
enum class Place { nose, mid, tail };
constexpr std::array<std::string_view, 3> place_names{"nose", "mid", "tail"};
constexpr float nose_row = 0.260f, mid_row = 0.009f, tail_row = -0.260f;
// How the grip tape holds a break together: not at all, or with the nose hanging, the board folded
// in the middle, the tail hanging.
enum class Hinge { none, nose, fold, tail };
// What a break leaves: the piece the board keeps, the piece that comes off and flies (none when
// the grip tape holds), and how the two hang together.
struct Break {
    std::uint32_t kept{}, loose{};
    Hinge hinge{};
};
constexpr Break broken_at(Place place, bool joined) noexcept {
    if (joined) return {whole_board, 0, place == Place::nose ? Hinge::nose : place == Place::mid ? Hinge::fold : Hinge::tail};
    // A clean break: the board keeps the larger piece, and of two halves the front one.
    switch (place) {
    case Place::nose: return {whole_board & ~static_cast<std::uint32_t>(nose), nose, Hinge::none};
    case Place::mid: return {nose | front | front_truck, back | tail | back_truck, Hinge::none};
    default: return {whole_board & ~static_cast<std::uint32_t>(tail), tail, Hinge::none};
    }
}
// How big a break is, for its sound. An end that hangs on by the grip tape is a small one; an end
// that comes off, or a board that folds a little, is in between; a board that folds right down or
// comes apart in the middle is the biggest. `fold_depth` is 0 to 1, how far a fold sags.
enum class Size { light, medium, heavy };
constexpr std::array<std::string_view, 3> size_names{"light", "medium", "heavy"};
constexpr Size break_size(Place place, bool joined, float fold_depth) noexcept {
    if (place == Place::mid) return !joined || fold_depth >= 0.7f ? Size::heavy : Size::medium;
    return joined ? Size::light : Size::medium;
}
// How likely a slam of `force` breaks the board: never up to 60% of `heavy`, always from `heavy` up,
// rising smoothly in between. A board only breaks from a hit the player can feel was a big one.
constexpr float break_chance(float force, float heavy) noexcept {
    if (heavy <= 0 || force >= heavy) return 1.0f;
    const auto low = 0.6f * heavy;
    if (force <= low) return 0.0f;
    const auto t = (force - low) / (heavy - low);
    return t * t * (3.0f - 2.0f * t);
}

// ---- where the parts of a broken board lie ------------------------------------------------------
constexpr float axle_height = 0.028f, front_axle = 0.2226f, back_axle = -0.2225f;
// How far each half of a board tips: a touch short of where its first splinter meets the ground
// (0.351 and 0.325 rad, measured on the mesh), so the end rests on the ground instead of digging in.
constexpr float front_tip = 0.340f, back_tip = 0.315f;
// Half a board on one truck does not stand level: it tips about its axle until the broken end lies
// on the ground. `towards` is +1 when the broken end is on the nose side of that axle.
inline Rigid tipped_on_axle(float axle_z, float towards, float angle) noexcept {
    const auto turned = about({1, 0, 0}, towards * angle);
    const Vec axle{0, axle_height, axle_z};
    return {turned, axle - turn(turned, axle)};
}
// A piece that hangs on by the grip tape: turned about the break line just under the deck's top,
// and drawn a hair away from the board, so the tape opens into a ragged crack with the wood of the
// broken end in it. `towards` is +1 for the nose side of the break.
constexpr float crack = 0.0025f; // how far the tape gapes along the board (m)
constexpr float grip_height = 0.0935f;
inline Rigid hung_on_grip(float break_z, float towards, float angle) noexcept {
    const auto turned = about({1, 0, 0}, towards * angle);
    const Vec hinge{0, grip_height - 0.004f, break_z};
    return {turned, hinge - turn(turned, hinge) + Vec{0, 0, towards * crack * 0.5f}};
}
// A board folded in the middle: each half turns about its own axle, the front by `depth` of the
// way to the ground and the back as far as brings its grip to the same height, and the two move
// along the board until the grip of both all but meets again at the break, where the tape holds.
inline void folded(float depth, Rigid& front_half, Rigid& back_half) noexcept {
    constexpr float grip_y = 0.095f;
    constexpr float rise = grip_y - axle_height, ahead = front_axle - mid_row, behind = mid_row - back_axle;
    const float a = front_tip * std::clamp(depth, 0.0f, 1.0f);
    const float height = rise * std::cos(a) - ahead * std::sin(a); // of the grip at the break, above the axles
    const float b = std::acos(std::clamp(height / std::hypot(rise, behind), -1.0f, 1.0f)) - std::atan2(behind, rise);
    front_half = tipped_on_axle(front_axle, -1.0f, a);
    back_half = tipped_on_axle(back_axle, 1.0f, b);
    const float front_end = front_axle - (rise * std::sin(a) + ahead * std::cos(a));
    const float back_end = back_axle + (rise * std::sin(b) + behind * std::cos(b));
    front_half.at.z += (back_end - front_end + crack) * 0.5f; // and a crack's width more: the tape gapes
    back_half.at.z -= (back_end - front_end + crack) * 0.5f;
}
// The deck sits on the board's rig a quarter turn about the upright axis (the rig's mount carries
// it), and the stretches hang on the rig's root: a motion worked out on the deck, carried over to
// the root's axes.
inline Rigid on_root(Quat mount, const Rigid& motion) noexcept {
    return {unit(mount * motion.facing * conj(mount)), turn(mount, motion.at)};
}

// Where something goes that is not to be seen: far below the board. The game's replay keeps where
// a bone is and how it is turned, and nothing of its scale, so a part is never hidden by shrinking.
constexpr Vec away{0.0f, -2000.0f, 0.0f};
// One of the four stretches of deck (nose, front, back, tail, in that order everywhere): whether
// it shows, and its motion from the place it has in a whole board.
struct Stretch {
    bool shown{};
    Rigid motion{};
};
// A broken board as it lies: its stretches and its trucks, in the rig root's axes.
struct Layout {
    std::array<Stretch, 4> stretch{};
    bool front_truck{}, back_truck{};
    Rigid front_motion{}, back_motion{};
};
// The piece a board keeps (`kept`, Piece bits), hanging together as `hinge` says. `angle` is how far
// an end droops (radians) or how deep a fold sags (0 to 1); `up` is how far the board is the right
// way up (1 on its wheels, -1 on its grip); `mount` is the rig's mount turn.
inline Layout lay_out(std::uint32_t kept, Hinge hinge, float angle, float up, Quat mount) noexcept {
    // `ahead` moves the front of the board (and all of it unless told otherwise), `rear` the back half.
    Rigid ahead{}, rear{}, hung{};
    if (hinge == Hinge::fold) {
        // The break sags towards the ground under the board. On its grip the fold would push nose
        // and tail into the ground instead, so there it stays shallow.
        const auto level = std::clamp((up + 0.2f) / 0.8f, 0.0f, 1.0f);
        folded(angle * (0.3f + 0.7f * level), ahead, rear);
    } else if (hinge == Hinge::nose) hung = hung_on_grip(nose_row, 1.0f, angle);
    else if (hinge == Hinge::tail) hung = hung_on_grip(tail_row, -1.0f, angle);
    else if (static_cast<bool>(kept & front_truck) != static_cast<bool>(kept & back_truck)) {
        // Half a deck on one truck: while the board is the right way up it tips onto its broken end
        // instead of standing level in the air.
        const auto level = std::clamp((up - 0.3f) / 0.5f, 0.0f, 1.0f);
        const auto amount = level * level * (3.0f - 2.0f * level);
        ahead = kept & front_truck ? tipped_on_axle(front_axle, -1.0f, front_tip * amount) : tipped_on_axle(back_axle, 1.0f, back_tip * amount);
    }
    ahead = on_root(mount, ahead);
    hung = on_root(mount, hung);
    rear = hinge == Hinge::fold ? on_root(mount, rear) : ahead;
    Layout out;
    out.stretch = {{{(kept & nose) != 0, hinge == Hinge::nose ? hung : ahead},
                    {(kept & front) != 0, ahead},
                    {(kept & back) != 0, rear},
                    {(kept & tail) != 0, hinge == Hinge::fold ? rear : hinge == Hinge::tail ? hung : ahead}}};
    out.front_truck = (kept & front_truck) != 0;
    out.back_truck = (kept & back_truck) != 0;
    out.front_motion = ahead;
    out.back_motion = rear;
    return out;
}
// What goes into the stretches' bones. The first three hang on the rig's root; the tail's hangs on
// the front stretch's, so its place is given from there. A stretch that does not show goes away.
inline std::array<Rigid, 4> bones_of(std::array<Stretch, 4> stretch) noexcept {
    for (auto& one : stretch)
        if (!one.shown) one.motion = {{}, away};
    const auto& from = stretch[1].motion;
    const auto& to = stretch[3].motion;
    return {{stretch[0].motion, stretch[1].motion, stretch[2].motion,
             {unit(conj(from.facing) * to.facing), turn(conj(from.facing), to.at - from.at)}}};
}
// A truck hangs on the deck's bone, which is moved `away` to hide the real deck, and that bone sits
// on the mount. To move the truck with a piece: the piece's motion seen from the deck, the deck's
// move taken back. `steer` is the truck's own turn, which it keeps; `place` where it sits on the deck.
constexpr Vec back_truck_place{0.0005f, -0.0183f, -0.2345f}, front_truck_place{0.0005f, -0.0183f, 0.2347f};
inline Rigid truck_bone(const Rigid& motion, Quat mount, Vec mount_at, Quat steer, Vec place) noexcept {
    const auto facing = unit(conj(mount) * motion.facing * mount);
    const auto at = turn(conj(mount), turn(motion.facing, mount_at) + motion.at - mount_at);
    return {unit(facing * steer), turn(facing, place) + at - away};
}
// The trucks' baseplates. The game's own truck model has them on the deck's bone, which goes away
// with the real deck when the board breaks, and they went with it. A deck pack's truck model has
// each baseplate on its truck's effector instead, a bone under the truck's that no model uses:
// wherever a piece carries the truck, its baseplate goes along. This is where that effector goes
// so that the baseplate sits on the deck however the truck steers: the truck's own turn and place
// taken back, its rest put in. `truck` is the truck's bone as the animation left it this frame.
struct TruckRest {
    Quat facing;  // the truck's bone on the deck's, at rest
    Vec at;
    Vec effector; // its effector on the truck's bone, at rest (not turned)
};
constexpr TruckRest back_truck_rest{{-0.3641458f, -0.3641458f, -0.6061335f, 0.6061335f}, {0.0005361f, -0.0183003f, -0.2344986f}, {0.0395021f, 0.0f, 0.0f}};
constexpr TruckRest front_truck_rest{{-0.6061674f, 0.6061674f, 0.3640894f, 0.3640894f}, {0.0005361f, -0.0182917f, 0.2346680f}, {0.0395677f, 0.0f, 0.0f}};
inline Rigid baseplate_bone(const Rigid& truck, const TruckRest& rest) noexcept {
    const auto back = conj(truck.facing);
    return {unit(back * rest.facing), turn(back, rest.at + turn(rest.facing, rest.effector) - truck.at)};
}
} // namespace dingosdk::board_break
