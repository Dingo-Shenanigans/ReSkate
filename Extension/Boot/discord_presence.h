#pragma once
#include <string>
#include <string_view>

// Discord Rich Presence: what the player is doing in ReSkate, shown on their Discord profile
// ("On San Vansterdam", "Server: EU 1", 12 of 32). The Discord app on the same PC takes it over
// its local pipe; nothing goes to Discord's servers from here, and without the app running
// nothing happens at all. Players can turn it off (the menu's Multiplayer settings).
//
// A session anyone may walk into (no password, not full, a server or a public lobby) also
// carries its join code, which gives the status Discord's Join button. Whoever presses it has
// Discord start ReSkate (the launcher, registered for that below) if it is not running, and
// then hand their game the code (take_join).
namespace dingosdk::discord_presence {
// The Discord application the presence belongs to (its name is what Discord shows as the game):
// the Application ID from discord.com/developers/applications. Empty: no presence at all.
// Its Rich Presence art asset named "resk8" is the picture.
inline constexpr std::string_view application_id = "1558319241114025986";

struct Presence {
    std::string details, state;  // the two lines under the name; empty: left out
    int party_size{}, party_most{}; // "(3 of 16)" after the state; 0: left out
    std::string party;           // what is the same for everyone in one session
    std::string join;            // the session's join code, when anyone may join it; else empty
    bool operator==(const Presence &) const = default;
};
// Whether there is any Discord status in this run: there is an application for it, and the
// launcher's own switch (Settings, "Discord status"; --no-discord) has not turned it off. Off
// there, the game never opens Discord's pipe, and the in-game switch is not offered.
bool available() noexcept;
// What to show from now on (client thread, as often as it likes: only a change is sent).
void update(Presence presence) noexcept;
// The player's own switch, kept in their profile. On unless they turned it off.
bool enabled() noexcept;
void set_enabled(bool on) noexcept;
// The join code of a session the player asked to join from Discord (a Join button, or an
// invite in a chat), once; empty when there is none. Any thread.
std::string take_join() noexcept;
// What Discord sent over its pipe, if it is a request to join: the code, or empty.
std::string join_request(std::string_view json);
// And if it is someone asking to join this player ("Ask to Join"): their Discord user ID, or
// empty. A status only has that button while its session is open to anyone, so the game says
// yes for the player at once, and Discord then lets them in as Join would.
std::string join_asker(std::string_view json);

// One message as Discord's pipe takes it: an opcode, a length and JSON. Exposed for tests.
std::string frame(unsigned opcode, std::string_view json);
// The command that shows `presence` (or, without one, clears it), since `started` (Unix seconds).
std::string activity_command(const Presence *presence, unsigned long process, long long started, unsigned long long nonce);
} // namespace dingosdk::discord_presence
