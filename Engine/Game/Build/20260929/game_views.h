#pragma once
#include "Engine/Game/Build/fingerprint.h"
#include <array>
#include <cstdint>

// The game's views, as it builds them each frame from its cameras (Extension/Rendering/view_offset.h): the earliest
// place the picture's camera is the picture's, upstream of everything the renderer works out from it.
//
// Each frame the game goes through its camera views (each with a local player's index at +0xbc: 0 the first), makes each
// one's transform from an entity its camera view names (0x3ebf3a0), and fills a view description in from it (fill_view):
// the description's first view (the render view, a 0x5c0-byte view whose camera is at +0x40) gets the transform, and the
// description keeps a second transform beside it (+0x1160). Right after each fill the replay's camera has its say
// (replay_view, which the game calls through a function it registers at start, 0x4bd4e0): while its replay camera is on
// (the replay editor, and the videos it exports), it sets the view's transform anew from what that camera works out each
// frame from the replay's own camera entity ("ReplayedGameCameraTransform") and its own state (0x4c72d0). The renderer
// then makes every view it draws from those descriptions (0x3f0bab0, then the world's render, cull and previous views:
// 0x50a0880). Found for Replay Shake on build 20260929 (the camera shake made of offsets applied here).
namespace dingosdk::game::build::v20260929::game_views {
// fill_view(description, id, settings, camera view, beside, view): fills a view description in (0x1240 bytes, the game's
// own list of the views it draws) from its view settings (field of view, near and far: set_fov and others) and the two
// transforms (Transform each): `beside` copied to the description's +0x1160, `view` set as the first view's transform
// last of all (set_transform). Called once a frame for each camera view the game draws, by its building of the views
// (0x3eb88f0) only, which keeps neither pointer.
inline constexpr Fingerprint fill_view{0x3ef19c0, {
    0x48,0x8b,0xc4,0x56,0x57,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0x08,0x01,0x00,0x00,
    0x48,0x89,0x58,0x18,0x48,0x8b,0xf9,0x48,0x8b,0x0d,0x12,0x41,0x4a,0x03,0x44,0x0f}};
// replay_view(description, context): the replay camera's say on a description just filled in. While the replay camera is
// on (its object's +0x28), the first view's transform anew (set_transform); otherwise it leaves it.
inline constexpr Fingerprint replay_view{0x4b9550, {
    0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x55,0x41,0x56,0x41,0x57,0x48,
    0x8d,0x6c,0x24,0xb0,0x48,0x81,0xec,0x50,0x01,0x00,0x00,0x48,0x83,0x3d,0xe5,0x8b}};
// set_transform(view, const Transform*): a view's transform (+0 to +0x90), and the view marked changed (+0x140) unless it
// holds that transform already. Only called, never hooked.
inline constexpr Fingerprint set_transform{0x3e745a0, {
    0xc5,0xf8,0x10,0x02,0xc5,0xf8,0xc2,0x09,0x00,0xc5,0xf8,0x50,0xc1,0x83,0xe0,0x07,
    0x3c,0x07,0x75,0x5e,0xc5,0xf8,0x10,0x42,0x10,0xc5,0xf8,0xc2,0x49,0x10,0x00,0xc5}};
// set_fov(view, float radians): a view's vertical field of view (+0x104), the horizontal one worked out from it and the
// view's aspect (+0x2b0, from +0x11c), and the view marked changed (+0x140). Only called, never hooked.
inline constexpr Fingerprint set_fov{0x3e743a0, {
    0x40,0x53,0x48,0x83,0xec,0x20,0xc5,0xfa,0x10,0x81,0x04,0x01,0x00,0x00,0xc5,0xf8,
    0x2e,0xc1,0xc5,0xf8,0x28,0xd1,0x48,0x8b,0xd9,0x7a,0x02,0x74,0x28,0xc5,0xfa,0x10}};

// A transform as the game hands it to a view (0x90 bytes, 0x3ebf3a0 makes it): the camera's world transform before its
// offset is added to its position (rows right, up, back, position), the same with the offset added (the one the renderer
// draws from: a view's camera, at its +0x40), and the offset. A view keeps it at its +0.
struct Transform {
    std::array<float, 16> before_offset{};
    std::array<float, 16> world{};
    std::array<float, 4> offset{};
};
static_assert(sizeof(Transform) == 0x90, "the game's layout");
// In a camera view (the game's): the index of the local player it is for.
inline constexpr std::uintptr_t camera_view_player = 0xbc;
// In a view description: its first view's transform (at +0: its camera, `world`, at +0x40), its vertical field of view
// (radians), and whether that view has a projection of its own (then its field of view is left alone).
inline constexpr std::uintptr_t description_transform = 0, description_camera = 0x40, description_fov = 0x104,
                                description_own_projection = 0x134;
} // namespace dingosdk::game::build::v20260929::game_views
