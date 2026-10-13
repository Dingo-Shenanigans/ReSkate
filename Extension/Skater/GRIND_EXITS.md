# Grind exit triggers

Pick the side you pop out of a grind to with **LT / RT**, independently of the trick and of the
body's rotation. In skate.'s own controls both come from the left stick at the same moment, so a
frontside 180 always pops out toward one side and a backside 180 toward the other. With this on, any
trick and any rotation can go out either side: a frontside 180 out to your toe side, a backside 180
out to your heel side, a kickflip out to whichever side you pick.

## Using it

Off until you switch it on: skate.'s menu, **Mod Options > Player > Grind exit triggers** (or the
ReSkate menu's skater page, or the console (`~`) with `grindexit exits on|off`). The choices are kept.

- **While grinding, press LT or RT** (or up to 1 s before the grind connects,
  for example as you ollie up to the rail), then do any trick
  out of the grind with the right stick as usual. Keep the trigger held until you land, then let go.
- **Input Mode** (`grindexit sides toeheel|travel`):
  - **Toe / Heel**: LT pops out to your toe side, RT to your heel side, riding normal, fakie or
    switch. Needs **your stance** (`grindexit stance goofy|regular`). On slides (boardslide,
    lipslide), where toes and heels point along the rail, LT / RT are left / right of travel.
  - **Left / Right**: LT pops out to the left of your direction of travel, RT to the right.
- **No trigger, or both**: the game's own exit.
- **Grabs**: a trigger used to pick the side is hidden from the game until you let go of it, so
  holding it through the pop does not grab. Let go and press it again in the air to grab
  (`grindexit guard on|off`).
- **Spins**: push the left stick for your spin as you always do. While the trigger is held in the
  grind, the stick's left / right reaches the game at the pop instead of before it (it cannot
  lean-balance the grind meanwhile), so the game's own lean exit cannot drag the board toward the
  spin and into the rail (`grindexit leanguard on|off`).
- **Pop-out strength** (`grindexit strength 10-100`, kept): how far to the side a trigger exit
  pops, as a share of the full exit (100%, the default, is the game's own sideways speed). Lower it
  for rails next to a wall. Very low values leave little clearance on exits over the rail.
- **Multiplayer**: works in any session or server, whatever the host's settings, throwdowns
  included. It adds no speed or height (the sideways speed is the game's own); it only changes which
  input picks the exit side. Server checks do not see it, since it changes no game data.

What it does not do: it does not make an exit easier than the game's physics allow. Popping out of a
smith or feeble over the rail, away from the side the board hangs on, still needs a good pop, as it
would on a real rail.

## How it works

At the pop the game calls its jump-trajectory setter with the launch velocity; the trainer already
hooks that call (`Extension/Trainer/trainer_jump.cpp`). For a pop out of a grind (its call site,
measured: `Engine/Game/Build/20260929/grind_exit.h`) only the sideways part of that velocity is
replaced, across the rail: the game's own sideways speed when it had one, mirrored to the chosen
side, else the game's usual 1.8 m/s. The speed along the rail, the pop's height, the trick, the flip
and the rotation are the game's.

| Part | Files |
|---|---|
| The switch, triggers, lean guard, toe / heel and the launch change | `grind_exit_control.*` |
| Console verbs | `grind_exit_commands.cpp` |
| Each physics step of the local skater, from No Bail's skeleton response hook | `no_bail.h` (`add_no_bail_step_listener`) |
| The launch hook | `Extension/Trainer/trainer_jump.*` (`set_trajectory_modifier`) |
| The game's view of the pad (triggers, left stick) | `Extension/UI/Overlay/input_capture.cpp` (`DingoSDKOverlaySetGamepadFilter`) |
| Measured addresses and behaviour | `Engine/Game/Build/20260929/grind_exit.h` |
| The menus | `skate_menu_skater.cpp`, `hub_page.cpp`, `native_tools_view.h` |

**Toe / heel.** The game keeps which foot leads in a flag (context + 0x13c4, bit 5), found by
recording the skater's state while riding regular and switch: it changed exactly at each change of
stance and never during grinds, pops or 180s. With the player's stance it gives the lead foot; the
toes face the skater root's right axis when the right foot leads (measured), the other way when the
left foot does. It was measured with a goofy skater, for whom "switch" and "left foot leads" are the
same thing; a regular skater's recording would confirm which of the two the flag means.

**How it was found.** Everything above was measured with a logging-only recorder (every physics step
around grinds, the pad and the launch calls, to CSV, plus snapshots of the skater's state memory). It
is not part of this change; it can be offered separately for re-measuring after a game update.

With the log level set to debug (`loglevel debug`), every pop out of a grind is written to
`ReSkate.log` with what was decided and why.

## Measured

Tested on build 25414733 with a goofy skater over four sessions: 48 pops out of grinds with the game's
own controls to learn its exits, then 144 pops whose side the triggers set. Forward, fakie and switch
50-50s, ollies, kick- and heelflips, shove-its and frontside / backside 180s went out to the chosen
side with the trick and the rotation unchanged. Frontside 180 out to the toe side, which the game's
controls cannot make, clipped the rail until the lean guard was added; with it, all 4 tries landed.
Heel-side exits from a fakie smith (over the rail) got out about 6 times in 16.
