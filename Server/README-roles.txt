ReSkate dedicated server: roles and permissions
===============================================

Who may do what on your server is in data/roles.json, next to data/bans.json.
The server writes the file the first time it starts. Edit it while the server
is stopped, or edit it and type "roles reload" in the server window. A file
with a mistake in it is not loaded: the server says what is wrong and keeps the
roles it had.

This file lists everything a role can be given: every permission and every
command.


The file
--------
  {
    "default_role": "player",
    "roles": [
      {
        "name": "moderator",
        "tag": "Mod",
        "color": "#5EC8FF",
        "rank": 50,
        "inherit": true,
        "commands": ["kick", "ban", "unban", "bans", "msg"],
        "permissions": ["reserved_slot", "anticheat_alerts"]
      }
    ],
    "players": {
      "76561198000000000": "moderator"
    }
  }

default_role   The role of every player who was given none. "" for no default
               role: players without a role then do what players always could
               (chat, vote, parties, private messages) and nothing else.
roles          Up to 32 roles.
players        Who was given which role, by SteamID64 (17 digits starting
               7656119). Up to 4096 players. A player has one role.


A role
------
name          1 to 24 characters: a-z, 0-9, - and _. Lower case. Not "none".
tag           The word shown before the player's name in chat and on their
              nametag. Up to 16 characters, "" for no tag. It cannot be a tag
              the game uses itself: Server, ReSkate, ReSkate Admin, Host,
              Friend, Dev, Developer, Staff, Homie, Centrix, Content Creator.
              A word the
              ReSkate team does not allow in chat is not shown as a tag either.
color         The tag's colour, "#RRGGBB".
rank          0 to 1000. See Rank below.
inherit       true (the default if left out): the role also has every command
              and permission the default role has. false: it has only what it
              lists itself. Use false for a role that takes something away,
              such as muted.
commands      The server commands the role may use: see Commands below.
permissions   What the role may do besides commands: see Permissions below.


Rank
----
A player cannot kick, ban or change the role of a player whose rank is the
same as theirs or higher, and cannot give out a role whose rank is the same as
theirs or higher. A player cannot change their own role. The server's own
window is above every role and can do all of it.

So a moderator (50) can mute a player (0) or make them a vip (10), and cannot
touch an admin (100) or make anyone a moderator.


Permissions
-----------
Put any of these names in a role's "permissions".

