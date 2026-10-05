#pragma once

#include <string>
#include <map>
#include <vector>

namespace dingosdk::mods {

// A playlist a mod declares for the game's music screen. `songs` are song ids, the
// "artist - title" the game registers a song under.
struct MusicPlaylist {
    std::string id;   // "mod:<mod name>:<playlist name>"
    std::string name; // shown in the music screen
    std::vector<std::string> songs;
    std::string artwork; // relative PNG path inside the mod; empty means no cover
    std::map<std::string, std::string> song_artwork;
};

// Reads a mod's reskate-music.json:
//   {"schema":1,"playlists":[{"name":"...","songs":["artist - title", ...]}]}
// Optional playlist "artwork" and root "song_artwork" (song-id -> PNG path) use mod-relative paths.
// Invalid optional artwork is ignored, without dropping valid music membership.
// Text must be non-empty, at most 255 bytes and free of control characters; a song entry that is
// not such text is dropped, as are playlists beyond 64 and songs beyond 1024 per playlist.
// Throws std::exception on a document that is not this shape, so the caller can skip the file.
std::vector<MusicPlaylist> parse_music_playlists(const std::string& mod, const std::string& json);

// A portable mod-relative PNG path: no traversal, drive names or alternate streams.
bool music_artwork_path(const std::string& path);

} // namespace dingosdk::mods
