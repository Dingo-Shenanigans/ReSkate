#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <limits>
#include <memory>
#include "utf8_text.h"

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
// The fingers, read off the skeleton's own parent table (animation_skeleton.h): each hand carries five
// chains, the thumb and four fingers, and an attach joint the game hangs props on which is not one of
// them. They are not edited - there is no row for them - but the studio closes them on the board.
inline constexpr std::uint16_t right_hand_joint = 49, left_hand_joint = 278;
inline constexpr std::array<std::array<std::uint16_t, 4>, 5> right_fingers{{
    {51, 52, 53, 0}, {54, 55, 56, 57}, {58, 59, 60, 61}, {62, 63, 64, 65}, {66, 67, 68, 69},
}};
inline constexpr std::array<std::array<std::uint16_t, 4>, 5> left_fingers{{
    {279, 280, 281, 282}, {284, 285, 286, 287}, {288, 289, 290, 291}, {292, 293, 294, 295}, {296, 297, 298, 0},
}};
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
    bool smooth{}; // smoothed (Smooth keys, Smooth chosen keys, a double-click): the timeline draws it round
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
// One number of an edit, as the timeline's channel rows show it: 0-2 its turn about X, Y and Z in degrees
// (to_euler), 3-5 its move along X, Y and Z.
float channel_value(const Offset& edit, int channel) noexcept;
// The edit with one of those numbers set and the others as they were.
Offset with_channel(Offset edit, int channel, float value) noexcept;

struct AnimationSource {
    std::string kind{"unknown"}; // native_asset, render_capture, unknown (legacy)
    std::string asset, rig, board_frame;
    std::string position_codec; // provenance of native source position decoding; empty for legacy
};
struct Animation {
    std::string name;  // the profile's name, also its file name
    std::string trick;  // what the player says it is (Kickflip, Heelflip ...)
    std::string author; // who made it, kept with it so a shared trick says whose it is
    std::string maker_key; // the key of the PC it was made on: on any other it plays, but is not opened to edit or copied
    bool editable{};       // its maker let others edit and copy it (the pack it was shared in said so)
    std::string label;  // the name it is shown by in MY TRICKS and the packs (PACKS); empty: its name
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
    // What the game said at takeoff when it was recorded: rolling fakie, and the board turned backwards
    // under him. Empty for recordings from before these were kept.
    std::optional<bool> fakie, board_backwards;
    // A stance version of a trick (its fakie, switch or switch fakie copy, make_stance_version) keeps the foot it leads
    // with: the studio opens it on that foot (a switch version shows him riding switch), and it is saved on it. Empty:
    // not a stance version.
    std::optional<bool> keep_leads_right;
    // Which upgrade this file has had (empty: made before upgrades). Older builds ignore it.
    std::string upgraded;
    // The board's keys are in the board's nose-forward frame (every save since 2026-10-22). Older files
    // made on a clip whose board points backwards had them the other way round; the upgrade turns them.
    bool board_nose_forward{};
    // The catch its maker set on the timeline (CATCH): the moment between the pop and the landing that the board is
    // caught, held in the air off a bigger drop than the clip's and then played on to its landing. Empty: the studio
    // finds the catch itself. Older builds ignore it.
    std::optional<float> catch_at;
    Track* track(std::uint16_t joint) noexcept;
    const Track* track(std::uint16_t joint) const noexcept;
    std::size_t key_count() const noexcept;
};
// Puts the animation's edits at `seconds` on `pose` (already holding the base pose).
void apply_edits(const Animation& animation, float seconds, std::span<Joint> pose) noexcept;
// Same anatomical joint and local edit axes as F8; stance never swaps authored limbs.
// Which limb a joint belongs to: 1 left arm, 2 right arm, 3 left leg, 4 right leg, 0 the rest of him.
// The authored layer can be let go of a limb at a time, so a grind can take the feet while the arms keep
// what was posed for them.
int limb_group(std::uint16_t joint) noexcept;
struct LimbWeights {
    std::array<float, 5> limb{{1, 1, 1, 1, 1}}; // [0] is everything that is not a limb
    float of(std::uint16_t joint) const noexcept { return limb[static_cast<std::size_t>(limb_group(joint))]; }
    bool whole() const noexcept {
        return limb[0] >= 1 && limb[1] >= 1 && limb[2] >= 1 && limb[3] >= 1 && limb[4] >= 1;
    }
};
// `studio`, when given, is the pose the keys were made on (the clip at `seconds`, mirrored when the keys
// are): a joint that is really bent goes towards that pose with the bend on it - what the studio shows -
// rather than having the bend added to whatever the game is doing then. `match` (0..1) is how far.
// `match_rest` (0..1) takes the joints with no bend towards the studio's pose too: the whole body then plays
// the recording, with the keys on it, instead of the game's own animation showing through.
void apply_authored_edit_layer(const Animation& animation, float seconds, std::span<Joint> native, float weight,
                               const LimbWeights* limbs = nullptr, std::span<const Joint> studio = {}, float match = 0,
                               float match_rest = 0);
