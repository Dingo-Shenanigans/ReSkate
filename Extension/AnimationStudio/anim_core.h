#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <limits>

// FOURTH Animation Studio's own data: a trick recorded from the game, and the keyframes a player puts
// on it. Nothing here touches the game. The game side (anim_studio.cpp) copies the pose the native
// animation just wrote into a Clip, and writes the evaluated pose back over the same buffer, the way
// first person shrinks the head (client_first_person.cpp): no native animation data, channel or
// asset is created or changed, and the next native update writes the whole pose again.
//
// A pose is parent-local per joint, {scale, Hamilton quaternion xyzw, translation}, in the numbering
// of the 395-joint AnimBase_Default_Skeleton the skater's animation output uses (Hips 7, Spine 42,
// Head 103). The hierarchy comes from AntSkeletonAsset (animation_skeleton.h),
// including its auxiliary targets; joint 1 (AITrajectory) carries world placement.
namespace dingosdk::anim_studio {
using Vec3 = std::array<float, 3>;
using Quat = std::array<float, 4>; // x, y, z, w

struct Joint {
    Vec3 scale{1, 1, 1};
    Quat rotation{0, 0, 0, 1};
    Vec3 position{};
    bool operator==(const Joint&) const = default;
};

// ---- maths ---------------------------------------------------------------------------------------
Quat multiply(const Quat& a, const Quat& b) noexcept; // a then b applied in a's frame: a * b
Quat conjugate(const Quat& q) noexcept;
Quat normalised(const Quat& q) noexcept;
Quat slerp(Quat a, const Quat& b, float t) noexcept;
Vec3 rotate(const Quat& q, const Vec3& v) noexcept;
Quat axis_angle(const Vec3& axis, float radians) noexcept;
// Degrees about X, then Y, then Z (each about the joint's own axes): the inspector's numbers.
Quat from_euler(const Vec3& degrees) noexcept;
Vec3 to_euler(const Quat& q) noexcept;
bool finite(const Joint& joint) noexcept;

// ---- the joints a player edits --------------------------------------------------------------------
struct EditableJoint {
    std::uint16_t index;
    std::string_view label; // as the timeline shows it
    bool moves;              // takes a position as well as a rotation (the hips)
};
// In the order of the timeline's rows.
std::span<const EditableJoint> editable_joints() noexcept;
const EditableJoint* editable(std::uint16_t joint) noexcept;
inline constexpr std::uint32_t skeleton_joints = 395;
inline constexpr std::uint16_t hips_joint = 7, placement_joint = 1;
// The animation rig's board mount, parented directly to AITrajectory. Distinct
// from the physical board's sixteen-bone rig (whose mount is bone 2).
inline constexpr std::uint16_t skateboard_root_joint = 380;
// Not a skater joint: the board (its deck's mount, which carries the deck, trucks and wheels). Its
// track turns and moves the board where the recording had it, in the board's own frame.
inline constexpr std::uint16_t board_joint = 900;
// Keys sit on whole frames of the timeline, 30 a second, never between two: anything that moves a key in
// time puts it back on one, so the timeline never grows odd fractions of a frame between its keys.
inline constexpr float key_fps = 30.0f;
float on_a_frame(float seconds) noexcept;
// The feet (343 and 10) ride this far above the board's mount, along the board's up: measured in the
// game, riding, 2026-10-18. The board is pulled under the lower foot by this much on the ground, and
// snapping puts a foot on the deck at this height in the air.
inline constexpr float feet_above_board = 0.104f;
// How long the board takes to go from under the feet to the clip's own at the pop, and back at the
// landing. Inside this much of either, the board is part way between the two.
inline constexpr float board_blend = 0.06f;

// ---- a recorded trick -----------------------------------------------------------------------------
struct Clip {
    std::uint32_t joints{};
    std::vector<float> times;  // seconds from the first frame, rising
    std::vector<Joint> frames; // times.size() * joints
    // The board each frame, relative to the skater's placement joint (1); empty when not recorded.
    std::vector<Joint> board;
    float takeoff{-1}, landing{-1}; // seconds, -1 when not seen
    bool empty() const noexcept { return times.empty() || !joints; }
    float duration() const noexcept { return times.empty() ? 0.0f : times.back(); }
    std::span<const Joint> frame(std::size_t i) const noexcept { return {frames.data() + i * joints, joints}; }
    // The pose at `seconds` (clamped), each joint blended between the two frames around it.
    void sample(float seconds, std::span<Joint> out) const noexcept;
    // The board at `seconds`, relative to the placement joint; false when the clip has none.
    bool sample_board(float seconds, Joint& out) const noexcept;
};
// Decide from the whole airborne trajectory, never independently at each displayed frame.
// This is a preview heuristic, not a reconstruction of the native graph or physics.
bool clip_has_vertical_trajectory(const Clip& clip) noexcept;

// ---- keyframes ------------------------------------------------------------------------------------
enum class Ease : std::uint8_t { smooth, linear, step };
// An edit of one joint: turned by `rotation` in its own frame (after the native pose), moved by
// `offset` in its parent's frame. Additive, so it rides on whatever the native animation does.
struct Key {
    float time{}; // seconds on the clip
    Quat rotation{0, 0, 0, 1};
    Vec3 offset{};
    Ease ease{Ease::smooth};
    bool smooth{}; // smoothed by the Smooth keys pass: the timeline draws it round instead of a diamond
};
struct Track {
    std::uint16_t joint{};
    std::vector<Key> keys; // by time
    bool hidden{}, locked{};
};
struct Offset {
    Quat rotation{0, 0, 0, 1};
    Vec3 offset{};
};
// Before the first key and after the last the nearest key holds.
Offset evaluate(const Track& track, float seconds) noexcept;

struct AnimationSource {
    std::string kind{"unknown"}; // native_asset, render_capture, unknown (legacy)
    std::string asset, rig, board_frame;
    std::string position_codec; // provenance of native source position decoding; empty for legacy
};
struct Animation {
    std::string name;  // the profile's name, also its file name
    std::string trick; // what the player says it is (Kickflip, Heelflip ...)
    Clip clip;
    std::vector<Track> tracks;
    float speed{1}; // Studio preview speed; gameplay source seconds stay at 1x.
    bool loop{true};
    // Which pop it is for: "" any, or "low", "mid", "high" (a trick can have one of each).
    std::string pop;
    // The stance the trick was recorded in: "regular" or "goofy" ("" for a file from before stances: regular).
    std::string stance;
    // Captured at takeoff, not after a 180/landing. Absent in legacy recordings.
    std::optional<bool> switch_stance;
    AnimationSource source;
    std::string entry{"ground"}; // observed ground/rail entry; unknown is never guessed as ground
    // The brackets on the timeline: the window the animation works in, from `start` to `end` (end < 0: to the
    // clip's end). Moving either bracket retimes unlocked tracks into the new window.
    float start{0};
    float end{-1};
    Track* track(std::uint16_t joint) noexcept;
    const Track* track(std::uint16_t joint) const noexcept;
    std::size_t key_count() const noexcept;
};
// Puts the animation's edits at `seconds` on `pose` (already holding the base pose).
void apply_edits(const Animation& animation, float seconds, std::span<Joint> pose) noexcept;
// Same anatomical joint and local edit axes as F8; stance never swaps authored limbs.
void apply_authored_edit_layer(const Animation& animation, float seconds, std::span<Joint> native, float weight);
// Explicit cross-frame transfer experiment; gameplay uses anatomical apply_authored_edit_layer.
// Transfers authored offsets through placement-relative joint frames onto a native pose.
// Unlike mirror_clip, this never copies recorded skeleton transforms into the target rig.
void retarget_edits(const Animation& animation, float seconds, std::span<const Joint> source,
                    std::span<Joint> target, bool mirror, bool reverse_facing = false);
// Add only authored edits over a fresh native pose; no recorded rest transforms or scales.
void apply_native_edit_layer(const Animation& animation, float seconds, std::span<const Joint> source,
                             std::span<Joint> native, bool mirror, float weight, bool reverse_facing = false);
Joint board_edited(const Joint& board, const Animation& edits, float seconds, const Quat& frame, bool mirror = false);
std::vector<Offset> pose_edit_deltas(std::span<const Joint> native, std::span<const Joint> edited);
void apply_pose_deltas(std::span<const Offset> deltas, std::span<Joint> native);
// Native/custom blend at explicitly edited bracket boundaries. Legacy full-clip windows stay at 1.
float animation_window_weight(const Animation& animation, float seconds) noexcept;
// Sets (adds or replaces) the key of `joint` at `seconds`; keys closer than `merge` are one key.
Key& set_key(Animation& animation, std::uint16_t joint, float seconds, const Offset& value, float merge = 1.0f / 120);
// Moves every key of every track by `delta` seconds (the start bracket moves with them); keys never go before 0.
void shift_keys(Animation& animation, float delta, float duration = std::numeric_limits<float>::infinity()) noexcept;
// Moves one bracket: 1 the start, 2 the end, to `to`. Only the keys sitting on that bracket go with it -
// everything between the brackets stays where it is, so nothing is stretched and the timeline keeps its
// whole frames. A key the bracket is pulled back past is left where it is and stops applying until the
// bracket is taken out over it again.
bool move_window_edge(Animation& animation, int which, float to, float duration) noexcept;
bool remove_key(Animation& animation, std::uint16_t joint, float seconds, float merge = 1.0f / 120);
// Moves a key to `to` (clamped to the clip), keeping the track in time order. Returns its new index.
std::size_t move_key(Track& track, std::size_t index, float to, float duration) noexcept;
// Smooths a track's keys: every key eases smoothly, keys that say nothing the curve would not have said
// anyway are dropped, and the rest are drawn towards their neighbours so the motion runs evenly. The keys
// at `keep_start` and `keep_end` (the brackets) are never moved or dropped. Returns how many keys it
// changed or removed; they are all marked `smooth`.
std::size_t smooth_track(Track& track, float keep_start, float keep_end) noexcept;

// ---- skeleton -------------------------------------------------------------------------------------
// The world transform of `joint` composed from the root through its animation-rig parents,
// `local` being a whole parent-local pose. Uniform scale, as the game's skater has.
Joint world(std::span<const Joint> local, std::uint16_t joint) noexcept;
std::int16_t parent(std::uint16_t joint) noexcept;
// Gameplay owns the skinned body, while native attachments/effector subtrees stay native.
bool gameplay_body_joint(std::uint16_t joint) noexcept;

// ---- dragging a limb (inverse kinematics) ---------------------------------------------------------
// Moves the joint `end` to `target` (in the frame world() gives) by bending the chain of `length` joints above
// it: the top of the chain stays where it is, the joints between bend, and the end is put where the drag is.
// `drag_joint` writes the bend as keys at `seconds`: the native pose is the clip's, the rest of the animation
// stays as it is. The end joint's own turn is not changed.
struct Chain {
    std::array<std::uint16_t, 8> joints{}; // from the top of the chain (joints[0]) down to the end
    std::size_t count{};
};
Chain chain_above(std::uint16_t end, std::size_t length) noexcept;
// The chain's new local rotations for the pose `pose` (which already has the animation's edits applied).
void solve_chain(std::span<const Joint> pose, const Chain& chain, const Vec3& target, std::span<Quat> rotations) noexcept;
void drag_joint(Animation& animation, std::span<const Joint> native, float seconds, std::uint16_t end, const Vec3& target, std::size_t length = 3);

// ---- goofy ----------------------------------------------------------------------------------------
// The other side of the body (a regular trick played goofy, and back): the skater mirrored across its
// middle, the X of its root. A rotation keeps its X and negates its Y and Z; a position or offset
// negates its X. Every joint is replaced by its mirror joint (skater_mirror.h), so a left foot takes the
// right foot's pose, reflected. Mirroring twice gives the same animation back.
Quat mirror_rotation(const Quat& q) noexcept;
Vec3 mirror_vector(const Vec3& v) noexcept;
Joint mirror_pose(const Joint& joint) noexcept;
// The clip on the other side: every frame, and the board. Only for a skater clip (joints == 395).
Clip mirror_clip(const Clip& clip);
// The animation on the other side: its clip, and every track and key on the mirror joints.
Animation mirror_animation(const Animation& animation);

// ---- files ----------------------------------------------------------------------------------------
// The profile: name, trick, settings and keyframes, as versioned JSON. The clip goes beside it in a
// small binary file. Both throw std::runtime_error on bad input; nothing half-read is returned.
std::string to_json(const Animation& animation);
Animation from_json(std::string_view text); // without the clip
std::vector<std::uint8_t> clip_bytes(const Clip& clip);
Clip clip_from_bytes(std::span<const std::uint8_t> bytes);
// A name safe as a file name ("Kickflip 2"): letters, digits, space, - and _ only.
std::string file_safe(std::string_view name);
} // namespace dingosdk::anim_studio
