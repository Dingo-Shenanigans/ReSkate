#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Spotify in the game's own music screen. A "Spotify" playlist there plays whatever the
// player's Spotify app is playing, and each of the player's own Spotify playlists, when they
// have signed in (spotify_web.h), is a playlist there that starts it in the app. The sound is
// Spotify's, heard through the game (music_inject.h); the song, its cover and the next button
// are Spotify's too, through Windows' media session (external_player.h).
//
// The game only takes a playlist that has one of its own registered songs in it, so each of
// these is given one, a different one each: that song being a playlist's only member is how
// the game's binding of it is known (the game's number for a playlist is not the hash of its
// id: tried on 2026-10-11). The song's own sound is what is replaced by Spotify's.
namespace dingosdk::spotify_station {
inline constexpr char playlist_id[] = "reskate:spotify";
inline constexpr char playlist_name[] = "Spotify";
// The ids of the player's own playlists in the music screen begin with this, their Spotify
// id after it.
inline constexpr char playlist_prefix[] = "spotify:";
// Whether the player uses Spotify here at all (an account on the game's device, or one kept
// for it, or a sign-in for their playlists): its tiles and its row are only there when so.
bool spotify_used() noexcept;
// What the plain Spotify station's playlist is called among the stations (it starts none).
inline constexpr char plain_station[] = "~plain";
// Whether a playlist of the music screen is one of these (the row they are shown in).
bool is_station(std::string_view id) noexcept;

struct Station {
    std::vector<std::array<unsigned char, 16>> rows; // its members, as the game identifies a song
    std::vector<std::string> tracks;                 // the Spotify track each is, or empty
    std::array<unsigned char, 16> carrier{}; // the song of the game's whose sound is replaced: a row, or one added when it is bound
    std::uintptr_t metadata{};               // the carrier's artist and title, as the game holds them
    std::string playlist;                    // the Spotify playlist it starts, or empty: whatever is playing
    std::string id;                          // its playlist's id in the music screen
};
// Set each time the catalog is read (local_music_assets.cpp). `table` is what a metadata
// asset begins with, by which one is known to be still there.
void set_stations(std::vector<Station> stations, std::uintptr_t table);
bool has_stations() noexcept;
// Their playlists' ids in the music screen.
std::vector<std::string> playlist_ids();
// Which of them a playlist of these `count` members (16 bytes each) is, or -1. `add` is set
// when its carrier is not among them and is to be added (`carrier`, 16 bytes). Any thread.
int station_of(const unsigned char* members, std::uint32_t count, unsigned char* carrier, bool& add) noexcept;
// The game began to play a song of its own (spotify_rows.cpp), by its metadata asset: while a
// station is on its sound is Spotify's (a speaker's in the world, a song grabbed from one), so
// its artist and title are made to read as Spotify's as well. Any thread.
void song_started(std::uintptr_t metadata) noexcept;
// Every song of the game's as it begins, station or none, by its channel: what each channel
// is playing is made to read as Spotify's when a station comes on (the speakers' songs then
// playing among them, which began before it). Any thread.
void song_begun(std::uint32_t channel, std::uintptr_t metadata) noexcept;
// Which of a station's rows a song is, or -1. Any thread.
int row_of(int station, const unsigned char* song) noexcept;
// The game began to play a row of ours (spotify_rows.cpp), which the player picked: Spotify
// is started at its track. Any thread.
void row_started(const unsigned char* row) noexcept;
// From the playlist bind hook, on whichever thread the game binds on.
// `station`: which of them the playlist bound is, or -1.
void bound(std::uint32_t channel, bool music_channel, std::uint32_t playlist, std::uint32_t members, int station) noexcept;
// While one is on, its song's artist and title are made to read as what Spotify is playing,
// which is what the game shows as now playing. They are put back when it is left, and before
// the catalog is read from them.
void restore_presentation() noexcept;
// Before the map changes: every song reads as itself again, and none is held on to (their
// assets go with the map). The thread that changes the level.
void before_level_transition() noexcept;
// The game's music player chose a song while one is on (local_music_playback.cpp:
// music_select_next_hook). `queue` is the player's queue, `asked` whether it was asked for its
// next song (its flags for that, at +0xf1 and +0xf2, are still up), which is Spotify's next
// unless it was this that asked (refresh_now_playing).
void song_chosen(std::uintptr_t queue, bool asked) noexcept;
// Has the game start its song again, which is when it looks up what it shows as now playing:
// the cover of a song Spotify has gone on to is only shown then. The client thread.
void refresh_now_playing() noexcept;
// Whether the game's music player is on one of them now, and which (-1: none). Any thread.
bool active() noexcept;
int current() noexcept;
// Whether a menu of the game's is up (the music screen is one), as last seen while a station
// is on (client_tick.cpp).
void set_music_screen(bool open) noexcept;
bool music_screen() noexcept;
// The player pressed next on a channel other than the station's (the one a grabbed speaker's
// music plays on): Spotify's next, as on the station's own. Any thread.
void next_pressed() noexcept;
// A speaker's music was grabbed and the player kept on Spotify (local_music_playback.cpp):
// the next song the game goes on to for it is not Spotify's next. Any thread.
void grabbed() noexcept;
// Whether one is on or was until a quarter of a minute ago (what is traced into the log
// while grabbing a speaker's music is being worked out).
bool recent() noexcept;
// The music channel it is on, or ~0u.
std::uint32_t channel() noexcept;
// The client thread, each frame: starts or pauses the outside player as one is entered or left.
void tick() noexcept;
// For the console.
std::string status();
}
