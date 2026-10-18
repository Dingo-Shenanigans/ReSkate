#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// FOURTH Animation Studio's own data: a trick recorded from the game, and the keyframes a player puts
// on it. Nothing here touches the game. The game side (anim_studio.cpp) copies the pose the native
// animation just wrote into a Clip, and writes the evaluated pose back over the same buffer, the way
// first person shrinks the head (client_first_person.cpp): no native animation data, channel or
// asset is created or changed, and the next native update writes the whole pose again.
//
// A pose is parent-local per joint, {scale, Hamilton quaternion xyzw, translation}, in the numbering
// of the 395-joint AnimBase_Default_Skeleton the skater's animation output uses (Hips 7, Spine 42,
// Head 103). Its first 386 joints are the render skeleton's, with the parents in
// Extension/Multiplayer/Net/skater_bone_parents.h; joint 1 (AITrajectory) carries the world placement.
namespace dingosdk::anim_studio {
using Vec3 = std::array<float, 3>;
using Quat = std::array<float, 4>; // x, y, z, w

struct Joint {
    Vec3 scale{1, 1, 1};
    Quat rotation{0, 0, 0, 1};
    Vec3 position{};
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
// Not a skater joint: the board (its deck's mount, which carries the deck, trucks and wheels). Its
// track turns and moves the board where the recording had it, in the board's own frame.
inline constexpr std::uint16_t board_joint = 900;

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

// ---- keyframes ------------------------------------------------------------------------------------
enum class Ease : std::uint8_t { smooth, linear, step };
// An edit of one joint: turned by `rotation` in its own frame (after the native pose), moved by
// `offset` in its parent's frame. Additive, so it rides on whatever the native animation does.
struct Key {
    float time{}; // seconds on the clip
    Quat rotation{0, 0, 0, 1};
    Vec3 offset{};
    Ease ease{Ease::smooth};
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

struct Animation {
    std::string name;  // the profile's name, also its file name
    std::string trick; // what the player says it is (Kickflip, Heelflip ...)
    Clip clip;
    std::vector<Track> tracks;
    float speed{1};
    bool loop{true};
    // Which pop it is for: "" any, or "low", "mid", "high" (a trick can have one of each).
    std::string pop;
    Track* track(std::uint16_t joint) noexcept;
    const Track* track(std::uint16_t joint) const noexcept;
    std::size_t key_count() const noexcept;
};
// Puts the animation's edits at `seconds` on `pose` (already holding the base pose).
void apply_edits(const Animation& animation, float seconds, std::span<Joint> pose) noexcept;
// Sets (adds or replaces) the key of `joint` at `seconds`; keys closer than `merge` are one key.
Key& set_key(Animation& animation, std::uint16_t joint, float seconds, const Offset& value, float merge = 1.0f / 120);
bool remove_key(Animation& animation, std::uint16_t joint, float seconds, float merge = 1.0f / 120);
// Moves a key to `to` (clamped to the clip), keeping the track in time order. Returns its new index.
std::size_t move_key(Track& track, std::size_t index, float to, float duration) noexcept;

// ---- skeleton -------------------------------------------------------------------------------------
// The world transform of `joint` composed from the root through its parents (skater_bone_parents),
// `local` being a whole parent-local pose. Uniform scale, as the game's skater has.
Joint world(std::span<const Joint> local, std::uint16_t joint) noexcept;
std::int16_t parent(std::uint16_t joint) noexcept;

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
