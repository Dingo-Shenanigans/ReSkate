# The throwdown kit: throwdown modes in Lua

A throwdown mode is a plugin. ReSkate knows no particular mode: a plugin registers its modes,
publishes documents that say what the player should see, and hears what the player does as
events. ReSkate shows the documents with skate.'s own native widgets: the Throwdowns page, the
setup page, the start flag, the waiting card, the in-game HUD and the match panel. A new mode needs
no change to ReSkate.

The full reference, with every field's limits, is on the docs site's throwdowns page
(plugins/api/throwdowns in the docs repo). Two plugins use it:

- [throwdown-modes-test](plugins/throwdown-modes-test): two modes through the whole native flow
  (card, setup page, flag, waiting card) and no match. The smallest example.
- [SmokeyDawg58-ReSkate_Throwdown](plugins/SmokeyDawg58-ReSkate_Throwdown): SmokeyDawg58's ten
  modes (Deathrace, 1-UP, Spot Jam and the rest), with their rules, HUDs and route editor, all in
  Lua. The full example.

Everything below needs the `throwdowns` permission in `reskate-plugin.json`. A plugin's documents
only drive its own modes, and its events only reach it.

## 1. Registering a mode

```lua
reskate.throwdowns.mode{
    id = "duel",                 -- unique in the plugin, 1 to 32 characters, no "/"
    name = "Duel",
    description = "Beat the last total. Three misses and you're out.",
    art = "UI/MyMod/Throwdowns/img_duel",   -- the card's logo: a texture or image asset path
    color = 0xff3434ff,          -- the card's tint, 0xAABBGGRR (optional)
    card = "spot",               -- the stock card it is drawn like: "skate", "spot" or "jam"
    replaces = "skate",          -- optional: takes that stock card's place on the page
    order = 10,                  -- its place among the plugins' modes
    flag = "World/MyMod/Flags/duel_Placement",  -- optional: the flag's placement preview prefab
    cursor_height = 4,           -- optional: metres the placement cursor may rise onto things
    players = {min = 1, max = 6, default = 2},
}
```

Calling it again with the same id updates the mode; `reskate.throwdowns.remove_mode(id)` takes it
off the page. The page shows the stock S.K.A.T.E. (unless a mode replaces it), Spot Battle and
Skate Jam, then every plugin mode by `order`. Clicking a card sends the `open` event.

## 2. Documents

```lua
reskate.throwdowns.ui{setup = ..., waiting = ..., hud = ..., panel = ..., flags = ..., match = ...}
```

Publish whenever something changes; every tick is fine, since an unchanged document costs only a
comparison. Any part left out shows nothing. In documents, `mode` is the plugin's own id ("duel").
Colours are 0xAABBGGRR, times are milliseconds, positions are vectors in world metres, and Steam
IDs are strings. Texts may also be numbers.

### setup: the setup page

The stock Spot Battle setup page, relabelled. It shows while the player is on it after `open`.

```lua
setup = {
    mode = "duel", title = "DUEL", art = "UI/...", description = "...", back = "BACK",
    players = {caption = "Players", min = 1, max = 6, step = 1, value = 2},
    timer = {caption = "Turn", min = 10, max = 120, step = 10, value = 20},   -- nil: no timer row
    rows = {                                    -- at most 12
        {id = "laps", kind = "choice", caption = "Laps", options = {"1", "2", "3"}, value = 1},  -- value from 1
        {id = "boost", kind = "slider", caption = "Boost", min = 0, max = 5, step = 1, value = 2},
        {id = "route", kind = "button", caption = "Route", label = "Place gates"},
        {id = "note", kind = "info", caption = "Area", label = "Anywhere"},
    },
    hide_native = {"rounds"},                   -- stock rows taken off (the default)
}
```

Moving a slider or a choice sends `setup_row`; pressing a button row sends `setup_button`. Confirm
places the start flag (section 4).

### waiting: the waiting card

Shown while the flag is down and the match has not started.

