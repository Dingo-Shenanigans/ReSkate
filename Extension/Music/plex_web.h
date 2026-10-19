#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The player's Plex account, signed in the way Plex's own apps do it: a PIN is asked of plex.tv,
// the player approves it in their browser (app.plex.tv/auth), and plex.tv hands back a token.
// Nothing is typed into the game or this page. The token is kept encrypted for their Windows
// account in <Local AppData>\ReSkate\plex.json, with the random id this install goes by.
namespace dingosdk::plex_web {
struct State {
    bool signed_in{}, busy{};
    std::string user, problem;
};
// Reads what was kept and checks that the sign-in still holds. Once, on a thread of its own,
// which does everything below.
void start() noexcept;
void login();  // opens the browser, and waits for the approval there
void logout(); // forgets the sign-in
State state();

// What the player's own Plex server says they are playing (its /status/sessions), with the
// album's picture fetched from the server. The server is found through plex.tv; the poll runs
// only while `now_playing` is being asked for.
struct NowPlaying {
    bool valid{}; // a music session of this account was seen a moment ago
    bool playing{};
    std::string title, artist, album;   // UTF-8
    std::vector<std::uint8_t> art;      // JPEG bytes from the server, or empty until fetched
    std::uint64_t art_revision{};       // changes whenever the picture does
};
NowPlaying now_playing();
enum class Action { play, pause, next, previous };
// Asks the Plex player of that session to do it, through the server. Any thread; false when there is no session.
bool control(Action action);
// The token of the sign-in, for what talks to the player's server; empty when signed out.
std::string token();
// This install's client identifier, which Plex wants on every request.
std::string client_identifier();
}
