#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>

// Skater effects other players see: the sparks, dust and puffs the game makes where a skater or
// their skateboard touches the world. Each is one contact; the game picks the effect from the
// surface's material and the speed.
namespace dingosdk::multiplayer {
struct Impact {
    std::array<float, 3> position{}; // where, in the world
    std::array<float, 3> velocity{}; // of the contact, metres a second; the game caps its length
    std::array<float, 3> normal{};   // of the surface
    std::uint16_t material{};        // the surface's index in the level's material table
    bool operator==(const Impact &) const = default;
};
inline constexpr std::size_t max_impacts = 3;        // in one packet: a frame or two of contacts
inline constexpr float max_impact_speed = 50;        // the game's own cap
inline constexpr std::uint16_t max_impact_material = 0x1fff;
inline constexpr std::size_t impact_wire_size = 12 + 6 + 3 + 2;
inline bool valid_impact(const Impact &impact) noexcept {
    float speed{}, facing{};
    for (unsigned i = 0; i < 3; ++i) {
        if (!std::isfinite(impact.position[i]) || std::abs(impact.position[i]) > 1000000.f ||
            !std::isfinite(impact.velocity[i]) || !std::isfinite(impact.normal[i]))
            return false;
        speed += impact.velocity[i] * impact.velocity[i];
        facing += impact.normal[i] * impact.normal[i];
    }
    // A little over the cap and the unit length: both are rounded on the way.
    return speed <= (max_impact_speed + 1) * (max_impact_speed + 1) && facing <= 1.1f &&
           impact.material <= max_impact_material;
}
inline bool valid_impacts(std::span<const Impact> impacts) noexcept {
    if (impacts.empty() || impacts.size() > max_impacts) return false;
    for (const auto &impact : impacts)
        if (!valid_impact(impact)) return false;
    return true;
}
// What a packet carries of one: the velocity to a centimetre a second, the normal in 127ths.
inline Impact wire_impact(Impact impact) noexcept {
    for (unsigned i = 0; i < 3; ++i) {
        impact.velocity[i] = std::round(std::clamp(impact.velocity[i], -max_impact_speed, max_impact_speed) * 100.f) / 100.f;
        impact.normal[i] = std::round(std::clamp(impact.normal[i], -1.f, 1.f) * 127.f) / 127.f;
    }
    return impact;
}
} // namespace dingosdk::multiplayer
