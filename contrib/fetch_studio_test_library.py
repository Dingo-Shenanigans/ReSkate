"""Fetch the pinned private library using GITHUB_TOKEN; never embed it in a build."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import urllib.request


class AssetRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, file, code, message, headers, url):
        redirected = super().redirect_request(request, file, code, message, headers, url)
        # Signed asset redirects authenticate themselves. Do not forward GitHub credentials.
        if redirected is not None:
            redirected.remove_header('Authorization')
        return redirected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, default=Path('config/studio-test-library.json'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    config = json.loads(args.config.read_text())
    if args.output.exists():
        with args.output.open('rb') as stream:
            if hashlib.file_digest(stream, 'sha256').hexdigest() == config['sha256']:
                print('Pinned native library already verified'); return
        raise SystemExit('Existing library differs; use a new output path')
    token = os.environ.get('GITHUB_TOKEN')
    if not token: raise SystemExit('GITHUB_TOKEN with contents:read is required for the private library')
    # The repository comes from the checked-in build configuration, never a command URL.
    parts = config['repository'].split('/')
    if len(parts) != 2 or any(not p or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-' for c in p) for p in parts):
        raise SystemExit('Invalid repository in library configuration')
    url = f"https://api.github.com/repos/{config['repository']}/releases/assets/{int(config['asset_id'])}"
    request = urllib.request.Request(url, headers={'Authorization': 'Bearer ' + token,
        'Accept': 'application/octet-stream', 'User-Agent': '22sh-studio-tester-build', 'X-GitHub-Api-Version': '2022-11-28'})
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(args.output.suffix + '.part')
    digest = hashlib.sha256(); size = 0
    try:
        with urllib.request.build_opener(AssetRedirect()).open(request, timeout=120) as response, temporary.open('xb') as output:
            while data := response.read(4 * 1024 * 1024):
                size += len(data)
                if size > config['bytes']: raise ValueError('Library download exceeds pinned size')
                digest.update(data); output.write(data)
        if size != config['bytes'] or digest.hexdigest() != config['sha256']:
            raise ValueError('Native library does not match its pinned size/SHA256')
        temporary.rename(args.output)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise
    print(f"Verified native library: {config['previews']} previews, {size} bytes")


if __name__ == '__main__': main()
