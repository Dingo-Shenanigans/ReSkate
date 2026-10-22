#pragma once
#include <atomic>
#include <cstdint>

namespace dingosdk::first_person {
// "First person in the camera cycle". With the game's Camera height control on, its camera
// height button (R3) writes the profile option UseHighCam as the opposite of the height it reads
// (true is the high camera). With the option on, ReSkate turns that into high -> low -> first
// person -> high by changing the value written: the step into first person keeps the low camera
// behind it, and the step out of first person always lands on the high camera.
//
// The Filmer (Extension/Filmer) can be in the cycle too, last: high -> low -> first person -> the
// Filmer -> high (without first person in it, or where first person cannot start: high -> low -> the
// Filmer -> high). Its step keeps the low camera behind it as well.
enum class CycleStep : std::uint8_t { none, enter, leave };
struct CycleState {
    bool enabled{};      // the option is on and the local skater is in control
    bool first_person{}; // first person has the camera
    bool can_enter{};    // first person can take the camera now: on the board, no Freecam
    bool filmer_enabled{};   // the Filmer's option is on and it can run (the local skater in control)
    bool filmer{};           // the Filmer (the AI filmer) has the camera
    bool filmer_can_enter{}; // the Filmer is off and can start
};
struct CycleWrite {
    bool high{};        // the height the game keeps
    CycleStep step{};   // what first person does
    CycleStep filmer{}; // what the Filmer does
};
constexpr CycleWrite cycle_height_write(const CycleState& state, bool high) noexcept {
    // The Filmer is last: out of it, the high camera.
    if (state.filmer_enabled && state.filmer) return {true, CycleStep::none, CycleStep::leave};
    if (state.enabled && state.first_person) {
        if (state.filmer_enabled && state.filmer_can_enter) return {false, CycleStep::leave, CycleStep::enter};
        return {true, CycleStep::leave, CycleStep::none};
    }
    if (high) { // the game steps low -> high: first person, else the Filmer, comes in between
        if (state.enabled && state.can_enter) return {false, CycleStep::enter, CycleStep::none};
        if (state.filmer_enabled && state.filmer_can_enter) return {false, CycleStep::none, CycleStep::enter};
    }
    return {high, CycleStep::none, CycleStep::none};
}

// The Filmer's half of the link (it runs in its own module). Each tick it publishes its flags; the
// height write (on the thread that handles the button) leaves the step it asks of the Filmer in
// `request`, taken on the Filmer's next tick.
enum FilmerFlag : std::uint8_t { filmer_flag_enabled = 1, filmer_flag_on = 2, filmer_flag_can_enter = 4 };
struct FilmerLink {
    std::atomic<std::uint8_t> flags{};
    std::atomic<std::uint8_t> request{}; // a CycleStep
    std::atomic<std::uint64_t> published{}; // GetTickCount64 when the flags were published
};
inline FilmerLink filmer_link{};
}
