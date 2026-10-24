#pragma once
#include "Extension/Plugins/plugin_object_physics.h"
#include <optional>

namespace dingosdk::placement_physics {
std::string configure(std::uintptr_t base, std::string_view plugin, std::uint64_t id, const std::optional<plugins::ObjectPhysics>&);
std::string velocity(std::string_view plugin, std::uint64_t id, const std::array<float, 3>&);
std::string pose(std::string_view plugin, std::uint64_t id, const std::array<float, 3>&, const std::array<float, 4>&);
std::optional<plugins::ObjectMotion> sample(std::string_view plugin, std::uint64_t id);
std::optional<plugins::MarkerEvents> marker_events(std::uintptr_t base, std::string_view plugin);
void release(std::string_view plugin) noexcept;
void client_tick() noexcept;
void server_tick() noexcept;
void reset() noexcept;
}
