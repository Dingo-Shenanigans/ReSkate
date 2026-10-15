#pragma once
// What the menus read about a style: names, targets and the model. No maths.
#include "Engine/Core/Console/command_registry.h"
#include "Engine/Core/Platform/path_text.h"
#include <array>
#include <bitset>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::style {
enum class Family : std::uint8_t { riding, grind, offboard, count };
inline constexpr std::uint32_t autosave_delay_ms = 750;
inline constexpr std::size_t family_count = static_cast<std::size_t>(Family::count);
inline constexpr std::array<std::string_view, family_count> family_names{"riding", "grind", "offboard"};

// The game's flip tricks in the game's numbering (FlipTrickType). The n* names are nollie tricks.
inline constexpr std::array<std::string_view, 33> flip_trick_names{
    "none", "ollie", "kickflip", "heelflip", "popshuvit", "varialkickflip", "inwardheelflip", "fspopshuvit",
    "varialheelflip", "hardflip", "360popshuvit", "360flip", "360inwardheelflip", "fs360popshuvit", "laserflip",
    "360hardflip", "nollie", "nkickflip", "nheelflip", "npopshuvit", "nhardflip", "nvarialheelflip", "nfspopshuvit",
    "nvarialkickflip", "ninwardheelflip", "nfs360popshuvit", "n360hardflip", "nlaserflip", "n360popshuvit", "n360flip",
    "n360inwardheelflip", "olliepop", "nolliepop"};
// A flip trick's timeline: 0 is the flick, 1 the catch, 2 the touchdown, 3 the end of the landing.
inline constexpr float trick_end = 3.0f;
inline constexpr std::size_t max_keys = 12;
// The target of a rotation: a state family, or one keyframe of one flip trick.
struct Target {
    bool trick{};
    std::uint8_t id{};
    std::uint8_t key{};
    auto operator<=>(const Target &) const = default;
};
// The place of `name` in `names`. ASCII case is ignored.
template <std::size_t N>
[[nodiscard]] std::optional<std::size_t> index_of(const std::array<std::string_view, N> &names, std::string_view name) noexcept {
    for (std::size_t i = 0; i < N; ++i)
        if (console::equal(names[i], name)) return i;
    return std::nullopt;
}
// The size of a table indexed by flip trick id: the 32 tricks and 0, none.
inline constexpr std::size_t flip_trick_count = flip_trick_names.size();
[[nodiscard]] constexpr bool is_flip_trick(std::int64_t id) noexcept { return id >= 1 && id < static_cast<std::int64_t>(flip_trick_count); }
[[nodiscard]] constexpr auto flip_trick_ids() noexcept {
    return std::views::iota(std::uint8_t{1}, static_cast<std::uint8_t>(flip_trick_count));
}
// ASCII case is ignored; "none" is not a trick.
[[nodiscard]] inline std::optional<std::uint8_t> parse_flip_trick(std::string_view name) noexcept {
    const auto id = index_of(flip_trick_names, name);
    if (!id || !is_flip_trick(static_cast<std::int64_t>(*id))) return std::nullopt;
    return static_cast<std::uint8_t>(*id);
}
[[nodiscard]] inline std::string sentence(std::string_view reason) {
    std::string text(reason);
    if (text.empty()) return text;
    if (text.front() >= 'a' && text.front() <= 'z') text.front() = static_cast<char>(text.front() - 'a' + 'A');
    if (text.back() != '.') text += '.';
    return text;
}
using TrickSet = std::bitset<flip_trick_count>;
inline std::optional<Target> parse_target(std::string_view name) noexcept {
    if (const auto family = index_of(family_names, name)) return Target{false, static_cast<std::uint8_t>(*family)};
    if (const auto id = parse_flip_trick(name)) return Target{true, *id};
    return std::nullopt;
}
// A preset's name is its file name, so it must be valid on every computer.
inline bool preset_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 40) return false;
    for (const char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    return !windows_device_name(name);
}

// The joints of Animation/Dingo/AnimBase_Default_Skeleton that a style can rotate.
inline constexpr std::array<std::string_view, 24> editable_joints{
    "Hips", "Spine", "Spine1", "Spine2", "Spine3", "Neck", "Neck1", "Head",
    "LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand", "RightShoulder", "RightArm", "RightForeArm", "RightHand",
    "LeftUpLeg", "LeftLeg", "LeftFoot", "LeftToeBase", "RightUpLeg", "RightLeg", "RightFoot", "RightToeBase"};
// ASCII case is ignored.
[[nodiscard]] inline std::optional<std::uint8_t> editable_joint(std::string_view name) noexcept {
    const auto joint = index_of(editable_joints, name);
    return joint ? std::optional<std::uint8_t>(static_cast<std::uint8_t>(*joint)) : std::nullopt;
}
inline constexpr float max_degrees = 120.0f;

// Pop, fall and landing in ms. Keyframes move at an even speed in real time, not in timeline parts.
using Pace = std::array<float, 3>;
inline constexpr Pace even_pace{500.0f, 500.0f, 500.0f};