```lua
waiting = {mode = "duel", title = "DUEL", art = "UI/...", status = "Enrolled skaters  |  Host starts",
           enrolled = 1, capacity = 6, can_start = true,
           countdown = {remaining_ms = 5000, total_ms = 10000}}   -- optional
```

Start sends `start`, Leave sends `leave`.

### hud: the in-game HUD

Slots of the native HUD, drawn with the widget family `theme` names (`HUD_<theme>_Title_..._Widget`
and so on; a mod brings its own in its assets). Every slot is optional.

```lua
hud = {
    active = true, theme = "race",
    header = {title, art, status, offset = {x, y}, icon_size = 96, frame_width = 704},  -- 0: the theme's
    layout = "side",                  -- where the board sits: "side" (right edge) or "bottom"
    counters = {"LEADER 1,200", "SCORE 900"}, score_width = 480, score_art = "",
    clock = Clock,                    -- the match clock
    board = {title = "LEADERBOARD", columns = {"POS", "NAME", "SCORE"},
             variant = "",            -- "stamps": a stamp per mark instead of joined letters
             style = "outlined",      -- or "plain"
             heading = "list",        -- or "shared"
             name_art = "", out_art = "", rank_art = "", value_art = "", value_width = 0, reserve = "",
             rows = {BoardRow, ...}},
    results = {title, notice, caption, heading_art = "", title_width = 0, style = "outlined", rows = {BoardRow, ...}},
    intro = {title, art_key, rules, until_ms},
    countdown = Clock,                -- 3-2-1-GO
    banner = {title, subtitle, art, serial, ms},  -- a new serial shows it again
    callout = {text, style, serial, ms},
    warning = {text, art},            -- shown while text is not empty
    card = {caption, title, image},   -- a trick card; image is a texture path
    list = {caption, items = {"KICKFLIP", ...}},
    position = {place = 1, medal = "gold", art = ""},   -- "gold" | "silver" | "bronze" | "white"
    timers = {{caption, text = "0:12.40" or Clock}, ...}, timer_art = "",
    compass = {at = vector, art = "UI/..."},
    markers = {{at = vector, text = "GATE 3", distance = true}, ...},  -- distance from the skater
    blackout = {text},
    editor = {title, variant = "builder",   -- or "compact"
              prompts = {{inputs = {"UI_DpadLeft"}, caption = "Move", glyph = "", hold = 0.5}, ...},
              info = {title, rows = {{caption, value}, ...}},
              toast = {text, alpha},
              modal = {text, actions = {{input, caption}, ...}}},
}
-- BoardRow: {rank, name, value, color, self = bool, out = bool,
--            marks = {{text = "S", on = true}, ...}, clock = Clock}
-- Clock: {remaining_ms, total_ms, sampled_ms, ticking = true, up = false}, or a stopwatch
--        {base_ms, sampled_ms, running, cap_ms}. The HUD ticks a clock itself between documents;
--        sampled_ms is reskate.time.now() when the numbers were true.
```

### panel: the match panel

The native details page the pause menu opens during a match.

```lua
panel = {title, description, art, score_caption = "SCORE", quit = "Quit",
         settings = {{icon = "players", caption = "Players", value = "4"}, ...},  -- at most 3;
                                                    -- icon "players" | "timer" | "rounds" | a texture
         prompts = {{input = "UI_DpadUp", caption = "Spectate", command = "spectate"}, ...},
                                                    -- 3 are drawn; a press sends panel_command only for
                                                    -- UI_DpadUp/Down/Left/Right, UI_L3Click and UI_R3Click
         players = {{id = "7656...", name, value, rank, outcome, portrait = "steam", self = true}, ...}}
```

Opening it sends `panel_open`; a prompt's button sends `panel_command` with its command.

### flags: flags for other players

```lua
flags = {{id = "f1", mode = "duel", leader = "7656...", game = 123, at = vector, yaw = 0,
          players = 1, capacity = 6, open = true,
          stats = {{title = "LAPS", value = "3"}, ...}}}   -- at most 3
```

