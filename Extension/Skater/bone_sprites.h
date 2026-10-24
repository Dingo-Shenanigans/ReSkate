#pragma once
#include <array>
#include <cstdint>

// The Bone Cam's X-ray bones: one sprite per bone, cut from a public-domain skeleton illustration
// (Mikael Häggström / LadyofHats, Wikimedia Commons) into bones.png (its bake). Each
// sprite has two anchors in its own pixels:  goes on the bone's first joint and  points along
// the bone toward its second. Generated from the bake's bones.json; regenerate both together.
namespace dingosdk::bonecam {
enum class BoneSprite : std::uint8_t {
    torso, skull, humerus_l, humerus_r, forearm_l, forearm_r, hand_l, hand_r,
    femur_l, femur_r, shin_l, shin_r, foot_l, foot_r, count
};
struct BoneSpriteRect {
    const char *name;
    int x, y, w, h;               // in the atlas
    std::array<float, 2> a, b;    // anchors, sprite pixels
};
inline constexpr int bone_atlas_width = 2048, bone_atlas_height = 858;
inline constexpr float bone_metres_per_pixel = 0.000976088f;
inline constexpr std::array<BoneSpriteRect, static_cast<std::size_t>(BoneSprite::count)> bone_sprites{{
    {"torso", 6, 6, 595, 656, {247.6f, 587.0f}, {251.0f, -67.0f}},
    {"skull", 1711, 6, 166, 243, {83.0f, 223.9f}, {83.0f, 4.4f}},
    {"humerus_l", 1200, 6, 123, 330, {61.3f, 17.2f}, {61.3f, 312.1f}},
    {"humerus_r", 1329, 6, 154, 323, {76.5f, 17.0f}, {76.5f, 305.7f}},
    {"forearm_l", 1570, 6, 135, 246, {81.4f, 13.8f}, {81.4f, 229.2f}},
    {"forearm_r", 1489, 6, 75, 256, {39.6f, 14.2f}, {39.6f, 239.9f}},
    {"hand_l", 6, 668, 157, 184, {78.2f, 11.4f}, {78.2f, 172.2f}},
    {"hand_r", 1883, 6, 142, 191, {70.5f, 11.7f}, {70.5f, 179.1f}},
    {"femur_l", 785, 6, 169, 507, {84.5f, 23.2f}, {84.5f, 454.9f}},
    {"femur_r", 607, 6, 172, 513, {85.8f, 23.2f}, {85.8f, 454.5f}},
    {"shin_l", 960, 6, 117, 401, {56.2f, 20.1f}, {56.2f, 380.8f}},
    {"shin_r", 1083, 6, 111, 400, {57.2f, 20.0f}, {57.2f, 379.7f}},
    {"foot_l", 169, 668, 157, 150, {78.0f, 10.0f}, {78.0f, 139.5f}},
    {"foot_r", 332, 668, 153, 146, {76.2f, 9.9f}, {76.2f, 135.6f}},
}};
} // namespace dingosdk::bonecam