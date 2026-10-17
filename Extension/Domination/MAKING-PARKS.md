# Making Domination parks, and packaging them as mods

A Domination park is one JSON file: the level it belongs to, its spots (footprint, height range, marker) and its starts. Making one needs the game and a few Python scripts. Sharing one needs no pull request: parks travel as mods.

The ten parks ReSkate ships were made this way. Their files are in `parks/`, the specs they were built from in `parks/specs/`, and the red-bowl park's maker in `tools/make_park.py`.

## What you need

- ReSkate with Domination, and the map the park is for.
- Python 3 with numpy, matplotlib and Pillow (`pip install numpy matplotlib pillow`; `find_features.py` also wants scipy).
- The console (`~`). Every command below is typed there.

## 1. Find the place

- `trainer where` prints where the skater stands (x, y, z; y is up).
- `domination tp <x> <y|ground> <z> [heading]` puts the skater somewhere, facing a heading in degrees from +z towards +x. `ground` stands them on the highest ground below 200 m, but only where the map has loaded collision (near the skater).
- `domination survey <places file> [half size] [step]` visits a list of places (one `x y z` per line) unattended. At each one it waits for the area to load, writes a height map of the square around it, and looks north, east, south and west for 1.5 s each. Take pictures of the game window alongside to compare candidates. `domination survey stop` ends it.

## 2. Measure it: a height map

Stand in the middle of the park first (`domination tp <x> <y> <z>`): the game loads collision only around the skater, so a scan of somewhere far away finds nothing, and `domination status` then reports every point missed. Give it a few seconds to load, then:

```
domination scan <x0> <z0> <x1> <z1> 0.25
```

It casts one ground ray per point of a 0.25 m grid, for 6 ms of each frame, and writes `scan-<time>.bin` to `%LOCALAPPDATA%\ReSkate\domination\`. `domination status` shows how far it got. After the step come the highest and lowest heights to look between, 60 and -40 by default. Set them round the ground you stand on (`trainer where` gives its height): San Vansterdam's parks are near 195 m up, so `domination scan ... 0.25 260 150` there. For a park under a roof or a bridge, set the top below it.

Render it:

```
python -I tools/render_scan.py scan-<time>.bin mypark
```

This writes `mypark-height.png` (heights over hillshade, a 10 m grid, game x to the right, z down), `mypark-slope.png` (flat dark, walls bright) and `mypark.npz`, the height map the next steps read. `tools/find_features.py mypark.npz mypark --outline x z x z ...` lists the raised features (ledges, boxes, banks, rails) and the dips (bowls, pools) inside an outline, as a starting point.

## 3. Draw the spots: a spec

From the pictures, write a spec: each spot's footprint seen from above, and the starts. The format is at the top of `tools/build_park.py`, and `parks/specs/` has nine real ones.

```json
{"id": "mypark", "level": "levels/game/bam_levelroot/bam_levelroot", "park": "My Park", "map": "San Vansterdam",
 "view": [-315, 1400, -138, 1615],
 "spots": [
   {"id": "big-bowl", "name": "Big bowl", "kind": "bowl", "points": [[-200, 1551], [-160, 1553], [-151, 1576], [-170, 1594], [-196, 1594]]},
   {"id": "long-ledge", "name": "Long ledge", "kind": "ledge", "a": [-265.5, 1513.5], "b": [-253, 1527.5], "r": 2.2}
 ],
 "starts": [[-292, 1440, 42], [-305, 1480, 68]]}
```

- **A spot** is a polygon (`points`, x and z) or a capsule round a line (`a`, `b`, radius `r`): rails and ledges.
- **A spot is something to own:** a bowl, a ledge group, a rail, a bank, a box. Spots may not overlap.
- **Ids** never change once a park is out: players with other spot ids can't play together.
- **Starts** are `[x, z, heading]`, outside the spots, facing into the park. One per player is best (up to 64).
- **For a map mod:** add `"package": "Owner-Name"` (its Thunderstore name), so a player without the map is told what to get.

## 4. Build the park file

```
python -I tools/build_park.py mypark.spec.json mypark.npz mypark.json mypark.png
```

It measures each spot from the height map: the height range runs from its floor - 1 m to its top + 5 m, and the marker floats 2.5 m over its top. It checks that no two spots overlap and no start stands on one, and draws the park over the height map in `mypark.png`. For a park under a bridge, add `"ceiling": <height>` to the spec.

## 5. Try it in game

1. Copy `mypark.json` to `%LOCALAPPDATA%\ReSkate\domination\parks\`.
2. Load the map. `domination parks` lists it; `domination set park mypark` picks it.
3. Stand on each spot: `domination status` names the spot you are on. The outlines on the ground show its edge.
4. Play a round alone: `domination start solo 60`.

A park file whose id is a built-in park's replaces that park, so this is also how a built-in park is changed and checked.

## 6. Share it

### As a park pack (a mod)

```
python -I tools/make_pack.py out My_Domination_Parks 1.0.0 "What's in it, at most 250 characters." mypark.json [more.json ...]
```

This writes `out/My_Domination_Parks-1.0.0.zip`:

| File | What |
|---|---|
| `manifest.json` | Thunderstore's manifest: name, version, description, no dependencies |
| `README.md` | the parks, the maps they need, how to play them |
| `icon.png` | 256 x 256, the parks' spots seen from above |
| `domination/<id>.json` | the park files |

- **Upload it** to Thunderstore's ReSkate community. Players install it from the launcher's MODS page.
- **Or unzip it** into `Mods\My_Domination_Parks\` by hand.
- **It carries no game data,** so it loads on any game build. Domination reads parks only from mods switched on in the launcher.
- **Everyone in a round needs the same version.** Players with another version of a park are told so.
- **A new version keeps the ids.** Raise the version number with every change.

The ten built-in parks are also packed this way, as `Domination_Parks_San_Vansterdam` and `Domination_Parks_Skate_3_Improved`:

```
python -I tools/make_pack.py out Domination_Parks_San_Vansterdam 1.0.0 "..." parks/sanvan-*.json
python -I tools/make_pack.py out Domination_Parks_Skate_3_Improved 1.0.0 "..." parks/skate3improved-redbowl.json parks/s3i-*.json
```

A pack's park overrides the built-in park of the same id, so these packs can update the shipped parks between ReSkate releases.

### Inside a map mod

A map's author can ship its parks with the map: put `domination-<id>.json` files beside the mod's `manifest.json`. They load whenever the map mod is switched on.

### Built into ReSkate (a pull request)

1. Add the park file to `parks/`, and its spec to `parks/specs/`.
2. Regenerate the built-in parks, every file in the order they appear in the menu:
   ```
   python -I tools/make_builtin.py domination_parks.h parks/sanvan-seaside.json parks/sanvan-bridge.json parks/sanvan-mcorp.json parks/sanvan-flowtopia.json parks/sanvan-elone.json parks/skate3improved-redbowl.json parks/s3i-hillpark.json parks/s3i-industrial.json parks/s3i-banks.json parks/s3i-loans.json parks/<new>.json
   ```
3. `ctest -R domination` parses every built-in park (the `domination` test fails on a bad one).
