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
| PR #4 / anim-pop-strength | 3e59f52 | Per-tick scripted pad, native pop grading and validated batch takes |

Two integration conflicts were resolved by keeping both features: native naming
and pop-intent includes; loading-screen settling and the new trick/pop batch API.
Review also found that the batch acceptance rules rejected manual recordings for
not matching a stale batch target. Manual takes now use their actual recognized
trick and native pop grade (or the existing air-time fallback). Scripted takes
continue rejecting a wrong trick or wrong/ungraded pop. Regression tests cover
both paths, missing board/name, unknown enum values and bounded grade timestamps.
The two 32-byte pop-setter fingerprints also match the inspected local Skate.exe;
that byte comparison does not establish runtime behavior or thread safety.

## Build and download

The **Studio tester build** workflow runs on pull requests and pushes to main or
the integration branch. It builds Windows Release binaries, runs C++ and Python
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
checks only. The integration branch's push trigger validates the workflow before
that default-branch merge. Repository/organization Actions access and billing are
controlled by its administrators; this change does not alter those settings.

For local reproduction:

```powershell
cargo fetch --locked --manifest-path SpotifyHelper/Cargo.toml
cmake --preset vs2022-x64 -DDINGOSDK_BUILD_ANIMATION_STUDIO_TESTS=ON -DDINGOSDK_LAUNCHER_AUTO_UPDATE=OFF
cmake --build --preset release --target dingosdk_runtime dingosdk_launcher dingosdk_animation_studio_tests dingosdk_trick_gesture_tests dingosdk_anim_graph_probe --parallel 4
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

Use a separate test installation and preserve its personal data. Follow the
included checklist: native 360 Flip/360 Hardflip body/board coherence, playback
and scrubbing, search/folders/inspector, gizmo and bracket editing, undo/redo,
save/reopen, normal/switch leg and timing parity, and optional solo batch recording
with low/mid/high grading and cancellation. Report exact commit, preset, stance,
F8 versus gameplay behavior and an exported diagnostic dump. Newly decoded phases
remain preview-only until a live replacement binding is implemented and verified.
