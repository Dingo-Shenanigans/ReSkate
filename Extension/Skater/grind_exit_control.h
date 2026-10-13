#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Grind exit control: pick the side you pop out of a grind to with LT / RT, independently of the
// trick and of the body's rotation (Skate's own controls tie both to the left stick).
//
// While on:
//   - A trigger pressed DURING a grind, or up to `prewindow` seconds (1) before the grind
//     connects, is "claimed": it picks the side of the next pop out of the grind. The player
//     chooses what LT / RT mean:
//       toe/heel     LT the toe side, RT the heel side, worked out from which foot leads (the
//                    game's stance flag, Engine/Game/Build/20260929/grind_exit.h, and the player's
//                    stance setting) and the skater's facing. On slides, where toe and heel point
//                    along the rail, and while the stance is not set: left/right of travel.
//       left/right   LT the left of the direction of travel, RT the right.
//     Both claimed: the game's own exit.
//   - At the pop, only the sideways part of the game's launch is replaced: the game's own
//     sideways speed when it had one, mirrored to the chosen side, else `sidespeed` (1.8 m/s, the
//     game's own). Along the rail, the pop's height, the trick, the flip and the body's rotation
//     are the game's.
//   - Pop-out strength (10-100%, kept): scales the sideways speed of every trigger-set exit, for
//     rails next to a wall. 100% is the behaviour above, the game's own full exit.
//   - Grab guard: a claimed trigger is hidden from the game until it is let go, so holding it
//     through the pop does not start a grab; press it again in the air to grab.
//   - Lean guard: while a claimed trigger is held in a grind, the left stick's left/right is
//     hidden from the game until the pop, so the game's own lean exit cannot start and pull the
//     board the other way; from the pop on the stick is the game's again, for the body's rotation.
//   - Multiplayer: on in any session, whatever the host's settings. It changes only how the local
//     player's inputs pick the exit side (the speed is the game's own); other players see the
//     result through the poses ReSkate already syncs.
// It is limited to the supported game build: the launch hook is the trainer's, which installs
// only when the game's code matches (Extension/Trainer/trainer_jump.cpp), and every address
// comes from Engine/Game/Build/20260929. With debug logging on, every
// pop out of a grind is written to ReSkate.log with
// what was decided and why. On/off, the side convention and the stance are kept in
// %LOCALAPPDATA%\ReSkate\grindexit\control.txt.
namespace dingosdk::grind_exit_control {
// Requires the validated build and No Bail started. Switches itself on when it was left on.
bool start(std::uintptr_t image_base) noexcept;
// Client thread, every tick, with the local client: keeps the launch hook ready, samples the
// skater's facing, writes the physics thread's decisions to the log.
void on_client_tick(std::uintptr_t client) noexcept;
// The console verbs: exits on|off|status, stance goofy|regular, sides toeheel|travel, strength <10-100>,
// guard on|off, leanguard on|off, prewindow <seconds>, sidespeed <m/s>.
std::string command(const std::vector<std::string>& words);

// For the menus, any thread.
bool available() noexcept;
bool enabled() noexcept;
bool toe_heel() noexcept;           // else left/right of travel
int stance() noexcept;              // 0 not set, 1 regular, 2 goofy
int strength() noexcept;            // pop-out strength, percent of the sideways speed (10-100; 100 the full exit)
}
