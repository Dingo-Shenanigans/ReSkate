ReSkate dedicated server
========================

A headless ReSkate lobby that runs on its own, without the game. Players find it
in the in-game server browser (Multiplayer > Servers), or join with its code.

Start it
--------
Run ReSkateServer.exe. The first run writes ReSkateServer.json next to it; edit
that file (at least "name" and "admins") and start the server again. Leave this
folder together: steam_api64.dll, steamclient64.dll, tier0_s64.dll and
vstdlib_s64.dll are how the server talks to Steam, and world-layers.json lets it
set world layers (time of day and so on) for everyone.

Start it with --config <file> to use another settings file instead, e.g.
ReSkateServer.exe --config grom.json. A file that does not exist yet is written
with the defaults, as on the first run.

Custom maps
-----------
Copy a custom map's mod folder from the game's Mods folder into a Mods folder
next to the server (only its reskate-levels.json is read). The map can then be
chosen by name. Players need the same map mod installed to join.

Players connect through Steam's relay network, so no ports need opening. If you
do forward UDP 27015-27016 (port, query_port), the browser also shows the
server's ping and players can join a little faster.

The server signs in to Steam anonymously and gets a new Steam ID, and so a new
join code, every time it starts. The browser always finds it by name.

Players and the server need the same ReSkate version.

Updates
-------
The server keeps itself on the latest ReSkate release. It checks when it starts
and every half hour after that. A new version found while players are on is
installed as soon as the server is empty: it restarts on its own, keeping
ReSkateServer.json, Mods and its logs. Type "update" to check and install
straight away (players are told to rejoin). Turn this off with
"auto_update": false, or start the server with --no-update.

ReSkateServer.json
------------------
The settings are in sections; each setting below is listed under the section it is in,
like "server": {"name": "My server"}. A file from an older version, with every setting at
the top and some under older names, is read as it is and written back this way the first
time the server starts.

"server" - The server itself: its name, who it lets in by password, and its ports.
name               Shown in the browser: 1-64 letters, numbers, spaces and - _ / [ ] ( ).
password           Empty for anyone; otherwise players type it to join.
welcome_message    A chat line sent to each player as they join.
listed             false hides the server; players then need the code.
max_players        1-249.
port, query_port   Steam game server ports (default 27015, 27016).
steam_token        A Steam game server login token, or empty (default). Without one
                   the server signs in anonymously and gets a new Steam ID every
                   start. With one it keeps the same Steam ID, printed at startup.
                   Make a token at steamcommunity.com/dev/managegameservers with
                   App ID 3354750; each running server needs its own. Keep it
                   private: anyone with it can sign in as your server.
                   The ReSkate team can set the in-game server browser to show
                   only servers that have one; the server says so in its log when
                   that hides it. Players can always join with the code.
auto_update        Install new ReSkate releases when nobody is on (default true).
activity_log       Log what players do (default true): throwdown drops placed,
                   joins, starts, turns and results; objects placed or removed;
                   how long players take to load.

"access" - Who runs the server and who always has a place on it.
admins             SteamID64s (as strings) who may change settings in-game.
reserved_players_slots
                   SteamID64s of the players with a reserved slot, e.g.
                   ["76561198000000000"] (default none). They can join when
                   the server is full, as the admins always can: each of them
                   adds a slot beyond max_players that only they use. With 32
                   players and 2 admins and 2 listed, anyone can join until
                   32 are on, and those four can still join after that, up to
                   36. The browser then shows 33/32 and so on. Nothing is
                   held back from everyone else.
use_global_bans    Turn away players the ReSkate team has banned from multiplayer
                   (default true). The list is read from api.reskate.dev at startup
                   and every ten minutes. false lets them in; the server's own
                   bans (data/bans.json) apply either way.

"maps" - What is skated.
map                The map everyone skates, named like the game's load command:
                   "San Vansterdam", "Isle of Grom", "Super Ultra Mega Resort",
                   "Stadium 1", or a custom map such as "bbcity" (see Custom maps).
pool               The maps players may vote for and the rotation goes through,
                   in order, e.g. ["San Vansterdam", "Isle of Grom", "bbcity"].
                   Empty (the default) allows every map the server knows.
                   Admins can still change to any map.
rotation_minutes   Minutes on each map before the server moves to the next
                   one in pool (default 0: off). Players get a minute's
                   warning; the clock waits while nobody is on, and starts
                   over whenever the map changes (by a vote or an admin too).
parks              Layout for each park lot, e.g. "skatepark_01", or "empty".
world_layer_sync   Force the "layers" below on every player.
layers             World layer key -> "on" / "off".

"players" - What players may do.
allow_noclip, allow_no_bail, allow_boosts
                   Let players use noclip (and tp) / No Bail / the forward and up
                   boosts (default true; admins always can).
