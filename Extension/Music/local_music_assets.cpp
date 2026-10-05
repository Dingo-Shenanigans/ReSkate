#include "Extension/Customization/local_customization_runtime.h"
#include "local_music_assets.h"
#include "music_artwork.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/local_music.h"
#include "Engine/Vfs/content_catalogs.h"
#include "Engine/Vfs/mod_catalog.h"
#include "Engine/Vfs/mod_music.h"
#include "Extension/Profile/runtime_internal.h"
#include <fstream>
#include <iterator>
#include <sstream>

namespace dingosdk::profile_runtime {
// Snapshot the game's registered music assets, as the cosmetic catalog does

// for OwnableData. No generated song table or invented playlist membership.

MusicAssetFunctions& music_asset_functions() {
    // The game reaches these Win32 APIs through its IAT thunks.
    // This is not the UI model's different, custom lock implementation.
    static MusicAssetFunctions functions{
        [](std::uintptr_t p) { EnterCriticalSection(reinterpret_cast<LPCRITICAL_SECTION>(p)); },
        [](std::uintptr_t p) { LeaveCriticalSection(reinterpret_cast<LPCRITICAL_SECTION>(p)); }};
    return functions;
}

bool music_asset_type(std::uintptr_t asset, std::uintptr_t vtable_rva) {
    std::uintptr_t vtable{};
    return asset && read(asset, vtable) && vtable == local_runtime().base + vtable_rva;
}

// reskate-music.json in each enabled mod: {"schema":1,"playlists":[{"name":..,
// "songs":["artist - title",..]}]}.
// Read on every call: it only runs while the music page waits for its catalog. A bad
// file is logged and skipped; its songs still play from the master playlist.
std::vector<mods::MusicPlaylist> mod_music_playlists() {
    std::vector<mods::MusicPlaylist> result;
    for (const auto& mod : mods::catalog().mods) {
        const auto path = mod.directory / L"reskate-music.json";
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) continue;
        try {
            std::ifstream in(path, std::ios::binary);
            auto playlists = mods::parse_music_playlists(mod.name, std::string((std::istreambuf_iterator<char>(in)), {}));
            for (auto& playlist : playlists) {
                if (!playlist.artwork.empty()) playlist.artwork = mod_music_artwork_url(mod.directory, playlist.artwork);
                for (auto& [id, artwork] : playlist.song_artwork) artwork = mod_music_artwork_url(mod.directory, artwork);
            }
            std::move(playlists.begin(), playlists.end(), std::back_inserter(result));
        } catch (const std::exception& failure) {
            dingosdk::logging::event(dingosdk::logging::Channel::music,
                dingosdk::Json{{"event", "mod_music_playlists_skipped"}, {"mod", mod.name}, {"reason", failure.what()}}.dump().c_str());
        }
    }
    return result;
}

