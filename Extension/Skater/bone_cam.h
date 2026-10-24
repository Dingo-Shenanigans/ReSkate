#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The Bone Cam: Skate 2's X-ray bail view. When the local skater wipes out (while a plugin's game
// asks for it, or on every bail if the player turns it on) the screen goes X-ray for a few seconds:
// every bone is drawn between the skater's real joints, a joint that stops dead cracks or breaks
// the bone beside it, and the injuries are listed. It never changes game speed: the game's own
// time scale steps rather than slows, and only a console command may change it.
namespace dingosdk::bonecam {
// One bone of the snapshot, drawn between its real joints; those that took a hard hit in red.
struct BoneCamBone {
    std::uint8_t sprite{};         // BoneSprite (bone_sprites.h)
    std::array<float, 3> a{}, b{}; // world: the bone's first joint, and a point along it
    std::uint8_t hurt{};           // 0 sound, 1 cracked, 2 broken
};
struct BoneCamInjury {
    std::string bone, what; // "LEFT FEMUR", "FRACTURED"
    bool severe{};
};
// What a slam leaves behind: marks on the ground where the body slid (a skid for the torso, thin
// scrapes for hands, knees and head), and the hit itself.
struct BoneCamMark {
    std::vector<std::array<float, 3>> path; // world, along the ground
    float alpha{};                          // 0..1, fading with age
    bool skid{};
};
// The skater's pose for a 3D X-ray, in world space. Sides are [0] right, [1] left. `valid` once a
// pose has been read this bail.
struct BoneCamPose {
    using Point = std::array<float, 3>;
    bool valid{};
    std::vector<Point> spine; // the joints from the hips up to the head, in order
    Point head{}, head_up{};  // the head joint and its up axis (unit)
    std::array<Point, 2> clavicle{}, shoulder{}, elbow{}, wrist{}, fingers{}, hip{}, knee{}, ankle{}, toe{};
    // Injuries by region (BoneSprite order): 0 sound, 1 cracked, 2 broken.
    std::vector<std::uint8_t> hurt;
};
struct BoneCam {
    bool active{};          // the X-ray itself
    BoneCamPose pose;
    bool effects{};         // marks or a hit to draw (with or without the X-ray)
    float fade{};           // 0..1: fades in when the bail starts, out at the end
    std::vector<BoneCamMark> marks;
    float hit{};                   // 0..1: the flash of an impact, fading fast
    float daze{};                  // 0..1: a concussion's after-effects (kept faint), fading
    std::array<float, 3> hit_at{}; // world: where it landed
    bool preview{};         // the menu's preview: the injuries are examples
    std::uint64_t serial{}; // one per bail
    std::vector<BoneCamBone> bones;
    std::vector<BoneCamInjury> injuries;
    std::array<float, 3> left{};    // world: from the skater's right shoulder toward the left one (which way the art faces)
    std::array<float, 16> camera{}; // world matrix: right, up, back, position rows
    float vertical_fov{};
};

// Game thread, every client tick after the trainer's (it reads the bail count the trainer watches).
void tick_bone_cam(std::uintptr_t base, std::uintptr_t client, bool playing) noexcept;
// `bonecam [game|on|off|test]`, `bonecam effects on|off`, `bonecam ring on|off` (game thread).
std::string bone_cam_command(const std::vector<std::string> &arguments);
std::string bone_cam_setting(); // "game", "on" or "off"
bool bone_cam_ringing() noexcept; // a concussion's ringing is on (not muted)
// The local skater just bailed (a plugin that watches the wipeout states says so; any thread).
void bone_cam_bail() noexcept;
// The "game" setting shows the Bone Cam while any owner (a plugin's game) asks for it; its slams
// and strikes are followed then even with the Bone Cam off. An owner's ask goes with release.
void bone_cam_game(std::string_view owner, bool on);
void bone_cam_release(std::string_view owner);
// How many times the skater's body has stopped dead (lost 7 m/s within a third of a second): the
// ground, a pole, a wall. A count, so each reader keeps its own place (a reader that cleared a flag
// hid the slam from every other). Only followed while the Bone Cam would show or a game asks.
std::uint64_t bone_cam_slams() noexcept;
// How many times the skater's head, chest, shoulders or hips struck an object at speed (a pole, a
// tree, a rail: nearly stopped while the body was still flying). Followed as bone_cam_slams is.
std::uint64_t bone_cam_strikes() noexcept;
// The X-ray's snapshot, with the latest camera (any thread).
BoneCam bone_cam();
// Fresh native hips position while the normal on-board telemetry is absent.
// Read-only; independent of whether Bone Cam visuals/effects are enabled.
std::optional<std::array<float,3>> bail_body_position() noexcept;
} // namespace dingosdk::bonecam
