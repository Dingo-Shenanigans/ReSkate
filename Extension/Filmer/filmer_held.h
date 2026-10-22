#pragma once
// The camera in a filmer's hand, in the game: your arm holding it out while you film (filmer_hold.h
// has the maths). The camcorder itself is a cosmetic worn in the hand (filmer_outfit.h).
//
// Your arm: as your skater's pose is handed to the renderer, its upper arm and forearm are bent so the hand holds
// the camera where the Filmer's lens is (out of reach, as near as the arm goes). Your pose goes to the
// others with the arm in it, so they see it held out. You never see your own camera while you film
// (you look through it).
#include "filmer_hold.h"
#include <cstdint>
#include <optional>
#include <string>

namespace dingosdk::filmer::held {
struct Request {
    bool hold{};             // the local arm holds the camera out this frame
    hold::Hand hand = hold::Hand::right;
    // Where the lens should be: in the world, or (relative) metres ahead of the shoulder along
    // `facing` and up from it.
    bool relative{};
    hold::Vec3 lens{};
    hold::Vec3 facing{0, 0, 1};
    hold::Quat aim{0, 0, 0, 1};
    bool worn{};             // the camcorder is worn as hand wear: the hand turns to aim it
    float crouch{};          // how far the hips go down while the camera is held (metres; 0: standing)
    float hunch{};           // how far the back curls over toward the camera (radians), the head looking down at it
    bool two_hands{};        // the other hand holds the camera too
    bool hide_own{};         // you look through the camera: your camera arm is not drawn on your own screen
    bool still{};            // on foot: the back, neck and head from a still standing pose (no idle fidgets)
    bool hide_board{};       // on foot: the board out of sight (yours and as the others see it)
    bool on_board{};         // riding: the knees straighten while a foot is down pushing
    float pivot{};           // on foot: the whole body turned on the spot after the camera (radians)
    float step{}, step_phase{}; // turning round on foot: how much the feet step (0..1), and where in the stride
};
// Game thread, every Filmer tick.
void set_local(const Request &request) noexcept;

// Client thread, every frame while in a level (`in_level`) and after the Filmer's tick: hooks the
// animation once.
void tick(std::uintptr_t base, std::uintptr_t client, bool in_level) noexcept;
// Which way your body faces (yaw, radians, as the Filmer's rig counts it), from your hips as the game
// last posed them, while that is fresh. Any thread.
std::optional<float> body_yaw() noexcept;
// One line for `filmer status` and the menu.
std::string status();
} // namespace dingosdk::filmer::held
