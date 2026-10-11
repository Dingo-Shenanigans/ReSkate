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
next to the server (its reskate-levels.json and manifest.json are read). The
map can then be chosen by name. Players need the same map mod installed to
join.

A player without it is offered it when the map is from Thunderstore: the game
downloads it, applies it and joins the server again. For that the folder has to
keep the name Thunderstore gives it: Owner-Name as the launcher installs it
(Sandos-Vancouver_Plaza), or Owner-Name-1.0.0 as its zip unpacks. The server
tells players that name, and the version from manifest.json when the file is
there. A map under any other folder name
works as before: players install it themselves.

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
chat_color         The colour of the server's own lines in chat: its "Server" badge
                   and name, as "#RRGGBB" (default "#8E5CFF", violet).
chat_text_color    The colour of the text of those lines (default "#D9C8FF",
                   lavender). Pick one that reads on a dark background.
                   Console: chat-color <#badge> [<#text>].
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
afk_kick_minutes   Remove a player who has been away this many minutes, 1-1440
                   (default 0: never). Away is not moving, speaking, typing
                   in chat or changing their objects. They are warned in chat
                   a minute before and can join again at once. Admins are
                   never removed for it. Console: afk-kick <minutes>|off.
word_warnings      The ReSkate team keeps a list of words that are not allowed
                   in chat at all (read from api.reskate.dev with the global
                   bans). A message with one is never passed on, whatever
                   this is set to, and its player is warned. After this many
                   warnings, 1-10 (default 3), the next one gets them kicked;
                   they can join again, and are kicked again at the next.
                   Warnings last until the server restarts. 0: nobody is
                   warned or kicked; the message is still not passed on.
allow_voice_chat   Allow voice chat.
voice_range        How far proximity voice reaches, 50-1000 m.
object_placement   everyone, admins (only admins can build), or nobody.
object_limit       How many objects each player may have placed, 1-1024
                   (default 100), or 0 for no limit. Admins are not limited.
                   A player at the limit deletes one to place another.
                   A player who places more than twice the limit plus 100
                   in a minute (a modified game animating objects by
                   respawning them) has theirs deleted for everyone, and
                   nothing they place is shared for a minute; the log
                   says who. Admins are exempt.
allow_object_scaling  Let players place objects bigger or smaller than their
                   own size (default true). false shares every player's
                   objects at their own size and turns the size controls
                   off in their park editor. Admins can still resize
                   theirs. Console: object-scaling on|off.
sync_effects       Let players see each other's skater effects (default true):
                   sparks and dust where a skater touches the world, and
                   the trails and fire of costumes and skateboards. false
                   relays none and players' games show each other without
                   them, which saves a little traffic and drawing on a busy
                   server. Console: effects on|off.
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
bone_reach_limit   How far, in metres, a bone of a skater's body or board may
                   be from the one it hangs from for the other players: 0.5
                   to 20 (default 1); 0 is no limit. The game never moves
                   them (the longest, a thigh, is 0.44), so the default
                   changes nothing for an ordinary player and stops a hacked
                   game from stretching its skater across the map. Every
                   player's own game also holds what it is shown to 2.
                   Console: bone-reach <0.5-20>|off.

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
threads            How many threads share the sending of each pass (default 0:
                   one for each of the machine's processors but one, up to
                   8; 1: a single thread, as before). Most of a full
                   server's work is building each player's update of everyone
                   else, which the threads do for several players at once.
                   What is sent is the same whatever the number. The server
                   says how many it uses when it starts. Takes effect on
                   restart.
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
                   Each vote may also have its own "seconds" and
                   "cooldown_seconds" (0: the ones above) and "min_players", the
                   players who must be on before anyone can start it (default 1).
starter_votes_yes  Whoever starts a vote has voted yes (default true).
custom             Votes of your own: a list, each running a server command
                   when it passes. {map} in the command is the current map,
                   {arg} the choice the player picked (one of "choices"; without
                   choices the vote takes no argument). For example:
                     {"name": "restart", "description": "Reload the current map",
                      "command": "map {map}", "percent": 60}
                     {"name": "noclip", "description": "Turn noclip on or off",
                      "command": "noclip {arg}", "choices": ["on", "off"]}
                   Players start them with /vote restart, /vote noclip off; /vote
                   list shows them. A name and each choice is 1-16 of a-z, 0-9,
                   - and _, and not one of the server's own (map, kick, tod,
                   list...). Each takes "enabled", "percent", "seconds",
                   "cooldown_seconds" and "min_players" as the others do. Up to 16.
