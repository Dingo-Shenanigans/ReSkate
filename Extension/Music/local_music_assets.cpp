#include "Extension/Customization/local_customization_runtime.h"
#include "Extension/Music/spotify_station.h"
#include "Extension/Music/spotify_cover.h"
#include "Extension/Music/spotify_web.h"
#include "Extension/Music/spotify_rows.h"
#include "Extension/Music/external_audio.h"
#include "Extension/Music/youtube_music.h"
#include <cstring>
#include "Extension/Music/local_music_ui.h"
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
#include <memory>
#include <atomic>
#include <mutex>
#include <sstream>
#include <thread>

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

namespace {
// reskate-music.json in each enabled mod: {"schema":1,"playlists":[{"name":..,
// "songs":["artist - title",..]}]}. A bad file is logged and skipped; its songs still play
// from the master playlist.
std::vector<mods::MusicPlaylist> read_mod_music_playlists() {
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

// What the files said, as last read, and when each was written then. Reading them takes a
// path check for every song's cover and, the first time, its PNG: seconds of disk for a large
// pack. That was done on the game's frame, holding native_mutex (which the park tick waits
// on), every 250 ms while the music page waited for its catalog, until the game's watchdog
// closed the game ("GameStall", 2026-10-15). So they are read on a thread of their own, and
// read again only when one of the files has changed.
struct ModMusic {
    std::mutex mutex;
    std::shared_ptr<const std::vector<mods::MusicPlaylist>> ready; // null until the first read is done
    std::vector<std::filesystem::file_time_type> stamps;            // of the files `ready` was read from
    bool reading{};
    std::uint64_t looked{};                                         // GetTickCount64, the last read was started
};
ModMusic& mod_music() {
    static auto* music = new ModMusic;
    return *music;
}
std::vector<std::filesystem::file_time_type> mod_music_stamps() {
    std::vector<std::filesystem::file_time_type> stamps;
    for (const auto& mod : mods::catalog().mods) {
        std::error_code error;
        const auto stamp = std::filesystem::last_write_time(mod.directory / L"reskate-music.json", error);
        stamps.push_back(error ? std::filesystem::file_time_type::min() : stamp);
    }
    return stamps;
}
void read_mod_music() noexcept {
    auto& music = mod_music();
    std::shared_ptr<const std::vector<mods::MusicPlaylist>> read;
    std::vector<std::filesystem::file_time_type> stamps;
    try {
        stamps = mod_music_stamps();
        bool same{};
        {
            std::lock_guard lock(music.mutex);
            same = music.ready && stamps == music.stamps;
        }
        if (!same) read = std::make_shared<const std::vector<mods::MusicPlaylist>>(read_mod_music_playlists());
    } catch (...) {}
    std::lock_guard lock(music.mutex);
    if (read) {
        music.ready = std::move(read);
        music.stamps = std::move(stamps);
    } else if (!music.ready) {
        // (It could not be read at all: the page is not kept waiting on it.)
        music.ready = std::make_shared<const std::vector<mods::MusicPlaylist>>();
    }
    music.reading = false;
}
// Starts a read when none is under way and there is none yet, or (`again`) the last began
// two seconds ago or more. Called with the mutex held.
void start_mod_music_read(ModMusic& music, bool again) {
    const auto now = GetTickCount64();
    if (music.reading || (music.ready && (!again || now - music.looked < 2000))) return;
    music.reading = true;
    music.looked = now;
    try {
        std::thread(read_mod_music).detach();
    } catch (...) {
        music.reading = false;
    }
}
} // namespace

std::shared_ptr<const std::vector<mods::MusicPlaylist>> mod_music_playlists() {
    auto& music = mod_music();
    std::lock_guard lock(music.mutex);
    start_mod_music_read(music, true);
    return music.ready;
}
void prepare_mod_music_playlists() {
    auto& music = mod_music();
    {
        std::lock_guard lock(music.mutex);
        start_mod_music_read(music, false);
    }
    // The Spotify stations' own pictures are written out and served the first time a catalog is
    // read, on the client thread: written now, on a thread of their own, they are ready by then.
    static std::atomic<bool> covers{};
    if (!covers.exchange(true)) {
        try {
            std::thread([] {
                try {
                    (void)spotify_cover::station_url();
                    (void)spotify_cover::liked_url();
                } catch (...) {}
            }).detach();
        } catch (...) {}
    }
}

bool read_music_catalog(MusicCatalog& result) {
    const auto base = local_runtime().base;
    auto& native = music_asset_functions();
    std::uintptr_t manager{};
    if (!native.lock || !native.unlock || !read(base + addr::local_music::asset_manager, manager) || !manager) return false;
    // (The Spotify station's song reads as itself here, not as what Spotify is playing.)
    spotify_station::restore_presentation();
    // The rows of the Spotify playlists (spotify_rows.h): one each, that says what picking the
    // playlist does. Registered before the songs are read, so they are among them.
    // Spotify's own (the plain station, then the player's playlists) only when the player uses
    // Spotify here: there is no tile of it, and no row, for one who does not.
    std::vector<spotify_web::Playlist> spotify_playlists;
    if (spotify_station::spotify_used()) {
        spotify_playlists.push_back({spotify_station::plain_station, spotify_station::playlist_name, {}, {}});
        for (auto& theirs : spotify_web::playlists()) spotify_playlists.push_back(std::move(theirs));
    }
    // (And the External Audio station, external_audio.h, when an application is picked for it:
    // it is in a row of its own in the music screen.)
    if (!external_audio::app().empty()) spotify_playlists.push_back({external_audio::station, external_audio::station_name, {}, {}});
    // (And YouTube Music's playlists, the player's folders of music and their radio stations,
    // youtube_music.h, each with its songs as its rows.)
    {
        std::size_t number{};
        for (const auto& theirs : youtube_music::playlists()) {
            spotify_web::Playlist playlist{youtube_music::station_prefix + std::to_string(number++), theirs.name, theirs.artwork, {}};
            if (!theirs.usable) continue;
            for (const auto& song : theirs.songs) playlist.tracks.push_back({song.artist, song.title, youtube_music::track_prefix + song.id});
            spotify_playlists.push_back(std::move(playlist));
        }
    }
    // For each station, its rows' ids and the Spotify track each is (none for a row that only
    // says what picking the playlist does).
    std::vector<std::vector<std::pair<std::string, std::string>>> spotify_row_ids;
    // The music screen takes only so many songs, and so many places in playlists, and with more
    // is given none at all (music_ui_catalog_valid). Rows are registered while there is room,
    // under those by a margin for the game's own; the rest of a playlist is left out of its rows
    // (it still plays through: only its list is shorter).
    std::uint32_t registered{};
    read(manager + 0x84, registered);
    std::size_t room = registered + 200 < music_ui_most_songs ? music_ui_most_songs - 200 - registered : 0;
    std::size_t places = music_ui_most_places / 2, left_out{};
    for (const auto& theirs : spotify_playlists) {
        auto& rows = spotify_row_ids.emplace_back();
        for (const auto& track : theirs.tracks) {
            // (Its id, "artist - title", has to be one the screen takes as well.)
            if (!music_ui_text(track.artist, 200) || !music_ui_text(track.title, 200) || track.artist.size() + track.title.size() + 3 > 255) continue;
            const bool known = spotify_rows::known(track.artist, track.title);
            if (!places || (!known && !room)) {
                ++left_out;
                continue;
            }
            if (auto id = spotify_rows::ensure(base, manager, track.artist, track.title); !id.empty() &&
                std::none_of(rows.begin(), rows.end(), [&](const auto& row) { return row.first == id; })) {
                rows.emplace_back(std::move(id), track.uri);
                --places;
                if (!known) --room;
            }
        }
        if (theirs.id == spotify_station::plain_station) rows.emplace_back(spotify_rows::ensure(base, manager, "Spotify", "Start Radio"), std::string());
        else if (rows.empty() && music_ui_text(theirs.name, 200)) rows.emplace_back(theirs.id == external_audio::station ? spotify_rows::ensure(base, manager, external_audio::station_name, "Start") : spotify_rows::ensure(base, manager, "Spotify", "Play " + theirs.name), std::string());
    }
    if (static std::size_t said{}; left_out != said) {
        said = left_out;
        if (left_out)
            dingosdk::logging::log(dingosdk::logging::Level::warning, dingosdk::logging::Channel::music,
                "Music screen: {} song(s) of your playlists are not listed: it has room for about {} songs in all. They still play; fewer or shorter playlists list them all.",
                left_out, music_ui_most_songs);
    }
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
    // Each song's identity as playlists hold it (the 16 bytes before its asset, when it has one).
    std::map<std::string, std::array<unsigned char, 16>, std::less<>> identities;
    std::map<std::string, std::uintptr_t, std::less<>> metadata_of;
    // Songs the game ships the sound of (those its own offline playlists hold), which a
    // Spotify station's song has to be: one whose sound is not there is never started.
    std::set<std::string, std::less<>> shipped;
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
            // What the song is. One that does not read as a song is left out of the screen, and
            // said once, rather than taking every song with it: a single song with no artist or
            // title (a music pack's, 2026-10-15) left the music screen empty with nothing in the
            // log. (A registry changing under the read is still caught by the check after it.)
            MusicSong song;
            const char* problem = [&]() -> const char* {
                if (!music_asset_type(graph, addr::local_music::graph_asset_vtable) || !read(graph + 0x20, metadata)) return "it is not a song's asset";
                metadata &= ~std::uintptr_t{4};
                if (!music_asset_type(metadata, addr::local_music::metadata_vtable) || !read(metadata + 0x18, artist) ||
                    !read(metadata + 0x20, title) || !read(graph + 0x28, tags)) return "its artist and title could not be read";
                if (!cosmetic_text(artist, song.artist) || !cosmetic_text(title, song.title)) return "its artist or title is not text";
                if (song.artist.empty() || song.title.empty()) return "it has no artist or no title";
                // Exact native registration input, NOT MusicGraph.NameHash.
                // The native map already resolves duplicate artist/title pairs.
                song.id = song.artist + " - " + song.title;
                if (cosmetic_hash(song.id) != hash) return "it is registered under another name";
                if (song_ids.contains(song.id)) return "another song has the same artist and title";

                std::uint32_t tag_count{};
                if (tags && !cosmetic_array(tags, 8, 128, tag_count)) return "its tags could not be read";
                for (std::uint32_t index = 0; index < tag_count; ++index) {
                    std::uintptr_t tag{}, enumeration{}, selection{}, path{}, name{};
                    if (!read(tags + index * 8ULL, tag)) return "its tags could not be read";
                    tag &= ~std::uintptr_t{4};
                    if (!music_asset_type(tag, addr::local_music::tag_vtable) || !read(tag + 0x18, enumeration) || !read(tag + 0x20, selection))
                        return "a tag of it is not a music tag";
                    enumeration &= ~std::uintptr_t{4}; selection &= ~std::uintptr_t{4};
                    std::string enum_path, playlist;
                    if (!enumeration || !read(enumeration + 0x18, path) ||
                        !cosmetic_text(path, enum_path)) return "a tag of it names nothing";
                    // Other music tags are not playlist membership declarations.
                    if (_stricmp(enum_path.c_str(), "Audio/Music/Playlist/DGO_MUS_Playlist_FilterTags") != 0)
                        continue;
                    if (!music_asset_type(selection, addr::local_music::tag_selection_vtable) || !read(selection + 0x18, name) ||
                        !cosmetic_text(name, playlist) || playlist.empty()) return "a playlist tag of it names no playlist";
                    if (playlist == "Invalid" || playlist == "ChallengeMusic_Test") continue;
                    if (std::find(song.playlists.begin(), song.playlists.end(), playlist) == song.playlists.end())
                        song.playlists.push_back(playlist);
                }
                return nullptr;
            }();
            if (problem) {
                static std::set<std::string, std::less<>> said;
                const auto what = song.artist.empty() && song.title.empty() ? std::string("a song") : "\"" + song.artist + " - " + song.title + "\"";
                // (Its asset's name, at +0x18 as for the tags' assets, says which mod it came from.)
                std::string asset;
                if (std::uintptr_t name{}; !graph || !read(graph + 0x18, name) || !cosmetic_text(name, asset) || asset.empty()) asset = "unnamed";
                if (said.size() < 64 && said.insert(what + problem + asset).second)
                    dingosdk::logging::log(dingosdk::logging::Level::warning, dingosdk::logging::Channel::music,
                        "Music screen: {} is left out of it: {} (asset {}).", what, problem, asset);
                if (!read(node + 0x10, node)) return false;
                continue;
            }
            song_ids.insert(song.id);
            for (const auto& playlist : song.playlists) playlists[playlist].push_back(song.id);
            if (std::uint32_t flags{}; graph >= 16 && read(graph + 0x14, flags) && (flags & 0x100)) {
                std::array<unsigned char, 16> identity{};
                if (dingosdk::memory::peek_bytes(graph - 16, identity.data(), identity.size()) && identities.emplace(song.id, identity).second)
                    metadata_of[song.id] = metadata;
            }
            if (!song.playlists.empty()) shipped.insert(song.id);
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
    // (Still being read for the first time: the page waits for them, as for the game's songs.)
    const auto mod_lists = mod_music_playlists();
    if (!mod_lists) return false;
    for (const auto& playlist : *mod_lists) {
        ++mod_playlists;
        merge(playlist.id, playlist.name, playlist.artwork, playlist.songs);
        for (const auto& [id, artwork] : playlist.song_artwork)
            if (const auto found = index.find(id); found != index.end() && !artwork.empty() && snapshot.songs[found->second].artwork.empty()) {
                snapshot.songs[found->second].artwork = artwork;
                ++song_artwork;
            }
    }
    // The Spotify stations (spotify_station.h): a playlist has to hold a registered song to be
    // one, so each is given one of them, a different one each: the first is whatever Spotify
    // is playing, the rest the player's own Spotify playlists. What plays while one is selected
    // is Spotify, not that song. Each is known again, when the game binds it, by that song
    // being its only member.
    {
        std::vector<spotify_station::Station> stations;
        const auto& own = spotify_playlists;
        std::size_t next{};
        std::string said;
        for (const auto& [song, identity] : identities) {
            if (next >= own.size()) break;
            if (!shipped.empty() && !shipped.contains(song)) continue;
            const auto metadata = metadata_of.find(song);
            if (metadata == metadata_of.end()) continue;
            std::string id = spotify_station::playlist_id, name = spotify_station::playlist_name, artwork = spotify_cover::station_url(), playlist;
            const auto entry = next++;
            if (own[entry].id != spotify_station::plain_station) {
                const auto& theirs = own[entry];
                if (!music_ui_text(theirs.name)) continue; // (a name the music screen cannot show)
                // (The music screen shows playlists in the order of their ids: Liked Songs' is
                // made to come first of these.)
                const bool liked = theirs.id == spotify_web::liked_songs;
                const bool tube = theirs.id.starts_with(youtube_music::station_prefix);
                id = theirs.id == external_audio::station ? std::string(external_audio::playlist_id)
                    : tube ? youtube_music::tile_id(std::strtoul(theirs.id.c_str() + std::strlen(youtube_music::station_prefix), nullptr, 10))
                    : spotify_station::playlist_prefix + (liked ? "!" + theirs.id : theirs.id);
                name = theirs.name;
                // (Spotify's own picture is for Spotify's playlists that have none.)
                artwork = liked && !spotify_cover::liked_url().empty() ? spotify_cover::liked_url()
                    : theirs.artwork.empty() && !tube && theirs.id != external_audio::station ? spotify_cover::station_url() : theirs.artwork;
                playlist = theirs.id;
            }
            // Its rows are the songs registered for it, and the game's song is added as it is
            // bound (local_music_playback.cpp); without any the game's song is its row.
            spotify_station::Station station{{}, {}, identity, metadata->second, playlist, id};
            std::vector<std::string> members;
            for (const auto& [row, track] : spotify_row_ids[entry])
                if (const auto found = row.empty() ? identities.end() : identities.find(row); found != identities.end() && index.contains(row)) {
                    station.rows.push_back(found->second);
                    station.tracks.push_back(track);
                    members.push_back(row);
                }
            if (members.empty()) {
                station.rows.push_back(identity);
                station.tracks.emplace_back();
                members.push_back(song);
            }
            merge(id, name, artwork, members);
            said += (said.empty() ? "" : "; ") + name + ": " + std::to_string(members.size()) + " row(s) over " + song;
            stations.push_back(std::move(station));
        }
        spotify_station::set_stations(std::move(stations), base + addr::local_music::metadata_vtable);
        static std::string last;
        if (said != last) {
            last = said;
            dingosdk::logging::log(dingosdk::logging::Level::debug, dingosdk::logging::Channel::music, "Spotify stations and the songs they were given: {}.", said.empty() ? "none" : said);
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
