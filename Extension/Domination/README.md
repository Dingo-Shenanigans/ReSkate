# Domination

skate. 3's Domination, for ReSkate. Own the most spots in a park. The best single trick landed on a spot owns it, and a higher one steals it. Play alone, or with everyone in a session.

How it's built, where the game logic is and how to change it safely: [ARCHITECTURE.md](ARCHITECTURE.md).

## Playing

Domination runs from the console (`~`); a menu for picking and starting custom multiplayer modes is a separate change (Custom Multiplayer), which Domination plugs into through `domination::snapshot()` and these commands.

1. Pick a park: `domination parks` lists every park, its map and whether that map is installed here; `domination set map next|previous` picks the map and `domination set park <id>` (or `next`, `previous`) a park on it. Without a pick, rounds are in the red-bowl park (when the Skate 3 map mod is installed), else in this map's first park.
2. Set the next round with `domination set`:
   - duration: 20 to 3600 seconds (5 minutes by default);
   - players: everyone in the session, or solo;
   - teams: `teams on` (red against blue), off by default.
3. Start it with `domination start`.
   - Each skater is put at one of the park's starts, facing the park; in a session, every player gets a different start.
   - Then **3, 2, 1, GO**, and the clock runs.
   - A park on another map: the round waits; `load` the park's map, which the reply names, and it starts once you're in. A park whose map mod isn't installed (or is switched off) says what to get instead.
