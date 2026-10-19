#pragma once
#include "Engine/Vfs/mod_music.h"
#include "Extension/Profile/runtime_internal.h"
#include <memory>

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

// Playlists the enabled mods declare in reskate-music.json (see Engine/Vfs/mod_music.h), with
// their covers' URLs, as last read. The files are read on a thread of their own, and again when
// one has changed: null until the first read is done. Never waits on the disk.
std::shared_ptr<const std::vector<mods::MusicPlaylist>> mod_music_playlists();
// Starts the first read of them, if it has not been started.
void prepare_mod_music_playlists();

bool read_music_catalog(MusicCatalog& result);
}
