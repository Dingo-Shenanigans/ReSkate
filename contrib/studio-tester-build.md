# Build and validate Animator Studio

This branch integrates the Studio 1.0.10 feature set into ReSkate. See [README](../README.md) for installation and the [Studio guide](animator-studio.md) for use and current limits. The fork's earlier release acceptance record is kept in [release acceptance](studio-release-checklist.md).

## Local Windows build

Install Visual Studio 2022's C++ desktop tools, CMake/Ninja and Python 3.11 or newer for the Python tests and packaging helpers. Use a Developer PowerShell, or dot-source `contrib/enter-msvc.ps1` in the shell that builds.

C++ dependencies are vendored. If Cargo is present, the Release runtime also builds the optional Spotify helper; its initial Cargo dependency fetch needs network access. Without Cargo the runtime builds without that helper. Python native-pipeline tests need the packages in `contrib/animator-studio/requirements-test.txt`.

```powershell
. ./contrib/enter-msvc.ps1
python -m pip install -r contrib/animator-studio/requirements-test.txt
# Optional, when building the Spotify helper:
# cargo fetch --locked --manifest-path SpotifyHelper/Cargo.toml
cmake --preset ci-windows
cmake --build build/ci-windows --config Release --target dingosdk_runtime dingosdk_launcher dingosdk_animation_studio_tests dingosdk_studio_pack_file_tests dingosdk_native_tools_view_tests dingosdk_arm_reach_tests dingosdk_carry_tests dingosdk_pose_place_tests dingosdk_anim_graph_probe dingosdk_plugin_tests dingosdk_trick_watch_tests dingosdk_board_material_tests --parallel 4
ctest --test-dir build/ci-windows -C Release --output-on-failure
python -m unittest discover -s contrib/animator-studio -p 'test_native*.py'
python -m unittest discover -s contrib/Test -p '*tests.py'
```

The preset enables Studio/plugin tests without forcing a fork version or disabling official updates. Outputs are under `build/ci-windows/Release`. Warnings are errors. CTest covers animation math/selection/serialization, pack rollback, native menus, arm reach, graph-probe self-tests, carry, pose placement, plugins, trick watch and board materials. Optional plugin fixture folders absent from a checkout are reported as skipped by the plugin suite.

## CI and downloads

[Studio tester build](../.github/workflows/studio-tester.yml) runs only when started manually. It builds runtime/launcher/test targets, runs C++ and Python regressions, then uploads ReSkate.dll directly. Artifacts expire after one day and repository access may be required to download them. They are temporary review builds, not permanent public releases.

The complete native-library ZIP is **manual opt-in**: start the workflow with `full_library` enabled. This adds the pinned library download, SHA256 and per-file validation, preview/index/document/name checks, isolated launcher staging and ZIP packaging. Ordinary PR builds do not run those complete-library steps.

```powershell
# GITHUB_TOKEN needs contents-read access while the pinned library is private.
python contrib/fetch_studio_test_library.py --output work/native-library.zip
python contrib/stage_studio_tester.py --build build/ci-windows --library work/native-library.zip --output work/tester-stage
python contrib/package_release_zip.py work/tester-stage ReSkateStudio-Test.zip
```

Use a fresh staging directory. The library asset, SHA256, counts and supported build are pinned in `config/studio-test-library.json`. The helper verifies the archive and each payload, refuses legacy position decoding and excludes personal presets/account files. Never include a development game installation in a public package.

If GitHub reports an artifact storage quota failure, inspect the failed step. Successful compilation/tests do not imply a deliverable was uploaded. Deleting obsolete artifacts and short retention prevent further storage accumulation; billing usage may still block uploads. Do not mark a failed upload as success to hide the problem.

## Validate a supplied library

The studio can rebuild an outdated native library from the installed game. When shipping the optional prebuilt library, validate that exact data with the reader from the intended source commit:

```powershell
$tool = './build/ci-windows/Release/dingosdk_animation_studio_tests.exe'
$library = 'work/tester-stage/ReSkate.Tricks/library'
& $tool --native-catalog $library
& $tool --native-index "$library/native-catalog.json"
& $tool --native-documents $library
& $tool --native-labels "$library/native-catalog.json" work/native-readable-names.json
```

Check every exit code when running these manually; CI does so. These validate decoding and metadata, not visual coherence or live gameplay bindings.

## Tester behavior

Preview speed changes watching speed only. Persistent gameplay selection uses ACTIVE and one enabled playlist. Fixed priority is resolved before a winning pack's Alternate variants; exact stance and Front foot matching must be tested separately. Different pop heights may use generated fallback versions, so compare the actual pop and source timing rather than assuming a saved low-pop tag excludes all other heights.

Catch-based timing on drops and catch/drop-preview tools are available; general airborne foot correction remains disabled. Native grind/transition inventory does not imply live replacement support.

An old experimental `ReSkate-NativeKickflipLab` mod can still override resources independently of Studio selections. Keep research mods out of release/test installations. Complete the [acceptance checklist](studio-release-checklist.md) on the exact DLL to be distributed.
