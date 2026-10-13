#pragma once
#include "Extension/Style/style_pose.h"
#include "Extension/Multiplayer/Net/protocol.h"
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The style editor's clips: one flip trick as the game animated it, frame by frame. No game dependencies.
namespace dingosdk::style {
using multiplayer::Pose;
using multiplayer::Transform;

struct ClipFrame {
    Pose pose;
    // Timeline time: 0 to 3 in the trick, seconds below 0 before the flick, 3 plus seconds after the end.
    float time = -1;
    std::uint32_t at{}; // milliseconds from the clip's first frame
    std::vector<Transform> rig; // the joints of the game's board
};
// A clip keeps this long before the flick and after the touchdown.
inline constexpr std::uint32_t lead_ms = 800, follow_ms = 800;
// A clip of another recipe_key is outdated. Raise recipe_version for a change no field shows: the recorder's steps, the stage or the cut.
inline constexpr std::uint32_t recipe_version = 1;
struct Recipe {
    std::uint32_t version{recipe_version};
    std::string game;                     // the game's Steam build: its animations and gestures
    std::uint32_t flick_ms{}, still_ms{}; // the scripted pad's flick, and the still time before it
    std::uint32_t lead_ms{style::lead_ms}, follow_ms{style::follow_ms};
};
[[nodiscard]] std::string recipe_text(const Recipe &recipe);
// 64-bit FNV-1a of the recipe's text.
[[nodiscard]] std::uint64_t recipe_key(const Recipe &recipe);
struct Capture {
    std::uint64_t key{};         // recipe_key of the recipe
    std::string recipe;          // recipe_text, for people. At most 95 characters
    std::string reskate;         // not in the key: a release that records the same keeps its clips
    std::uint64_t recorded_at{}; // seconds since 1970
    bool operator==(const Capture &) const = default;
};
struct Clip {
    std::uint8_t trick{};
    std::vector<ClipFrame> frames;
    Capture capture;
    std::uint32_t began{}; // the recording-clock time of the first frame. Not saved
};
// Sets each frame's timeline time from its part number. False if the trick has no catch or no landing.
bool retime(Clip &clip);
[[nodiscard]] Pose sample(const Clip &clip, float time);
// `amount` is 0 to 1. Poses of different shapes give `a`.
[[nodiscard]] Pose blend(const Pose &a, const Pose &b, float amount);
[[nodiscard]] float time_at(const Clip &clip, float milliseconds) noexcept;
[[nodiscard]] float ms_at(const Clip &clip, float time) noexcept;
[[nodiscard]] std::uint32_t duration(const Clip &clip) noexcept;

// decode_clip throws on invalid data and refuses another clip_version: record that trick again.
inline constexpr std::uint32_t clip_version = 1;
[[nodiscard]] std::vector<std::uint8_t> encode_clip(const Clip &clip);
[[nodiscard]] Clip decode_clip(std::span<const std::uint8_t> bytes);
// Reads only the first clip_header_bytes. Nothing for another clip_version.
inline constexpr std::size_t clip_header_bytes = 176;
[[nodiscard]] std::optional<Capture> clip_capture(std::span<const std::uint8_t> head);

// Parent-local joints as read from the game.
struct RigFrame {
    std::uint32_t at{};
    std::vector<Transform> joints;
};
struct RigJoints {
    std::uint16_t trajectory{}, deck{}, left_foot{}, right_foot{};
    std::span<const std::int32_t> parents;
};
[[nodiscard]] Transform world(const std::vector<Transform> &joints, std::span<const std::int32_t> parents, std::size_t joint);
[[nodiscard]] Transform compose(const Transform &parent, const Transform &child) noexcept;
[[nodiscard]] Transform inverse(const Transform &transform) noexcept;
// `anchor`, given on a board at `old_entity`, moved with the board to `entity`. It keeps its scale.
[[nodiscard]] Transform anchored(const Transform &entity, const Transform &old_entity, const Transform &anchor) noexcept;
// Cuts one jump from a skater's recording. `board` is a live board pose, its entity transform first.
std::optional<Clip> clip_from_capture(std::uint8_t trick, const std::vector<RigFrame> &frames, const RigJoints &joints,
                                      const std::vector<Transform> &board, const Transform &deck_to_board, std::string &why);
} // namespace dingosdk::style
