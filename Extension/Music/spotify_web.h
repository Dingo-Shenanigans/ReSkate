#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The player's Spotify account, through Spotify's Web API: their playlists, for the music
// screen's Spotify row (spotify_station.h), and starting one of them in their Spotify app.
//
// Spotify gives this to an application registered with it, by its client id. ReSkate has
// none of its own built in: the id is a setting ("music spotify client <id>" in the console), of an
// application whose redirect address is http://127.0.0.1:47824/callback. The player signs in
// once in their browser ("music spotify login"); what is kept is the token that renews the sign-in,
// encrypted for their Windows account, in <Local AppData>\ReSkate\spotify.json. Starting a
// playlist needs Spotify Premium (Spotify's rule) and the Spotify app open.
namespace dingosdk::spotify_web {
inline constexpr unsigned short callback_port = 47824;
// The id Liked Songs goes by here (Spotify does not count it among the playlists).
inline constexpr char liked_songs[] = "liked";
struct Track {
    std::string artist, title, uri;
};
struct Playlist {
    std::string id, name;
    std::string artwork;       // served by the artwork server (music_artwork.h), or empty
    std::vector<Track> tracks; // its first fifty
};
// A song of what Spotify has lined up, with where its album's picture is (Spotify's address).
struct Queued {
    std::string artist, title, uri, cover;
};
struct State {
    bool has_client{}, signed_in{}, busy{};
    std::string user, problem;
    std::size_t playlists{};
};
// Reads what was kept and, when the player had signed in, fetches their playlists. Once, on a
// thread of its own, which does everything below.
void start() noexcept;
void set_client(const std::string& id);
std::string client();
void login();  // opens the browser
void logout(); // forgets the sign-in
void refresh(); // fetches the playlists again
// The playlists as last fetched. Any thread.
std::vector<Playlist> playlists();
// Starts a playlist in the player's Spotify app, at one of its tracks when `track` (its
// Spotify address) is given.
void play(const std::string& playlist, const std::string& track = {});
// What Spotify has lined up, as last fetched: the song that is playing, then those after it
// (Spotify tells of twenty or so). Empty when the player has not signed in. `want_queue` has
// it fetched again, no more often than every few seconds unless `now`; any thread.
std::vector<Queued> queue();
void want_queue(bool now = false);
State state();
}
