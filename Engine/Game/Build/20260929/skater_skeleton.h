#pragma once
#include <cstddef>
#include <string_view>

namespace dingosdk::game::build::v20260929::skater_skeleton {
// Supported SHA-256 fbce74d5e28ef525dbba2cb4adbebc13405bdbd88f31bc940bca45e4ae88b8f9.
// The skater's skeleton as skate. itself has it, read from the installed game's data
// (Extension/HallOfMeat/hall_of_meat_skeleton.h): the player's render skeleton, and the Dem Bones
// costume, a full-body skeleton outfit skinned to it. Both were first used by gBGYo's ReSkate
// fork (github.com/gBGYo/ReSkate); read on 2026-10-06.
//
// The render skeleton: 386 bones (Hips 7, Neck1 102, Head 103, the hands with their fingers, the
// face, the board) and how they hang together.
inline constexpr std::string_view skeleton_toc = "Win32/levels/game/bam_levelroot/bam_levelroot.toc";
inline constexpr std::string_view skeleton_bundle = "win32/levels/game/bam_levelroot/bam_levelroot";
inline constexpr std::string_view skeleton_asset = "characters/maincharacters/common/animbase/animbase_default_renderskeleton";
// The costume's skinned mesh, in six levels of detail: 46,083 triangles at the finest, then
// 31,310, 22,031 (twice), 13,817 and 4,605.
inline constexpr std::string_view mesh_toc = "Win32/items.toc";
inline constexpr std::string_view mesh_bundle =
    "win32/characters/maincharacters/generic/cas/clothing/unlicensed/generic/apparel/fullbody/costume/dembones/2026/"
    "gen_costume_dembones_complex_dmpreset_cas_main_bundlereftable";
inline constexpr std::string_view mesh_asset =
    "characters/maincharacters/generic/cas/clothing/unlicensed/generic/apparel/fullbody/costume/dembones/2026/"
    "gen_costume_dembones_base_mesh";
// 13,817 triangles on 12,222 vertices: every bone's shape, few enough to pose and draw on the
// CPU every frame.
inline constexpr std::size_t mesh_lod = 4;
}
