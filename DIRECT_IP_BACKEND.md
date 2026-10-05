# Direct IP dedicated-server implementation map

This maps the existing files to change or review so clients can join a dedicated
ReSkate machine by `IP:port`. It assumes Steam IDs remain the player identities.
Removing Steam entirely would require a broader rewrite.

The exact set of edits depends on the backend design. Files marked for review
may retain their existing behavior if authenticated Steam identities are preserved.

## Core implementation

| Area | Files | Work needed |
| --- | --- | --- |
| Client connection and server listener | [steam_transport.cpp](Extension/Multiplayer/Steam/steam_transport.cpp), [steam_transport.h](Extension/Multiplayer/Steam/steam_transport.h) | Add `ConnectByIPAddress` and `CreateListenSocketIP`; handle connection callbacks and authenticated identities. |
| Address and join-code parsing | [protocol.cpp](Extension/Multiplayer/Net/protocol.cpp), [protocol.h](Extension/Multiplayer/Net/protocol.h) | Support an IP endpoint instead of requiring only a Steam-ID session code. |
| Join command | [session_commands.cpp](Extension/Multiplayer/Session/session_commands.cpp) | Route IP joins to the new connection method. |
| Client connection state | [session_internal.h](Extension/Multiplayer/Session/session_internal.h), [session.cpp](Extension/Multiplayer/Session/session.cpp) | Store the endpoint and establish the connected host's identity. |
| Packet admission and routing | [session_receive.cpp](Extension/Multiplayer/Session/session_receive.cpp), [session_send.cpp](Extension/Multiplayer/Session/session_send.cpp) | Review handshake, host checks, and peer routing for the IP backend. |
| Dedicated-server startup | [Server/main.cpp](Server/main.cpp) | Select the backend and start the IP listener. |
| Server configuration | [server_config.cpp](Server/server_config.cpp), [server_config.h](Server/server_config.h) | Add bind address, gameplay port, and backend settings. |
| Dedicated-server session | [server_host.cpp](Server/server_host.cpp), [server_host.h](Server/server_host.h) | Pass listener settings and generate usable IP join information. |
| Steam server bootstrap | [steam_server.cpp](Server/steam_server.cpp), [steam_server.h](Server/steam_server.h) | Review Steam initialization, port allocation, identity, and advertising alongside the IP listener. |
| Join textbox | [multiplayer_lobbies.cpp](Extension/UI/Overlay/multiplayer_lobbies.cpp), [skate_menu.h](Extension/UI/Overlay/skate_menu.h) | Accept IP addresses and ports; current validation rejects dots and colons. |
| Console joining | [console_commands.cpp](Extension/Multiplayer/console_commands.cpp) | Expose and document joining by IP. |
| Connection display | [session_view.cpp](Extension/Multiplayer/Session/session_view.cpp), [session_model.h](Engine/Game/Multiplayer/session_model.h) | Show the endpoint and selected backend. |

## Optional server-browser support

For click-to-join IP servers in the browser, include:

- [steam_server_browser.cpp](Extension/Multiplayer/Steam/steam_server_browser.cpp)
- [steam_server_browser.h](Extension/Multiplayer/Steam/steam_server_browser.h)

These currently turn listings into Steam-ID join codes. The browser would need
to preserve the IP endpoint and pass it into the direct-IP join flow.

## Build integration

- [cmake/Runtime.cmake](cmake/Runtime.cmake)
- [cmake/Apps.cmake](cmake/Apps.cmake)
- [CMakeLists.txt](CMakeLists.txt)

Register any new backend source/header files and test targets in the applicable
client and dedicated-server builds.

## Testing

- [protocol_tests.cpp](Extension/Multiplayer/Test/protocol_tests.cpp): address
  parsing and join-format validation.
- [mesh_tests.cpp](Extension/Multiplayer/Test/mesh_tests.cpp): identity and routing
  behavior relevant to direct-IP sessions.
- [server_config_tests.cpp](Server/Test/server_config_tests.cpp): bind address,
  gameplay port, and backend configuration.

Add client/server connection tests for successful joins, rejected identities,
disconnects, reconnects, and coexistence with the existing Steam connection path.

## Hosting instructions

- [Server/README.txt](Server/README.txt)
- [Server/README-linux.md](Server/README-linux.md)

Document bind versus public addresses, gameplay ports, firewall/router setup,
how clients join, and any Steam authentication requirements that remain.

This document is an implementation map. No direct-IP backend has been implemented
as part of creating it.
