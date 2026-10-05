# ReSkate dedicated server on Linux

Native x86_64 build. Same lobby protocol as `ReSkateServer.exe`; needs no game install.
Self-update is disabled on Linux (V1) — update by replacing the binary.

## Requirements

- 64-bit Linux (tested: CachyOS/Arch, Ubuntu 22.04+ should work).
- `cmake >= 3.24`, `g++ >= 12` (C++20), `libssl-dev` (OpenSSL), `libcurl` headers optional (not needed for V1).
- `curl` on the machine that runs the server: it is how the server reads the ReSkate team's
  global ban list (`api.reskate.dev`). Without it the server says so in its log and enforces
  only its own bans. Not needed with `"global_bans": false`.
- Steam shared libs beside the binary: `libsteam_api.so` + `steamclient.so`
  (Valve proprietary, not in git — but bundled in the CI/release assets,
  like the Windows zip bundles its DLLs; otherwise fetch them, see below).

## Where the Steam `.so` files come from

Same split as Windows (`steam_api64.dll` + `steamclient64.dll`/`tier0_s64.dll`/`vstdlib_s64.dll`):
the release ZIP is expected to ship them; this repo never vendors Valve binaries
(see `External/README.md`, `External/manifest.json`).

- `libsteam_api.so`: from the Steamworks SDK `redistributable_bin/linux64/`
  (official download via `partner.steamgames.com`; any recent 1.x works — the server
  tries `SteamGameServerNetworkingSockets_SteamAPI_v013` then `v012`, and uses
  stable flat exports `SteamGameServer015`/`SteamNetworkingUtils004`).
  It is generic; the app is selected at runtime via `SteamAppId=3354750`.
- `steamclient.so` (+ `tier0`/`vstdlib` if present): **not** in the SDK.
  Get it from SteamCMD or a Steam client install, e.g.:
  ```sh
  steamcmd +@sSteamCmdForcePlatformType linux +login anonymous +app_update 1007 +quit
  # then copy steamapps/common/Steamworks\ SDK\ Redist/linux64/steamclient.so
  # next to ReSkateServer (same folder as libsteam_api.so)
  ```
  or copy/symlink an existing client copy (`~/.steam/steam/linux64/steamclient.so`,
  Valve also documents `~/.steam/sdk64/steamclient.so` — the server looks there,
  so `ln -s <folder>/steamclient.so ~/.steam/sdk64/steamclient.so` works).
  The server `dlopen`s `libsteam_api.so`, which in turn loads `steamclient.so`; either adjacent or
  `LD_LIBRARY_PATH` works.

Arch/CachyOS:

```sh
sudo pacman -S base-devel cmake openssl
```

Ubuntu/Debian:

```sh
sudo apt install build-essential cmake libssl-dev
```

## Build

```sh
cmake --preset linux-x64
cmake --build --preset linux-release -j$(nproc)
./build/linux/ReSkateServer
```

Binary: `build/linux/ReSkateServer` (ELF, `OUTPUT_NAME ReSkateServer`).

Portable tests:

```sh
cmake --preset linux-x64 -DDINGOSDK_BUILD_MULTIPLAYER_TESTS=ON
cmake --build --preset linux-release -j$(nproc)
ctest --test-dir build/linux --output-on-failure
```

Expected: 9 passed (`multiplayer_parties`, `server_activity`, `server_speed_check`,
`server_config`, `server_global_bans`, `word_filter`, `multiplayer_bandwidth`, `multiplayer_lanes`,
`multiplayer_prediction`). Windows-only tests (voice, lobbies, UI) stay `WIN32`-gated.

## Run

1. Put `ReSkateServer` in its own folder, then fetch the Steam libs
   (shipped as `setup-linux-server-libs.sh` beside the binary):
   ```sh
   ./setup-linux-server-libs.sh    # uses the folder it sits in; or --server-dir DIR
   ```
   Or do it by hand, see below.
2. First run writes `ReSkateServer.json`; edit `name` + `admins`, restart.
3. Optional: `world-layers.json` for time-of-day / world layers.
   Normal server runs only *read* this file (`world_layer_scan::read`, JSON only)
   and work on Linux — verified. Get it by either:
   - Windows export once: `ReSkateServer.exe --export-world-layers "<Skate folder>" world-layers.json`,
     then copy to the Linux folder; or
   - copy from a Windows player's cache `%LOCALAPPDATA%\ReSkate\cache\<build>\world-layers.json`
     (same game build; the server tells admins this path in `layer-sync` errors).
   Without it every player keeps their own layers; with it + `world_layer_sync` the server forces them.
4. No ports need opening (Steam relay). Optional UDP `27015-27016` (`port`,
   `query_port`) for browser ping + faster joins.

Without `libsteam_api.so` the server exits 1 with:
`Cannot load .../libsteam_api.so. Put libsteam_api.so (Steamworks SDK) next to the server.`

Full command list: `Server/README.txt` (same on Linux; binary name differs).

## systemd

See `contrib/reskate-server.service`. Install:

```sh
sudo cp contrib/reskate-server.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now reskate-server
```

The server writes its own `ReSkateServer.log` next to the binary; the console
output goes to the journal (`journalctl -u reskate-server -f`).

## Notes / limits (V1)

- `auto_update` / `update` command: `updates_enabled()==false` on Linux.
  `check_for_update` reports “self-update is not supported on Linux”.
- `--export-world-layers` on Linux: the reader works, the scanner needs the
  Windows game (`Data/layout.toc` + CAS) and `oo2core_9_win64.dll` (Oodle).
  Oodle blocks throw `Oodle CAS data is only supported on Windows` (caught,
  exit 1, no crash). Export on Windows, copy the JSON over — no game needed
  on the server itself.
- `content_cache` dir: `%LOCALAPPDATA%` on Windows, `$XDG_CACHE_HOME`/`~/.cache` on Linux.
- Crypto interop verified: OpenSSL `PKCS5_PBKDF2_HMAC(SHA256, 100k)` +
  `HMAC-SHA256` matches Windows `BCrypt` (checked against Python `hashlib`).
