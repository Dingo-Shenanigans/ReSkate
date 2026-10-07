#include "Extension/Multiplayer/Steam/steam_friend_join.h"
#include <iostream>
#include <stdexcept>
using namespace dingosdk;
using namespace dingosdk::multiplayer;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    try {
        check(steam_join_target("+reskate_lobby 9001") == 9001, "ReSkate lobby target parses");
        for (const auto* value : {"", "+connect_lobby 9001", "+reskate_lobby ", "+reskate_lobby 0",
                "+reskate_lobby -1", "+reskate_lobby 9001 extra", "+reskate_lobby 9001\n",
                "+reskate_lobby 18446744073709551616", "9001"})
            check(!steam_join_target(value), "Unrelated or malformed Steam requests must be ignored");
        MultiplayerModel model;
        model.active = model.hosting = model.public_host = model.lobby_listed = model.local_ready = true;
        model.public_lobby = 9001; model.invite = "private-session-secret";
        const auto open = model;
        check(steam_join_presence(model) == "+reskate_lobby 9001", "Public host advertises a lobby ID without its session code");
        model.password_required = true;
        check(steam_join_presence(model).empty(), "Password-protected sessions have no one-click join");
        model = open; model.public_host = false;
        check(steam_join_presence(model).empty(), "Private hosts are never advertised");
        model = open; model.public_lobby = 0;
        check(steam_join_presence(model).empty(), "Direct private codes cannot leak into presence");
        model = open; model.players = model.capacity;
        check(steam_join_presence(model).empty(), "Full sessions stop advertising");
        model = open; model.local_ready = false;
        check(steam_join_presence(model).empty(), "Loading sessions stop advertising");
        model = open; model.lobby_listed = false;
        check(steam_join_presence(model).empty(), "Closed listings stop advertising");
        model = open; model.active = false;
        check(steam_join_presence(model).empty(), "Ended sessions stop advertising");
        model = open; model.hosting = model.public_host = model.lobby_listed = false; model.connected = true;
        check(steam_join_presence(model) == "+reskate_lobby 9001", "Connected guests can advertise their verified public lobby");
        model.connected = false;
        check(steam_join_presence(model).empty(), "Guests cannot advertise before admission");
        // Where a player skates, for friends' server lists: any public session, joinable or not.
        check(steam_session_target("9001") == 9001, "A session id parses");
        for (const auto* value : {"", "0", "-1", "9001 ", "+reskate_lobby 9001", "18446744073709551616", "lobby"})
            check(!steam_session_target(value), "Malformed session values must be ignored");
        model = open;
        check(steam_session_presence(model) == "9001", "A public host tells friends its listing");
        model.password_required = true; model.players = model.capacity; model.local_ready = false;
        check(steam_session_presence(model) == "9001", "A full, protected or loading public session is still told");
        model = open; model.public_host = false;
        check(steam_session_presence(model).empty(), "A private host is never told");
        model = open; model.public_lobby = 0;
        check(steam_session_presence(model).empty(), "A session joined by a private code is never told");
        model = open; model.lobby_listed = false;
        check(steam_session_presence(model).empty(), "A closed listing is not told");
        model = open; model.active = false;
        check(steam_session_presence(model).empty(), "An ended session is not told");
        model = open; model.hosting = model.public_host = model.lobby_listed = false;
        check(steam_session_presence(model).empty(), "A guest is not told before admission");
        model.connected = true;
        check(steam_session_presence(model) == "9001", "A connected guest tells friends the server they are on");
        std::cout << "Steam friend join checks passed.\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
