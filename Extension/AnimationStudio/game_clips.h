#pragma once
#include "anim_core.h"
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The game's own animation clips, read out of its data as Clips: a ClipControllerAsset on the skater's
// AnimBase_Default_Rig whose animation is Eclipse coded, as the skater's trick clips are
// ("animation/dingo/c_proto_onb_trick_o_kickflip_high"). Ported from ReSkate Studio's animation export
// (modding/models/native_animation.cpp, rig_metadata.cpp, sdk/resources/eclipse_animation.cpp).
//
// Each joint's rotation, position and scale are the clip's channels where it has them, else the rig's
// default pose; the clip's frames are its own (60 a second for the tricks). The board is Skateboard_Root,
// relative to the placement joint, as a recording keeps it. The takeoff and landing are found from the
// board's height, as make_trick_library.py did: the game's trick clips stay in place.
namespace dingosdk::anim_studio::game_clips {
class Library {
public:
    // Reads the level root's and the game configuration's manifests and the skater's rig (about a
    // quarter of a second). Throws std::runtime_error with why.
    explicit Library(const std::filesystem::path& game_root);
    ~Library();
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;
    // The animation assets' names (lower case, "animation/..."), in the level root's order, containing
    // `containing` (all when empty), at most `most`. Not every one is a clip: load says.
    std::vector<std::string> names(std::string_view containing, std::size_t most) const;
    // The clip, or std::runtime_error with why (not one, another rig, another codec).
    Clip load(std::string_view name) const;
    // A joint's name in the skater's skeleton ("LeftFoot"), or empty.
    std::string_view joint_name(std::uint16_t joint) const;
    // The stance a clip (any of the skater's: a game clip or a recording) stands in at its first frame:
    // goofy when the right foot is nearer the board's front truck than the left. Empty when it cannot
    // be told (no board under the feet).
    std::optional<bool> goofy(const Clip& clip) const;
    // The clip mirrored for the other stance: left and right swapped (Left* and Right* joints trade
    // places), reflected across the vertical plane along the board's length, so the board keeps its
    // nose ahead. Each joint keeps its own axes: what turns is how far it is from its rest pose
    // (the rig's default pose), mirrored, from its counterpart's rest. Throws without a board.
    Clip mirror(const Clip& clip) const;

private:
    struct Data;
    std::unique_ptr<Data> data_;
};

// The game's library, read once in the background from the first call (prepare starts it): nullptr
// until it is ready, and failure() says why when it could not be read.
void prepare() noexcept;
std::shared_ptr<const Library> library() noexcept;
std::string failure();
}
