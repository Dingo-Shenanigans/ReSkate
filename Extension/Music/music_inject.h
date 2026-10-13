#pragma once
#include <cstdint>
#include <string>

// Spotify heard through the game: while the Spotify station is selected (spotify_station.h),
// the sound of the song the game is playing for it is replaced, as the game's sound engine
// produces it, by what Spotify is playing (app_audio_capture.h). The game's music volume, its
// fades and its pausing all still apply, as it is still the game's song to the game.
namespace dingosdk::music_inject {
bool install(std::uintptr_t base) noexcept;
// Which of the game's playing sound files is replaced while the station is on:
//   0  none
//   1  the stereo ones a minute long or more (songs), the default: the station's own and the
//      speakers' in the world
//   2  every stereo one
//   3  the station's own song only: the speakers keep their music
int mode() noexcept;
// The game is beginning the station's own song (spotify_rows.cpp): the sound file it asks for
// next is the one that is replaced. Any thread.
void station_song_begins() noexcept;
void set_mode(int mode) noexcept;
// How much louder or quieter than Spotify plays it, 1 as it is.
float gain() noexcept;
void set_gain(float gain) noexcept;
// The client thread, each frame: starts the capture when the station is first on, turns
// Spotify's own output down while its sound is going through the game, and writes what the
// game's sound files are into the log.
void tick() noexcept;
std::string status();
}
