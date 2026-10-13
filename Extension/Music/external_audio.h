#pragma once
#include <string>
#include <vector>

// "External Audio" in the game's music screen: the sound of an application the player picks
// (a browser, a media player), heard through the game as Spotify's is (app_audio_capture.h
// takes it from Windows as it is played, and turns the application's own output down while it
// goes through the game). What it is playing, and play, pause and next, are by Windows' media
// session for it when it has one (external_player.h).
//
// Which application is picked in the setup page (spotify_dashboard.h) and kept in
// <Local AppData>\ReSkate\external-audio.json.
namespace dingosdk::external_audio {
// What its station's playlist is called among the Spotify stations (spotify_station.h).
inline constexpr char station[] = "~external";
inline constexpr char station_name[] = "External Audio";
// Its playlist's id in the music screen, and what the ids of its row there begin with.
inline constexpr char playlist_id[] = "external:audio";
inline constexpr char playlist_prefix[] = "external:";
// The application picked ("chrome.exe"), or empty.
std::string app();
void set_app(const std::string& executable);
// The applications that have sound going to the speakers now, to pick from. Any thread.
std::vector<std::string> apps();
// Whether its station is the one that is on.
bool on() noexcept;
void set_on(bool on) noexcept;
}
