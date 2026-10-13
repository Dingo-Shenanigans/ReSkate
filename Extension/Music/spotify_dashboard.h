#pragma once
#include <string>

// A page in the player's browser (setup_page.html, built into the DLL) for setting the music
// sources up, in the mod's own colours and type, served by the game on this computer
// alone (http://127.0.0.1:8421): the game's Spotify device and whether an account is on it,
// the sign-in that lists the player's playlists (its client id, signing in and out), what is
// playing with play, pause and next, and how loud it is through the game. "music setup" in
// the console opens it.
//
// It listens on the loopback address only, answers only requests addressed to it by that
// address, and takes changes only from its own page (a header a page of another site cannot
// send without being refused first).
namespace dingosdk::spotify_dashboard {
// Starts serving, once, on a thread of its own.
void start() noexcept;
// The page's address, or empty when it is not being served.
std::string address();
// Opens it in the player's browser.
bool open() noexcept;
// Keeps the sound's settings (loudness, levelling, the equaliser, the speakers) as they are
// now, for the next time the game is run: after one is changed from somewhere other than the page.
void save_settings() noexcept;
}
