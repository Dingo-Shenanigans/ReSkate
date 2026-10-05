#pragma once
#include "Engine/Game/Skater/style_pose.h"
#include "Extension/Multiplayer/Net/protocol.h"
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The style editor's clips: one flip trick as the game animated it, frame by frame, on the trick's timeline. No game dependencies.
namespace dingosdk::style {
using multiplayer::Pose;
using multiplayer::Transform;

struct ClipFrame {
    Pose pose;
    // Timeline time: 0 to 3 in the trick, seconds below 0 before the flick, 3 plus seconds after the end.
    float time = -1;
    std::uint32_t at{}; // milliseconds from the clip's first frame
    // The game's camera for this frame (rows: right, up, backward, position). fov is 0 when not recorded.
    std::array<float, 16> view{};
    float fov{};
    std::vector<Transform> rig; // the joints of the game's board
};
struct Clip {
    std::uint8_t trick{};
    bool learned{}; // learned from Skatepedia's demonstration
    std::vector<ClipFrame> frames;
    std::uint32_t began{}; // the recording-clock time of the first frame. Not saved
};
// A root move longer than this between two frames is a jump of the demonstration back to its start.
inline constexpr float teleport_metres = 1.0f;

// Sets the exact timeline time of each frame from its part number. Returns false if the trick has no catch or no landing.
bool retime(Clip &clip);
// The game slows parts of its demonstration. Rewrites frame times from ground distance at constant roll speed. False if not possible.
bool unwarp(Clip &clip);
// The pose at a timeline time, blended between the two frames around it.
[[nodiscard]] Pose sample(const Clip &clip, float time);
// `a` moved by `amount` (0 to 1) toward `b`. Poses of different shapes give `a`.
[[nodiscard]] Pose blend(const Pose &a, const Pose &b, float amount);
// The timeline time at `milliseconds` into playback at recorded speed.
[[nodiscard]] float time_at(const Clip &clip, float milliseconds) noexcept;
// The playback milliseconds at a timeline time. The reverse of time_at.
[[nodiscard]] float ms_at(const Clip &clip, float time) noexcept;
[[nodiscard]] std::uint32_t duration(const Clip &clip) noexcept;

// A clip as a file. decode_clip throws on invalid data. A file of another version is refused, and the editor learns that clip again.
inline constexpr std::uint32_t clip_version = 4;
[[nodiscard]] std::vector<std::uint8_t> encode_clip(const Clip &clip);
[[nodiscard]] Clip decode_clip(std::span<const std::uint8_t> bytes);

// One frame of another character's pose as read from the game: parent-local joints.
struct RigFrame {
    std::uint32_t at{};
    std::vector<Transform> joints;
};
// The skater skeleton joints that clip_from_capture reads.
struct RigJoints {
    std::uint16_t trajectory{}, deck{}, left_foot{}, right_foot{};
    std::span<const std::int32_t> parents;
};
// The world transform of a joint, composed from the root.
[[nodiscard]] Transform world(const std::vector<Transform> &joints, std::span<const std::int32_t> parents, std::size_t joint);
[[nodiscard]] Transform compose(const Transform &parent, const Transform &child) noexcept;
[[nodiscard]] Transform inverse(const Transform &transform) noexcept;
// Cuts one jump from a recording of Skatepedia's skater. `board` is a live board pose, entity transform first. Sets `why` on failure.
std::optional<Clip> clip_from_capture(std::uint8_t trick, const std::vector<RigFrame> &frames, const RigJoints &joints,
                                      const std::vector<Transform> &board, const Transform &deck_to_board, std::string &why);
} // namespace dingosdk::style