What every player normally does (the default "player" role has all four; leave
one out of a role with "inherit": false to take it away):

  chat               Talk in chat. Without it the player is muted: their
                     messages are not passed on, and they are told so.
  votes              Start the votes that are switched on (/vote map, /vote
                     kick, /vote tod, the server's own votes) and vote in them
                     (/yes, /no, /1, /2...).
  parties            Form, join and invite to parties, and party chat (/p).
  whisper            Send private messages (/w <player> <message>).

Extras:

  reserved_slot      Can join when the server is full. Each player whose role
                     has it adds one slot beyond max_players that only such
                     players use, the same as being in reserved_players_slots.
  unlimited_objects  No limit on placed objects; can build when placement is
                     "admins"; can resize objects when object-scaling is off.
  movement           Can use noclip, teleport, No Bail and the boosts when the
                     server has turned them off for players. Not while one of
                     them is locked for the player (lock, throwdown_locks).
  no_afk_kick        Is never removed for being away (afk-kick).
  vote_kick_immune   Cannot be the target of a kick vote.
  anticheat_alerts   Is told in chat when the anti-cheat finds something (for
                     example a skateboard kept on another player).
  polls              Can start a poll (/poll) when "votes polls" is "admins",
                     and can end anybody's poll early (/poll end).


Commands
--------
Put any of these names in a role's "commands". A player whose role has a
command types it in chat with a / in front (/kick Name), or in the game's
console as "mp server <command>", or uses the control for it in the game's
menus: the game shows a player the controls their role gives them.

  "*"   Every command below except role and reserved. A role has those two
        only when it lists them by name as well, e.g. ["*", "role"].

Any role with at least one command can also use help and roles (the list).

Players
  players              Connected players and their SteamID64s.
  kick                 kick <player> [minutes|restart]: off the server for
                       that long, or for as long as kick-time says.
  kick-time            kick-time <minutes>|restart: how long a kick lasts.
  ban                  ban <player or SteamID64> [name]: for good.
  unban                unban <SteamID64>.
  bans                 The ban list. (A role with ban, unban or bans also sees
                       the list in the game's menu.)
  tpall                tpall [player]: everyone to you, or to a player.
  tphere               tphere <player>: one player to you (in the game only).
  teleport             teleport <players> <player|x y z>: players to a player or
                       a place. <players> is one player, or SteamID64s joined
                       by commas.
  role                 role <player or SteamID64> <role>|none: give a player a
                       role or take theirs away, and "roles reload". Only by
                       name: "*" does not include it.
  reserved             reserved [add|remove <player or SteamID64>]: the list of
                       players with a reserved slot. Only by name: "*" does
                       not include it.

Messages
  msg                  msg <player> <text>: a private message from staff.
  msg-party            msg-party <player> <text>: to that player's whole party.
  msg-admins           msg-admins <text>: to everyone on who has a command.
  announce             announce <text>: to everyone, in chat and on a card.
  announce-to          announce-to <player> <text>: to one player, on a card.
  announcements        The messages posted on a timer: list, add, remove,
                       clear, interval.

Map and world
  map                  map <name>: change the map. Also lets the player pick
                       the server's level from Levels or Travel in the game.
  maps                 The maps this server knows. (maps reload, which reads the
                       Mods folder again, takes the map command.)
  map-pool             map-pool [add|remove <map>|clear]: the maps players vote
                       between and the rotation uses.
  rotation             rotation [<minutes>|off]: change the map on a timer.
  park                 park <lot> <layout>, park random: the park layouts.
  park-mod             The park mods (see Park mods in README.txt): list, add,
                       remove, clear and reload spawned parks, and "park-mod
                       config add|remove" to set one up (saved to the config).
  layer-sync           layer-sync on|off: force the server's world layers on
                       everyone.
  layer                layer <key> default|on|off: one world layer.
  layers               layers <key>=<mode> ...: several at once. The game's
                       World layers page uses this one.
  tod                  tod <time|default>: time of day for everyone.

Server settings
  name                 name <text>: the server's name.
  password             password <text|off>.
  welcome              welcome <text|off>: the message new players are sent.
  listed               listed on|off: shown in the server browser.
  chat-color           chat-color <#badge> [<#text>]: the server's own chat
                       colours (also chat-colour).
  status               Name, map, players, invite code.
  net                  net [player]: how the connections are doing.
  activity-log         activity-log on|off.

What players may do
  voice                voice on|off: voice chat. (The menu calls it voice-allow.)
  voice-range          voice-range <50-1000>: how far voice carries, in metres.
  placement            placement everyone|admins|nobody: who can build. (The
                       menu calls it object-placement.) "admins" means the
                       roles with unlimited_objects.
  objects              objects <number>|off: objects each player may place.
                       (The menu calls it object-limit.)
  object-scaling       object-scaling on|off: whether players may resize objects.
  clear-objects        Delete every placed object.
  effects              effects on|off: whether players see each other's effects.
  noclip               noclip on|off. (The menu calls it noclip-allow.)
  nobail               nobail on|off. (The menu calls it nobail-allow.)
  boosts               boosts on|off. (The menu calls it boosts-allow.)
  tuning               tuning on|off: everyone on the game's own physics. (The
                       menu calls it tuning-enforce.)
  parties              parties [on|off]: list the parties, or allow them.
  party-size           party-size <2-8>.
  afk-kick             afk-kick <minutes>|off.
  announce-throwdowns  announce-throwdowns on|off.
  lock                 lock <player|SteamID64> <tool[,tool...]|all>: take noclip,
                       nobail, boosts or tuning from one player.
  unlock               unlock <player|SteamID64> [<tool[,tool...]|all>].
  locks                The players with locked tools.
  throwdown-locks      throwdown-locks <tools>|off: what a throwdown locks for
                       its players.

Votes and polls
  votes                Every vote setting: which votes are on, their percent,
                       seconds, cooldown and min-players, "votes polls",
                       "votes poll-seconds", "votes starter-yes".
  vote                 vote <map|kick|tod|custom vote> [argument]: start a vote
                       as the server.
  vote-cancel          Stop the running vote.
  poll                 poll <question> | <answer> | <answer>...: start a poll
                       as the server; poll end.

Network
  distances            distances <full> <half> <half-return> <low>: how far
                       away skaters update at each rate.
  crowd                crowd <n>|off: the most poses a second one player is sent.
  rate                 rate <KB/s>: the send rate to each player.
  tps                  tps <n>: updates a second.

Anti-cheat
  speed-check          speed-check off|warn|kick: games running fast.
  board-check          board-check off|warn|kick: a skateboard kept on another
                       player.
  score-check          score-check [off|warn|kick]: mods that change scoring.
  score-allow          score-allow [<fingerprint>|remove <fingerprint>].
  bone-scale           bone-scale <1-8>|off: how far a mod may resize part of a
                       skater for the others.
  bone-reach           bone-reach <0.5-20>|off: how far a bone may be moved.

Not for any role (the server's own window only): say, poll-run, update, quit.

A name a role lists that is not a command does nothing: check the spelling
against this list.


Managing roles while the server runs
------------------------------------
  roles                             List the roles and who has them.
  roles reload                      Read data/roles.json again.
  role <player or SteamID64> <role> Give a player a role.
  role <player or SteamID64> none   Take it away: they are the default role again.

The player is told in chat when their role changes, and their game shows the
new tag and controls at once. Changes made with role are saved to the file.


The roles a new server starts with
----------------------------------
  player      rank 0, no tag. The default role.
              permissions: chat, votes, parties, whisper
  vip         rank 10, tag VIP.
              permissions: reserved_slot
  moderator   rank 50, tag Mod.
              commands: kick, ban, unban, bans, players, msg, msg-party,
                        msg-admins, tphere, vote-cancel, announce-to, role
              permissions: reserved_slot, no_afk_kick, vote_kick_immune,
                           anticheat_alerts, polls
  admin       rank 100, tag Admin.
              commands: "*", role, reserved
              permissions: all of them
  muted       rank 0, no tag, "inherit": false.
              permissions: votes, parties

vip, moderator and admin inherit from player, so they also chat, vote, use
parties and whisper.


Examples
--------
A helper who can only message and teleport players:

  {"name": "helper", "tag": "Helper", "color": "#7CFC9A", "rank": 20,
   "commands": ["msg", "tphere", "players"], "permissions": ["no_afk_kick"]}

A builder who is never limited, on a server where only staff build:

  {"name": "builder", "tag": "Builder", "color": "#FFA94D", "rank": 15,
   "commands": [], "permissions": ["unlimited_objects"]}

Someone who runs events: changes the map, the time and the parks, and
announces things, but cannot kick anyone:

  {"name": "events", "tag": "Events", "color": "#C792EA", "rank": 30,
   "commands": ["map", "map-pool", "rotation", "tod", "park", "announce",
                "poll", "vote", "vote-cancel"],
   "permissions": ["polls", "reserved_slot"]}

A player who may talk but not start votes:

  {"name": "no-votes", "tag": "", "color": "#FFFFFF", "rank": 0,
   "inherit": false, "commands": [],
   "permissions": ["chat", "parties", "whisper"]}

Everybody gets a tag: give the default role one.

  {"name": "player", "tag": "Skater", "color": "#FFFFFF", "rank": 0,
   "commands": [], "permissions": ["chat", "votes", "parties", "whisper"]}


From a server set up before roles
---------------------------------
An older ReSkateServer.json lists its admins under "access". The first time
the new server starts they are put in the admin role and taken out of the
config. "admin add", "admin remove" and "admins" are gone: use role and roles.