// The clip's pose at `seconds`, mirrored the game's way when `mirrored` (for apply_authored_edit_layer).
std::vector<Joint> studio_pose(const Clip& clip, float seconds, bool mirrored);

// Which foot a clip leads with, read from the clip itself rather than from its label: the game mirrors the
// body in the skater's own frame, so with the right foot on that frame's +X side it is the game's own
// (goofy) layout - the right foot leads - and with the left foot there it is mirrored - the left leads.
// Taken before the pop, feet on the board. Empty when the feet are too close together to tell.
std::optional<bool> clip_leads_right(const Clip& clip);
// The pop and the landing of one of the game's own trick clips (the library, and animations made from one),
// put where the game has them. The library found the pop at the board's first low point, which on most
// clips is right but on many low pops is a later dip, up to 0.3 s after the pop: in game the trick then
// started halfway through the flip and stood waiting before the landing. The game leaves the ground about
// 0.05 s after its board starts to tip, in its own clips and in game alike, so a pop marked later than that
// is moved there. The landing is the board's deepest low between 0.25 and 0.85 s after the pop (the
// library's own rule), which a wrong pop had looked for in the wrong place (four 360 shuv-its landed with
// the board still turning, or 0.2 s after it was down). Not for a recording: its marks are the game's own.
// Returns whether it moved either.
bool place_native_marks(Clip& clip) noexcept;
// A zip with every file stored as it is (no compression), names as given (use '/'): for sharing packs.
std::uint32_t crc32_of(const std::string& data) noexcept;
std::string stored_zip(const std::vector<std::pair<std::string, std::string>>& files);

// ---- joint limits ----------------------------------------------------------------------------------
// How far each editable joint may turn, measured from the game's own animation (skater_joint_limits.h):
// a twist range about the bone and how far the bone may swing in each direction. A knee or an elbow is a
// hinge, a shoulder a wide cone. `local` is the joint's whole parent-local rotation, not an edit.
bool has_joint_limit(std::uint16_t joint) noexcept;
Quat limit_rotation(std::uint16_t joint, const Quat& local) noexcept;
// Keeps every joint the animation has a real key on inside its limits; joints without keys (the game's own
// pose, which is always natural) are left alone.
void limit_keyed_joints(const Animation& animation, float seconds, std::span<Joint> pose) noexcept;
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
// Smooths only the keys at `times`: each eases smoothly, is drawn a little towards its neighbours (as
// smooth_track draws them) and is marked round. Keys on the brackets keep their pose. Returns how many
// keys changed.
std::size_t smooth_keys_at(Track& track, std::span<const float> times, float keep_start, float keep_end) noexcept;

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
// Puts a foot (or any joint at the end of a two-bone limb) at `to` (in the frame world() gives) and, given, turned as `turn`,
// `share` of the way, by bending the limb (thigh and shin) the way it already bends: the hip stays where it is. In game:
// the feet back on the deck where the studio has them (seat_feet_on_board).
void place_foot(std::vector<Joint>& pose, std::uint16_t foot, const Vec3& to, const Quat* turn, float share) noexcept;

