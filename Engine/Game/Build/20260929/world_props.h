#pragma once
#include <cstdint>

// Props spawned by ReSkate itself (Extension/Multiplayer/Remote/native_prop.cpp): an object
// blueprint loaded on the map, created as a client-only entity the way the remote skateboard is
// (skater_entities.h, native_skater.h) and moved every frame by the plain entity transform setter
// (skater_entities::place_entity). Found in game on 2026-10-15 with the traffic cone
// (world/props/common/construction/cmn_prp_trafficcone_01/cmn_prp_trafficcone_01): the creation
// list made one entity, and only place_entity moved it smoothly (the blueprint transform did not).
namespace dingosdk::game::build::v20260929::world_props {
// An ObjectBlueprint's vtable (the cone's). The skater code's blueprint vtable is
// engine::blueprint_vtable; an object blueprint has its own.
inline constexpr std::uintptr_t object_blueprint_vtable = 0x638dfa8;
// The entity a static-model object blueprint creates on the client (the cone's).
inline constexpr std::uintptr_t static_model_entity_vtable = 0x6403720;
} // namespace dingosdk::game::build::v20260929::world_props
