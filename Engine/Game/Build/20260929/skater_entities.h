#pragma once
#include <array>
#include <cstdint>

// Skater and skateboard entity functions and types shared by the AI skaters,
// the remote multiplayer actors and the local client source spawn.
namespace dingosdk::game::build::v20260929::skater_entities {
// Blueprint asset TypeInfo. Named character blueprints derive from it.
inline constexpr std::uintptr_t blueprint_type = 0x7586ce8;
// ObjectBlueprint ClassInfo: a world object's blueprint (props, checkpoints), a different branch
// of the type hierarchy from the skater Blueprint above.
inline constexpr std::uintptr_t object_blueprint_type = 0x75043a8;
// A world object blueprint's mesh and its bounds, read in a running game on 2026-10-21 (a trash bag
// 0.66 x 0.58 x 0.62 m, a park bench 3.08 x 0.86 x 0.71 m, a traffic cone 0.32 x 0.60 x 0.32 m,
// each with its origin at its base): the blueprint's object data right after it (blueprint + 0x80,
// also its +0x68), whose mesh asset is at +0xd8; the mesh asset's loaded mesh at +0x70, and that
// mesh's box at +0x28 -> {min float[4], max float[4]}, in the object's own frame. A blueprint
// without a mesh there (a widget, a logic object) shows nothing when it is made.
inline constexpr std::uintptr_t object_data_offset = 0x80;            // blueprint
inline constexpr std::uintptr_t object_data_vtable = 0x64062a0;
inline constexpr std::uintptr_t object_data_mesh_offset = 0xd8;       // object data -> mesh asset
inline constexpr std::uintptr_t mesh_asset_vtable = 0x64e6f30;
inline constexpr std::uintptr_t mesh_asset_loaded_offset = 0x70;      // mesh asset -> loaded mesh
inline constexpr std::uintptr_t loaded_mesh_vtable = 0x64f2ba8;
inline constexpr std::uintptr_t loaded_mesh_bounds_offset = 0x28;     // loaded mesh -> {min, max}
// Secondary vtables of the skater animation component (engine::skater_component_vtable) at +0x38 and +0x40.
inline constexpr std::array<std::uintptr_t, 2> skater_component_base_vtables{0x61705e8, 0x6170650};
// ClientSkaterSource::applyAppearance. It enables the customization component and requests
// appearance mode 1 (default/session) when the source data has no recipe. Reference only.
inline constexpr std::uintptr_t source_apply_appearance = 0x511d20;

// Entity creation. The 0x190-byte creation descriptor is constructed in place by
// descriptor_init(descriptor, id, parent bus, transform); descriptor_destroy(descriptor + 0x10)
// releases its internal list state. create_entity(result, descriptor, blueprint, 0, 0) returns
// {entity, retained sublevel, ...}.
inline constexpr std::uintptr_t descriptor_init = 0x2e90d90;
inline constexpr std::array<unsigned char, 12> descriptor_init_prefix{
    0x40,0x53,0x55,0x56,0x57,0x48,0x83,0xec,0x28,0x49,0x8b,0xc0};
inline constexpr std::uintptr_t descriptor_destroy = 0x4ca880;
inline constexpr std::array<unsigned char, 17> descriptor_destroy_prefix{
    0x48,0x89,0x5c,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0x91,0x10,0x01,0x00,
    0x00};
inline constexpr std::uintptr_t create_entity = 0x2e93070;
inline constexpr std::array<unsigned char, 14> create_entity_prefix{
    0x40,0x53,0x48,0x83,0xec,0x50,0x48,0x8b,0x84,0x24,0x80,0x00,0x00,0x00};
// Destroys an entity (entity, owner). Hooked so SDK-created actors are forgotten first.
inline constexpr std::uintptr_t destroy_entity = 0x2b689f0;
inline constexpr std::array<unsigned char, 14> destroy_entity_prefix{
    0x8b,0x41,0x28,0x4c,0x8b,0xc2,0x48,0x8b,0xd1,0x48,0x0f,0xba,0xe0,0x0a};
// Releases one reference on an entity bus or a retained sublevel.
inline constexpr std::uintptr_t release_reference = 0x2b808d0;
inline constexpr std::array<unsigned char, 16> release_reference_prefix{
    0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0xb8,0xff,0xff,0xff,0xff,0xf0,0x0f};

// Placement. place_entity is the skater/board entity vtable slot +0x158 transform setter.
// initialize_placement(entity, transform, 0, 1) runs the entity's +0x88/+0xc8 lifecycle
// callbacks and binds/initializes its owned components.
inline constexpr std::uintptr_t place_entity = 0x53ed70;
inline constexpr std::array<unsigned char, 12> place_entity_prefix{
    0x41,0xb1,0x01,0x45,0x0f,0xb6,0xc1,0xe9,0x44,0xfe,0xa3,0x02};
// Generic SpatialEntity setter used by place_entity. Copies both matrix halves
// into the owned transform collection, then notifies components via +0x108.
inline constexpr std::uintptr_t move_spatial_entity = 0x2f7ebc0;
inline constexpr std::array<unsigned char, 22> move_spatial_entity_prefix{
    0x48,0x83,0xec,0x48,0x4c,0x8b,0x59,0x70,0xc5,0xfc,0x10,0x02,
    0xc5,0xfc,0x10,0x4a,0x20,0x41,0x0f,0xb6,0x53,0x0a};
inline constexpr std::uintptr_t initialize_placement = 0x2f7d670;
inline constexpr std::array<unsigned char, 15> initialize_placement_prefix{
    0x4c,0x8b,0xdc,0x53,0x55,0x56,0x57,0x48,0x83,0xec,0x68,0xf6,0x41,0x28,0x08};

// Returns an entity's customization (appearance) component.
inline constexpr std::uintptr_t customization_component = 0x53f0e0;
inline constexpr std::array<unsigned char, 12> customization_component_prefix{
    0x48,0x8d,0x15,0x71,0xc3,0xca,0x06,0xe9,0x64,0x44,0x63,0x02};
// Expected code of engine::set_customization_flag.
inline constexpr std::array<unsigned char, 8> set_customization_flag_prefix{
    0x48,0x8b,0x09,0xe9,0xb8,0x1b,0x00,0x00};

// Skater component animation physics flag setters (component, value) for +0xc7 and +0xcb.
inline constexpr std::uintptr_t physics_c7_setter = 0xf9cac0;
inline constexpr std::array<unsigned char, 13> physics_c7_setter_prefix{
    0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0x81,0xa0,0x00,0x00,0x00};
inline constexpr std::uintptr_t physics_cb_setter = 0xf9d020;
}
