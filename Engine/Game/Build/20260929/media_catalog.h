#pragma once
#include "Engine/Game/Build/fingerprint.h"
#include <cstdint>

// The game's media catalogue: thumbnails and labels of saved replay drafts (and the game's own
// park saves), kept as numbered folders with a manifest.json under <CatalogVFSRoot>/<user id>
// (Extension/Settings/replay_drafts.cpp; analysis/replay-editor.md, section 6).
namespace dingosdk::game::build::v20260929::media_catalog {
// The backend services object (engine.h backend_services) when it is the game's own.
inline constexpr std::uintptr_t services_vtable = 0x6070f98;
// lookup(services): the registered "catalog" container, or null.
inline constexpr Fingerprint lookup{0x90c420, {
    0x48,0x89,0x5c,0x24,0x10,0x57,0x48,0x83,0xec,0x30,0x48,0x8d,0x99,0x90,0x04,0x00,
    0x00,0x48,0x8b,0xf9,0x48,0x8b,0xcb,0xe8,0x44,0x39,0xc8,0x00,0x41,0xb9,0x07,0x00}};
// add_container(services): makes the container and registers it. The services constructor
// calls it among its online registrations (0x282fa0), none of which run offline.
inline constexpr Fingerprint add_container{0x279720, {
    0x48,0x8b,0xc4,0x53,0x57,0x41,0x56,0x48,0x83,0xec,0x50,0x48,0x89,0x68,0x20,0x48,
    0x89,0x70,0xe0,0x4c,0x89,0x60,0xd8,0x4c,0x89,0x68,0xd0,0x4c,0x8b,0xe9,0x48,0x8b}};
// user_added(listener, &user): the container's answer to a user signing in (its listener, at
// container + 0x60, is subscribed to "user_manager" when there is one). It makes the catalogue
// for the user's id (an engine string at user + 0x18) and keeps it at container + 0x68: the
// constructor (0x63be70) lists the folder and loads each entry (0x659450 -> 0x657480), then a
// posted task (0x659980) sets the catalogue's ready flag.
inline constexpr Fingerprint user_added{0x651bb0, {
    0x48,0x89,0x5c,0x24,0x20,0x56,0x48,0x83,0xec,0x30,0x48,0x8b,0xda,0x48,0x89,0x6c,
    0x24,0x48,0x48,0x8b,0x15,0x1f,0x6e,0xe3,0x06,0x48,0x8b,0xe9,0xb9,0x60,0x00,0x00}};
inline constexpr std::uintptr_t container_vtable = 0x60cd120;
inline constexpr std::uintptr_t listener_vtable = 0x60cd140;
inline constexpr std::uintptr_t container_listener = 0x60;
inline constexpr std::uintptr_t container_catalogue = 0x68;
// In a catalogue: its folder (an engine string), its entries (0x130 bytes each) and whether
// its first load is done. Updating and deleting an entry need the flag.
inline constexpr std::uintptr_t catalogue_folder = 0x0;
inline constexpr std::uintptr_t catalogue_entries = 0x38;
inline constexpr std::uintptr_t catalogue_entry_size = 0x130;
inline constexpr std::uintptr_t catalogue_ready = 0x5c;
}
