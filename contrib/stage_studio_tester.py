"""Stage a complete tester build from compiled binaries and a pinned native library."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import stat
import subprocess
import zipfile

from package_release_zip import check_name


def sha256(path):
    with path.open('rb') as stream: return hashlib.file_digest(stream, 'sha256').hexdigest()


def stage(repo, build, library, output, config, commit, run_url=''):
    repo, build, library, output = (Path(p).resolve() for p in (repo, build, library, output))
    if output.exists(): raise FileExistsError('Use a new staging directory')
    if sha256(library) != config['sha256'] or library.stat().st_size != config['bytes']:
        raise ValueError('Native library SHA256/size mismatch')
    cache = (build / 'CMakeCache.txt').read_text(encoding='utf-8-sig')
    if 'DINGOSDK_LAUNCHER_AUTO_UPDATE:BOOL=OFF' not in cache:
        raise ValueError('Tester launcher must be configured with DINGOSDK_LAUNCHER_AUTO_UPDATE=OFF')
    binaries = {name: build / 'Release' / name for name in ('ReSkate.dll', 'ReSkateLauncher.exe')}
    for path in binaries.values():
        with path.open('rb') as stream:
            if stream.read(2) != b'MZ': raise ValueError('Missing Windows binary: ' + path.name)
    with zipfile.ZipFile(library) as archive:
        names = set()
        for entry in archive.infolist():
            check_name(entry.orig_filename)
            name = entry.orig_filename
            if entry.is_dir() or name.casefold() in names or stat.S_ISLNK(entry.external_attr >> 16):
                raise ValueError('Duplicate, directory or symlink library entry: ' + name)
            names.add(name.casefold())
            if name not in ('manifest.json', 'READ-ME-FIRST.txt') and not name.startswith('ReSkate.Tricks/library/'):
                raise ValueError('Library archive contains files outside the native library')
        catalog = json.loads(archive.read('ReSkate.Tricks/library/catalog.json'))
        index = json.loads(archive.read('ReSkate.Tricks/library/native-catalog.json'))
        manifest = json.loads(archive.read('manifest.json'))
        if len(catalog) != config['previews'] or len(index) != config['inventory']:
            raise ValueError('Library inventory does not match pinned build configuration')
        if any(entry.get('position_codec') != 'native-piec-v1-extent' for entry in catalog):
            raise ValueError('Library contains outdated native position decoding')
        listed = {entry['path']: entry for entry in manifest['files']}
        payload_names = {name for name in archive.namelist() if name.startswith('ReSkate.Tricks/library/')}
        if set(listed) != payload_names or len(listed) != len(manifest['files']):
            raise ValueError('Library manifest is incomplete or duplicated')
        for entry in catalog:
            name = entry['file']
            if Path(name).name != name or not name.endswith('.clip') or '..' in name:
                raise ValueError('Invalid native clip filename')
            if 'ReSkate.Tricks/library/' + name not in listed: raise ValueError('Catalog references a missing clip')
        output.mkdir(parents=True)
        for i, name in enumerate(sorted(payload_names), 1):
            target = output / name
            if not target.resolve().is_relative_to(output): raise ValueError('Unsafe extraction path')
            target.parent.mkdir(parents=True, exist_ok=True)
            data = archive.read(name)
            expected = listed[name]
            if len(data) != expected['size'] or hashlib.sha256(data).hexdigest() != expected['sha256']:
                raise ValueError('Native payload failed its manifest: ' + name)
            target.write_bytes(data)
            if i % 5000 == 0: print(f'Extracted and verified {i}/{len(payload_names)} native files', flush=True)
        (output / 'native-library-manifest.json').write_text(json.dumps(manifest), encoding='utf-8')
    for name, path in binaries.items(): shutil.copy2(path, output / name)
    for path in (repo / 'External').rglob('*'):
        if path.is_file() and any(word in path.name.upper() for word in ('LICENSE', 'COPYING', 'OFL', 'NOTICE')):
            target = output / 'licenses' / path.relative_to(repo / 'External')
            target.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(path, target)
    for name in ('LICENSE', 'LICENSE.txt', 'LICENSE.md'):
        if (repo / name).is_file(): shutil.copy2(repo / name, output / name)
    info = dict(Version='studio-tester-candidate', SourceCommit=commit, BuildRun=run_url,
        SupportedSteamBuild=config['steam_build'], RuntimeSHA256=sha256(output / 'ReSkate.dll'),
        LauncherSHA256=sha256(output / 'ReSkateLauncher.exe'), LibrarySHA256=config['sha256'],
        NativePreviews=config['previews'], NativeInventory=config['inventory'], NativeUnsupported=config['unsupported'],
        BinaryAutoUpdate=False, IsolatedUserData='UserData via START-STUDIO.cmd',
        VisualValidation='Pending tester checklist; automated checks do not run the game')
    (output / 'build-info.json').write_text(json.dumps(info, indent=2), encoding='utf-8')
    (output / 'START-STUDIO.cmd').write_bytes(b'@echo off\r\nsetlocal\r\ncd /d "%~dp0"\r\nset "LOCALAPPDATA=%~dp0UserData"\r\nif not exist "%LOCALAPPDATA%" mkdir "%LOCALAPPDATA%"\r\nif not exist "%LOCALAPPDATA%" exit /b 1\r\nReSkateLauncher.exe --no-update\r\n')
    (output / 'READ-ME-FIRST.txt').write_text(f'''ReSkate Animator Studio - tester candidate
Commit: {commit}
CI run: {run_url or 'Local build'}
Supported Steam game build: {config['steam_build']}

INSTALL
Use a separate ReSkate test folder. This ZIP contains the launcher, runtime and
all {config['previews']} decoded native previews/tree data; it does not contain the game.
You need your own skate. Steam copy. The launcher can install its supported game
into that test folder. For an existing test installation, close the game and back
up ReSkate.dll, ReSkateLauncher.exe and ReSkate.Tricks/library before extracting.
Leave personal presets, recorded takes, saved animations and UserData intact.
Extract the whole ZIP together. Launch START-STUDIO.cmd to use a separate profile
under this folder's UserData directory. Opening the launcher EXE directly uses
the normal AppData profile instead. Binary auto-update is disabled for this
candidate so the upstream release cannot replace the test DLL.

TEST THE STUDIO
Open through the ReSkate menu or F8. Search is above TRICKS. Native asset tree
contains {config['inventory']} entries; Show: Editable clips lists available previews.
Newly decoded source previews do not automatically replace their gameplay phases.
{config['unsupported']} clip controllers remain explicitly unsupported for preview.

CHECK BEFORE CALLING THIS STABLE
1. Open 360 Flip high and 360 Hardflip high; play, pause and scrub. Check that the
   board and body remain coherent and playback does not skip or jump.
2. Search "tre flip high", browse grind entry/loop/exit folders, and follow a
   native document/resource reference. Confirm no missing/outdated library error.
3. Move/rotate a joint with a gizmo, undo/redo, change time brackets and reopen a
   saved personal animation. Confirm the edit survives restart.
4. Test an authored leg edit during the corresponding normal and switch tricks;
   compare the affected physical leg and source timing with its studio preview.
5. Optional: Record all tricks in solo play without AI skaters; keep the game in
   front. Check low/mid/high grading and cancel cleanly. This can take a while.

REPORT A PROBLEM
F8 > PLAYBACK > Export diagnostics. Include build-info.json, the exported diagnostic
folder, trick/preset name, stance and whether the issue is in F8 or live gameplay.
This is a candidate with automated checks, not a claim that visual testing passed.
''', encoding='utf-8')
    print(f'Staged complete tester candidate: {output}', flush=True)
    return info


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--run-url', default='')
    args = parser.parse_args()
    config = json.loads((args.repo / 'config/studio-test-library.json').read_text())
    commit = subprocess.check_output(['git', '-c', 'safe.directory=' + args.repo.resolve().as_posix(), 'rev-parse', 'HEAD'], cwd=args.repo, text=True).strip()
    stage(args.repo, args.build, args.library, args.output, config, commit, args.run_url)


if __name__ == '__main__': main()
