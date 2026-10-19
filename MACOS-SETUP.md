# ReSkate on macOS (Steam for Mac + NotProton / CrossOver)

ReSkate has no macOS build. It runs through a Wine-based Steam compatibility tool on top of the
Steam for Mac client, set up much like Linux (see the README). Found and first tested by codejunky
on an M1 Pro with macOS 26 and NotProton on CrossOver 26.3: the launcher showed *Welcome, <your
name>* and the game reached the Hub.

## Setup

1. Install skate. with Steam for Mac.
2. Install the compatibility tool, then in Steam → skate. → Properties → Compatibility, force it.
3. Put `ReSkateLauncher.exe` and `ReSkate.dll` in a new, empty folder of their own, for example
   `~/Games/ReSkate` (not Steam's skate. folder: the launcher refuses to run from there).
4. In Steam → skate. → Properties → Launch Options, enter this with your own folder's path:

   ```
   /bin/bash -c 'export ROSETTA_ADVERTISE_AVX=1; exec "${@/*EAAntiCheat.GameServiceLauncher.exe//Users/you/Games/ReSkate/ReSkateLauncher.exe}"' -- %command%
   ```

5. Press **Play** on skate. in your Steam library. Start it from Steam rather than running the
   launcher yourself: the launcher finds your Steam account through what Steam gives the
   compatibility tool. The first time, it installs its own copy of the game into that folder.

### Why this launch option

- **`/bin/bash`, not `bash`.** Steam for Mac runs a launch option directly, without a shell, so
  `bash` alone fails to start (*Failed to spawn process*, OS error 260); the full path works.
- **`ROSETTA_ADVERTISE_AVX=1`.** Without it the game stops at a *Below Min Spec* dialog: it checks
  that the CPU reports F16C and OSXSAVE, which Rosetta 2 runs but does not report unless told to.
  It has to be set before Wine starts, so it goes in the launch option.
- **The `${@/…/…}` swap** is the Linux one: Steam would start EA's anti-cheat launcher, which does
  not run under Wine, and this starts ReSkate's launcher in its place.

The setup above, with the launcher in a folder of its own, has not been tested on a Mac yet (it was
first tested with the launcher beside `Skate.exe`, the setup ReSkate used then).

## If the launcher says Steam is not running

The launcher reads the signed-in account from Steam's `config/loginusers.vdf`. Under a Wine-based
tool on macOS, Steam points the tool inside its app bundle rather than at that folder, and the tool
keeps no record of the running Steam client where Windows has one; the launcher looks up from the
bundle for `config/loginusers.vdf` and, started by Steam this way, takes the account it marks as
most recent. If it still says Steam is not running, check that skate. was started from the Steam
library with the launch option above, and that Steam is signed in.
