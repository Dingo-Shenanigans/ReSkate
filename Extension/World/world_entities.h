#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
// shown object is taken away until the joint comes back. `upright`: the object stands level at the
// joint, turned only about world up by the joint's heading, rather than leaning with the joint.
// `turn` false: the object follows the joint's position only; its matrix's rotation is the world's,
// and its position an offset in world axes. `ground` (metres, 0 none): the object's height is the
// floor's within that far below the joint, plus its matrix's height, when there is one (the object's
// own bodies are not floor); else the joint's, as without it.
struct Attach {
    std::uint64_t player{};
    std::uint16_t joint{};
    bool upright{};
    bool turn{true};
    float ground{};
};
// The client physics world, as the runtime gives it (Extension/Objects/ParkEditor/world_pick.cpp;
// Engine/Game/Build/20260929/physics_bodies.h): a body is {world, index | generation << 32}. A
// world object made not `solid` has its bodies ghosted (skaters go through them), found in its
// entity, a few levels down, as the world followed by a body the world says is valid.
using Body = std::array<std::uint64_t, 2>;
struct Physics {
    std::uintptr_t (*world)() noexcept{};
    bool (*valid)(const Body&) noexcept{};
    std::uint32_t (*owner)(const Body&) noexcept{};
    bool (*ghost)(const Body&, bool) noexcept{};
    // The floor's height under `from`, down to `depth` metres below it, ignoring `ignored`.
    std::optional<float> (*floor)(const std::array<float, 3>& from, float depth, const std::vector<Body>& ignored) noexcept{};
    // The body a ray from `from` to `to` meets first, and every body of that body's owner (empty
    // when it has more than `most`: the level's, or another shared owner's).
    std::optional<Body> (*hit)(const std::array<float, 3>& from, const std::array<float, 3>& to) noexcept{};
    std::vector<Body> (*owned)(const Body& body, std::size_t most) noexcept{};
    // Test only (objbody): one of the store's per-body setters by name, on a body.
    bool (*test)(std::string_view name, const Body& body) noexcept{};
};
// Test only: the next client tick runs the named per-body setter on every live object's bodies.
void test_bodies(std::string name);
void set_physics(const Physics& physics) noexcept;
// Where a skater's joint is in the world now, or empty (set by the runtime: the local skater's pose
// and the session's drawn poses). Called on the client thread.
using JointSource = std::optional<Matrix> (*)(std::uint64_t player, std::uint16_t joint) noexcept;
void set_joint_source(JointSource source) noexcept;

// An object of `owner` (a plugin's id). False and why when it cannot be kept.
std::string set(std::string_view owner, std::uint32_t id, std::string asset, const Matrix& matrix, bool hidden,
                std::optional<Attach> attach = std::nullopt, bool solid = true);
void remove(std::string_view owner, std::uint32_t id);
void clear(std::string_view owner);
Status status(std::string_view owner, std::uint32_t id);
// Every shown object of the owner exists in the world.
bool ready(std::string_view owner);
// The names of the world object blueprints the game has loaded now (what `set` can make), those
// starting with `prefix` (ASCII case ignored), sorted, at most `most`. Read from every asset
// domain's named-asset table; kept for two seconds.
std::vector<std::string> blueprints(std::uintptr_t base, std::string_view prefix, std::size_t most, bool meshes_only = false);
// A world object blueprint's mesh box in its own frame (min, max), when it has a mesh and the mesh
// is loaded; empty for one that shows nothing, or that is not loaded.
using Box = std::array<std::array<float, 3>, 2>;
std::optional<Box> blueprint_bounds(std::uintptr_t base, std::string_view blueprint);
std::size_t count(std::string_view owner);
// The native entities of every owner's objects in the world now (client thread).
std::vector<std::uintptr_t> live_entities();

// Runtime glue (client_tick.cpp, runtime_init.cpp, native_skater.cpp's destroy hook).
void tick(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept;
void before_level_transition(std::uintptr_t base, unsigned next) noexcept;
void entity_destroyed(std::uintptr_t entity) noexcept;

inline constexpr std::size_t max_per_owner = 512, max_total = 1024;
} // namespace dingosdk::world_entities
