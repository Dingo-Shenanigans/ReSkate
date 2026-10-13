# ReSkate dedicated server: roles

Who may do what on a server is in `data/roles.json`, next to `data/bans.json`. The server writes the file the first time it starts. Edit it while the server is stopped, or edit it and type `roles reload`. A file with a mistake isn't loaded: the server says what's wrong and keeps the roles it had.

The plain-text reference that ships with the server, with every command described, is [README-roles.txt](README-roles.txt).

## The file

```json
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
```

| Key | What it is |
|---|---|
| `default_role` | The role of every player who was given none. `""` means no default: those players can chat, vote, use parties and whisper, and nothing else. |
| `roles` | Up to 32 roles. |
| `players` | Who has which role, by SteamID64. Up to 4,096 players; one role each. |

## A role

| Field | Meaning |
|---|---|
| `name` | 1–24 characters: `a-z`, `0-9`, `-`, `_`. Not `none`. |
| `tag` | The word shown before the player's name in chat and on their nametag. Up to 16 characters, `""` for none. It can't be a tag the game uses itself (Server, ReSkate, ReSkate Admin, Host, Friend, Dev, Developer, Staff, Homie, Centrix, Content Creator). |
| `color` | The tag's colour, `#RRGGBB`. |
| `rank` | 0–1000. See [Rank](#rank). |
| `inherit` | `true` (the default): the role also has everything the default role has. `false`: only what it lists itself. Use `false` for a role that takes something away, such as muted. |
| `commands` | Server commands the role may use. See [Commands](#commands). |
| `permissions` | What it may do besides commands. See [Permissions](#permissions). |

## Rank

- A player can't kick, ban or change the role of anyone whose rank is the same as theirs or higher.
- A player can't give out a role whose rank is the same as theirs or higher, and can't change their own role.
- The server's own window is above every role.

So a moderator (50) can mute a player (0) or make them a VIP (10), but can't touch an admin (100) or make anyone a moderator.

## Permissions

What every player normally has (leave one out of a role with `"inherit": false` to take it away):

| Permission | Allows |
|---|---|
| `chat` | Talking in chat. Without it the player is muted and told so. |
| `votes` | Starting the votes that are on, and voting. |
| `parties` | Parties and party chat (`/p`). |
| `whisper` | Private messages (`/w`). |

Extras:

| Permission | Allows |
|---|---|
| `reserved_slot` | Joining when the server is full. |
| `unlimited_objects` | No object limit; building when placement is `admins`; resizing when object scaling is off. |
| `movement` | Noclip, teleport, No Bail and boosts when the server has turned them off for players. |
| `no_afk_kick` | Never removed for being away. |
| `vote_kick_immune` | Can't be the target of a kick vote. |
| `anticheat_alerts` | Told in chat what the anti-cheat finds. |
| `polls` | Starting polls when polls are set to `admins`, and ending anyone's poll. |

## Commands

A player whose role has a command types it in chat with `/` in front (`/kick Name`), uses `mp server <command>` in the game's console, or uses the matching control in the game's menus. The game only shows a player the controls their role gives them.

- `"*"` is every command **except** `role` and `reserved`. A role has those two only if it lists them by name, e.g. `["*", "role"]`.
- Any role with at least one command can also use `help` and `roles`.

| Group | Commands |
|---|---|
| Players | `players`, `kick`, `ban`, `unban`, `bans`, `tpall`, `tphere`, `teleport`, `role`, `reserved` |
| Messages | `msg`, `msg-party`, `msg-admins`, `announce`, `announce-to`, `announcements` |
| Map and world | `map`, `maps`, `map-pool`, `rotation`, `park`, `park-mod`, `layer-sync`, `layer`, `layers`, `tod` |
| Server settings | `name`, `password`, `welcome`, `listed`, `chat-color`, `status`, `net`, `activity-log` |
| What players may do | `voice`, `voice-range`, `placement`, `objects`, `object-scaling`, `clear-objects`, `effects`, `noclip`, `nobail`, `boosts`, `tuning`, `parties`, `party-size`, `afk-kick`, `kick-time`, `announce-throwdowns` |
| Votes and polls | `votes`, `vote`, `vote-cancel`, `poll` |
| Network | `distances`, `crowd`, `rate`, `tps` |
| Anti-cheat | `speed-check`, `board-check`, `score-check`, `score-allow`, `bone-scale`, `bone-reach` |

Server window only, never for a role: `say`, `poll-run`, `update`, `quit`. A name that isn't a command does nothing, so check spelling against this list. What each command does is in [README-roles.txt](README-roles.txt).

## Managing roles while the server runs

```
roles                               list the roles and who has them
roles reload                        read data/roles.json again
role <player or SteamID64> <role>   give a player a role
role <player or SteamID64> none     take it away (back to the default role)
```

The player is told in chat when their role changes, their game shows the new tag and controls at once, and the change is saved to the file. To mute someone: `role <player> muted`.

## The roles a new server starts with

| Role | Rank | Tag | Has |
|---|---|---|---|
| `player` (default) | 0 | none | `chat`, `votes`, `parties`, `whisper` |
| `vip` | 10 | VIP | `reserved_slot` |
| `moderator` | 50 | Mod | Commands: `kick`, `ban`, `unban`, `bans`, `players`, `msg`, `msg-party`, `msg-admins`, `tphere`, `vote-cancel`, `announce-to`, `role`. Permissions: `reserved_slot`, `no_afk_kick`, `vote_kick_immune`, `anticheat_alerts`, `polls` |
| `admin` | 100 | Admin | Commands: `"*"`, `role`, `reserved`. Every permission. |
| `muted` | 0 | none | `inherit: false`; only `votes` and `parties` |

`vip`, `moderator` and `admin` inherit from `player`, so they also chat, vote, use parties and whisper.

## Examples

A helper who can only message and teleport players:

```json
{"name": "helper", "tag": "Helper", "color": "#7CFC9A", "rank": 20,
 "commands": ["msg", "tphere", "players"], "permissions": ["no_afk_kick"]}
```

A builder who is never limited, on a server where only staff build:

```json
{"name": "builder", "tag": "Builder", "color": "#FFA94D", "rank": 15,
 "commands": [], "permissions": ["unlimited_objects"]}
```

Someone who runs events (map, time, parks, announcements) but can't kick:

```json
{"name": "events", "tag": "Events", "color": "#C792EA", "rank": 30,
 "commands": ["map", "map-pool", "rotation", "tod", "park", "announce", "poll", "vote", "vote-cancel"],
 "permissions": ["polls", "reserved_slot"]}
```

A player who may talk but not start votes:

```json
{"name": "no-votes", "tag": "", "color": "#FFFFFF", "rank": 0, "inherit": false,
 "commands": [], "permissions": ["chat", "parties", "whisper"]}
```

Everybody gets a tag: give the default role one.

```json
{"name": "player", "tag": "Skater", "color": "#FFFFFF", "rank": 0,
 "commands": [], "permissions": ["chat", "votes", "parties", "whisper"]}
```

## Good to know

- **Two different "admins":** a server's `admin` role (tag "Admin") is the owner's own staff. "ReSkate Admin" is the ReSkate team's tag, comes from the ReSkate backend, and can't be given by a server.
- **First admin on a new server:** type `role <your SteamID64> admin` in the server window.
- **Older configs:** admins listed under `"access"` in `ReSkateServer.json` are moved into the `admin` role the first time the new server starts. `admin add`, `admin remove` and `admins` are gone; use `role` and `roles`.
- **Server window:** the server's own window has a roles editor for creating and changing roles without editing the file.
