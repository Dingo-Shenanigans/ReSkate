#pragma once
#include "Engine/Core/Json/json.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The Animator Studio's trick library, built on the player's own PC from their own copy of the game: every clip of
// the skater's animation (animation/dingo) is read out of the game's bundles, decoded and written as a studio clip,
// with the catalog TRICKS lists and the index of the game's animation tree. Nothing of the game's ships with the mod.
//
// A port of contrib/animator-studio's direct decoder - decode_native_tree.py --direct, native_pose.py,
// native_piecewise.py and make_trick_library.py - reading the bundles with Engine/Vfs instead of the probe's dumps.
// It writes the same files, named the same way, so a library built here and one built by those scripts agree.
namespace dingosdk::anim_studio::native_library {
inline constexpr int joint_count = 395;
inline constexpr int board_joint_index = 380; // Skateboard_Root

// One joint of one pose: scale (3), rotation (quaternion x y z w), position (3), as the studio's clips keep them.
using Joint10 = std::array<float, 10>;
struct Decoded {
    std::vector<float> times;    // seconds, 60 a second from 0
    std::vector<Joint10> frames; // times.size() * joint_count, frame by frame
    bool board{};                // the clip has the board's channels
};
// A clip the decoder does not take: an unmeasured encoding, rig or timing. Anything else going wrong is an error.
struct Unsupported : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ---- decoding (on their own, to be tested) ----
struct QuaternionTrack {
    std::vector<double> frames;
    std::vector<std::array<double, 4>> values;
};
struct VectorTrack {
    std::vector<double> frames;
    std::vector<std::array<double, 3>> values;
};
// A PIEC resource's rotation tracks (quaternion codecs 1 and 2) and its position / scale tracks (vector codec 1), by
// channel name, as native_piecewise.py reads them.
std::vector<std::pair<std::string, QuaternionTrack>> decode_quaternions(std::span<const std::byte> blob, const std::vector<std::string>& channels);
std::vector<std::pair<std::string, VectorTrack>> decode_vectors(std::span<const std::byte> blob, const std::vector<std::string>& channels);
// The skater's pose every tick from 0 to `end_frame` (60 a second): the rig's default pose with the tracks laid over it.
Decoded decode_pose(std::span<const std::byte> blob, const std::vector<std::string>& channels, const std::vector<Joint10>& defaults,
                    const std::vector<std::string>& names, std::int64_t end_frame);
// The studio's clip file ("FAS3"): joints that never move kept once, the rest and the board every frame.
std::vector<std::byte> encode_clip(const Decoded& clip, float takeoff, float landing);
// A trick's pop and landing, in seconds: the studio's own rule (anim_core's place_native_marks), so what the file says
// is what the studio uses - the pop 0.05 s after the board tips past 6 degrees (where the game leaves the ground), the
// landing the board's deepest low 0.25 to 0.85 s after. A board that never tips falls back to make_trick_library.py's
// first low of its height.
std::pair<float, float> trick_marks(const Decoded& clip);
// The folder an asset is shown in (decode_native_tree.py's category, index_native_assets.py's classify).
std::string category(const std::string& asset, const std::string& root_type);

// ---- building ----
struct Report {
    std::size_t assets{}, clips{}, tricks{}, unsupported{}, errors{};
    std::string first_error;
};
// Told every so often what is being done: `done` of `total` in the step named. Returning false stops the build (it
// throws Stopped).
using Progress = std::function<bool(std::string_view step, std::size_t done, std::size_t total)>;
struct Stopped {};
// A file of the library that could not be written (a full disk, a locked file): the whole build fails, so a library with
// files missing never takes the place of a good one. (A clip that cannot be decoded is one error, and the build goes on.)
struct WriteFailed : std::runtime_error {
    using std::runtime_error::runtime_error;
};
// Builds a whole library into `output` (made if it is not there; meant to be an empty staging folder): the clips,
// catalog.json, native-catalog.json, the native resources' details and build-info.json (with `stamp` in it).
Report build(const std::filesystem::path& game_root, const std::filesystem::path& output, const Progress& progress, const std::string& stamp);
// One asset's typed document as the studio's inspector shows it (the shape contrib's probe writes), read from the
// game: `toc` and `bundle` as the native catalog's row has them.
Json document_from_game(const std::filesystem::path& game_root, const std::string& toc, const std::string& bundle, const std::string& asset);
} // namespace dingosdk::anim_studio::native_library
