#pragma once
#include <cstdint>
#include <string>
#include <vector>

// What a music player outside the game is playing, and its transport buttons: Spotify's
// desktop app first, else whichever player Windows counts as the current one. This goes through
// Windows' own media session interface (the one the volume flyout and media keys use), so it
// needs no login and no account with anyone; the sound stays that player's own.
namespace dingosdk::external_player {
struct Track {
    bool available{}; // a player with a session is running
    bool spotify{};   // that player is Spotify
    bool playing{};
    bool can_next{}, can_previous{}, can_pause{}, can_play{};
    std::string app;                    // the player's application id, as Windows names it
    std::string title, artist, album;   // UTF-8
    std::vector<std::uint8_t> artwork;  // the cover as the player gives it (PNG or JPEG bytes), or empty
    std::uint64_t revision{};           // changes whenever anything above does
    std::uint64_t artwork_revision{};   // changes whenever the cover does
};
enum class Command { play, pause, toggle, next, previous };

// Starts the watcher (a thread of its own); once. Nothing is read before this.
void start() noexcept;
// Stops it and waits for its thread. Safe when it was never started.
void stop() noexcept;
// The last track seen. Any thread; a copy.
Track now();
// Only what changes often, without copying the cover. Any thread.
std::uint64_t revision() noexcept;
// Asks the player to do it. Any thread; done by the watcher, a moment later.
void send(Command command) noexcept;
// Whose session it is: Spotify's (empty, as it begins), or that of the application of this
// name ("chrome.exe"), for which Windows' current one is taken when it has none of its own.
void use_app(const std::string& executable);
}
