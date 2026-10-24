#pragma once
// The game keeps much of its tuning outside Gameplay/SkatePhysicsTuning, in data-defined classes
// (push speeds, on-foot jumps, flips and rolls, dive and glide, bail speeds...). Their defaults
// and field order ship in the game's data (trainer_classes.inc); a live copy has no name to look
// up, so it is found by searching writable memory for the class's defaults laid out as the
// class lays them out. Game thread, except the search itself, which runs on its own thread.
#include <cstddef>
#include <cstdint>
#include <string>

namespace dingosdk::trainer {
struct ClassField {
    const char *name;
    float stock;
    std::uint16_t offset;
    // The game was seen reading it (hardware read watches on every copy) while a scripted skater
    // rode, pushed, ollied, flipped, ran, jumped and sprinted, or while a player ground, pumped,
    // climbed, vaulted and bailed; or its effect was measured. False: no read turned up in any
    // of that, and no other copy of the numbers was found in memory.
    bool seen;
};
struct ClassSpec {
    const char *group; // shown in the menu
    const char *key;   // in value ids: "<key>.<field>"
    std::uint16_t first, count, anchor;
};
#include "trainer_classes.inc"
inline constexpr std::size_t class_count = sizeof(class_specs) / sizeof(class_specs[0]);
inline constexpr std::size_t class_field_count = sizeof(class_fields) / sizeof(class_fields[0]);

// Starts a search for every class's live copies unless one is running. Copies found earlier
// that still hold what the trainer last wrote are kept.
void find_classes() noexcept;
bool finding_classes() noexcept;
// How long the running search has taken, in ms (0: none is running).
std::uint64_t class_search_ms() noexcept;
// How many searches have finished (0: none yet).
std::uint64_t class_searches() noexcept;
// A number that changes when a search finds something it did not know (or loses something).
std::uint64_t class_search_found() noexcept;
// Copies of class `index` found.
std::size_t class_copies(std::size_t index) noexcept;
// What field `field` (an index into class_fields) should hold. Written by apply_classes.
void want_class_value(std::size_t field, float value) noexcept;
// What field `<key>.<field>` is wanted to hold (its default while nothing else is wanted); 0 when there is no such field.
float class_value_wanted(const char *key, const char *field) noexcept;
// Whether anything is wanted that is not the game's own (a field, or the flip speed): until
// then there is nothing to write and no reason to search.
bool classes_wanted() noexcept;
// Writes the wanted values into every copy found; call each tick. Returns the fields written.
std::size_t apply_classes() noexcept;
// Call each tick (it acts once a second): writes again what the game has put back to its own
// numbers. True when something wanted has nowhere left to go, so a new search is due.
bool refresh_classes(std::uint64_t now) noexcept;
std::string classes_summary();
// Board flip trick speed. The game's flip trick tuning is a class of eight curves and no numbers
// (speed against the flick, against the ollie...); the search finds it as eight curve references
// in a row and apply_classes multiplies the outputs of its four speed curves by this.
void want_flip_speed(float factor) noexcept;
// The player has a flip speed of their own set, whether or not it is written right now (it is
// written only while the board is in the air): the curves must be found either way.
void flip_speed_in_use(bool used) noexcept;
// How many flip trick speed curves were found (0: the multiplier does nothing yet).
std::size_t flip_curves() noexcept;
// The game hurries a flip trick so that it is round 1/6 s before the landing it predicts, however
// slow the flip speed is set. `kept` false holds that rule off: the board turns as slowly as set
// and lands however far round it got. The search finds the rule's number; flip_gates says whether
// it did (0: nothing to switch yet).
// `seconds`: how long before the landing it predicts the game has a flip round. The game's own is
// flip_gate_stock; flip_gate_off never hurries a flip; anything else moves the catch.
inline constexpr float flip_gate_stock = 1.0f / 6.0f, flip_gate_off = -1000.0f;
void want_flip_gate(float seconds) noexcept;
// Something else needs the game states the search finds (the pump power): a reason to search.
void states_in_use(bool used) noexcept;
std::size_t flip_gates() noexcept;
// Where field `field` (an index into class_fields) lives: each copy's address and kind ('c' the
// class's own layout, 's' the eight-byte slots, 'n' native code's copy). Returns how many.
struct FieldCopy {
    std::uintptr_t address{};
    char kind{};
};
std::size_t class_field_copies(std::size_t field, FieldCopy *out, std::size_t capacity) noexcept;
} // namespace dingosdk::trainer
