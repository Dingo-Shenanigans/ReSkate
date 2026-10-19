// Reviewed native contract for supported_build::game_sha256 (September 29 2026).
// Where the three world pools ReSkate enlarges (entity_pages.h, spatial_buckets.h)
// can be read for how full they are: for telling whether a level needs more than
// the game's own sizes.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace dingosdk::game::build::v20260929::world_pool_usage {
// Entity snapshots: the page allocator, (owner, allocator, pages). It fills the
// snapshot's 128 page pointers at pages[0..127], each page zeroed when handed out,
// so the highest non-zero entry is the highest entity slot in use.
inline constexpr std::uintptr_t entity_page_allocator_rva = 0x048bd000;
inline constexpr std::array<std::uint8_t, 10> entity_page_allocator_entry{
    0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xec};
// The game's own entity slots (128 pages of 1,024).
inline constexpr std::size_t entity_slots_stock = 131'072;

// Render instances: the pool is [[global] + 0x4a0] + 0x1300 (as the one caller of
// its allocator loads it), and its next-index counter, taken with lock xadd, is
// the most it has ever handed out.
inline constexpr std::uintptr_t render_instance_load_rva = 0x050e59ac;
inline constexpr std::array<std::uint8_t, 27> render_instance_load{
    0x48, 0x8b, 0x05, 0x25, 0xcf, 0x7c, 0x02, 0x48, 0x8b, 0xd9, 0x48, 0x8b, 0xd1, 0x48,
    0x8b, 0xa8, 0xa0, 0x04, 0x00, 0x00, 0x48, 0x8d, 0x8d, 0x00, 0x13, 0x00, 0x00};
inline constexpr std::uintptr_t render_instance_owner_global = 0x078b28d8;
inline constexpr std::ptrdiff_t render_instance_manager_offset = 0x4a0;
inline constexpr std::ptrdiff_t render_instance_pool_offset = 0x1300;
inline constexpr std::uintptr_t render_instance_counter_rva = 0x050e23ec;
inline constexpr std::array<std::uint8_t, 8> render_instance_counter{
    0xf0, 0x0f, 0xc1, 0xb7, 0x08, 0x10, 0x00, 0x00}; // lock xadd [rdi+0x1008], esi
inline constexpr std::ptrdiff_t render_instance_counter_offset = 0x1008;
// The game's own render instances (512 pages of 512).
inline constexpr std::size_t render_instances_stock = 262'144;

// Spatial buckets: a query, (root, ...), four register arguments; the root's node
// count and each node's block count are at spatial_buckets.h's offsets.
inline constexpr std::uintptr_t spatial_query_rva = 0x01967420;
inline constexpr std::array<std::uint8_t, 15> spatial_query_entry{
    0x4c, 0x8b, 0xdc, 0x4d, 0x89, 0x4b, 0x20, 0x41, 0x55, 0x41, 0x56, 0x48, 0x83, 0xec, 0x68};
inline constexpr std::ptrdiff_t spatial_nodes_offset = 0x130;
// The game's own blocks per bucket (its root bucket overflowed at 2,049).
inline constexpr std::size_t spatial_blocks_stock = 2'048;
} // namespace dingosdk::game::build::v20260929::world_pool_usage
