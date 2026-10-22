#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// Models placed in the world by ReSkate: an object blueprint loaded on the map, created as a
// client-only entity and moved every frame (Engine/Game/Build/<build>/world_props.h).
namespace dingosdk::multiplayer {
// Other players' FPV drones. The drone's own model (made by ReSkate Studio from
// assets/models/fpv/drone.glb) when it is loaded; until then a stand-in prop of the game's.
inline constexpr std::string_view drone_model_blueprint = "reskate/drone/fpv_drone";
inline constexpr std::string_view drone_model_stand_in = "world/props/common/construction/cmn_prp_trafficcone_01/cmn_prp_trafficcone_01";

// The client update thread, every frame while in a level: shows a model at each other player's
// flying drone (drone_beacon.h), moves it, and takes it away when they land or leave. `show`
// false (other players' drones hidden) takes them all away.
void tick_drone_models(std::uintptr_t base, std::uintptr_t client, bool show) noexcept;
std::string drone_models_status();

// The test that found how (dronemodeltest): spawns a named blueprint a few metres in front of the
// local skater and moves it round a circle. Mode 0 the entity transform setter (what works),
// 1 the blueprint transform then the setter, 2 the blueprint transform alone. Empty name: stop.
inline constexpr std::string_view prop_test_default = drone_model_stand_in;
void request_prop_test(std::string blueprint, int mode);
// A pretend other player flying a drone in front of the local skater (dronemodeltest remote), to
// see their drone's model and marker without a second player. false lands it.
void request_fake_remote_drone(bool fly);
void tick_prop_test(std::uintptr_t base, std::uintptr_t client) noexcept;
std::string prop_test_status();
// Where the test's prop is now, while it runs (client update thread): drone_sound.cpp plays a
// motor there, so the sound can be heard without a second player.
std::optional<std::array<float, 3>> prop_test_position() noexcept;

// The engine's entity destruction hook (native_skater.cpp): a prop the engine destroys (a map
// change takes them all) is forgotten before it is freed. Any thread; cheap with none placed.
void prop_entity_destroyed(std::uintptr_t entity) noexcept;

// Client-owned world models. Handles are opaque bookkeeping: callers retain them
// between client ticks and never modify their fields. No native call runs from a
// destructor or render thread. The shared destruction hook invalidates each
// generation before the engine frees its entity, including on level changes.
struct ClientPropHandle {
    std::uintptr_t entity{}, parent{}, context{}, base{};
    std::uint64_t generation{};
};
// Optional private observation only. Coherence does not establish renderer use.
struct ClientPropTransformReadback {
    ClientPropHandle owner{};
    std::uintptr_t collection{};std::uint8_t count{},first{},extra{};
    std::array<float,16> requested{},observed{};
    bool attempted{},bytes_read{},owner_fenced{},collection_fenced{},matrix_unchanged{},matrix_finite{},matrix_valid{};
    const char* issue="not_requested";
};
struct ClientPropPlacement {
    std::string_view blueprint;
    std::array<float, 16> transform{};
    // Null keeps the existing path. Storage must outlive this synchronous batch.
    ClientPropTransformReadback* readback{};
};
struct ClientPropBatchResult {
    bool ready{};
    unsigned created{}, placed{}, live{};
    std::string issue;
};
// At most sixteen static-model object blueprints; no stand-ins. All missing
// blueprints must be loaded before creation starts. Placement uses the engine's
// normal entity setter, with the same verified local owner and native TLS realm.
ClientPropBatchResult update_client_props(std::uintptr_t base, std::uintptr_t client,
    std::uintptr_t expected_local_entity, std::span<ClientPropHandle> handles,
    std::span<const ClientPropPlacement> placements) noexcept;
// True once every handle is removed or the engine has already destroyed it.
// Wrong-thread/context calls retain live handles for a later client tick.
bool release_client_props(std::span<ClientPropHandle> handles) noexcept;
} // namespace dingosdk::multiplayer
