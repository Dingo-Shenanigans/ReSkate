#pragma once
#include <cstdint>
#include <string>

// Songs of our own in the game's registry of music, for the rows of the Spotify playlists in
// the music screen (spotify_station.h). A row there is a registered song, shown by the artist
// and title of its asset: these are copies of a registered song's asset with an artist and
// title of their own, registered by the game's own routine. They are in no playlist of the
// game's, so the game never chooses one to play. (Being tried.)
namespace dingosdk::spotify_rows {
// A song of this artist and title, registered with the game's music manager when it is not
// yet: its id in the music screen ("artist - title"), or empty when it could not be made.
// The thread that reads the catalog; not with the manager's lock held.
std::string ensure(std::uintptr_t base, std::uintptr_t manager, const std::string& artist, const std::string& title);
// Whether a row of that artist and title has been registered already (it then costs the music
// screen nothing more to use it again).
bool known(const std::string& artist, const std::string& title);
// The id ("artist - title") of the row known to the game by these 16 bytes, or empty.
std::string id_of(const unsigned char* row);
// Watches for the game beginning to play one of these (the player picked its row), which
// spotify_station is told of. Once.
bool install(std::uintptr_t base) noexcept;
// Adds a song (the 16 bytes the game knows it by) to a playlist of the game's, by the
// playlist's number, with the game's own routine. Any thread.
bool add_member(std::uintptr_t base, std::uintptr_t manager, std::uint32_t playlist, const unsigned char* song) noexcept;
}
