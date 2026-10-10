#pragma once
#include <cstdint>

namespace dingosdk::first_person {
// "First person in the camera cycle". With the game's Camera height control on, its camera
// height button (R3) writes the profile option UseHighCam as the opposite of the height it reads
// (true is the high camera). With the option on, ReSkate turns that into high -> low -> first
// person -> high by changing the value written: the step into first person keeps the low camera
// behind it, and the step out of first person always lands on the high camera.
enum class CycleStep : std::uint8_t { none, enter, leave };
struct CycleState {
    bool enabled{};      // the option is on and the local skater is in control
    bool first_person{}; // first person has the camera
    bool can_enter{};    // first person can take the camera now: on the board, no Freecam
};
struct CycleWrite {
    bool high{};      // the height the game keeps
    CycleStep step{}; // what first person does
};
constexpr CycleWrite cycle_height_write(const CycleState& state, bool high) noexcept {
    if (!state.enabled) return {high, CycleStep::none};
    if (state.first_person) return {true, CycleStep::leave};
    if (high && state.can_enter) return {false, CycleStep::enter};
    return {high, CycleStep::none};
}
}
