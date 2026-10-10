#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

// Saved performance settings for the menu's SETTINGS > PERFORMANCE tab: engine settings that help on
// weak hardware (dynamic resolution, shadows, post effects, cloth jobs), compiling cached shaders
// while loading, and the GI update throttle from engine_tweaks. Values are saved in the profile as
// "ReSkate.Perf.<key>".
//
// They are pushed to the engine on the game update thread, only when something can have changed:
// once the saved values are read, after a change in the menu, and when a level or sublevel becomes
// active (the engine can put its own values back while loading), with one follow-up check a few
// seconds later. Between those events tick() costs a couple of comparisons.
namespace dingosdk::graphics_tuning {
enum class Kind { toggle, slider };
// Which menu card an option sits in (SETTINGS > PERFORMANCE, in this order).
enum class Group { smoothness, lighting_shadows, image_effects, world, cpu };

struct Option {
    const char* key;   // saved as ReSkate.Perf.<key>
    const char* label;
    const char* hint;
    Group group;
    Kind kind;
    float stock;       // the game's own value: at stock the engine setting is left alone
    float initial;     // ReSkate's default, used until the player saves a choice (the game's own today)
    float low_spec;    // what the low-spec preset picks
    float minimum, maximum;
    const char* format;       // slider text (ImGui format)
    const char* stock_format; // slider text at the game's own value
};

std::span<const Option> options() noexcept;

// False until the saved values have been read (first game update tick). Any thread.
bool loaded() noexcept;
// A toggle is 1 (on, the game's look) or 0 (off). Any thread.
float value(std::size_t index) noexcept;
// Menu thread. The value applies on the next game tick and is saved about a second after the last
// change, in one write.
void set_value(std::size_t index, float value) noexcept;
void apply_low_spec() noexcept;
// Back to ReSkate's defaults (`initial`): the game's own values.
void restore_defaults() noexcept;
// True when every option is at its low-spec / default value.
bool is_low_spec() noexcept;
bool is_default() noexcept;

// Bootstrap only (Runtime/bootstrap.cpp): settings the engine reads while it starts, such as
// compiling cached shaders ahead, are also set from the game's startup script so they apply from
// launch. False (with the reason) if that hook cannot be registered; nothing else depends on it.
bool start_startup_settings(std::uintptr_t base, std::string& error) noexcept;

// Game update thread only (Runtime/client_tick.cpp), every tick, with the client state
// (Engine/Game/World/client_state.h). Cheap unless something is due.
void tick(unsigned client_state) noexcept;
} // namespace dingosdk::graphics_tuning
