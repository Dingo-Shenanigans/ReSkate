#pragma once
#include "Engine/Vfs/mod_music.h"
#include "Extension/Profile/runtime_internal.h"

namespace dingosdk::profile_runtime {
struct MusicSong {
    std::string id, artist, title;
    std::string artwork; // content-cache cdn:/ id or registered mod loopback URL; empty when unknown
    std::vector<std::string> playlists;
};

struct MusicPlaylist {
    std::string id;
    std::string name;    // display name from the content cache; empty for native groups
    std::string artwork; // content-cache cdn:/ id or registered mod loopback URL; empty when unknown
    std::vector<std::string> songs;
};

struct MusicCatalog {
    std::vector<MusicSong> songs;
    std::vector<MusicPlaylist> playlists;
};

struct MusicAssetFunctions {
    game::NativeModelFunctions::Lock lock{}, unlock{};
};

MusicAssetFunctions& music_asset_functions();

bool music_asset_type(std::uintptr_t asset, std::uintptr_t vtable_rva);

// Playlists the enabled mods declare in reskate-music.json (see Engine/Vfs/mod_music.h).
std::vector<mods::MusicPlaylist> mod_music_playlists();

bool read_music_catalog(MusicCatalog& result);
}
