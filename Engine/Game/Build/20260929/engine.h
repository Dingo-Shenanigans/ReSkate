#pragma once
#include <cstdint>

// Engine addresses shared by several features. Addresses only one feature uses
// belong in that feature's own header.
namespace dingosdk::game::build::v20260929::engine {

// Functions
// UI control destructor shared by the cosmetic and music list vtables.
inline constexpr std::uintptr_t ui_control_destroy = 0x214930;
// Audio manager vtable slot +0x20.
inline constexpr std::uintptr_t audio_manager_update = 0x43c2f0;
// Entitlement check request.
inline constexpr std::uintptr_t check_entitlements = 0x774340;
// Constructs an engine error value.
inline constexpr std::uintptr_t construct_error = 0x83de30;
// Animation definition count getter (definition+0x1a0 -> +0xc).
inline constexpr std::uintptr_t animation_count_getter = 0x11e5040;
// Releases an engine-owned text handle.
inline constexpr std::uintptr_t native_text_release = 0x1586050;
inline constexpr std::uintptr_t string_assign = 0x1587620;
// Returns the calling thread's game context.
inline constexpr std::uintptr_t current_context = 0x18b6ae0;
// Settings manager lookup by descriptor.
inline constexpr std::uintptr_t settings_lookup = 0x18d6ef0;
// UI data model lookup.
inline constexpr std::uintptr_t model_find = 0x19193b0;
// Game query context constructor.
inline constexpr std::uintptr_t query_context_constructor = 0x276f3e0;
// Skateboard render pose publisher.
inline constexpr std::uintptr_t board_render_pose = 0x2e48610;
// Assigns a future from a promise state.
inline constexpr std::uintptr_t assign_future = 0x3dc5bd0;
// Engine array allocation.
inline constexpr std::uintptr_t array_allocate = 0x3dc7540;
inline constexpr std::uintptr_t delegate_copy = 0x3dd5540;
inline constexpr std::uintptr_t delegate_destroy = 0x3dd5780;
// Sets a flag on a customization/indicator property.
inline constexpr std::uintptr_t set_customization_flag = 0x4654e10;
// Table functions graphs bind PostEventDelayedFrames (0x9967fc07) and
// PostEventDelayedSeconds (0x5e3aee8e) calls to.
inline constexpr std::uintptr_t post_event_delayed_frames = 0x4664e80;
inline constexpr std::uintptr_t post_event_delayed_seconds = 0x4665040;
// Completes a promise with a result or error.
inline constexpr std::uintptr_t complete_promise = 0x58b1cf0;
inline constexpr std::uintptr_t delegate_invoke = 0x58b1d60;
// Player card info constructor.
inline constexpr std::uintptr_t card_info_construct = 0x59dde00;

// Vtables
// Connection controller.
inline constexpr std::uintptr_t controller_vtable = 0x606dc60;
inline constexpr std::uintptr_t allocator_adapter_vtable = 0x606df70;
inline constexpr std::uintptr_t source_manager_vtable = 0x6079940;
inline constexpr std::uintptr_t skater_audio_component_vtable = 0x607a500;
inline constexpr std::uintptr_t audio_manager_vtable = 0x607b520;
// Client game context; state at +0xc4.
inline constexpr std::uintptr_t client_vtable = 0x60842c0;
inline constexpr std::uintptr_t client_skater_source_vtable = 0x6084fa8;
inline constexpr std::uintptr_t skater_entity_vtable = 0x6087908;
inline constexpr std::uintptr_t cosmetics_manager_vtable = 0x6089cc8;
// Current game server.
inline constexpr std::uintptr_t server_vtable = 0x60e0890;
// DelMarGameSettings.
inline constexpr std::uintptr_t game_settings_vtable = 0x6145be0;
inline constexpr std::uintptr_t skater_component_vtable = 0x6170410;
inline constexpr std::uintptr_t board_component_vtable = 0x6170e38;
inline constexpr std::uintptr_t board_entity_vtable = 0x6171808;
inline constexpr std::uintptr_t board_data_vtable = 0x6186fa0;
// Blueprint objects, including the board and UI widget blueprints.
inline constexpr std::uintptr_t blueprint_vtable = 0x6187008;
// Skater appearance entity.
inline constexpr std::uintptr_t skater_appearance_vtable = 0x61c9ec0;
inline constexpr std::uintptr_t camera_vtable = 0x6255870;
inline constexpr std::uintptr_t board_holder_vtable = 0x63db690;
inline constexpr std::uintptr_t local_player_vtable = 0x63e2d30;
inline constexpr std::uintptr_t local_player_manager_vtable = 0x63e30a0;
inline constexpr std::uintptr_t server_player_vtable = 0x63e4108;
inline constexpr std::uintptr_t server_player_manager_vtable = 0x63e41d0;

// Type information
inline constexpr std::uintptr_t cosmetic_slot_type = 0x72290b8;
inline constexpr std::uintptr_t persona_id_type = 0x723bcf8;
inline constexpr std::uintptr_t player_info_type = 0x72591d8;
inline constexpr std::uintptr_t board_blueprint_type = 0x7288f80;
inline constexpr std::uintptr_t reference_type = 0x7493030;
inline constexpr std::uintptr_t bool_type = 0x765d8e8;
inline constexpr std::uintptr_t uint32_type = 0x765da28;
inline constexpr std::uintptr_t int32_type = 0x765da68;
inline constexpr std::uintptr_t uint64_type = 0x765daa8;
inline constexpr std::uintptr_t int64_type = 0x765dae8;
inline constexpr std::uintptr_t float_type = 0x765db28;
// CString.
inline constexpr std::uintptr_t string_type = 0x765dcc0;

// Globals
// uint32 offset of the partition record in a game context.
inline constexpr std::uintptr_t context_partition_offset = 0x6fc2430;
// uint32 offset of the type/realm record in a game context.
inline constexpr std::uintptr_t context_type_offset = 0x6fc24b8;
// uint32 offset of the client connection in a game context.
inline constexpr std::uintptr_t context_client_connection_offset = 0x715d5e8;
// uint32 offset of the player manager in a game context.
inline constexpr std::uintptr_t context_player_manager_offset = 0x7161720;
// Pointer.
inline constexpr std::uintptr_t backend_services = 0x71e17e0;
// Pointer.
inline constexpr std::uintptr_t audio_manager = 0x71e3978;
// Pointer.
inline constexpr std::uintptr_t appearance_manager = 0x71eaba0;
// Pointer array indexed by partition * 7 + kind.
inline constexpr std::uintptr_t source_managers = 0x71ec560;
// Pointer.
inline constexpr std::uintptr_t loadout_manager = 0x71ec8c8;
// Pointer.
inline constexpr std::uintptr_t cosmetics_manager = 0x71ee610;
// Pointer.
inline constexpr std::uintptr_t client_game_manager = 0x71ef898;
// Pointer.
inline constexpr std::uintptr_t location_owner = 0x71f4288;
// Pointer.
inline constexpr std::uintptr_t ui_manager = 0x71f5ef0;
// The UI manager's UI model registry (the ModelManager every UI model is made in), a pointer at this
// offset in it.
inline constexpr std::uint32_t ui_model_registry = 0x140;
// Pointer.
inline constexpr std::uintptr_t game_server = 0x71f8ad0;
// DelMarGameSettings handle.
inline constexpr std::uintptr_t game_settings_handle = 0x725b018;
// Pointer.
inline constexpr std::uintptr_t default_arena = 0x72f6e78;
// The engine's empty CString.
inline constexpr std::uintptr_t empty_cstring = 0x72fe704;
// Pointer.
inline constexpr std::uintptr_t settings_manager = 0x7482ce0;
// Pointer.
inline constexpr std::uintptr_t ui_allocator = 0x74889e8;
// uint8.
inline constexpr std::uintptr_t entity_creation_ready = 0x75c26f0;
// uint32 registration offset between player base classes.
inline constexpr std::uintptr_t player_parent_offset = 0x75c44d8;
// Pointer array indexed by domain.
inline constexpr std::uintptr_t domain_owners = 0x7726d10;
// uint32 engine TLS slot.
inline constexpr std::uintptr_t tls_index = 0x78d40e0;
}
