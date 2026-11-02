#pragma once
#include <array>
#include <cstdint>

// The hooks that let a claimed placement be selected and edited while it moves, and be skated as a
// moving surface (Extension/Objects/placement_adapters.cpp). Functions are checked by their first 16
// bytes; every call site in transform_calls, moving_calls and material_calls is checked to be a direct
// call (E8 rel32) to the function it is listed for, since the hooks tell those callers apart by return
// address (site + 5). The addresses were found by the PR #356 author against this build.
namespace dingosdk::game::build::v20260929::placement_adapters {
// The dropper's selection: select(controller, const entity list*, bool quiet) starts one; finish
// (controller, const entity list*, bool keep, bool cancel, bool force) ends it, committing unless
// cancel. pose(float out[12], const query*) -> out reads an entity's transform (scale, rotation,
// position), as the selection does to start from an object's pose.
inline constexpr std::uintptr_t select = 0x7bdb20, finish = 0x7c1710, pose = 0x287ac10;
inline constexpr std::array<unsigned char, 16> select_prefix{0x4c, 0x8b, 0xdc, 0x55, 0x53, 0x57, 0x49, 0x8d,
                                                             0xab, 0x18, 0xfe, 0xff, 0xff, 0x48, 0x81, 0xec};
inline constexpr std::array<unsigned char, 16> finish_prefix{0x4c, 0x8b, 0xdc, 0x55, 0x53, 0x57, 0x49, 0x8d,
                                                             0xab, 0x08, 0xff, 0xff, 0xff, 0x48, 0x81, 0xec};
inline constexpr std::array<unsigned char, 16> pose_prefix{0x48, 0x89, 0x5c, 0x24, 0x18, 0x57, 0x48, 0x81,
                                                           0xec, 0x30, 0x01, 0x00, 0x00, 0x48, 0x8b, 0x05};
// The dropper's own calls of the transform setter (park_editor.h movement_contracts[2]), all in the
// selection code 0x7c1cc8-0x7d161c: a pose set from one of them is a preview or commit of an edit.
inline constexpr std::array<std::uintptr_t, 11> transform_calls{0x7c1cc8, 0x7c65c3, 0x7c9fa1, 0x7cb1dd, 0x7cbc73, 0x7cef58,
                                                                0x7cf547, 0x7d0785, 0x7d0c61, 0x7d0f30, 0x7d161c};
// Callers of motion_type (placement_physics.h) in the skater's surface code that treat a dynamic body
// as something not to stand or grind on: they are shown a claimed surface as keyframed (moving_calls)
// or, for the material lookups, fixed (material_calls).
inline constexpr std::array<std::uintptr_t, 11> moving_calls{0xfb6cc6,  0xfb7534,  0xfc1cfc,  0xfeb7f9,  0x1027c6d, 0x1027c7c,
                                                             0x1044140, 0x1044ce9, 0x1045784, 0x10457a4, 0x105709d};
inline constexpr std::array<std::uintptr_t, 2> material_calls{0x102bcf5, 0x102bd47};
// friction(grind, float, float, float): a grind's friction step, reading the skater context's velocity.
// point_velocity(const Handle* body, float out[4], const float point[4]) -> out: a body's velocity at
// a world point. shape_body(const Handle* shape, Handle* out) -> out: the body a shape belongs to.
// edges(cache): gathers authored grind edges around a grind context (see query_half). edge_query
// (registry, const float bounds[8], out) -> bool: gathers edges overlapping a box, bounds being its min
// (floats 0-2) and max (floats 4-6). chain(const u64 identity[2], {const Edge*, u32 count}* out) ->
// bool: the whole chain an edge belongs to.
inline constexpr std::uintptr_t friction = 0x4811800, point_velocity = 0x25d7060, shape_body = 0x25d3cb0, edges = 0x47e2350,
                                edge_query = 0x47e0e10, chain = 0x47e61d0;
inline constexpr std::array<unsigned char, 16> friction_prefix{0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x57,
                                                               0x48, 0x81, 0xec, 0xc0, 0x00, 0x00, 0x00, 0x48};
inline constexpr std::array<unsigned char, 16> point_velocity_prefix{0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x7c,
                                                                     0x24, 0x10, 0x48, 0x8b, 0x01, 0x48, 0x8b, 0xfa};
inline constexpr std::array<unsigned char, 16> shape_body_prefix{0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x4c, 0x8b,
                                                                 0x11, 0x48, 0x8b, 0xda, 0x44, 0x8b, 0x49, 0x08};
inline constexpr std::array<unsigned char, 16> edges_prefix{0x48, 0x83, 0xec, 0x08, 0x48, 0x8b, 0x01, 0x4c,
                                                            0x8d, 0x41, 0x10, 0xf7, 0x80, 0xcc, 0x13, 0x00};
inline constexpr std::array<unsigned char, 16> edge_query_prefix{0x40, 0x56, 0x48, 0x8b, 0xf2, 0x4d, 0x85, 0xc0,
                                                                 0x0f, 0x84, 0x9f, 0x01, 0x00, 0x00, 0x41, 0xc7};
inline constexpr std::array<unsigned char, 16> chain_prefix{0x48, 0x83, 0xec, 0x18, 0x4c, 0x8b, 0x05, 0xfd,
                                                            0xf2, 0xfa, 0x02, 0x4c, 0x8b, 0xd1, 0x49, 0x8b};
// The selection controller's set of selected entities, a hash set: bucket array, bucket count, size,
// and each node's next pointer (a node starts with its u64 entity).
inline constexpr std::uintptr_t selection_buckets = 0x248, selection_count = 0x250, selection_size = 0x254, selection_next = 0x70;
// local_placements.h object_root writes a root {entity, flags} where flags (+8) carries
// root_dynamic_restriction (0x800) for a placed object; while it is set the game keeps the object's
// body fixed. root_hook clears it in the returned copy (never in the entity) for claimed objects only.
// What the bit is called in the game is not known; its effect is the PR author's finding.
inline constexpr std::uintptr_t root_flags = 8;
inline constexpr std::uint32_t root_dynamic_restriction = 0x800;
// An edge list is a u32 count, then at edge_data at most 100 edges of 0x70 bytes. A grind context
// (the edges cache's first pointer) has its flags (context_flags; disabled_query set means it gathers
// nothing) and position; the gather box is that position +/- query_half, a global vec3 that is
// (1.2, 1.2, 1.2) in this build. shape_generations is a world state's byte array of shape generations.
inline constexpr std::uintptr_t edge_data = 0x10, shape_generations = 0xff8, context_flags = 0x13cc, context_position = 0x70,
                                query_half = 0x65e7540;
inline constexpr std::uint32_t disabled_query = 0x400000;
// A grind's skater context pointer (grind + grind_context).
inline constexpr std::uintptr_t grind_context = 8;
// A skater's board (core + skater_board, skater_movement.h) has its physics object (board_physics,
// whose vtable is skater_state.h board_physics_vtable) and that its parts: a count of board_part_count
// (u32), the owner back pointer and the deck part's position.
inline constexpr std::uintptr_t board_physics = 0x18, board_parts = 0x20, parts_owner = 0x10, parts_position = 0x50;
inline constexpr std::uint32_t board_part_count = 9;
// The support rays cast down through the deck to find a claimed surface under the board: from
// board_ray_above above the deck's position to board_ray_below under it, at these offsets across it.
inline constexpr float board_ray_above = .4f, board_ray_below = .6f;
inline constexpr std::array<float, 7> board_ray_offsets{0.f, -.08f, .08f, -.16f, .16f, -.24f, .24f};
} // namespace dingosdk::game::build::v20260929::placement_adapters
