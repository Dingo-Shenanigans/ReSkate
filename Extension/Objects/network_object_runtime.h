#pragma once
#include "Engine/Game/World/network_objects.h"
#include "object_placements.h"
#include <array>
#include <optional>
#include <span>
#include <string>

namespace dingosdk {
std::optional<NetworkObjectSnapshot> capture_local_network_objects();
void set_lobby_object_guest(bool guest);
// Host-requested wipe of this guest's session objects. The guest's own save is
// swapped out while in a lobby and is never touched. False means busy (an edit
// or delete is in flight); call again later.
bool clear_lobby_guest_objects();
void set_remote_network_objects(std::string_view map, std::span<const NetworkObjectOwner> owners);
void clear_remote_network_objects();
void remove_remote_network_objects(std::uint64_t owner, std::uint64_t epoch);
std::string network_object_status();
// The style stage's objects on `map`. They are made, moved and removed like other players' objects, and never
// saved. Each call moves them again, even where nothing changed. An empty list removes them. False when the map is
// not the one being placed on.
bool set_stage_objects(std::string_view map, std::span<const profile::PlacedObject> objects);
// The stage object `id` as the game has it, once it exists.
std::optional<profile::PlacedObject> stage_object(std::uint64_t id);
// Where objects are placed now. `world` changes each time the world's objects are reset (a new map, or the same map
// loaded again), which removes the stage's objects.
struct StagePlace {
    std::string map;
    std::uint64_t world{};
    bool operator==(const StagePlace &) const = default;
};
// Where objects are placed now, or nothing when they cannot be placed now (a load, or a failed save).
std::optional<StagePlace> stage_map();
// Client update thread, every frame: queues the next of other players' objects
// while some are still to be created (a no-op otherwise).
void tick_network_objects() noexcept;
} // namespace dingosdk
namespace dingosdk::profile_runtime {
void reset_network_object_world();
// Retire a local entity without deleting anything from its saved layout.
void retire_local_network_object(std::uint64_t entity, const profile::PlacedObject& object);
void update_network_objects();
void update_network_object_moves();
bool network_objects_inflight();
// Private correlation IDs stay in our local queue and callback scope. Never put
// them in the native source enum or use pose matching to infer ownership.
inline constexpr std::uint32_t network_object_source_mask = 0xff000000;
inline constexpr std::uint32_t network_object_source_prefix = 0xd1000000;
inline bool is_network_object_source(std::uint32_t source) {
    return (source & network_object_source_mask) == network_object_source_prefix;
}
// ReSkate/Steam owner for correlation only, never a native simulation owner.
std::uint64_t network_object_creation_owner(std::uint32_t source);
bool network_object_observe(std::uint64_t entity, const profile::PlacedObject &object,
                            std::uint32_t source = 0);
bool network_object_native_ready();
std::uint32_t network_object_extra_budget() noexcept;
void move_network_object(std::uint64_t entity, const profile::PlacedObject &object);
} // namespace dingosdk::profile_runtime