// Flip trick id -> the timeline time of each of its keyframes.
using KeyTimes = std::map<std::uint8_t, std::vector<float>>;
// The blend out of each keyframe that has one: the ms from the keyframe back to the game's pose. Others blend to the next keyframe.
using BlendOuts = std::map<Target, float>;
inline constexpr float min_blend_out_ms = 50.0f, max_blend_out_ms = 2000.0f;
[[nodiscard]] inline std::vector<float> keys_of(const KeyTimes &times, std::uint8_t trick) {
    const auto found = times.find(trick);
    return found != times.end() ? found->second : std::vector<float>{};
}
enum class Part : std::uint8_t { pop, fall, landing };
inline constexpr std::array<std::string_view, 3> part_names{"pop", "fall", "landing"};
static_assert(part_names.size() == static_cast<std::size_t>(Part::landing) + 1);
[[nodiscard]] constexpr Part next_part(Part part) noexcept { return part == Part::pop ? Part::fall : Part::landing; }
[[nodiscard]] constexpr float part_start(Part part) noexcept { return static_cast<float>(part); }

// The editor's names: trick titles, capture limits and reasons, its console limits, its load, its playhead.
inline constexpr std::array<std::string_view, 33> flip_trick_titles{
    "None", "Ollie", "Kickflip", "Heelflip", "Pop Shuvit", "Varial Kickflip", "Inward Heelflip", "FS Pop Shuvit",
    "Varial Heelflip", "Hardflip", "360 Pop Shuvit", "360 Flip", "360 Inward Heelflip", "FS 360 Pop Shuvit", "Laserflip",
    "360 Hardflip", "Nollie", "Nollie Kickflip", "Nollie Heelflip", "Nollie Pop Shuvit", "Nollie Hardflip",
    "Nollie Varial Heelflip", "Nollie FS Pop Shuvit", "Nollie Varial Kickflip", "Nollie Inward Heelflip",
    "Nollie FS 360 Pop Shuvit", "Nollie 360 Hardflip", "Nollie Laserflip", "Nollie 360 Pop Shuvit", "Nollie 360 Flip",
    "Nollie 360 Inward Heelflip", "Quick Ollie", "Quick Nollie"};
// The editor captures tricks 1 to 30. The quick ollie and the quick nollie (31, 32) cannot be captured.
inline constexpr std::uint8_t last_capturable = 30;
[[nodiscard]] constexpr bool capturable(std::int64_t id) noexcept { return id >= 1 && id <= last_capturable; }
inline constexpr std::string_view not_capturable_text = "the quick ollie and the quick nollie cannot be captured";
inline constexpr std::string_view not_flip_trick_text = "that is not a flip trick";
inline constexpr std::string_view busy_text = "a capture or a recording is running";
inline constexpr std::string_view solo_recording_text = "record your tricks in solo play";
inline constexpr std::string_view full_server_text = "the style editor is not available on a server this full";
inline constexpr std::string_view older_clip_text =
    "this clip was recorded for an older game version; 'Record all my tricks' records it again";
inline constexpr std::string_view stage_closed_text = "the stage is closed";
[[nodiscard]] inline std::string with_stage_status(std::string what, std::string_view status) {
    if (status == stage_closed_text) return what;
    return status.starts_with(what) ? std::string(status) : what + ": " + std::string(status);
}
[[nodiscard]] inline std::uint8_t first_trick(const TrickSet &tricks) noexcept {
    for (const auto id : flip_trick_ids())
        if (tricks.test(id)) return id;
    return 0;
}
inline constexpr int max_step_frames = 600;
inline constexpr float min_speed = 0.1f, max_orbit_step = 20.0f;
// The load of the trick that the editor shows. A new trick starts a new load.
struct EditorLoad {
    enum class Phase : std::uint8_t { none, loading, failed, ready, unrecorded };
    std::uint8_t trick{};
    Phase phase{};
    std::string reason;
    bool operator==(const EditorLoad &) const = default;
};
// The stand-in's clip position on a flip trick's timeline, for the menu playhead.
struct Playhead {
    std::uint8_t trick{};
    float time{};
    bool editor{};  // the stand-in shows a clip of this trick
    bool playing{};
    bool wanted{};  // the editor screen is wanted, with or without a shown clip
};

struct StyleRotation {
    Target target;
    std::uint8_t joint{}; // index into editable_joints
    std::array<float, 3> degrees{};
    bool operator==(const StyleRotation &) const = default;
};
struct StyleModel {
    bool enabled{}, share{true}, saved{true};
    bool available{true};
    std::string unavailable_reason;
    bool auto_save{true}, unsaved{};  // unsaved: without auto save, edits that are not in the file
    std::string save_issue;           // also set when the preset could not be read
    std::string undo_name, redo_name;
    std::string preset;
    std::vector<std::string> presets;
    std::vector<StyleRotation> rotations;
    KeyTimes times;
    BlendOuts blend_outs;
    Pace pace{even_pace}; // the shown clip's
    TrickSet clips;
    TrickSet outdated_clips;
    TrickSet to_record;
    EditorLoad editor_load;
    bool server_too_full{}; // the server is too close to the editor stand-in's slot
    bool recording_tricks{};
    bool operator==(const StyleModel &) const = default;
};
} // namespace dingosdk::style