allow_parties      Let players form parties (default true): invite each other
                   from the game's Social menu, a player card, the ReSkate
                   Multiplayer menu or chat (/party invite <player>). Party
                   members join each other's coop challenges, see each other on
                   the map and talk with /p <message>.
party_size         Most players in one party, 2-8 (default 8).
allow_voice_chat   Allow voice chat.
voice_range        How far proximity voice reaches, 50-1000 m.
object_placement   everyone, admins (only admins can build), or nobody.
object_limit       How many objects each player may have placed, 1-1024
                   (default 100), or 0 for no limit. Admins are not limited.
                   A player at the limit deletes one to place another.
announce_throwdowns  Tell everyone in chat when a throwdown drop is placed
                   (default true).

"anti_cheat" - What the server checks, and what it does about it.
speed_hack         Catch players whose game runs faster than normal (Cheat
                   Engine's speedhack and the like), measured from the timing
                   of what their game sends: "warn" (default) takes them out of
                   throwdowns and coop challenges until their speed is normal
                   again and tells them so (the log has it for the admins), "kick" removes them from the
                   server, "off" does not check.
modified_scoring   Catch players whose mods change how many points tricks
                   score (per-trick points, the scoring multipliers, the
                   throwdown scoring logic) or how the skater handles (core
                   physics, wipeouts, trick gestures). Each player's ReSkate checks its
                   mods at launch and reports the result when joining:
                   "warn" (default) takes them out of throwdowns and coop
                   challenges (the server stops passing their throwdown
                   messages on) and everyone is told in chat, "kick" removes
                   them, "off" does not check. A player has to restart Skate
                   without the mod to take part again.
allowed_scoring_mods Scoring fingerprints accepted like the game's own, for a
                   server that runs on a scoring mod everyone installs
                   (16 hex digits each; score-check lists each player's).
enforce_tuning     Players skate with the game's own Gameplay/SkatePhysicsTuning,
                   not copies they edited (default true). Edited tuning (truck
                   positions and the rest) otherwise shows on their skater for
                   everyone.
bone_scale_limit   How far a mod may resize part of a skater for the other
                   players (a "big head" mod and the like): the most a bone may
                   be scaled, 1 to 8 (default 2); 0 is no limit. The game's
                   own skater height is a scale too, so 1 shows every skater
                   at the same height and build as well as stopping mods; 2
                   leaves height alone and still halves the largest heads.
                   The player with the mod still sees it on their own screen.
                   Console: bone-scale <1-8>|off.

"network" - How players connect and how much they are sent. The defaults suit most servers.
use_steam_relay    How players reach the server: true (default) or false.
                   true    Through Steam's relay network. Nothing to open, and
                           the server never sees a player's address. The route
                           is Steam's choice and can be a long way round.
                   false   Straight to this server's "port" over UDP, which
                           must be open to the internet. The shortest route,
                           so the lowest ping. The server sees the addresses
                           of the players who connect this way, as any
                           dedicated server does; players never see each
                           other's.
                   With false the server still answers through the relays: a
                   game tries the port first and uses the relays if it gets no
                   answer in a few seconds, and a player can turn direct
                   connections off in their own settings. So false never
                   keeps anyone out. Steam vouches for who each player is
                   either way. "net" shows who is direct, and the log has a
                   [direct] line for each player who asks to connect that way.
send_rate          The most the server sends one player, in KB/s (default 900,
                   128-16384). About 1100 is what reaches a player through
                   Steam's relays: set higher, what is lost is resent until the
                   connection is full of resends and half of everything is
                   lost. Console: rate <KB/s>, also for the players already on.
crowd_budget       The most position updates a second one player is sent
                   (default 600, 0 for no limit). Players near each other are
                   sent at the full rate, 20 a second; this only matters once
                   more are in one place than budget / 20, about 30. Then the
                   nearest stay at the full rate and the farthest of the crowd
                   drop to 10 and 5 a second, instead of everyone's connection
                   filling up. About 1000 is what reaches a player through
                   Steam's relays; more than that was seen to lose half.
                   Console: crowd <n>|off.
pack_ms            How long a message to a player may wait to go in the same
                   packet as the next ones, in milliseconds (default 10, 0 to
                   50). A full server sends each player hundreds of small
                   messages a second, each in a packet of its own with its own
                   headers; packed, the same updates take fewer packets, less
                   bandwidth and less CPU. It adds up to that long to when an
                   update arrives. Voice is never held back. 0 sends every
                   message at once, as before. Takes effect on restart.
finger_distance    Past this many metres (default 25) a player's fingers are not
                   sent moving: they stay as they were, and move again when the
                   player is nearer. Fingers are nearly half of every position
                   update and cannot be made out at that distance. 0 always
                   sends them.
distances          When far-away players update less often (metres).
steam_debug        true logs what Steam's own networking says it is doing: each
                   connection asked for, and what it refused or ignored and
                   why. For finding out why players cannot connect; it is a
                   lot of text, so turn it off again afterwards.

"votes" - What players may vote on.
votes              Player votes, each off until turned on:
                     "map": {"enabled": true, "percent": 60}   /vote map <map>
                                                   (a map in maps.pool)
                     "kick": {"enabled": true, "percent": 60}  /vote kick <player>
                     "time_of_day": {"enabled": true, "percent": 50}  /vote tod <time>
                                                   (needs world_layer_sync)
                   "percent" is the share of connected players whose yes passes
                   it. "seconds" (default 30) is how long a vote runs and
                   "cooldown_seconds" (default 60) how long a player waits before
                   starting another. Players vote with /yes and /no in chat;
                   admins cannot be vote-kicked.

Every change made from the console or by an admin is saved back to this file.

Bans are kept in data/bans.json, beside this file: the players who can never
join, managed with ban / unban. The ReSkate team's own list is separate: see
use_global_bans. A config from an older version that still has "bans" in it has
them moved there the first time the server starts.

Console and admin commands
--------------------------
Type these in the server window. Admins run the same commands in the game with
the console command "mp server <command>". Their replies arrive in chat.
Admins can also type any of them in chat with a / in front (/kick, /map, /votes).
Admins can also change the server's map by picking a level in Levels or Travel,
and change voice, distances, placement and kicks from the Multiplayer menu.

  help                          A short list of every command.
  status                        Name, map, players, code.
  players                       Connected players and their SteamID64s.
  reserved                      Who has a reserved slot, and how many extra slots there are.
  reserved add|remove <player or id>   (console only)
  net [player]                  How the connections are doing right now: traffic,
                                queues, the server's own loop timing, and the twelve
                                connections doing worst. With a player, that one's
                                ping, delivery, queue and when it was last heard from.
                                The log also gets a [network] line every minute and,
                                when a player's connection ends, how Steam says it ended.
  say <text>                    Chat as the server (console only).
  msg <player> <text>           Private message, shown to them as "[DM from <you>] ...".
                                Name start (one word) or SteamID64.
  msg-party <player> <text>     Message everyone in that player's party ("[DM from <you> to party]").
  msg-admins <text>             Message every admin who is online ("[DM from <you> to admins]").
                                Players can whisper each other with /w <player> <text> in chat.
  kick <player>                 Until the server restarts. Name start or SteamID64.
                                Admins cannot kick or ban each other; the console can.
  ban <player or id> [name]     For good.   unban <id>   bans
  map <name>                    e.g. map San Vansterdam, map grom, map bbcity
  maps                          The maps this server knows.
  map-pool [add|remove <map>|clear]   The maps players vote between and the
                                rotation uses (see maps.pool).
  rotation [<minutes>|off]      Change the map on a timer (see maps.rotation_minutes).
  name <text>   password <text|off>   welcome <text|off>   listed on|off
  voice on|off   voice-range <m>
  distances <full> <half> <half-return> <low>
  placement everyone|admins|nobody   clear-objects
  objects <number>|off          How many objects each player may have placed.
  noclip on|off   nobail on|off   boosts on|off
                                What players may use (admins always can).
  tuning on|off                 Everyone on the game's own physics tuning.
  tpall [player]                Everyone to you (admins in game) or to a player.
  tphere <player>               One player to you (admins in game).
  park <construction|historic|financial> <layout>
  layer-sync on|off   layer <key> default|on|off
  layers <key>=<mode> ...       Several world layers at once, each default, on or off.
  tod <default|morning|noon|afternoon|evening|night|weatherday|weathernight>
                                Time of day on every map (needs layer-sync on).
  votes [map|kick|tod on|off|<percent>]   The vote settings (see votes).
  votes seconds <n>   votes cooldown <n>   vote-cancel
  activity-log on|off           Log player activity (see activity_log).
  announce-throwdowns on|off    Chat message when a throwdown is placed.
  parties [on|off]              List the parties, or allow them (off ends them all).
  party-size <2-8>              Most players in one party.
  speed-check off|warn|kick     What happens to players whose game runs fast.
  score-check [off|warn|kick]   What happens to players whose mods change scoring
                                or physics; with no argument, every player's result.
  score-allow [<fingerprint>|remove <fingerprint>]   Accept a scoring mod's
                                fingerprint like the game's own (or list them).
  admin add|remove <player or id>   admins      (console only)
  update                        Check for a new release and install it now (console only).
  quit, exit or stop            Shut the server down (console only).
