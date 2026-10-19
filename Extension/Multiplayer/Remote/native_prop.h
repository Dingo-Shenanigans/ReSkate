#pragma once
#include <array>
#include <cstdint>
#include <optional>
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
} // namespace dingosdk::multiplayer