// ---- goofy ----------------------------------------------------------------------------------------
// The other side of the body (a regular trick played goofy, and back): the skater mirrored across its
// middle, the X of its root. A rotation keeps its X and negates its Y and Z; a position or offset
// negates its X. Every joint is replaced by its mirror joint (skater_mirror.h), so a left foot takes the
// right foot's pose, reflected. Mirroring twice gives the same animation back.
Quat mirror_rotation(const Quat& q) noexcept;
Vec3 mirror_vector(const Vec3& v) noexcept;
Joint mirror_pose(const Joint& joint) noexcept;
// The board under the game's mirror: not turned end for end, its offset across reflected.
Joint mirror_board_pose(const Joint& board) noexcept;
// A board key (in the board's own frame) under the game's mirror: across is what reflects.
Quat mirror_board_rotation(const Quat& q) noexcept;
Vec3 mirror_board_vector(const Vec3& v) noexcept;
// The clip on the other side: every frame, and the board. Only for a skater clip (joints == 395).
Clip mirror_clip(const Clip& clip);
// The same, but only across the skinned body (gameplay_body_joint): the placement, the board's own joints
// and the effector subtrees are left exactly as they were. The render rig parts company with the animation
// rig after Ground (375), so mirroring every joint puts those on the wrong bones and tears the skater up.
Clip mirror_body_clip(const Clip& clip);
// One frame, mirrored the same way: out of the parent-local pose into the skater's own frame, reflected
// there, each joint swapped for its opposite number, and back to parent-local against mirrored parents.
void mirror_pose_frame(std::span<const Joint> pose, std::span<Joint> out, bool body_only = true);
// The animation on the other side: its clip, and every track and key on the mirror joints.
Animation mirror_animation(const Animation& animation);

// ---- files ----------------------------------------------------------------------------------------
// The profile: name, trick, settings and keyframes, as versioned JSON. The clip goes beside it in a
// small binary file. Both throw std::runtime_error on bad input; nothing half-read is returned.
std::string to_json(const Animation& animation);
Animation from_json(std::string_view text); // without the clip
std::vector<std::uint8_t> clip_bytes(const Clip& clip);
Clip clip_from_bytes(std::span<const std::uint8_t> bytes);
// ---- which of the game's own clips is playing -----------------------------------------------------
// The game says which trick it is doing, never which of its clips it plays - and most of what it plays is
// not a trick at all (setups, loops, exits, the pieces a trick is blended from). Nothing in its memory was
// found to name the playing clip (two probes, 2026-10-21), so it is recognised the way a person would: by
// the skater's pose. A piece is playing when the body has been moving through it, in step, for the last
// quarter of a second. Looking like one frame of it is not enough - riding looks like the first frame of
// half the tricks - so the window has to move through the piece, keep in time with it, and fit better
// than holding any one of its frames still. Tried offline on 38 recordings: it found the kickflip pieces in
// the kickflips, at the right time, and nothing in the rest.
struct PieceMatch {
    std::size_t piece{}; // index into the clips given to set_pieces
    bool mirrored{};     // the piece is playing the other way round
    float time{};        // where in the piece, seconds
    float score{};       // how far the pose is from it: 0 is exact
};
// A grind, by the pose it holds. A grind barely moves: the game holds the pose of its loop and plays the loop over and
// over with its balance layered on top, so the moving match (PieceTracker::update) seldom takes one. While the game is
// grinding, the held pose itself says which saved loop it is: the spine and the legs (the arms are the balance's),
// compared with every frame of each clip, as made and mirrored. Each clip's nearest frame, the nearer way round, nearest
// first; `score` as PieceMatch's (the mean of 1 - |dot| over the joints compared: 0 is the same pose).
struct HeldPose {
    std::size_t piece{}; // index into the clips given
    bool mirrored{};
    float time{};        // its nearest frame, seconds
    float score{1};
};
// `with_arms`: the arms are compared too (a grab is in the hands).
std::vector<HeldPose> held_pose_fits(std::span<const Clip* const> clips, std::span<const Joint> pose, bool with_arms = false);
class PieceTracker {
public:
    // The clips to look for, each as it is and mirrored. A clip that barely moves cannot be told from
    // standing still and is left out (looking_for says how many are looked for).
    void set_pieces(const std::vector<const Clip*>& clips);
    // One frame of the game's own pose (before any edit). The piece playing now, if one is.
    std::optional<PieceMatch> update(double at, std::span<const Joint> pose);
    void reset() noexcept;
    std::size_t looking_for() const noexcept { return models_.size(); }

private:
    struct Model {
        std::size_t piece{};
        bool mirrored{};
        std::vector<std::uint16_t> joints;
        std::vector<Quat> frames; // times.size() * joints.size()
        std::vector<float> times;
    };
    struct Sample {
        double at{};
        std::vector<Quat> rotation; // the body joints, below Ground
    };
    struct Fit {
        float score{1}, still{1}, time{-1};
        bool moving{};
    };
    Fit fit(const Model& model, const std::array<const Sample*, 8>& window, const std::array<float, 8>& ages) const;
    std::vector<Model> models_;
    std::deque<Sample> history_;
    double last_at_{-1};
    int locked_{-1}, candidate_{-1};
    int streak_{};
    float candidate_from_{}, candidate_last_{};
    float play_time_{}, last_fit_time_{};
    double advanced_at_{};
};

