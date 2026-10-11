#pragma once
#include <array>
#include <cstdint>

// Skater effects (sparks, dust, landing puffs, costume and board effects): the game's skater
// effects manager.
namespace dingosdk::game::build::v20260929::native_vfx {
struct Function {
    std::uintptr_t rva;
    std::array<unsigned char, 16> prefix;
};
// (manager, key, component): a skater's effects component registers with the manager.
inline constexpr Function register_skater{
    0x4874800, {0x48,0x8b,0xc4,0x53,0x48,0x83,0xec,0x60,0x48,0x89,0x68,0x08,0x48,0x8d,0x59,0x78}};
// (manager, key, impact): the effect of one contact with the world, chosen by the surface's
// material and the speed. Each call is one effect that ends by itself. Reads the calling
// thread's world.
inline constexpr Function impact{
    0x486cda0, {0x48,0x8b,0xc4,0x4c,0x89,0x40,0x18,0x48,0x89,0x50,0x10,0x48,0x89,0x48,0x08,0x48}};
// (manager, key): the skater's effects are stopped and built again from its settings at the
// manager's next update. Takes the manager's own lock.
inline constexpr Function refresh{
    0x4874a20, {0x40,0x53,0x48,0x83,0xec,0x40,0x48,0x89,0x6c,0x24,0x50,0x48,0x8d,0x99,0xb0,0x00}};
// (manager, uint64 *effects_entity, int id, settings, char flag, char queued): builds a skater's
// effects from its settings, a pointer to an array of effect sets (pointers, bit 2 a flag;
// the count sits four bytes before the first). Called for each refreshed skater from the
// manager's update.
inline constexpr Function build_effects{
    0x48700c0, {0x44,0x89,0x44,0x24,0x18,0x48,0x89,0x54,0x24,0x10,0x48,0x89,0x4c,0x24,0x08,0x55}};
// Manager: the root of its map of registered skaters; a node's id and effects component.
inline constexpr std::size_t manager_skaters_root = 0x58, skater_node_id = 0x20, skater_node_component = 0x30;
// An effects component: what holds its entity.
inline constexpr std::size_t component_owner = 0x18;
// (rig, joint id, 1): the joint's index in the rig's skeleton, or -1. Reads no pose.
inline constexpr Function joint_index{
    0x12e08b0, {0x48,0x8b,0x89,0x38,0x01,0x00,0x00,0xe9,0x84,0x76,0x5b,0x03,0xcc,0xcc,0xcc,0xcc}};
// An effects component's rig: component -> its rig's owner -> the rig; the rig's skeleton. An
// effect set's effects (an array of pointers), and the joint each sits on (-1: none).
inline constexpr std::size_t component_rig_owner = 0x50, rig_owner_rig = 0x78, rig_skeleton = 0x138, set_items = 0x30,
                             item_joint = 0x44;
// How an effect set is started, and the value for one that is always on.
inline constexpr std::size_t set_trigger = 0x80;
inline constexpr std::int32_t trigger_always = 3;
// (out): the calling thread's world, or null.
inline constexpr Function thread_world{
    0x18b6ae0, {0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x65,0x48,0x8b,0x04,0x25,0x58}};
// Offsets into the world, each stored as a 32-bit value: the effects manager, and the object
// that holds the level's material grid.
inline constexpr std::uintptr_t manager_offset = 0x77abaa8, materials_offset = 0x74f8e38;
inline constexpr std::size_t manager_ready = 0x240;        // byte: the manager is in use
inline constexpr std::size_t materials_ready = 0x50;       // byte: the grid object is in use
inline constexpr std::size_t materials_grid = 0x28;        // the grid
inline constexpr std::size_t grid_data = 8;                // its data (bit 2 is a flag)
inline constexpr std::size_t grid_index_table = 0x48;      // data: pointer to the index table
// The effects component of a skater entity.
inline constexpr std::uintptr_t component_vtable = 0x65ec508;
inline constexpr std::size_t component_size = 0x68;
// The impact: position, velocity and normal as four floats each, then the material.
inline constexpr std::size_t impact_size = 0x40, impact_velocity = 0x10, impact_normal = 0x20, impact_material = 0x30;
inline constexpr std::uint32_t material_valid = 0x20;
inline constexpr unsigned material_shift = 6;
}
