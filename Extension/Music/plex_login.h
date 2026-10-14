#pragma once
#include <string>

// Signing in to Plex (plex.tv's PIN login: a short code the player types at plex.tv/link), so
// that the cover of what Plexamp plays can be had from the player's own Plex server. The token
// is kept sealed so that only this Windows account can read it.
namespace dingosdk::plex_login {
struct State {
    bool signed_in{};
    bool pending{};      // a code is out and waiting to be entered
    std::string code;    // the code to enter at plex.tv/link
    std::string problem; // why the last try failed, or empty
};
State state();
// The token to ask a Plex server with, or empty when not signed in.
std::string token();
// Asks plex.tv for a code and waits (in a thread of its own) for it to be entered.
void begin();
void sign_out();

struct Reply {
    unsigned long status{};
    std::string body;
};
// One GET from a host (a Plex server or plex.tv); its body up to `largest` bytes.
Reply get(const std::string& host, unsigned short port, bool secure, const std::string& path, std::size_t largest = 4 * 1024 * 1024);
}