Each shows on other players' games as a native flag (map point, bubble, map card, Enroll card).
Their Join reaches this plugin on their game as `join`.

### match: what a running match asks of the game

```lua
match = {markers = true,      -- session markers and Return are blocked; the pause menu's Quit is the plugin's
         input_lock = true,   -- no skating input (a countdown)
         offboard = true,     -- the skater is kept off the board
         hidden = {"7656..."},-- players whose skater, nametag, map dot and chat bubble are hidden
         teleports = true,    -- the plugin may teleport the skater even where noclip is off
         in_activity = true}  -- the game treats the player as in an activity
```

## 3. Events

```lua
reskate.on("throwdown_ui", function(e) -- e = {action, mode, args}: args are strings
end)
```

| action | args |
| --- | --- |
| `open` | game: a card was clicked, and the flag registration began (section 4) |
| `setup_row` | row id ("players" and "timer" for those), value (a choice's from 0) |
| `setup_button` | row id |
| `setup_back` | game |
| `setup_confirm` | players, timer seconds |
| `flag_placed` | game, x, y, z, yaw |
| `flag_left` | game |
| `start` | game |
| `leave` | game |
| `quit` | (the pause menu's or the panel's Quit) |
| `join` | flag id, leader, game |
| `panel_open` | |
| `panel_command` | command |
| `dialog` | dialog id, button, text, row, file text (section 5) |

## 4. The native flag

One native flag registration exists at a time. Clicking a mode's card begins it at once, with the
mode's players and the setup page, and then sends `open {game}`. Confirm on the setup page goes on
to native flag placement (`setup_confirm`, then `flag_placed`), then to the waiting card, until the
plugin starts the match or the owner leaves.

```lua
reskate.throwdowns.flag.begin(mode, game, {players, seconds, setup_page = true, cursor_height})
-- false while another registration is busy; setup_page = false goes straight to placement
reskate.throwdowns.flag.state()
-- {owned, waiting, placing, busy, mine, mode, game}: mode and game only when it is one of the plugin's
reskate.throwdowns.flag.leave()   -- refused, finished or cancelled: the registration goes
```

## 5. Services a mode uses

None of these knows a throwdown mode; any plugin may use them under its permission.

| Call | Permission | What it does |
| --- | --- | --- |
| `reskate.files.list/read/write/remove(path)` | files | the plugin's own data folder, for saved routes and the like: 1 MB a file, 1000 files, 64 calls a second |
| `reskate.ui.dialog(id, {title, message, field, list, buttons, file})`, `reskate.ui.close_dialog(id)` | ui.screen and throwdowns | an overlay dialog: a text field, a list with buttons on each row, up to 4 buttons, a file to open; answers come as `dialog` events, Esc answers "cancel" |
| `reskate.skater.push(velocity)`, `push_wait()`, `push_reset()` | skater.control | the skater's velocity once it is back on its board |
| `reskate.skater.hold(on)` | skater.control | the skater held where it is |
| `reskate.physics.competition(on)`, `stock_ready()`, `prepare()` | skater.control | the game's stock physics while the player competes |
| `reskate.skater.telemetry()`, `reskate.skater.goofy()` | skater.read | speed, air time, the last jump, the physics state |
| `reskate.skater.bone_cam(word, ...)` | camera | the Bone Cam |
| `reskate.camera.free(...)`, `reskate.camera.pad_flight(on)` | camera | the free camera, flown from the pad while placing things from the air |
| `reskate.session.world()`, `reskate.session.spectate(...)` | session.read | the session's world id and map; spectating a player |
| `reskate.tricks.feed(on)`, `reskate.tricks.gesture(name)` | events.tricks | skate.'s trick list as `trick_notes` events; a flip trick's flick diagram |
| `reskate.input.pad_bits/key/wheel/game_buttons/allowed/focus` | input | the pad, keys and wheel, and game buttons kept from the game |
| `reskate.entities.set(id, blueprint, transform, hidden?)`, `remove`, `clear`, `ready`, `status` | world.entities | world objects of the plugin's own (gates, arrows, flags) |
