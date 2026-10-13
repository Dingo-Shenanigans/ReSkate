#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The cover of what Spotify is playing, as something the game's music screen can show: the
// picture Windows gives for it (a JPEG, usually) written out as a PNG and served by the
// artwork server the mods' covers come from (music_artwork.h).
namespace dingosdk::spotify_cover {
// Asks for `image` (the bytes Windows gave) to be made ready, on a thread of its own.
// `revision` tells one picture from the next; asking again for the same one does nothing.
void want(const std::vector<std::uint8_t>& image, std::uint64_t revision) noexcept;
// Where the last picture made ready is served from, or empty when there is none yet. A new
// address for each picture: the game keeps what it has fetched from an address.
std::string url();
// Where the station's own picture (assets/spotify.png, built into the DLL) is served from, for
// its tile among the playlists; empty when the DLL was built without one.
std::string station_url();
// The same for Liked Songs' picture (assets/liked.png).
std::string liked_url();
// Any picture Windows can read, served as a PNG under `name` ("x.png"); the address, or empty.
// On the caller's thread, which must have COM started.
std::string serve(const std::vector<std::uint8_t>& image, const std::string& name);
}
