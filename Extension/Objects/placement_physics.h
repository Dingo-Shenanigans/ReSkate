#pragma once
#include "Extension/Plugins/plugin_object_physics.h"
#include <optional>

// Physics on a plugin's own local placements (reskate.objects.physics; offline only): a claimed
// object's client replica falls, rolls and is knocked over, and its server copy follows it. A claimed
// object's moved pose is never saved to the park; released, it goes back to its layout pose.
namespace dingosdk::placement_physics {
std::string configure(std::uintptr_t base, std::string_view plugin, std::uint64_t id, const std::optional<plugins::ObjectPhysics>&);
std::string velocity(std::string_view plugin, std::uint64_t id, const std::array<float, 3>&);
std::string pose(std::string_view plugin, std::uint64_t id, const std::array<float, 3>&, const std::array<float, 4>&);
std::optional<plugins::ObjectMotion> sample(std::string_view plugin, std::uint64_t id);
std::optional<plugins::MarkerEvents> marker_events(std::uintptr_t base, std::string_view plugin);
std::optional<plugins::ObjectContact> contact(std::string_view plugin, const std::array<float,3>& from, const std::array<float,3>& to);
// Whether a plugin holds physics on the placement saved as `id` (its pose must not be saved).
bool claimed(std::uint64_t id) noexcept;
void release(std::string_view plugin) noexcept;
// The client tick (plugin_runtime.cpp) and the server park tick (local_placements_runtime.cpp).
void client_tick() noexcept;
void server_tick() noexcept;
// A level's teardown and the next world's start (runtime_init.cpp's level transition callback).
void before_level_transition(unsigned next) noexcept;
void reset() noexcept;
}
