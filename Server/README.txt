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
name               Shown in the browser: 1-64 letters, numbers, spaces and - _ [ ] ( ).
map                The map everyone skates, named like the game's load command:
                   "San Vansterdam", "Isle of Grom", "Super Ultra Mega Resort",
                   "Stadium 1", or a custom map such as "bbcity" (see Custom maps).
map_pool           The maps players may vote for and the rotation goes through,
                   in order, e.g. ["San Vansterdam", "Isle of Grom", "bbcity"].
                   Empty (the default) allows every map the server knows.
                   Admins can still change to any map.
map_rotation_minutes  Minutes on each map before the server moves to the next
                   one in map_pool (default 0: off). Players get a minute's
                   warning; the clock waits while nobody is on, and starts
                   over whenever the map changes (by a vote or an admin too).
max_players        1-249.
password           Empty for anyone; otherwise players type it to join.
welcome            A chat line sent to each player as they join.
listed             false hides the server; players then need the code.
auto_update        Install new ReSkate releases when nobody is on (default true).
global_bans        Turn away players the ReSkate team has banned from multiplayer
                   (default true). The list is read from api.reskate.dev at startup
                   and every ten minutes. false lets them in; the server's own
                   "bans" apply either way.
votes              Player votes, each off until turned on:
                     "map": {"enabled": true, "percent": 60}   /vote map <map>
                                                   (a map in map_pool)
                     "kick": {"enabled": true, "percent": 60}  /vote kick <player>
                     "time_of_day": {"enabled": true, "percent": 50}  /vote tod <time>
                                                   (needs world_layer_sync)
                   "percent" is the share of connected players whose yes passes
                   it. "seconds" (default 30) is how long a vote runs and
                   "cooldown_seconds" (default 60) how long a player waits before
                   starting another. Players vote with /yes and /no in chat;
                   admins cannot be vote-kicked.
announce_throwdowns  Tell everyone in chat when a throwdown drop is placed
                   (default true).
parties            Let players form parties (default true): invite each other
                   from the game's Social menu, a player card, the ReSkate
                   Multiplayer menu or chat (/party invite <player>). Party
                   members join each other's coop challenges, see each other on
                   the map and talk with /p <message>.
party_size         Most players in one party, 2-8 (default 8).
speed_check        Catch players whose game runs faster than normal (Cheat
                   Engine's speedhack and the like), measured from the timing
                   of what their game sends: "warn" (default) takes them out of
                   throwdowns and coop challenges until their speed is normal
                   again and tells the admins, "kick" removes them from the
                   server, "off" does not check.
score_check        Catch players whose mods change how many points tricks
                   score (per-trick points, the scoring multipliers, the
                   throwdown scoring logic) or how the skater handles (core
                   physics, wipeouts, trick gestures). Each player's ReSkate checks its
                   mods at launch and reports the result when joining:
                   "warn" (default) takes them out of throwdowns and coop
                   challenges (the server stops passing their throwdown
                   messages on) and everyone is told in chat, "kick" removes
                   them, "off" does not check. A player has to restart Skate
                   without the mod to take part again.
score_allow        Scoring fingerprints accepted like the game's own, for a
                   server that runs on a scoring mod everyone installs
                   (16 hex digits each; score-check lists each player's).
activity_log       Log what players do (default true): throwdown drops placed,
                   joins, starts, turns and results; objects placed or removed;
                   how long players take to load.
port, query_port   Steam game server ports (default 27015, 27016).
tps                Network updates per second: 20, 30, 60 or 120.
voice_chat         Allow voice chat.
voice_range        How far proximity voice reaches, 50-1000 m.
distances          When far-away players update less often (metres).
object_placement   everyone, admins (only admins can build), or nobody.
noclip, no_bail,   Let players use noclip (and tp) / No Bail / the forward and up
boosts             boosts (default true; admins always can).
enforce_tuning     Players skate with the game's own Gameplay/SkatePhysicsTuning,
                   not copies they edited (default true). Edited tuning (truck
                   positions and the rest) otherwise shows on their skater for
                   everyone.
parks              Layout for each park lot, e.g. "skatepark_01", or "empty".
world_layer_sync   Force the "layers" below on every player.
layers             World layer key -> "on" / "off".
admins             SteamID64s (as strings) who may change settings in-game.
bans               Players who can never join. Managed with ban / unban.
                   The ReSkate team's own list is separate: see global_bans.

Every change made from the console or by an admin is saved back to this file.

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
                                rotation uses (see map_pool).
  rotation [<minutes>|off]      Change the map on a timer (see map_rotation_minutes).
  name <text>   password <text|off>   welcome <text|off>   listed on|off
  tps 20|30|60|120   voice on|off   voice-range <m>
  distances <full> <half> <half-return> <low>
  placement everyone|admins|nobody   clear-objects
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