bool read_music_catalog(MusicCatalog& result) {
    const auto base = local_runtime().base;
    auto& native = music_asset_functions();
    std::uintptr_t manager{};
    if (!native.lock || !native.unlock || !read(base + addr::local_music::asset_manager, manager) || !manager) return false;
    // Native registration/unregistration uses this same lock.
    // Copy data while held; never retain a borrowed asset pointer in the UI.
    native.lock(manager + 0x20);
    struct Unlock {
        game::NativeModelFunctions::Lock function;
        std::uintptr_t address;
        ~Unlock() { function(address); }
    } unlock{native.unlock, manager + 0x20};
    std::uintptr_t current{}, buckets{}, sentinel{};
    std::uint32_t bucket_count{}, count{};
    if (!read(base + addr::local_music::asset_manager, current) || current != manager ||
        !read(manager + 0x78, buckets) || !buckets ||
        !read(manager + 0x80, bucket_count) || !bucket_count || bucket_count > 16384 ||
        !read(manager + 0x84, count) || !count || count > 8192 ||
        !read(buckets + bucket_count * 8ULL, sentinel)) return false;

    MusicCatalog snapshot;
    snapshot.songs.reserve(count);
    std::map<std::string, std::vector<std::string>, std::less<>> playlists;
    std::set<std::uintptr_t> visited;
    std::set<std::string, std::less<>> song_ids;
    for (std::uint32_t bucket = 0; bucket < bucket_count; ++bucket) {
        std::uintptr_t node{};
        if (!read(buckets + bucket * 8ULL, node)) return false;
        while (node && node != sentinel) {
            if (visited.size() >= count || !visited.insert(node).second) return false;
            std::uint32_t hash{};
            std::uintptr_t graph{}, metadata{}, artist{}, title{}, tags{};
            if (!read(node, hash) || hash % bucket_count != bucket ||
                !read(node + 8, graph)) return false;
            graph &= ~std::uintptr_t{4};
            if (!music_asset_type(graph, addr::local_music::graph_asset_vtable) || !read(graph + 0x20, metadata)) return false;
            metadata &= ~std::uintptr_t{4};
            if (!music_asset_type(metadata, addr::local_music::metadata_vtable) || !read(metadata + 0x18, artist) ||
                !read(metadata + 0x20, title) || !read(graph + 0x28, tags)) return false;
            MusicSong song;
            if (!cosmetic_text(artist, song.artist) || !cosmetic_text(title, song.title) ||
                song.artist.empty() || song.title.empty()) return false;
            // Exact native registration input, NOT MusicGraph.NameHash.
            // The native map already resolves duplicate artist/title pairs.
            song.id = song.artist + " - " + song.title;
            if (cosmetic_hash(song.id) != hash || !song_ids.insert(song.id).second) return false;

            std::uint32_t tag_count{};
            if (tags && !cosmetic_array(tags, 8, 128, tag_count)) return false;
            for (std::uint32_t index = 0; index < tag_count; ++index) {
                std::uintptr_t tag{}, enumeration{}, selection{}, path{}, name{};
                if (!read(tags + index * 8ULL, tag)) return false;
                tag &= ~std::uintptr_t{4};
                if (!music_asset_type(tag, addr::local_music::tag_vtable) || !read(tag + 0x18, enumeration) || !read(tag + 0x20, selection)) return false;
                enumeration &= ~std::uintptr_t{4}; selection &= ~std::uintptr_t{4};
                std::string enum_path, playlist;
                if (!enumeration || !read(enumeration + 0x18, path) ||
                    !cosmetic_text(path, enum_path)) return false;
                // Other music tags are not playlist membership declarations.
                if (_stricmp(enum_path.c_str(), "Audio/Music/Playlist/DGO_MUS_Playlist_FilterTags") != 0)
                    continue;
                if (!music_asset_type(selection, addr::local_music::tag_selection_vtable) || !read(selection + 0x18, name) ||
                    !cosmetic_text(name, playlist) || playlist.empty()) return false;
                if (playlist == "Invalid" || playlist == "ChallengeMusic_Test") continue;
                if (std::find(song.playlists.begin(), song.playlists.end(), playlist) == song.playlists.end()) {
                    song.playlists.push_back(playlist);
                    playlists[playlist].push_back(song.id);
                }
            }
            snapshot.songs.push_back(std::move(song));
            if (!read(node + 0x10, node)) return false;
        }
    }
    std::uint32_t current_count{};
    std::uintptr_t current_buckets{};
    if (visited.size() != count || !read(base + addr::local_music::asset_manager, current) || current != manager ||
        !read(manager + 0x78, current_buckets) || current_buckets != buckets ||
        !read(manager + 0x84, current_count) || current_count != count) return false;
    // The registered assets only declare the playlists the game ships offline
    // (the Universal production library); the licensed stations live in the
    // content-cache music chunk. Merge that membership, intersected with the
    // songs actually registered, so no unregistered id can ever be referenced.
    const auto native_playlists = playlists.size();
    const auto& cache = content_cache::catalogs();
    std::map<std::string, std::string, std::less<>> playlist_names, playlist_artwork;
    std::size_t cache_playlists = 0, mod_playlists = 0, matched = 0, unmatched = 0, added = 0;
    std::size_t song_artwork = 0;
    for (auto& song : snapshot.songs)
        if (const auto found = cache.music_song_artwork.find(song.id); found != cache.music_song_artwork.end()) {
            song.artwork = found->second;
            ++song_artwork;
        }
    std::map<std::string, std::size_t, std::less<>> index;
    for (std::size_t i = 0; i < snapshot.songs.size(); ++i) index.emplace(snapshot.songs[i].id, i);
    const auto merge = [&](const std::string& id, const std::string& name, const std::string& artwork,
                           const std::vector<std::string>& tracks) {
        auto& members = playlists[id];
        for (const auto& track : tracks) {
            const auto found = index.find(track);
            if (found == index.end()) { ++unmatched; continue; }
            ++matched;
            auto& song = snapshot.songs[found->second];
            if (std::find(song.playlists.begin(), song.playlists.end(), id) != song.playlists.end())
                continue;
            song.playlists.push_back(id);
            members.push_back(song.id);
            ++added;
        }
        if (!members.empty() && !name.empty()) playlist_names[id] = name;
        if (!members.empty() && !artwork.empty()) playlist_artwork[id] = artwork;
    };
    for (const auto& [id, entry] : cache.music_playlists) {
        ++cache_playlists;
        merge(id, entry.name, entry.artwork, entry.tracks);
    }
    // Playlists music mods declare in reskate-music.json (ReSkateMusicPacker writes it):
    // a name and the ids of songs the mod adds, which carry no station tag of their own.
    for (const auto& playlist : mod_music_playlists()) {
        ++mod_playlists;
        merge(playlist.id, playlist.name, playlist.artwork, playlist.songs);
        for (const auto& [id, artwork] : playlist.song_artwork)
            if (const auto found = index.find(id); found != index.end() && !artwork.empty() && snapshot.songs[found->second].artwork.empty()) {
                snapshot.songs[found->second].artwork = artwork;
                ++song_artwork;
            }
    }
    for (auto it = playlists.begin(); it != playlists.end();)
        it = it->second.empty() ? playlists.erase(it) : std::next(it);
    {
        std::ostringstream event;
        event << "{\"event\":\"music_catalog_membership\",\"songs\":" << snapshot.songs.size()
              << ",\"native_playlists\":" << native_playlists
              << ",\"cache_playlists\":" << cache_playlists
              << ",\"mod_playlists\":" << mod_playlists
              << ",\"matched\":" << matched << ",\"unmatched\":" << unmatched
              << ",\"added\":" << added << ",\"artwork\":" << song_artwork << ",\"playlists\":" << playlists.size() << "}";
        dingosdk::logging::event(dingosdk::logging::Channel::music, event.str());
    }
    // Registry order is bucket order, not authored ordering. Use stable IDs for
    // deterministic UI order; do not claim this is the original service order.
    // Authored mod playlists keep their defined track positioning.
    std::sort(snapshot.songs.begin(), snapshot.songs.end(), [](const auto& a, const auto& b) {
        return a.id < b.id;
    });
    for (auto& [id, songs] : playlists) {
        if (!id.starts_with("mod:"))
            std::sort(songs.begin(), songs.end());
        MusicPlaylist row;
        row.id = id;
        row.songs = std::move(songs);
        if (const auto found = playlist_names.find(id); found != playlist_names.end())
            row.name = found->second;
        if (const auto found = playlist_artwork.find(id); found != playlist_artwork.end())
            row.artwork = found->second;
        snapshot.playlists.push_back(std::move(row));
    }
    result = std::move(snapshot);
    return true;
}
}
