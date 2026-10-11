# Pterodactyl

`egg-reskate-server.json` runs the Linux dedicated server on [Pterodactyl](https://pterodactyl.io)
(also works on Pelican). It installs the server from the ReSkate GitHub release and the server then
updates itself, as described in `Server/README-linux.md`.

## Import

1. Admin area → **Nests** → **Import Egg**, choose `egg-reskate-server.json` and a nest.
2. **Servers** → **Create New** with the **ReSkate Server** egg:
   - the **primary allocation** is the game port (`port`),
   - add a **second allocation** and put its port into **Query Port** (`query_port`). It must
     differ from the game port.
3. 256 MB of memory is plenty for a typical server; give it about 300 MB of disk (the Steam
   libraries are in the tarball).

## What the egg does

- **Install** (`install.sh`, also inlined in the egg): reads the release's `launcher.json`,
  downloads the `server_linux` tarball, checks its SHA-256 and unpacks it into the server's
  folder. `ReSkateServer.json`, `Mods` and the logs are not in the tarball, so **Reinstall** keeps
  them. On the first install it writes the panel's ports into `ReSkateServer.json`; the server adds
  every other setting with its default on its first start.
- **Start**: `./ReSkateServer`. The panel sees the server as running at `is up on`; **Stop** sends
  `quit`, so it signs out of Steam cleanly. The console takes every server command.
- **Before each start** the panel writes `port`, `query_port` and `steam_token` into
  `ReSkateServer.json`, so set those in the panel, not in the file.
- **Updates**: the server installs new releases itself once nobody is on (`auto_update`), using the
  image's `curl` and `tar`, and carries on in the same process. Reinstalling also updates it.

## Variables

| Variable | Env | Default | |
|---|---|---|---|
| Query Port | `QUERY_PORT` | `27016` | Second UDP allocation, Steam server queries |
| Steam Token | `STEAM_TOKEN` | empty | Game server login token (App ID 3354750, one per server); see `steam_token` in `Server/README.txt` |
| Release | `RELEASE` | `latest` | `latest`, or a tag such as `v1.1.4` (1.1.4 or newer: older releases have no Linux server) |

## Image

`ghcr.io/parkervcp/yolks:debian` (Debian 13). The server needs glibc 2.38 or newer and OpenSSL 3
(`libcrypto.so.3`); it uses `curl` for the global ban list and its updates, and `tar` to unpack them.
