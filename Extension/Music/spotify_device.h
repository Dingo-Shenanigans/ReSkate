#pragma once
#include "external_player.h"
#include <string>

// The game as a device in the player's Spotify app ("ReSkate"), by way of ReSkateSpotify.exe
// (SpotifyHelper/, built on librespot), which is started beside ReSkate.dll. Spotify plays to
// it as it would to a speaker: its sound comes to the game as it is (app_audio_capture.h's
// buffer is filled with it, in the place of what is captured from the Spotify app), with the
// song's name and cover, and the game's play, pause and next go to it.
//
// Who the account is comes from the Spotify app: the player picks "ReSkate" among its devices
// once, on the same network, and it is kept. Until an account is on it, the Spotify station
// goes on working through the Spotify app on this computer (external_player.h).
namespace dingosdk::spotify_device {
// Starts the helper, once; nothing when it is not beside the DLL.
void start() noexcept;
// Whether a Spotify account is on the device: it is then what the station plays and shows.
bool connected() noexcept;
// Whether it has an account or one kept to sign in with: the player uses Spotify here.
bool in_use() noexcept;
// What it is playing, as external_player gives it.
external_player::Track now();
std::uint64_t revision() noexcept;
void send(external_player::Command command) noexcept;
// Has it play here, taking over from wherever Spotify is playing.
void activate() noexcept;
// The plain Spotify station: whatever Spotify was playing goes on here; when it was playing
// nothing, the player's Liked Songs are played, in no order.
void start_radio() noexcept;
// A playlist ("spotify:playlist:..."), at one of its tracks when given; or these tracks, at one.
void load(const std::string& context, const std::string& track = {}) noexcept;
void load_tracks(const std::string& tracks_spaced, unsigned index) noexcept;
// The library asked for the whole program to be ended (music_inject.cpp stops that): the device
// is stopped for this run, and an account it will not take is forgotten.
void library_gave_up() noexcept;
// For the console.
std::string status();
}
