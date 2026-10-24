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
// A plugin's camera can be in the cycle too, last (reskate.camera.cycle): high -> low -> first
// person -> the plugin's camera -> high (without first person in it, or where first person cannot
// start: high -> low -> the plugin's camera -> high). Its step keeps the low camera behind it as well.
enum class CycleStep : std::uint8_t { none, enter, leave };
struct CycleState {
    bool enabled{};      // the option is on and the local skater is in control
    bool first_person{}; // first person has the camera
    bool can_enter{};    // first person can take the camera now: on the board, no Freecam
    bool plugin_enabled{};   // a plugin's camera is in the cycle now
    bool plugin{};           // that camera is on
    bool plugin_can_enter{}; // that camera is off and can start
};
struct CycleWrite {
    bool high{};        // the height the game keeps
    CycleStep step{};   // what first person does
    CycleStep plugin{}; // what the plugin's camera does
};
constexpr CycleWrite cycle_height_write(const CycleState& state, bool high) noexcept {
    // The plugin's camera is last: out of it, the high camera.
    if (state.plugin_enabled && state.plugin) return {true, CycleStep::none, CycleStep::leave};
    if (state.enabled && state.first_person) {
        if (state.plugin_enabled && state.plugin_can_enter) return {false, CycleStep::leave, CycleStep::enter};
        return {true, CycleStep::leave, CycleStep::none};
    }
    if (high) { // the game steps low -> high: first person, else the plugin's camera, comes in between
        if (state.enabled && state.can_enter) return {false, CycleStep::enter, CycleStep::none};
        if (state.plugin_enabled && state.plugin_can_enter) return {false, CycleStep::none, CycleStep::enter};
    }
    return {high, CycleStep::none, CycleStep::none};
}

// The plugin's half of the link (the plugin runtime keeps it). Each tick the plugin in the cycle has
// its flags published; the height write (on the thread that handles the button) leaves the step it
// asks of the plugin's camera in `request`, taken on the plugin runtime's next tick.
enum PluginFlag : std::uint8_t { plugin_flag_enabled = 1, plugin_flag_on = 2, plugin_flag_can_enter = 4 };
struct PluginLink {
    std::atomic<std::uint8_t> flags{};
    std::atomic<std::uint8_t> request{};    // a CycleStep
    std::atomic<std::uint64_t> published{}; // GetTickCount64 when the flags were published
};
inline PluginLink plugin_link{};
}
