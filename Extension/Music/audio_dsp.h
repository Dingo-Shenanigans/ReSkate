#pragma once

// How the sound of the music sources (Spotify, YouTube Music, the player's own files, radio,
// another application) is shaped on its way into the game: an equaliser of five bands, and a
// levelling of its loudness, so that a quiet song and a loud one are heard much the same. Both
// are set from the setup page (spotify_dashboard.h) and are as the player left them. The
// game's own music is not touched.
namespace dingosdk::audio_dsp {
inline constexpr int bands = 5;
// What each band is centred on, in Hz (the first and the last are shelves: all below, all above).
inline constexpr float centres[bands] = {60.0f, 250.0f, 1000.0f, 4000.0f, 12000.0f};
inline constexpr float most_decibels = 12.0f;

// A band's gain in decibels, from -12 to 12 (0 leaves it as it is).
void set_band(int band, float decibels) noexcept;
float band(int band) noexcept;
void set_levelling(bool on) noexcept;
bool levelling() noexcept;

// Shapes `frames` of stereo sound at `rate` Hz, where it is. It keeps what it needs of the
// sound before, so it is to be given the sound in the order it is heard.
void process(float* interleaved, unsigned frames, unsigned rate) noexcept;
}
