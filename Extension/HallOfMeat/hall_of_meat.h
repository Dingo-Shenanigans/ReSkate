#pragma once
#include "hall_of_meat_card.h"
#include "hall_of_meat_model.h"
#include "hall_of_meat_skeleton.h"
#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

// Hall of Meat, as in skate. 3: when the local skater bails, the bones it hurt show over the
// world, yellow where a hit bruised one and red where one broke it, with the bail's card, its
// time and Meat counting until the body comes to rest; both stay a second after the skater gets
// up, then fade out. A bone breaking flashes the screen's edges red and, in single player, slows
// the game down a moment (hall_of_meat_slow_motion.h).
//
// Each physics step of the local skater (hall_of_meat_skater.h) feeds the bail
// (hall_of_meat_model.h): what each body hit, and whether it is a ragdoll. The skeleton is skate.'s
// own skeleton mesh (hall_of_meat_skeleton.h) posed as the renderer draws the skater
// (hall_of_meat_render.h); the card (hall_of_meat_card.h) takes the place of skate.'s bottom left HUD
// while it shows (hall_of_meat_hud.h). The overlay draws both (hall_of_meat_overlay.h). Each map's best
// Meat is saved with the profile.
namespace dingosdk::hall_of_meat {
// Requires the validated build, No Bail started, and the local profile loaded: the switch starts
// from the saved choice, off until a player switches it on.
bool start(std::uintptr_t image_base) noexcept;
// Client thread, every tick: switches the renderer's and the UI model's hooks with the feature (off,
// none of them is called); a bail ends when the local skater is gone (a respawn, a teleport);
// logs each finished bail, saves a new best, hides skate.'s HUD corner while the card shows and
// runs the slow motion.
void on_client_tick() noexcept;
// Client thread, with the level being played (empty without one): loads that map's best when
// the map changes.
void set_level(std::string_view level) noexcept;
// Applies at once and saves the choice with the profile.
void set_enabled(bool enabled) noexcept;
// The switch, any thread: whether it started, and whether it is on. The menu reads it through the
// overlay's model (overlay::HallOfMeatModel), the console directly.
bool available() noexcept;
bool enabled() noexcept;

// skate.'s skeleton mesh posed as the renderer drew the skater, in world space, with that picture's
// camera, and how each body was hurt.
struct Skeleton {
    std::array<float, 16> camera{}; // world matrix: right, up, back, position rows
    float vertical_fov{};           // degrees
    std::vector<game::Vec3> positions, normals; // one per vertex
    std::shared_ptr<const SkeletonMesh> mesh;   // its triangles, and each vertex's body
    float alpha{}; // fades the whole skeleton out
    std::array<Injury, skater_body::count> injuries{};
    std::array<float, skater_body::count> flashes{}; // 1 the moment a body is hit, falling to 0
};
// What the overlay draws now; nothing (no skeleton, no card, no pulse) while riding.
struct Frame {
    Skeleton skeleton; // no positions while the renderer's picture is not there
    Card card;
    float break_pulse{}; // the screen's red edge when a bone breaks, 0 to 1
};
// Render thread, every presented frame.
Frame frame();
}
