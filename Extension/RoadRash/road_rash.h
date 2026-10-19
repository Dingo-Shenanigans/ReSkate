#pragma once
#include <cstdint>

// Road Rash: bails leave marks on the local skater that build up over a session. After ten bails
// the leg and then the arm of the side that took more of them show road dust and a graze, later
// real scrapes and bruises, and from twenty raw wounds on knee, shin, elbow, forearm and knuckles
// and a bruised face; the other side follows a few bails behind. A mark never gets smaller until
// the skater is healed (the console's `roadrash heal`) or the game is closed.
//
// The marks are not drawn by ReSkate: they are cosmetic items of a content mod, tattoo items for
// the limbs and moles items for the face (road_rash_model.h has their names), which this puts into
// the skater's recipe while it plays, through the game's own recipe copy (skater_items.h). Without
// that mod installed nothing ever shows. A slot that holds one of the player's own tattoos or moles
// keeps it, unless the player lets the wounds cover it; what was there comes back with healing.
// Nothing is saved to the profile's outfit: the game builds the skater from the profile again at
// every level change, and the marks are put back on from the session's count.
//
// The game feeds a skater's items either from the player's outfit or from a recipe of the skater's
// own, never both: one that wears a recipe handed to it gets nothing of what the player changes in
// the game's menus. So the marks are on only while the game is played. While a game menu is up,
// with nothing to show, on healing and when switched off, the skater is given back to the game,
// which puts the outfit's own items back and feeds it again (skater_items.h); the marks go back on,
// over the outfit as it is by then, a second after the menu has gone.
//
// Which side a bail hit comes from the ragdoll's own contacts, as Hall of Meat reads them
// (Extension/HallOfMeat/hall_of_meat_skater.h); a bail is the physics state selector choosing
// Wipeout (Extension/Skater/no_bail.h). Nothing is hooked for it.
namespace dingosdk::road_rash {
// Requires the validated build, No Bail started and the local profile loaded: the switches start
// from the saved choices (on, with blood, tattoos kept, until a player changes them).
bool start(std::uintptr_t image_base) noexcept;
// Client thread, every tick, with the local client.
void on_client_tick(std::uintptr_t client) noexcept;

// The switches, any thread; a change applies at the next tick and is saved with the profile.
bool available() noexcept;
bool enabled() noexcept;        // off: the marks come off and no bail is counted
bool blood() noexcept;          // off: the worst wounds are dry, with scabs
bool covers_tattoos() noexcept; // on: wounds take the place of the player's own tattoos and moles
void set_enabled(bool enabled) noexcept;
void set_blood(bool blood) noexcept;
void set_covers_tattoos(bool covers) noexcept;
// Any thread, done at the next tick: takes every mark off and starts counting again.
void heal() noexcept;
// Any thread, done at the next tick: counts as if the skater had bailed this often, half of it on
// each side, to look at a stage without slamming for it.
void set_bails(unsigned count) noexcept;
// How many bails are counted now.
unsigned bails() noexcept;
}
