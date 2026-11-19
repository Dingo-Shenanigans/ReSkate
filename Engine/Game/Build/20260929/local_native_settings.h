#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace dingosdk::game::build::v20260929::local_native_settings {
// Pointer to DingoProfileSettingsManager; its query vector contains the three
// option groups (LocalPerDevice, then the two cloud groups).
inline constexpr std::uintptr_t profile_settings_manager = 0x71eff00;
// The shared settings predicate is bool(). The two callers retain the current
// query in RCX, so its hook preserves that otherwise incidental register.
inline constexpr std::uintptr_t gate_rva = 0x5c6700;
inline constexpr std::array<std::uint8_t, 20> gate_prefix{
    0x48,0x89,0x5c,0x24,0x18,0x56,0x48,0x83,0xec,0x20,
    0x48,0x89,0x7c,0x24,0x30,0xe8,0xac,0x5d,0x2e,0x01};
// PC local storage configuration consumes (platform, configuration), returning
// a uint32 result. Its path is UTF-8 char*, not a Windows wide string.
inline constexpr std::uintptr_t configure_rva = 0x5493080;
inline constexpr std::array<std::uint8_t, 19> configure_prefix{
    0x40,0x53,0x48,0x81,0xec,0x40,0x01,0x00,0x00,0x48,
    0x8b,0x05,0x30,0x13,0xd3,0x01,0x48,0x33,0xc4};
struct Call { std::uintptr_t instruction; std::array<std::uint8_t, 5> bytes; };
inline constexpr std::array<Call, 2> calls{{
    {0x5d2450, {0xe8,0xab,0x42,0xff,0xff}},
    {0x5cc638, {0xe8,0xc3,0xa0,0xff,0xff}},
}};
inline constexpr std::size_t call_size = 5;
inline constexpr std::uint32_t invalid_path_result = 11;

// Only this layout belongs to the supported executable. Pure guards receive
// it explicitly; neither they nor their callers embed a second native layout.
struct Layout {
    static constexpr std::uintptr_t lowest_address = 0x10000;
    static constexpr std::uintptr_t highest_address = 0x00007fffffffffffULL;
    static constexpr std::size_t pointer_alignment = 8, pointer_size = 8;
    static constexpr std::uintptr_t query_manager_rva = profile_settings_manager;
    static constexpr std::uintptr_t query_begin = 0x40, query_end = 0x48;
    static constexpr std::size_t query_slots = 3;
    static constexpr std::uintptr_t query_method_slot = 8;
    static constexpr std::uintptr_t query_vtable_rva = 0x608fc38, query_method_rva = 0x204a50;
    static constexpr std::array<std::uint8_t, 5> query_method_prefix{0x0f,0xb6,0x41,0x70,0xc3};
    static constexpr std::size_t query_size = 0x90;
    static constexpr std::uintptr_t query_storage = 0x60, query_completed = 0x70;
    static constexpr std::uintptr_t query_list_request = 0x74, query_read_request = 0x78;
    static constexpr std::uintptr_t query_load_state = 0x80, query_deferred_load = 0x84;
    static constexpr std::uintptr_t query_save_state = 0x88, query_deferred_save = 0x8c;

    static constexpr std::uintptr_t manager_rva = 0x73c7a98, provider_rva = 0x74cbc50;
    static constexpr std::uintptr_t service_vtable_rva = 0x60e9950, service_method_rva = 0x18ac8a0;
    static constexpr std::uintptr_t user_vtable_rva = 0x634f110, user_method_rva = 0x272c9b0;
    static constexpr std::array<std::uint8_t, 8> user_method_prefix{0x0f,0xb6,0x81,0xb8,0x01,0x00,0x00,0xc3};
    static constexpr std::uintptr_t service_method_slot = 0x28, user_method_slot = 0x30;
    static constexpr std::size_t manager_size = 0x40, tree_node_size = 0x30;
    static constexpr std::uintptr_t manager_head = 0x18, manager_key = 0x38, manager_sentinel = 8;
    static constexpr std::uintptr_t tree_left = 8, tree_right = 0, tree_key = 0x20, tree_service = 0x28;
    static constexpr std::size_t maximum_nodes = 64, maximum_records = 64;
    static constexpr std::size_t service_size = 0x250, record_stride = 0x28;
    static constexpr std::uintptr_t records_begin = 0x240, records_end = 0x248;
    static constexpr std::uintptr_t record_user = 8, record_index = 0x1c, record_removed = 0x20;
    static constexpr std::size_t user_size = 0x1b9;
    static constexpr std::uintptr_t user_index = 0x140, user_provider = 0x148, user_guest = 0x1b8;

    static constexpr std::uintptr_t pc_storage_vtable_rva = 0x68254b8;
    static constexpr std::size_t configuration_size = 0x98, maximum_path_bytes = 260;
    static constexpr std::uintptr_t configuration_path = 0x90;
};
inline constexpr Layout guard_layout{};
}
