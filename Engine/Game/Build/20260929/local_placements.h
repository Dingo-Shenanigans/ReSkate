#pragma once
#include <array>
#include <cstdint>

namespace dingosdk::game::build::v20260929::local_placements {
// Placed-object manager vtable; its id -> entity hash table starts at +0x10.
inline constexpr std::uintptr_t placement_manager_vtable = 0x60e0380;
// uint32 registration offset of the server player's component holding the native
// simulation owner (+0x98). 0x7b3470/0x7befd0 activate rendering and collision
// only for that owner.
inline constexpr std::uintptr_t player_component_offset = 0x6f7cf60;
// uint32 offset of the client context record whose +0x100 byte is 1 once ready.
inline constexpr std::uintptr_t client_ready_offset = 0x7215038;
// Pointer to the teleport manager; its first uint32 is 0 while idle.
inline constexpr std::uintptr_t teleport_manager = 0x71ee790;
// The game's own ghosting of placed objects (walk-through, and solid again only once the
// skater is clear of them). The client keeps a 0x50-byte record per object; 0x7b3470 decides
// each one's ghost byte (+0x3e) from a forced-solid byte (+0x3c), a forced-ghost byte (+0x3b),
// a mode (everyone solid, party and friends solid, only your own solid) and whether the local
// player is the native owner (0x7befd0). ReSkate's copies of other players' objects all have
// the local player as native owner, so only the forced bytes can ghost them.
// Expression native 0x9447529e: void (const u64* entity, bool ghost). true forces the ghost;
// false lets go of a forced ghost (or, on an object that was not forced, forces it solid),
// and either way the record is decided again at once. Client realm, client thread.
inline constexpr std::uintptr_t object_ghost = 0x7ae620;
// What that native does, in parts. It finds the record store through the calling thread's
// current context (0x18b6ae0 reads it from thread-local storage) plus client_ready_offset;
// the store is ready when its +0x100 byte is set and keeps its records at +0x48..+0x50.
// A record: entity u64 at +0x10, a group id at +0x28, active at +0x3a, forced ghost +0x3b,
// forced solid +0x3c, a "decide now" byte +0x3d, ghost +0x3e, shown +0x3f. 0x7b33e0 decides one
// record again: void (store*, record*). The rules it follows are the build kit settings
// (0x18d6ef0(*0x7482ce0, 0x723a510)): nothing is decided while their +0x8f byte is clear.
inline constexpr std::uintptr_t thread_context = 0x18b6ae0, object_ghost_decide = 0x7b33e0;
// A realm's context by its id: void* (context* out, u32 realm). ReSkate's placement updates run
// in the local server's context, so the client's store is reached through the client realm.
// Setting the store's +0xfb byte has the client decide every record again on its next update.
// A record's entity is the client's copy, with an id of its own. What the two sides share is the
// object's number: the index part of the server's entity (bits 8 up), which the client's
// component has at +0x24 and its records at +0x28 (the "group id" above).
inline constexpr std::uintptr_t realm_context = 0x18b6e40;
inline constexpr std::array<unsigned char, 16> realm_context_prefix{
    0x89,0x54,0x24,0x10,0x53,0x48,0x83,0xec,0x30,0x4c,0x8b,0x05,0xe8,0xb7,0x70,0x05};
inline constexpr std::uint32_t client_realm = 0xbf0f9789, server_realm = 0x98be5555;
inline constexpr std::uintptr_t settings_lookup = 0x18d6ef0, settings_manager = 0x7482ce0, build_kit_settings = 0x723a510;
inline constexpr std::array<unsigned char, 16> object_ghost_prefix{
    0x48,0x89,0x6c,0x24,0x20,0x56,0x48,0x83,0xec,0x20,0x48,0x8b,0xf1,0x0f,0xb6,0xea};
// Expression native 0xcba97fb7: bool (const u64* entity), whether the placed object that entity
// belongs to is the local player's (its component's owner at +0x10 against the local player's
// id). The object's menu is the owner's or a visitor's by it. object_root finds that object from
// any entity of it: void (u64 out[2], u64 entity, bool); out[0] is the object's own entity.
// object_component: void (const u64 object[2], u64 out[]); out[1] is set and out[2] is the
// placed-object component when the object has one.
inline constexpr std::uintptr_t object_owned = 0x7ad210, object_root = 0x6fdb00, object_component = 0x46aa70;
// Expression native 0x0b092a7b: bool (const u64 *const *entities), an array of placed objects'
// entities (count in the u32 before its first element, top bit apart). Whether all of them may
// be edited now; the owner is not part of it. The object card's script (flow_buildkit
// aa058fc, graph 0x25bf9270) puts the Edit button on the card by it, and Delete or
// Enable/Disable by object_owned.
inline constexpr std::uintptr_t objects_editable = 0x7a6a10;
inline constexpr std::array<unsigned char, 16> objects_editable_prefix{
    0x40,0x53,0x55,0x48,0x83,0xec,0x28,0x48,0x8b,0xd9,0xba,0x89,0x97,0x0f,0xbf,0x48};
// What was tried and left (2026-10-13): giving the client's copies their real owner (component
// +0x10). The game then hides them unless its store's +0xfa is set (its service's answer to
// whether others' objects may be seen) and their +0x3a is not 0x7f, takes the far ones away
// (0x7c4c50), and looks the owner's card up by a model the session does not publish.
inline constexpr std::array<unsigned char, 16> object_component_prefix{
    0x40,0x53,0x48,0x81,0xec,0x80,0x00,0x00,0x00,0x48,0x8b,0x05,0x40,0x99,0xd5,0x06};
inline constexpr std::array<unsigned char, 16> object_owned_prefix{
    0x48,0x81,0xec,0xc8,0x00,0x00,0x00,0x48,0x8b,0x05,0xa2,0x71,0xa1,0x06,0x48,0x33};
inline constexpr std::array<unsigned char, 16> object_root_prefix{
    0x48,0x89,0x5c,0x24,0x18,0x55,0x57,0x41,0x56,0x48,0x8d,0x6c,0x24,0xb9,0x48,0x81};
// Reference only (PlacementCreateRecipe mirrors these, nothing calls them): the
// create-message handler 0x87da20; the free-roam drop 0x7af5b0, whose park
// context byte (127) comes from 0x6f7c750; the source enum codec 0x58f96c0/0x58f8c50.
}
