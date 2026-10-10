# Server scripts: the Lua API

A ReSkate dedicated server can run Lua 5.4 scripts that add chat commands. A script can list players, message them, kick, ban or teleport them, and change the map, the time of day and the placed objects.

The example scripts in this folder (`hello.lua`, `rules.lua`, `goto.lua`, `props.lua`) are ready to copy.

- [Quick start](#quick-start)
- [Where commands run](#where-commands-run)
- [`server.command`](#servercommand)
- [Players](#players)
- [Chat](#chat)
- [Moderation](#moderation)
- [World](#world)
- [Logging](#logging)
- [What Lua can do here](#what-lua-can-do-here)
- [Limits](#limits)
- [Errors](#errors)
- [Recipes](#recipes)

## Quick start

1. Make a `scripts` folder next to `ReSkateServer` (or `ReSkateServer.exe`).
2. Put a `.lua` file in it:

   ```lua
   -- scripts/hello.lua
   server.command("hello", function(player, args)
     return "hi " .. player.name .. ": " .. args
   end)
   ```

3. Start the server. Typing `/hello world` in chat answers `hi <your name>: world`.

The server loads scripts once, when it starts:
- **Order:** files load in name order. Prefix names with numbers (`10-rules.lua`, `20-goto.lua`) to control the order.
- **Loading errors:** a file that fails to load is logged and skipped. Commands it registered before the error stay registered.
- **Changes:** to pick up edits, restart the server.
- **Shared state:** all files share one Lua state, so a global set in one file is visible in the others. Globals keep their values between commands until the server restarts.
- **Encoding:** files can be saved as UTF-8 with or without a byte-order mark. Only source text is accepted; see [What Lua can do here](#what-lua-can-do-here).

## Where commands run

| Who | Types | Notes |
|---|---|---|
| A player | `/hello world` | Normal commands only, never `admin = true` ones. |
| An admin | `/hello world` | All script commands. |
| The server console | `hello world` | All script commands. The caller is the console's [player table](#the-player-table). |

The server's own commands always come first. A script command named `kick`, `vote`, `party` or `w`, for example, never runs from chat. Give your commands names the server doesn't already use.

`/help` in chat lists the script commands that player may run, and `help` in the console lists them all.

## `server.command`

```lua
server.command(name, function(player, args) ... end [, options])
```

Adds the chat command `/name`.

**`name`**
- 1 to 32 characters: letters, digits, `-` or `_`.
- Case doesn't matter: names are stored in lowercase, and players' typing is lowercased before matching.
- Any other name is an error at load time.
- Registering a name a second time replaces the first function.

**`function(player, args)`** runs each time someone uses the command.
- `player` is the caller's [player table](#the-player-table).
- `args` is the rest of the line after the command name, with leading and trailing spaces trimmed. It is `""` when nothing follows.
- `args` is exactly what the player typed. Treat it as untrusted.

**What the function returns is the reply:**
- A string is sent to the caller. Each `\n` starts a new chat line. A reply shows at most 12 lines, and each line is cut to 200 bytes.
- A number is sent as text.
- `nil` (or no `return`) sends nothing to a player. An admin sees `Done.`
- If the function errors, the caller sees `The /name command failed.` and the log gets the details. See [Errors](#errors).

**`options`** is an optional table:

| Field | Meaning |
|---|---|
| `admin = true` | Only admins and the console can run it. To other players it doesn't exist: they get `Unknown command`, and `/help` doesn't list it. |

```lua
server.command("bring", function(player, args)
  local target = server.player(args)
  if not target then return "Who?" end
  if not player.x then return "You have no position yet." end
  server.teleport(target, player.x + 2, player.y + 1, player.z)
end, {admin = true})
```

## Players

### The player table

Functions that describe a player return a plain table:

| Field | Type | Meaning |
|---|---|---|
| `id` | `integer` | SteamID64. `0` for the console. |
| `name` | `string` | The name shown in game. `"Server"` for the console. |
| `admin` | `boolean` | Whether they are an admin. Always `true` for the console. |
| `objects` | `integer` | How many objects they have placed. Not set for the console. |
| `x`, `y`, `z` | `number` | Their position, in metres. Not set until their game has sent one, and never for the console. |

The table is a snapshot taken when you asked. It doesn't update, and changing it changes nothing on the server.

Check for a position before you use it:

```lua
if not p.x then return p.name .. " has no position yet." end
```

### Naming a player (`who`)

Wherever a function takes `who`, you can pass any of:
- a SteamID64, as an integer: `76561198000000000`
- a player table (only its `id` is used)
- a string: the start of one connected player's name (case doesn't matter), or a SteamID64 written as text

A name that matches no one, or more than one player, matches nobody. Only connected players count, except for [`server.ban`](#serverbanwho).

### `server.players()`

Returns an array of player tables, one per connected player. The console is not included.

```lua
for _, p in ipairs(server.players()) do
  print(p.name, p.objects)
end
```

### `server.player(who)`

Returns that player's table, or `nil, "no such player"`.

## Chat

Text sent to players is cleaned before it goes out. Control characters and line breaks are removed, invalid UTF-8 is dropped, and each line is cut to 200 bytes.

### `server.say(text)`
Sends one chat line, from the server, to everyone. Returns nothing.

### `server.tell(who, text)`
Sends chat lines to one player. Each `\n` starts a new line, up to 12 lines. Returns `true`, or `false` when `who` matches nobody.

### `server.announce(text)`
Posts a chat line to everyone, and shows it on the announcement card if the server's announcement card is on. It is logged as an announcement. Returns nothing.

## Moderation

These functions work like the console commands of the same name, with console rights. They return the server's answer as a string (`"Bob was kicked until the server restarts."`), or `nil, "no such player"`. Every use is written to the log as `[script] ...`.

### `server.kick(who)`
Disconnects the player. They can't rejoin until the server restarts.

### `server.ban(who)`
Bans the player and disconnects them. `who` can also be the SteamID64 (an integer) of someone who has already left. The ban is saved in `ReSkateServer.json`.

> Scripts act with console rights, so they **can** kick and ban admins. If a command lets players choose who gets kicked or banned, check `target.admin` first.

### `server.teleport(who, x, y, z)`
Moves the player to that position (metres). Returns `true` if the teleport was sent. Returns `false` if `who` matches nobody or their game hasn't finished loading the map.

`x`, `y` and `z` must be ordinary numbers within 1,000,000 of the origin. `NaN`, infinity or anything further away is an error. To put someone next to another player, offset the position a little (as `goto.lua` does) so they don't land inside them.

## World

### `server.map([name])`
- `server.map()` returns the current map's name.
- `server.map(name)` changes the map. It takes the same names as the `map` console command, and returns the server's answer, such as `"Changing map to ..."` or `"No single map is called ..."`.

Only maps the server has can be loaded: the game's own maps, or ones from mods in the `Mods` folder next to the server.

### `server.tod(time)`
Sets the time of day for everyone: `morning`, `noon`, `afternoon`, `evening`, `night`, `weatherday`, `weathernight`, or `default`. Returns the server's answer. This needs `world-layers.json` next to the server; without it, the answer says so.

### `server.clear_objects()`
Deletes every object players have placed. Returns the server's answer, such as `"Deleted 12 placed objects."`

## Logging

### `server.log(...)` and `print(...)`
These write one line to the server's log and console, starting with `[script]`. The arguments are joined with spaces, the same way `print` joins them. Line breaks become spaces, and the line is cut to 200 bytes.

## What Lua can do here

Scripts get standard Lua 5.4 with these libraries:

| Available | Not available |
|---|---|
| The base functions (`pairs`, `ipairs`, `pcall`, `error`, `tostring`, `tonumber`, `type`, `select`, `setmetatable`, `rawget`...) | `load`, `loadfile`, `dofile`, `require`, `collectgarbage` |
| `string` (all of it except `string.dump`) | `io`, `debug`, `package`, `coroutine` |
| `table`, `math`, `utf8` | `os.execute`, `os.exit`, `os.getenv`, `os.remove`, `os.rename`, `os.tmpname`, `os.setlocale` |
| `os.time`, `os.clock`, `os.date` | `__gc` metamethods: `setmetatable` refuses a metatable that has one |

Script files must be source text. A precompiled (`luac`) file is refused, and scripts can't load code at runtime. Whatever players type therefore only ever reaches a script as a string, and never runs as code.

## Limits

These keep a buggy command from freezing or crashing the server. A command that hits a limit stops, and its player is told it failed.

| Limit | Value | Error in the log |
|---|---|---|
| Work per command (and per file while it loads) | About 2 million Lua instructions, a few milliseconds | `the script ran too long` |
| Memory, all scripts together | 32 MB | `not enough memory` |
| Work per string-pattern call (`find`, `match`, `gsub`, or each step of `gmatch`) | About 2 million matching steps | `pattern too complex` |

A few things to know:
- **`pcall` can't extend the time limit.** It can catch the error, but from then on every instruction raises it again, so the command still stops.
- **Big globals cost every later command.** Data kept in globals counts against the 32 MB for as long as the server runs. Keep only what you need.
- **Patterns are safe with player text.** A pattern a player types, even a deliberately slow one, stops with `pattern too complex` instead of stalling the server. For a plain substring search, `string.find(text, word, 1, true)` is still the faster choice.
- **Scripts don't trigger scripts.** A script command never runs another script command. If a script action (such as `server.map`) would reach one, it acts as if that command doesn't exist.

## Errors

| When | What happens |
|---|---|
| Loading, at startup | The log shows `Script <file>: <file>:<line>: <message>`. The file's remaining code doesn't run. Commands it already registered stay. |
| A command errors | The player sees `The /<name> command failed.`, and the log shows `[script] /<name> failed: <file>:<line>: <message>`. Players never see the file or the message. |
| A bad argument to a `server` function | A normal Lua error, such as `bad argument #2 to 'teleport' (not a place in the world)`. Catch it with `pcall` if you want to handle it. |

`server.player`, `server.kick` and `server.ban` don't error when no one matches. They return `nil, "no such player"`. `server.tell` and `server.teleport` return `false`.

## Recipes

**Reply on several lines**
```lua
server.command("info", function()
  return "Map: " .. server.map() .. "\nPlayers: " .. #server.players()
end)
```

**Take more than one argument**
```lua
server.command("tpto", function(player, args)
  local x, y, z = args:match("^(%S+)%s+(%S+)%s+(%S+)$")
  x, y, z = tonumber(x), tonumber(y), tonumber(z)
  if not (x and y and z) then return "/tpto <x> <y> <z>" end
  local ok, sent = pcall(server.teleport, player, x, y, z)
  if not ok then return "That is not a place in the world." end
  return sent and "Teleported." or "You can't be teleported yet."
end, {admin = true})
```

**Remember something between commands**
```lua
local seen = {}   -- lives until the server restarts

server.command("checkin", function(player)
  seen[player.id] = os.time()
  return "Checked in."
end)
```

**Don't let players act on admins**
```lua
server.command("votekick", function(player, args)
  local target = server.player(args)
  if not target then return "No single player matches that." end
  if target.admin then return "You can't target an admin." end
  -- ...
end)
```

**Debug a script**

Use `print(...)` and watch the server console. For details on a failed command, look for the `[script] /<name> failed:` line in the log.
