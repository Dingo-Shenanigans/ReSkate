# Map teleports

Zones a custom map ships that send the skater somewhere else when ridden into: a pipe that
comes out on a roof, a door into the next area, a loop of tubes. The skater comes out facing
the zone's heading and keeps its speed, the way Tony Hawk's Project 8's teleports work.
Built into ReSkate; the zones come from the map mod.

Code: [map_teleports.cpp](../Extension/Travel/map_teleports.cpp) (client thread: reads the
file, watches the skater, asks for the teleport), [map_teleports_model.h](../Extension/Travel/map_teleports_model.h)
(the rules on their own, with [tests](../Extension/Travel/Test/map_teleports_tests.cpp)), and the
carried speed in [client_noclip.cpp](../Extension/Skater/client_noclip.cpp) (physics thread).

## What the player gets

- Riding through a zone teleports the skater at once, through the game's own teleport (the one
  console `tp` uses), upright at the zone's spot.
- The skater's line and facing turn together onto the spot's heading: it leaves along that
  heading at the speed it came in with, still fakie if it rode in fakie. Up and down speed stay
  the game's own. A zone can keep the skater's own heading instead (`"orient": false`) or drop
  the speed (`"keep_speed": false`).
- A skater on the board comes out on the board; a skater on foot comes out on foot, facing the
  spot, with no speed.
- After a teleport nothing fires for 0.75 s, and the zone the skater came out in waits until
  the skater has left it, so two zones that send to each other cannot bounce.
- Players on a version of ReSkate without this, and maps without the file, are unchanged.

## For map makers: `teleports.json`

Put a `teleports.json` in your mod folder (beside `manifest.json`). World axes, metres, y up,
as `trainer.json` and the trainer's `where` give them. Headings are degrees about up: 0 faces
+z, 90 faces +x.

```json
{
  "schema": 1,
  "zones": [
    {
      "name": "Factory pipe",
      "center": [279.42, 18.05, -54.09],
      "size": [2.5, 3.6, 0.0],
      "yaw": 0,
      "target": [287.88, -1.12, -52.58],
      "heading": 90,
      "keep_speed": true,
      "orient": true
    }
  ]
}
```

- `center`, `size`: the box, its full width (x), height (y) and depth (z) before it is turned.
  A size of 0 is a flat panel; it is given a few centimetres. The skater's line between two
  frames is tested against the box, so a thin panel is not missed at speed.
- `yaw`: the box turned about up, in degrees.
- `target`, `heading`: where the skater comes out and the way it faces.
- `"level"` (a level asset) is optional: without it the file stands for every level the mod's
  `reskate-levels.json` adds. Up to 256 zones; a zone with a missing or bad value is left out
  and the log says how many.
- A zone in a doorway with a solid door behind it should reach about a metre out in front of the
  door. Otherwise the door stops the skater before the zone fires, and there is no speed left to
  carry.
- Nothing goes in `manifest.json`. Ship the file in the same folder as `layout.toc` and
  `reskate-levels.json`, and bump the package version so players get it.

## Multiplayer

Teleports are local: each player's own game sends them through the zone, and the others see
them move, as with console `tp`. A map's zones work whatever the host's noclip and teleport
setting is, since they are part of the map.

## Not done yet

- A grind is ended by the teleport (Project 8 does the same). Coming out onto a rail and
  carrying on grinding is not tried yet.
- No message or blur on screen; the log has a line for each teleport.