// ---- the game's own clip, and how far into it it is ------------------------------------------------
// A saved trick plays as the game plays its own: the game never says which clip it plays for a trick or how
// far into it it is, but its own pose (before any edit) says both. Each animation update that pose is matched
// against the clips it may be playing for the trick - the trick's own clip and the library's low, mid and high
// pop of it, each as it is and mirrored - and the edits are laid on at the time read off it. A clip the game
// plays that is not the edit's own (another pop) is read across through the pop and landing marks. When the
// game goes into another animation its pose stops matching and the trick lets go: the game leads, the edits
// only ride on it. (Measured on testers' dumps: the clip playing is told apart from the others by 7 degrees or
// more, and its time to about a frame.)
struct NativeModels {
    struct Model {
        std::size_t clip{};    // which clip given to build_native_models
        bool own{};            // the edit's own clip: its time is the edit's
        bool mirrored{};       // compared mirrored (the game mirrors the body by stance)
        float up{-1}, down{-1}, duration{};
        std::vector<std::uint16_t> joints; // the live joints compared, in order
        std::vector<float> times;
        std::vector<Quat> frames; // times.size() * joints.size()
    };
    std::vector<Model> models;
    float edit_up{-1}, edit_down{-1}; // the edit's own clip's pop and landing
    // A time in a model's clip as a time in the edit's own clip: the same before the pop, stretched between the
    // pop and the landing, the same again after the landing. model_time is the other way.
    float edit_time(const Model& model, float seconds) const noexcept;
    float model_time(const Model& model, float edit_seconds) const noexcept;
};
// `clips[own]` is the edit's own clip; the others are the clips the game may play for its trick instead.
std::shared_ptr<const NativeModels> build_native_models(const std::vector<std::shared_ptr<const Clip>>& clips, std::size_t own);
class NativeClock {
public:
    static constexpr float lock_degrees = 4.5f; // how near the game's pose has to be to a clip to follow it
    static constexpr float good_degrees = 3.0f; // nearer than this the edits are on in full
    static constexpr float lose_degrees = 6.0f; // further than this (and than it began) for a few updates: lost
    struct Reading {
        bool locked{};   // found the game's clip
        bool lost{};     // no longer on it (the game's own landing, its next animation, the clip's end): the
                         // time goes on at the clip's own pace from there
        float time{-1};  // seconds into the edit's own clip
        float error{-1}; // degrees, the game's pose against the clip at that time (still measured once lost)
        float baseline{-1}; // the error it had while it was followed: how near this clip comes at best
        int clip{-1};    // which clip (as given to build_native_models)
        bool mirrored{};
        float confidence{}; // 1 on the clip, falling to 0 as the game blends away from it
        bool switched{};    // moved onto another of the clips, that fits the game better, this update
    };
    void start(std::shared_ptr<const NativeModels> models) noexcept; // a new trick: look for its clip again
    void reset() noexcept;
    // One animation update: the game's own pose and the seconds the game moved on since the last.
    Reading update(std::span<const Joint> pose, float step) noexcept;
    bool active() const noexcept { return static_cast<bool>(models_); }

private:
    float error_at(const NativeModels::Model& model, std::size_t frame, std::span<const Joint> pose) const noexcept;
    std::shared_ptr<const NativeModels> models_;
    int locked_{-1};
    float time_{-1}, error_{-1}, confidence_{}, baseline_{-1};
    int bad_{}, updates_{};
    int candidate_{-1}, streak_{}; // another clip fitting better, and for how many updates
    bool lost_{};
};
// A name safe as a file name ("Kickflip 2"): letters, digits, space, - and _ only.
std::string file_safe(std::string_view name);
} // namespace dingosdk::anim_studio
