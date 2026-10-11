#!/usr/bin/env bash
# Steam shared libraries for the Linux ReSkateServer.
#
# Installs next to ReSkateServer:
#   libsteam_api.so  (Steamworks SDK redistributable_bin/linux64)
#   steamclient.so, libtier0_s.so, libvstdlib_s.so (SteamCMD app 1007, SDK Redist)
# and links ~/.steam/sdk64/steamclient.so (where libsteam_api.so looks).
#
# Usage:
#   ./setup-linux-server-libs.sh [--server-dir DIR] [--sdk-url URL] [--force] [--no-steamclient]
#
# Defaults:
#   --server-dir: directory containing ReSkateServer (or current directory)
#   --sdk-url:   the pinned SDK mirror below (Valve binaries; official source:
#                partner.steamgames.com). Pinned to a commit and checked
#                against SDK_SHA256, because this library is loaded into the
#                server: a branch name is whatever that branch holds today.
set -euo pipefail

SERVER_DIR=""
# Mirror commit df2baabf574a, libsteam_api.so 385840 bytes.
SDK_COMMIT="df2baabf574a738ef1ea90a7e89339107fc0a279"
SDK_URL="https://raw.githubusercontent.com/rlabrecque/SteamworksSDK/$SDK_COMMIT/redistributable_bin/linux64/libsteam_api.so"
# Empty, or --sdk-url given, skips the hash check: pass --sdk-sha256 to keep it.
SDK_SHA256="eb2dd015b84177cf4f4326fe578aab375fd8931bbbd719c7492420d9777007fe"
FORCE=0
NO_STEAMCLIENT=0

while [ $# -gt 0 ]; do
    case "$1" in
        --server-dir) SERVER_DIR="${2:?missing DIR}"; shift 2 ;;
        --sdk-url) SDK_URL="${2:?missing URL}"; SDK_SHA256=""; shift 2 ;;
        --sdk-sha256) SDK_SHA256="${2:?missing SHA256}"; shift 2 ;;
        --force) FORCE=1; shift ;;
        --no-steamclient) NO_STEAMCLIENT=1; shift ;;
        -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
        *) if [ -z "$SERVER_DIR" ] && [ -d "$1" ]; then SERVER_DIR="$1"; shift
           else echo "Unknown argument: $1" >&2; exit 2; fi ;;
    esac
done

if [ -z "$SERVER_DIR" ]; then
    if [ -x "./ReSkateServer" ]; then SERVER_DIR="."
    else echo "error: give --server-dir DIR containing ReSkateServer" >&2; exit 2; fi
fi
SERVER_DIR="$(cd "$SERVER_DIR" && pwd)"
[ -x "$SERVER_DIR/ReSkateServer" ] || { echo "error: no ReSkateServer in $SERVER_DIR" >&2; exit 2; }

command -v curl >/dev/null || { echo "error: curl is required" >&2; exit 2; }
command -v tar >/dev/null || { echo "error: tar is required" >&2; exit 2; }

# 1. libsteam_api.so ---------------------------------------------------------
if [ -f "$SERVER_DIR/libsteam_api.so" ] && [ "$FORCE" -eq 0 ]; then
    echo "libsteam_api.so already present, skipping (use --force to replace)."
else
    echo "Downloading libsteam_api.so ..."
    tmp="$(mktemp)"
    curl -fSL --retry 3 -o "$tmp" "$SDK_URL"
    if [ "$(wc -c <"$tmp")" -lt 100000 ]; then echo "error: download looks wrong ($(wc -c <"$tmp") bytes)" >&2; rm -f "$tmp"; exit 2; fi
    if command -v nm >/dev/null && ! nm -D --defined-only "$tmp" 2>/dev/null | grep "SteamAPI_SteamGameServerNetworkingSockets_SteamAPI_v01" >/dev/null; then
        echo "error: downloaded file has no game-server sockets export" >&2; rm -f "$tmp"; exit 2
    fi
    # The size and export checks above catch a wrong file, not a swapped one.
    if [ -n "$SDK_SHA256" ]; then
        command -v sha256sum >/dev/null || { echo "error: sha256sum is required" >&2; rm -f "$tmp"; exit 2; }
        got="$(sha256sum "$tmp" | cut -d' ' -f1)"
        if [ "$got" != "$SDK_SHA256" ]; then
            echo "error: libsteam_api.so does not match the expected hash" >&2
            echo "  expected $SDK_SHA256" >&2
            echo "  got      $got" >&2
            echo "  (a newer mirror commit needs --sdk-sha256 with its hash)" >&2
            rm -f "$tmp"; exit 2
        fi
        echo "Verified libsteam_api.so against its expected hash."
    fi
    cp "$tmp" "$SERVER_DIR/libsteam_api.so"
    chmod 644 "$SERVER_DIR/libsteam_api.so"
    rm -f "$tmp"
    echo "Installed $SERVER_DIR/libsteam_api.so"
fi

# 2. steamclient.so (+ tier0/vstdlib) ------------------------------------------
if [ "$NO_STEAMCLIENT" -eq 1 ]; then
    echo "Skipping steamclient (--no-steamclient)."
elif [ -f "$SERVER_DIR/steamclient.so" ] && [ "$FORCE" -eq 0 ]; then
    echo "steamclient.so already present, skipping (use --force to replace)."
else
    work="$(mktemp -d)"
    trap 'rm -rf "$work"' EXIT
    echo "Fetching SteamCMD SDK Redist (app 1007) ..."
    curl -fSL --retry 3 -o "$work/steamcmd.tar.gz" \
        "https://steamcdn-a.akamaihd.net/client/installer/steamcmd_linux.tar.gz"
    tar xzf "$work/steamcmd.tar.gz" -C "$work"
    ( cd "$work" && ./steamcmd.sh +@sSteamCmdForcePlatformType linux +login anonymous +app_update 1007 +quit )
    if [ ! -f "$work/linux64/steamclient.so" ]; then echo "error: steamclient.so not downloaded" >&2; exit 2; fi
    cp "$work/linux64/steamclient.so" "$SERVER_DIR/"
    for extra in libtier0_s.so libvstdlib_s.so; do
        [ -f "$work/linux64/$extra" ] && cp "$work/linux64/$extra" "$SERVER_DIR/"
    done
    chmod 644 "$SERVER_DIR/"*.so
    rm -rf "$work"
    trap - EXIT
    echo "Installed steamclient.so (+ tier0/vstdlib if present)."
fi

# 3. sdk64 symlink (where libsteam_api.so looks) --------------------------------
if [ -f "$SERVER_DIR/steamclient.so" ]; then
    mkdir -p "$HOME/.steam/sdk64"
    ln -sf "$SERVER_DIR/steamclient.so" "$HOME/.steam/sdk64/steamclient.so"
    echo "Linked ~/.steam/sdk64/steamclient.so"
fi

# 4. Verify ---------------------------------------------------------------------
echo "---"
ls -lh "$SERVER_DIR/ReSkateServer" "$SERVER_DIR/"*.so
if command -v nm >/dev/null; then
    nm -D --defined-only "$SERVER_DIR/libsteam_api.so" 2>/dev/null \
        | grep -o "SteamGameServerNetworkingSockets_SteamAPI_v[0-9]*" | sort -u | sed 's/^/sockets: /'
fi
echo "Done. Next: edit $SERVER_DIR/ReSkateServer.json (name, admins), then run $SERVER_DIR/ReSkateServer"