polls              Who may ask everyone a question with up to six answers:
                   "off", "admins" (default) or "everyone".
                     /poll Next map? | Grom | San Vansterdam | Stadium
                   Players answer on the card or with /1, /2... A poll runs
                   nothing; it ends after "poll_seconds" (default 60), or when
                   whoever started it (or an admin) types /poll end.
Server votes       The console (and scripts that talk to it) starts the same
                   votes and polls: "vote tod night", "vote map grom",
                   "vote noclip off", "poll Next map? | Grom | Stadium" and
                   "poll end". A vote still has to be on, and its "min_players"
                   met; a poll may be asked whatever "polls" says. The server has
                   no vote of its own and no cooldown. Chat reads "The server
                   started a vote to ..." or "The server asks: ...".
                   "poll-run <command> | <question> | <answer>..." also runs a
                   command for the answer that wins, {answer} replaced by it
                   (nothing on a tie or when nobody answered), e.g.
                     poll-run tod {answer} | Time of day? | morning | noon | night

"announcements" - Messages from the server.
messages           Lines the server posts in turn, one every "interval_minutes"
                   (0: off) while players are on. Each is at most one chat line.
                   Announcements show as a card at the top of the screen, not
                   in chat; :emotes: in them (and in polls) show as images. Admins announce something once with announce <text>,
                   or to one player only with announce-to <player> <text>.

"commands" - Chat commands of your own, e.g. /discord or /rules. A list, each:
name               1-16 of a-z, 0-9, - and _; not one of the server's own
                   commands (help, party, vote, kick, map...).
reply              A chat line sent back to whoever typed it (optional).
command            A server command, or a list of up to 8, run as the console
                   (optional). {player} is the SteamID64 of whoever typed it,
                   {map} the current map and {arg} the rest of what they
                   typed. Each command needs a reply, a command, or both.
admin              Only admins may use it (default false).
                   For example:
                     {"name": "discord", "reply": "Join us: discord.gg/..."}
                     {"name": "rules", "command": "announce-to {player} No griefing!"}
                     {"name": "restart", "admin": true, "command": ["map {map}"]}
                   They are not listed in /help: tell players about them yourself.
                   Up to 32. Mind what {arg} lets players put into a command.

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
  object-scaling on|off         Whether players may resize the objects they place.
  effects on|off                Whether players see each other's skater effects.
  noclip on|off   nobail on|off   boosts on|off
                                What players may use (admins always can).
  tuning on|off                 Everyone on the game's own physics tuning.
  tpall [player]                Everyone to you (admins in game) or to a player.
  tphere <player>               One player to you (admins in game).
  park <construction|historic|financial> <layout>
  park random                  Randomize all three park slots (excludes empty lots).
  layer-sync on|off   layer <key> default|on|off
  layers <key>=<mode> ...       Several world layers at once, each default, on or off.
  tod <default|morning|noon|afternoon|evening|night|weatherday|weathernight>
                                Time of day on every map (needs layer-sync on).
  votes [map|kick|tod on|off|<percent>]   The vote settings (see votes).
  votes seconds <n>   votes cooldown <n>   vote-cancel
  votes <vote> seconds|cooldown|min-players <n>   One vote's own limits; <vote>
                                is map, kick, tod or a custom vote's name, which
                                also takes on|off|<percent>.
  votes polls off|admins|everyone   votes poll-seconds <n>   votes starter-yes on|off
  vote <map|kick|tod|<custom vote>> [argument]   Start a vote as the server
                                (see Server votes).
  poll <question> | <answer> | <answer>...   Ask everyone (2 to 6 answers).
  poll end                      End the running poll now.
  poll-run <command with {answer}> | <question> | <answer>...   A poll whose
                                winning answer runs the command (console only).
  announce <text>               Tell everyone, in chat and on a card.
  announce-to <player> <text>   Tell one player, on a card.
  announcements [list | add <text> | remove <n> | clear | interval <minutes>|off]
                                The messages posted on a timer.
  activity-log on|off           Log player activity (see activity_log).
  announce-throwdowns on|off    Chat message when a throwdown is placed.
  parties [on|off]              List the parties, or allow them (off ends them all).
  party-size <2-8>              Most players in one party.
  afk-kick <minutes>|off        Remove players who have been away that long.
  speed-check off|warn|kick     What happens to players whose game runs fast.
  score-check [off|warn|kick]   What happens to players whose mods change scoring
                                or physics; with no argument, every player's result.
  score-allow [<fingerprint>|remove <fingerprint>]   Accept a scoring mod's
                                fingerprint like the game's own (or list them).
  admin add|remove <player or id>   admins      (console only)
  update                        Check for a new release and install it now (console only).
  quit, exit or stop            Shut the server down (console only).
