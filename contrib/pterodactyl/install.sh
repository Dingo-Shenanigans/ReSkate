#!/bin/bash
# ReSkate dedicated server (Linux) for Pterodactyl: the tarball the release's launcher.json
# names ("server_linux"), checked against its SHA-256 and unpacked into the server's folder.
# ReSkateServer.json, Mods and the logs are not in the tarball, so a reinstall keeps them.
set -euo pipefail
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl jq tar

repo="https://github.com/Dingo-Shenanigans/ReSkate"
if [ -z "${RELEASE:-}" ] || [ "${RELEASE}" = "latest" ]; then
    base="${repo}/releases/latest/download"
else
    base="${repo}/releases/download/${RELEASE}"
fi
server="${SERVER_DIR:-/mnt/server}"
mkdir -p "${server}"
work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

echo "Reading ${base}/launcher.json"
curl -fsSL --retry 3 -o "${work}/launcher.json" "${base}/launcher.json"
url="$(jq -r '.server_linux.url // empty' "${work}/launcher.json")"
sha="$(jq -r '.server_linux.sha256 // empty' "${work}/launcher.json")"
version="$(jq -r '.server_linux.version // empty' "${work}/launcher.json")"
if [ -z "${url}" ] || [ -z "${sha}" ]; then
    echo "This release has no Linux server (no \"server_linux\" in launcher.json). Use 1.1.4 or newer."
    exit 1
fi
case "${url}" in
    asset:*) url="${base}/${url#asset:}" ;;
    https://*) ;;
    *) echo "launcher.json names no usable download: ${url}"; exit 1 ;;
esac

echo "Downloading the ReSkate Linux server ${version}"
curl -fSL --retry 3 -o "${work}/server.tar.gz" "${url}"
echo "${sha}  ${work}/server.tar.gz" | sha256sum -c -
tar -xzf "${work}/server.tar.gz" -C "${server}" --strip-components=1
chmod +x "${server}/ReSkateServer"

# Steam looks for steamclient.so in ~/.steam/sdk64 too (HOME is the server's folder in the
# container); a link keeps pointing at the copy the self-update replaces.
mkdir -p "${server}/.steam/sdk64"
ln -sf ../../steamclient.so "${server}/.steam/sdk64/steamclient.so"

# First install: the panel's ports. The server adds every other setting with its default on
# its first start; the panel writes the ports (and steam_token) again before each start.
if [ ! -f "${server}/ReSkateServer.json" ]; then
    printf '{\n  "port": %s,\n  "query_port": %s\n}\n' "${SERVER_PORT:-27015}" "${QUERY_PORT:-27016}" > "${server}/ReSkateServer.json"
fi
echo "Installed the ReSkate Linux server ${version}."
