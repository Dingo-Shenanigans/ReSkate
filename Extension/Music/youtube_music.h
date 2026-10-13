#pragma once
#include "external_player.h"
#include <string>
#include <vector>

// The playlists of the player's own in the game's music screen that are not Spotify's, each a
// tile there with its songs as rows, in a row of its kind:
//   "YouTube Music"   videos and playlists added by their links; a song's sound is fetched by
//                     yt-dlp, which this runs
//   "Local Files"     folders of music on this computer (MP3, M4A, AAC, Ogg, FLAC, WAV)
//   "Online Radio"    internet radio stations, each a tile of its own, found by name in the
//                     radio-browser.info directory or added by their address
// All are added in the setup page (spotify_dashboard.h), decoded by the library that is built
// into the DLL (SpotifyHelper/src/stream.rs), and heard through the game as Spotify's is.
//
// yt-dlp is not part of ReSkate: the player has it fetched from the setup page, from its own
// releases on GitHub, into <Local AppData>\ReSkate\bin. What is added is kept in
// <Local AppData>\ReSkate\youtube-music.json.
namespace dingosdk::youtube_music {
// What a station's playlist is called among the stations (spotify_station.h): this and its
// number; what a row's track is called: this and the song's id.
inline constexpr char station_prefix[] = "~yt:";
inline constexpr char track_prefix[] = "yt:";
// What the ids of each kind's playlists in the music screen begin with.
inline constexpr char playlist_prefix[] = "youtube:";
inline constexpr char files_prefix[] = "localfiles:";
inline constexpr char radio_prefix[] = "radio:";

enum class Kind { youtube, files, radio };
struct Song {
    std::string id, artist, title; // the id is the video's, or one made of the source
    std::string source;            // the file, or the station's address; none for a video
};
struct Playlist {
    std::string name, url;  // the link it was added by; the folder; the station's picture
    std::string artwork;    // served by the artwork server, or empty
    std::vector<Song> songs;
    Kind kind{};
    bool usable{true}; // (a YouTube playlist is not, until yt-dlp is there)
};
// A radio station found by its name.
struct Found {
    std::string name, address, icon, about;
};
struct State {
    bool available{}; // the DLL was built with the library that decodes
    bool tools{};     // yt-dlp is there
    bool busy{};
    unsigned percent{}; // of yt-dlp fetched, while it is being
    std::string problem;
    Kind problem_kind{}; // which kind the problem is one of
    std::string browser; // whose YouTube sign-in yt-dlp uses, or empty
    bool cookies{};      // a cookies.txt of the player's is kept, and is what it uses
    std::string runtime; // the JavaScript runtime yt-dlp is given ("node", "deno"), or empty: a sign-in does not work without one
    std::vector<Playlist> playlists;
    std::vector<Found> found; // the stations of the last search
    std::string searched;     // what it was for
    // What is playing, when one of these is: which playlist, and which song of it.
    bool playing{};
    bool loading{}; // a song was asked for and its sound has not begun yet
    std::size_t playing_list{}, playing_song{};
};
// Reads what was kept, once, on a thread of its own, which does what takes time below.
void start() noexcept;
State state();
// All of them, in the order their stations are numbered.
std::vector<Playlist> playlists();
// Whether there is a playlist of a kind to show.
bool has(Kind kind);
// The id of a playlist's tile in the music screen.
std::string tile_id(std::size_t playlist);
// Fetches yt-dlp. Adds what a link is of (a video, a playlist). Takes a playlist of any kind out.
void fetch_tools();
void add(const std::string& link);
void remove(std::size_t playlist);
// Adds a folder's music (and what is in the folders inside it), or reads it again when it is
// there already. Empty for the player's Music folder.
void add_folder(const std::string& path);
// Looks for radio stations by name, into the state. Adds one.
void search_radio(const std::string& query);
void add_station(const std::string& name, const std::string& address, const std::string& icon);
// The browser whose YouTube sign-in yt-dlp is to use ("firefox", "edge", "chrome", "brave",
// "opera", "vivaldi"; empty for none): YouTube asks for one before it plays songs it has
// marked as not for everyone.
void set_browser(const std::string& browser);
// The same sign-in as a file: a Netscape cookies.txt the player exported from their browser
// (what private and age-restricted songs need), kept as
// <Local AppData>\ReSkate\youtube-cookies.txt and used before any browser. Empty takes it away.
// False when what was given is no such file.
bool set_cookies(const std::string& text);

// Whether a song that takes a moment to come is said in the corner of the game ("Loading
// song...", and one that could not be played); it is, unless the player turns it off.
bool notices() noexcept;
void set_notices(bool on) noexcept;
// Whether one of its stations is the one that is on.
bool on() noexcept;
void set_on(bool on) noexcept;
// The rate of the sound it is giving, in Hz.
unsigned rate() noexcept;
// Plays a playlist, from a song of it when given (its id), else from where it was.
void play(std::size_t playlist, const std::string& song = {});
// Goes to a song of the playlist that is playing, by its place in it (the setup page's queue).
void play_at(std::size_t playlist, std::size_t song);
// The picture in the tags of a song that is a file (as it is there: a JPEG or a PNG), or nothing.
std::vector<std::uint8_t> art(std::size_t playlist, std::size_t song);
// What is playing, as external_player gives it; and play, pause, next and previous.
external_player::Track now();
std::uint64_t revision() noexcept;
void send(external_player::Command command) noexcept;
}
