#pragma once
#include <cstdint>
#include <string>

// The sound one application is playing, taken from Windows as it is played (an application
// loopback capture, Windows 10 version 2004 and later), for the game's radio to play in the
// place of a song (music_inject.h). The application's own sound to the speakers can be turned
// down to nothing meanwhile, so it is heard once.
namespace dingosdk::app_audio {
struct State {
    bool capturing{};
    unsigned long process{};       // the application's process id, 0 when it is not running
    std::uint64_t frames{};        // stereo frames captured so far
    std::uint64_t read{};          // frames handed to the reader so far
    std::uint64_t starved{};       // frames the reader wanted and was not given
    unsigned readers{};            // readers that read in the last second
    float level{};                 // the loudest sample of the last second, 0 to 1
    bool muted{};                  // its own output is turned down by this
    std::string problem;           // why it is not capturing, or empty
};
inline constexpr unsigned capture_rate = 48000;

// Starts capturing `executable` ("Spotify.exe"), on a thread of its own; it waits for the
// application when it is not running and follows it when it restarts. Once.
void start(const wchar_t* executable) noexcept;
void stop() noexcept;
// Starts it when it is not, and has it capture this application from now on.
void target(const wchar_t* executable) noexcept;
// Turns the application's own output down to nothing, or puts it back. Any thread; done by
// the capture thread.
void mute_source(bool mute) noexcept;
// A reader (the game's audio threads; no locks, no allocation): fills `frames` frames of
// `left` and `right` at `rate` Hz with what was captured. Whatever it has not got is silence.
// Returns the frames that were real. Each `reader` (any number that tells one from another)
// has its own place in what was captured, so two of them reading at once both get all of it;
// up to eight at a time, one that has not read for a second giving up its place.
// Puts back an application's output that was left turned down (the game ended before it could).
// On a thread of its own.
void restore_source(const wchar_t* executable) noexcept;
unsigned read(std::uint64_t reader, float* left, float* right, unsigned frames, float rate) noexcept;
State state();
// Sound from somewhere else in the place of what is captured (spotify_device.h): while it is
// `on`, what is captured is not kept, and `push` fills the buffer, with frames of two floats
// at `rate` Hz. One thread pushes.
void set_external(bool on, unsigned rate) noexcept;
void push(const float* interleaved, unsigned frames) noexcept;
// How far what was last pushed or captured is ahead of the reader that has read the most (in
// frames), or a great many when nothing has read for a second: how much there is yet to hear.
std::uint64_t ahead_frames() noexcept;
}
