#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Native world objects that ReSkate keeps for its plugins: one entity of a world object blueprint
// (a prop, a gate's arch or bolt, a traffic cone) at a full 4x4 transform, as SmokeyDawg58's fork
// spawned its gates, arrows, cones and beacon. Owners say what they want; the client
// tick creates, moves and destroys the native entities in the verified creation scope, a few each
// frame. A level change takes every object away: owners place theirs again in the next world.
namespace dingosdk::world_entities {
// Rows of a 4x4 world transform: right, up and forward axes (with their scale), then position.
using Matrix = std::array<float, 16>;

enum class Status { none, waiting, loading, missing, ready, hidden };

// An object kept on a joint of a skater (of the 395 the animation uses): `player` 0 for the local
// skater, else another player in the session. Its matrix is then in the joint's frame, and it moves
// with the joint every frame; while the joint is not known it waits, and after a second without it a
// shown object is taken away until the joint comes back.
struct Attach {
    std::uint64_t player{};
    std::uint16_t joint{};
};
// Where a skater's joint is in the world now, or empty (set by the runtime: the local skater's pose
// and the session's drawn poses). Called on the client thread.
using JointSource = std::optional<Matrix> (*)(std::uint64_t player, std::uint16_t joint) noexcept;
void set_joint_source(JointSource source) noexcept;

// An object of `owner` (a plugin's id). False and why when it cannot be kept.
std::string set(std::string_view owner, std::uint32_t id, std::string asset, const Matrix& matrix, bool hidden,
                std::optional<Attach> attach = std::nullopt);
void remove(std::string_view owner, std::uint32_t id);
void clear(std::string_view owner);
Status status(std::string_view owner, std::uint32_t id);
// Every shown object of the owner exists in the world.
bool ready(std::string_view owner);
std::size_t count(std::string_view owner);

// Runtime glue (client_tick.cpp, runtime_init.cpp, native_skater.cpp's destroy hook).
void tick(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept;
void before_level_transition(std::uintptr_t base, unsigned next) noexcept;
void entity_destroyed(std::uintptr_t entity) noexcept;

inline constexpr std::size_t max_per_owner = 512, max_total = 1024;
} // namespace dingosdk::world_entities