4. **During the round:**
   - markers float over the spots in their owner's colour, with the owner's name and score, and each spot's edge near you glows faintly on the ground in the same colour (fading with distance, and hidden behind walls and ramps);
   - arrows at the screen's edge point to the nearest spots you don't own (your team's count as yours), with their distance;
   - the timer is at the top and the spot list on the right, with standings in multiplayer;
   - each landed trick says what it did: TAKEN, new best, or what the holder has;
   - a spot that changes hands flashes, and the feed under the timer says who took it from whom; a take is heard (the game's own menu "select" sound) and so is a loss ("cancel").
5. **At the end:** for 12 s, or until you ride off, the results: who won (the most spots; a tie on spots goes to the higher total of points on them), the standings with spots, time held, steals and points, the round's best trick, the most steals and the longest held, and who owns each spot.

**Every round starts with every spot open**, alone too: only that round's tricks count.

**Multiplayer:** in a session the rounds are everyone's.
- **Who runs them:** the session's host starts and stops rounds. On a dedicated server (no host player), or if the host leaves, the player with the lowest Steam ID does. Anyone else's `domination start` says who runs them; `domination start solo` still plays a round alone.
- **Joining:** everyone on this build on the same map is taken into a round the moment it starts, and anyone who arrives later joins it at once. A solo round in progress gives way to it.
- **Leaving:** `domination stop` takes a player out; they aren't pulled back in. `domination join` puts them back in.

**Teams** (`domination set teams on`): red against blue. The round's authority (see below) hands the teams out, as even as they can be, and a player who joins late goes to the smaller team; nobody is moved once placed. A spot is its owner's team's, a steal is a spot taken from the other team, and the team with the most spots wins. Teams need other players: a solo round is free-for-all.

**For a menu:**
- `domination::snapshot()` gives everything a menu shows, including who runs the session's rounds (`controls`, `controller`).

**Console:**

| Command | What it does |
|---|---|
| `domination start [seconds] [solo]` | Start a round |
| `domination join` | Back into the session's round after leaving it |
| `domination stop` | Stop the round (the starter), or leave it |
| `domination status` | What's going on |
| `domination spots` | The spots and their scores |
| `domination parks` | Every park, its map, and whether that map is installed here |
| `domination scan <x0> <z0> <x1> <z1> [step] [top] [bottom]` | Park-making: a height map of an area, written to a file |
| `domination tp <x> <y\|ground> <z> [heading]` | Park-making: a teleport facing a heading |
| `domination survey <places file> [half size] [step]` \| `stop` | Park-making: visit a list of places, a height map and four looks at each |
| `domination stats` | The rounds you have played to the end: in all, with others and won, per map and per park (saved with your profile as `ReSkate.Domination.*`; the tab shows them too) |
| `domination set duration <s>\|players everyone\|solo\|teams on\|off\|map next\|previous\|park <id>\|next\|previous` | The next round's settings (`domination set` alone shows them) |

## How a trick is read

Everything comes from skate.'s own score HUD view model, `ScoringHUDViewModel`. Its field names are in the game's type data, and `domination_model.h` describes the fields as measured in play:
- **A trick:** `ActionActive` goes on, then off.
- **Its points:** the rise in `LineScore` one tick later. They're taken unscaled and then multiplied by the new `Multiplier`.
- **Its spot:** the spot where the skater spent most of the trick, using the skater's position against each spot's footprint and height range. A trick that spends its time over no spot counts for the spot it landed on, or else the spot the skater left less than a second before it began (a flip off a ledge's end into the open).
- **Bails:** skate. scores slams (`IsSlam`). They own nothing: a slam or a wipeout scores the trick going on nothing.

## Multiplayer messages

- **Transport:** Domination's messages ride in the session's throwdown packets, which hosts and dedicated servers relay unread. They start with the tag byte `0xD0`. The session tick hands them to Domination before the throwdown relay reads its inbox (`Extension/Multiplayer/Session/session.cpp`).
- **Format:** see `domination_wire.h` (version 4). There are three kinds:
  - **round:** running, stopped (at a moment) or left (a player leaving; never taken for a stop), with the starter, the options (teams) and the time;
  - **board:** claims, each with its spot, player, score and time. A new best goes out at once as a board of one claim, and every participant sends the whole board it knows every 5 s, so a player who joins late, or missed a message, ends up with the same board. A board too long for one message goes out in turns;
  - **teams:** everyone's team.
- **Ownership:** every copy keeps every player's best claim per spot, and every claim that was ever someone's best. The owner is the higher score, then the earlier claim, then the lower Steam ID. Copies that have seen the same claims always agree, whatever order the claims came in, and replay the round the same way (time held, steals).
- **The rules in one place:** `domination_sync.h` holds every rule that makes the players' games agree, apart from the game: who runs rounds, joining, the clock, teams, the board and the results. The game gives it the session's players, the player's tricks and the messages that arrive, each with a time, and sends what it puts out.
- **The authority:** the controller while it's in the round, else the participant with the lowest Steam ID heard from in the last 25 s. It hands out the teams. Every participant says the round again every 5 s (for late players and players coming back), but everyone's clock follows only the authority's: a gap above 300 ms is halved at each of its round messages. A player who leaves says so every 2 s for 10 s. The controller (or the round's starter) stops a round for everyone, at the moment they stopped it: claims made after it, before the stop arrived, are dropped on every copy alike.
- **Two rounds started at once:** the lower round number stands, and the other round's players move to it if nothing has been claimed in theirs yet.
- **After the end:** the board goes out every 2 s for 10 s, then every 10 s for 3 minutes (or until the next round), and claims made before the end are taken whenever they come, so a trick landed on the last second, or by a player whose connection dropped across the end, counts everywhere and the results agree. A stopped round's stop goes out again with it from every player whose copy it ended, so a stop that was missed, lost, or sent once by a controller who then changed maps still reaches everyone; a stop that arrives after a copy's own end moves that end back to it.
- **Tested with many players over a bad network:** `Test/domination_net_tests.cpp` runs up to 16 simulated players through the same rules, with messages late (up to 530 ms), reordered, lost (up to 15%) and doubled, clocks off by up to 30 s and running at their own speed, players joining late and leaving, the host leaving, and two rounds started at once. Every player ends with the same round, owners, claims, teams and results, and the clocks agree during play (within 70 ms on a normal network, about 400 ms with half-second lag).
- **Checks:** a claim must be for one of the park's spots, inside the round's play time, and not more than 5 s ahead of this player's clock. A round takes claims from 64 players at most and keeps at most 1024 claims per spot. A finished round still takes late claims while its results are shown, and the results update.
- **The park:** whoever runs the rounds picks it. Another player on that map plays it with them: their game switches to that park when the round starts, whatever they had picked.
- **Mismatches:** a player on another version of Domination, or with another version of the park file, is told once per player why their rounds don't meet.
- **Load:** a full board message is at most 3.4 KB (200 claims), within the 4 KB throwdown limit, and a player sends a few messages per 5 s, well under the session's 60.
- **No other changes:** the server, the relay and the protocol version are untouched. Players on older builds drop these messages, with one warning each in their log.

**Not yet tried between two real players.** The network simulation runs up to 16 simulated players through the same rules.

## Parks

A park is a JSON file (format in `domination_model.h`) with these parts:
- **level:** the level asset it belongs to;
- **map** and **package** (optional): the map's name as players know it, and for a map mod its Thunderstore package (`Owner-Name`), so a player without the map is told what to get;
- **spots:** each one a polygon footprint, or a capsule around a rail, plus a height range and a marker position;
- **starts:** positions and headings for the skaters.

Spots may not overlap; the loader refuses parks that do. A spot can be a bowl, a ledge group, a rail, and so on.

A park file, with one spot of each shape (game coordinates, y up; `x`/`z` seen from above):

```json
{
  "schema": 1,
  "level": "levels/game/dingolevel_reskate_skate3improved/dingolevel_reskate_skate3improved",
  "park": "Red-bowl park", "map": "Skate 3 Improved", "package": "333-Skate_3_Improved",
  "spots": [
    {"id": "funbox", "name": "Funbox", "kind": "box", "shape": "polygon",
     "points": [[364.97, 389.06], [345.07, 394.39], [342.43, 384.54], [362.33, 379.21]],
     "y": [0.85, 9.35], "marker": [353.7, 6.85, 386.8]},
    {"id": "top-rail", "name": "Long rail", "kind": "rail", "shape": "capsule",
     "a": [370.5, 384.3], "b": [397.5, 379.2], "radius": 1.8,
     "y": [2.25, 9.06], "marker": [384.01, 6.56, 381.75]}
  ],
  "starts": [{"position": [343.5, 2.55, 469.2], "heading": 160}]
}
```

- **id:** stays the same across versions: multiplayer goes by it (players with other spot ids can't play together);
- **y:** the height range a skater counts as on the spot in;
- **marker:** where its marker floats;
- **heading:** degrees from +z towards +x;
- **limits:** up to 64 spots and 64 starts.

**Where parks come from:**
- **Built in** (`domination_parks.h`):
  - five on San Vansterdam, where the game puts its own skate challenges: Rolling Waves (12 spots), Bonski Park under the bridge (8), Hartcore Plaza (12), FlowTopia (9) and El-One Courts (5);
  - five on the Skate 3 map mod (Skate3Improved): the red-bowl park (20 spots: bowls, banks, ledges, rails, the funbox, four mounds and a curb), Hillside park (9: a deep bowl, a pool, the full pipe, hips, a pyramid and ledges), Industrial bowls (7), Sunken banks (6) and Loans plaza (6).
- **Files:** `<LOCALAPPDATA>\ReSkate\domination\parks\*.json`, and in every mod switched on in the launcher:
  - `Mods\<mod>\domination*.json`, beside its manifest, so a map mod ships its own parks;
  - `Mods\<mod>\domination\*.json`, so a mod can be a pack of parks and nothing else.
- **A park pack on Thunderstore** is a zip of `manifest.json`, `icon.png`, `README.md` and a `domination` folder of park files. It ships no game data, so it loads on any game build. Players install it from the launcher's MODS page like any mod, and its parks appear in the Map and Park dropdowns. Everyone in a round needs the same park file (the park's spot ids are checked; players with another version are told).
- **Priority:** a file overrides a built-in park of the same name.

**Making a park, and sharing it as a mod:** [MAKING-PARKS.md](MAKING-PARKS.md) goes through it step by step: finding the place (`domination tp`, `domination survey`), a height map of it (`domination scan`, `tools/render_scan.py`), the spots in a spec, the park file (`tools/build_park.py`), trying it in game, and packaging parks as a mod (`tools/make_pack.py`) or building them into ReSkate (`tools/make_builtin.py`). The ten built-in parks' files are in `parks/`, with the specs they were built from in `parks/specs/`.

## Tests

Configure with `-DDINGOSDK_BUILD_DOMINATION_TESTS=ON`, then run `ctest -R domination`. The tests cover:
- park files;
- reading tricks;
- claims on a spot;
- multiplayer boards in any order, and a late joiner given one whole board;
- round replays (time held, steals, the best trick) and the standings;
- teams;
- the messages;
- the built-in park;
- `domination_net` (`Test/domination_net_tests.cpp`): whole rounds between up to 16 simulated players over a bad network (see Multiplayer messages), and a dozen random rounds with random trouble: players (the host too) dropping out of the session and back, leaving and rejoining the round, cut off a while, the round stopped early. `dingosdk_domination_net_tests fuzz <n> [first seed]` runs n of those (odd seeds play a second round soon after the first) and names the seeds of any whose players disagree (11,000 pass with MSVC; the tests' own runs pass with GCC too).

Optional extras:
- `-DDINGOSDK_DOMINATION_PARK=<park.json>` also checks a real park file.

## Also in this change

- **Turned teleports:** `teleport_local_skater_heading` puts the skater at a position facing a heading. Before, every teleport faced +Z.
