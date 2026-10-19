// Reviewed native contract for supported_build::game_sha256 (September 29 2026).
// A spatial transaction queue (SpatialQueryManager's, and one in each world's client and render
// spatial managers) hands every object it tracks a cull ID from a free list. The list is made
// once, when the queue is, at SpatialQueryManagerSettings.InitialCullIdListSize entries (the
// game's config: 40,000; 80,000 in GameView.cfg), and never grows: an insert with the list empty
// reads entry 0xffffffff of it and crashes (0x489cfe9, loading a large custom map).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace dingosdk::game::build::v20260929::spatial_cull_ids {
// make_list(list, allocator): looks the settings up, sizes the list's two arrays (u32 next IDs,
// u64 per-ID entries) to InitialCullIdListSize and chains them into the free list. Its only
// callers are the two queue constructors (0x489b518, 0x489ba10).
inline constexpr std::uintptr_t make_list_rva = 0x0489b770;
inline constexpr std::array<std::uint8_t, 15> make_list_entry{
    0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xec, 0x20, 0x45, 0x33, 0xf6, 0x48};
// lea rdx, [settings type 0x77ae5e0] ... mov rcx, [settings manager 0x7482ce0];
// call settings_lookup; test rax, rax; je; mov esi, [rax + 0x28]; jmp; mov esi, 1.
inline constexpr std::uintptr_t settings_type_load_rva = 0x0489b78d;
inline constexpr std::array<std::uint8_t, 7> settings_type_load{0x48, 0x8d, 0x15, 0x4c, 0x2e, 0xf1, 0x02};
inline constexpr std::uintptr_t size_read_rva = 0x0489b7ae;
inline constexpr std::array<std::uint8_t, 27> size_read{
    0x48, 0x8b, 0x0d, 0x2b, 0x75, 0xbe, 0x02, 0xe8, 0x36, 0xb7, 0x03, 0xfd, 0x48, 0x85,
    0xc0, 0x74, 0x05, 0x8b, 0x70, 0x28, 0xeb, 0x05, 0xbe, 0x01, 0x00, 0x00, 0x00};
inline constexpr std::uintptr_t settings_manager = 0x07482ce0;
inline constexpr std::uintptr_t settings_lookup = 0x018d6ef0;
inline constexpr std::uintptr_t settings_type = 0x077ae5e0;
inline constexpr std::size_t initial_cull_id_list_size = 0x28;
} // namespace dingosdk::game::build::v20260929::spatial_cull_ids
