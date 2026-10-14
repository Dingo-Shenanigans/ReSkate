#pragma once
#include "external_player.h"

// Plexamp does not publish a Windows media session, so external_player.h has nothing to read
// or drive for it. Plexamp's own remote-control server (127.0.0.1:32500, the one Plex's apps
// use to cast to it) says what it is playing and takes play, pause and skip; it needs no
// login for the player on this computer.
namespace dingosdk::plexamp_player {
// What Plexamp is playing (Track::available is false when it is not running or has nothing
// loaded). The cover is not read: it is on the Plex server, behind the user's token.
external_player::Track look();
// Sends the command to Plexamp; false when it could not be reached.
bool send(external_player::Command command) noexcept;
}
