# Studio tester candidate

`22sh/tester-stability` integrates the current animation work into one candidate.
It preserves contributor commits and does not replace their branches or merge the
candidate into main automatically. Passing automated checks is a prerequisite for
distribution; the visual/gameplay checklist still determines whether it is stable.

## Source audit

The initial audit covered every advertised repository branch, all four PR heads
(including their reviews/comments), and commits reachable through those refs.
Unadvertised, unreachable server objects cannot be enumerated by a normal clone.

| Source | Audited head | Included work |
| --- | --- | --- |
| main | 20fe28b | Existing runtime and native decoding baseline |
| PR #1 / 22sh/native-animation-fixes | acb8127 | Native pose corrections and crouch selection |
| PR #2 / 22sh/switch-playback-fixes | d88ae0b | Anatomical joint edits and source timing |
| PR #3 / 22sh/native-animation-tree | 0c6d16d | Native tree, 4,004 previews, readable names, search and upgrade guidance |
| feature/unreal-bone-transform-gizmos | c09775b | Joint gizmos, snapping, camera shots, solo transforms and timeline work |
| test/gizmos-with-pr3 | a01501d | Combined studio UX, loading art, keep-clear controls and ReSkate-menu entry |
| PR #4 / anim-pop-strength | aa7cca7 | Per-tick scripted pad, crouch-duration pop control, native grading and validated batch takes |

Two integration conflicts were resolved by keeping both features: native naming
and pop-intent includes; loading-screen settling and the new trick/pop batch API.
Review also found that the batch acceptance rules rejected manual recordings for
not matching a stale batch target. Manual takes now use their actual recognized
trick and native pop grade (or the existing air-time fallback). Scripted takes
continue rejecting a wrong trick or wrong/ungraded pop. Regression tests cover
both paths, missing board/name, unknown enum values and bounded grade timestamps.
The latest PR #4 revision replaces pop-strength scaling with crouch-duration
control and a read-only pop-grade hook. Its additional crouch-state fix is already
covered by the shared playback classifier and regression tests; no second runtime
classifier is introduced. Manual recording acceptance and bounded grade timestamps
are preserved through the hook rename. The retained 32-byte integer-setter
fingerprint matches the inspected local Skate.exe; that byte comparison does not
establish runtime behavior or thread safety.

## Build and download

The **Studio tester build** workflow runs on pull requests and pushes to main.
Each PR update schedules one build, avoiding duplicate push/PR jobs and cache
scopes for the same revision. It builds Windows Release binaries, runs C++ and Python
regressions, downloads a pinned private release asset, verifies its SHA256 and
every native payload, and checks all packaged previews/index/documents/names with
the production readers before publishing a ZIP.

Open a successful run's summary and download `ReSkateStudio-Test-<commit>.zip`.
The single ZIP includes the launcher, runtime, complete native library/inspector
data, licenses, `build-info.json`, launch shortcut and tester instructions. It
contains no game installation, user presets, recorded takes or credentials.
Binary auto-update is disabled so the candidate is not silently replaced by an
upstream release. `START-STUDIO.cmd` also isolates the game's AppData profile under
the test folder's `UserData` directory; opening the launcher EXE directly retains
its usual AppData behavior. Artifacts expire after seven days; rerun to build a fresh one.

After the workflow reaches the repository's default branch, collaborators can use
**Actions → Studio tester build → Run workflow**, choose a source branch, and keep
**full_library** enabled to produce the complete package. Turning it off runs build
checks only. The integration PR validates the workflow before
that default-branch merge. Repository/organization Actions access and billing are
controlled by its administrators; this change does not alter those settings.

For local reproduction:

CI uses the `ci-windows` Ninja Multi-Config preset from an MSVC developer shell,
with two compiler jobs, embedded compiler symbols (linked PDBs are retained),
sccache for C++ and a separate cache for Cargo sources and the Spotify helper.
Rust and C++ have separate timing steps. The first cold run populates these
caches; later runs reuse unchanged compilations. The 75-minute job limit leaves
room for the complete library checks, ZIP creation and upload on a cold runner;
it is a safety limit, not a build-time target. Local Visual Studio builds keep
their existing preset and compiler settings.
New revisions keep the running build and its cache work; only the newest pending
revision is retained by the workflow's concurrency group.

```powershell
cargo fetch --locked --manifest-path SpotifyHelper/Cargo.toml
cmake --preset vs2022-x64 -DDINGOSDK_BUILD_ANIMATION_STUDIO_TESTS=ON -DDINGOSDK_LAUNCHER_AUTO_UPDATE=OFF
cmake --build --preset release --target dingosdk_runtime dingosdk_launcher dingosdk_animation_studio_tests dingosdk_anim_graph_probe --parallel 4
ctest --test-dir build/vs2022-x64 -C Release --output-on-failure
python -m pip install -r contrib/animator-studio/requirements-test.txt
python -m unittest discover -s contrib/animator-studio -p 'test_native*.py'
python -m unittest discover -s contrib/Test -p '*tests.py'
python contrib/fetch_studio_test_library.py --output work/native-library.zip
python contrib/stage_studio_tester.py --build build/vs2022-x64 --library work/native-library.zip --output work/tester-stage
python contrib/package_release_zip.py work/tester-stage ReSkateStudio-Test.zip
```

The private download uses `GITHUB_TOKEN` with contents-read access. It is never
written to the package and is stripped from signed asset redirects. The asset ID,
checksum, expected size/counts and supported Steam build are pinned in
`config/studio-test-library.json`; changing the library requires a deliberate
configuration update. `stage_studio_tester.py` refuses incomplete/legacy libraries,
missing binaries, enabled binary auto-update and an existing staging directory.

## Tester acceptance

Preview speed controls the studio timeline only. Gameplay source keys keep 1x
timing so that slow-motion editing cannot desynchronize an additive limb/board
layer from the game's native trick. A low/mid/high binding matches only its own
pop; an explicitly Any binding may match all heights. Selection rechecks native
pop grades during the recognition window without rewinding source time. The
exported 360 Flip low regression was verified with the production save reader.

An older local `ReSkate-NativeKickflipLab` research mod replaces native resources
independently of studio presets. Deleting a saved kickflip cannot unload that
external mod. It is excluded from tester packages; remove it from the active Mods
folder (with the game closed) when testing unmodified native kickflips.

Use a separate test installation and preserve its personal data. Follow the
included checklist: native 360 Flip/360 Hardflip body/board coherence, playback
and scrubbing, search/folders/inspector, gizmo and bracket editing, undo/redo,
save/reopen and normal/switch leg and timing parity. Report exact commit, preset, stance,
F8 versus gameplay behavior and an exported diagnostic dump. Newly decoded phases
remain preview-only until a live replacement binding is implemented and verified.
