#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// A plugin's own lights (reskate.world.light): spot lights made through the game's own light renderer, as the game makes
// the spot lights of its levels (its PbrSpotLight entity's way: Engine/Game/Build/<build>/light_tables.h). The game lights
// the world with them as with its own lights: every surface, its shading and its shadows, in the replay editor's exported
// videos as on the screen. A camera's light that follows the camera, a lamp placed in a spot.
//
// Nothing else in the game knows of them (they are no entity's): they are taken away when the plugin clears them, when it
// stops, and when the level goes (a new renderer never had them).
namespace dingosdk::plugin_lights {
struct Light {
    // Where it is and which way it shines: rows right, up, back and position (as a camera's), and it shines along -back.
    std::array<float, 16> transform{};
    float lumens{12000};          // its brightness, in the renderer's units (the game's own lamps: 10,000 to 25,000,000)
    float range{10};              // metres it reaches
    float beam{70}, core{35};     // degrees across: its beam, and the bright core inside it
    std::array<float, 3> colour{1, 1, 1};
    bool shadows{true};           // it casts shadows
};
inline constexpr std::size_t most_each = 8;   // lights a plugin has at once
inline constexpr std::size_t most_lights = 32; // and all plugins together
// Game thread (the client tick). `owner`'s light `name` made, moved or changed as `light` says, or taken away (none).
// Empty, or why not.
std::string set(std::uintptr_t base, std::string_view owner, std::string_view name, std::optional<Light> light) noexcept;
// Every light of `owner`'s taken away (its plugin stopped). Game thread.
void clear(std::uintptr_t base, std::string_view owner) noexcept;
// Game thread, every tick: lights made in a renderer that is gone (a level loaded) are forgotten.
void tick(std::uintptr_t base) noexcept;
} // namespace dingosdk::plugin_lights
