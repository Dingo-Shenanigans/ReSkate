# Factory Reset verification

Factory Reset is part of the existing Mod Manager. The red sidebar button sits
directly below Thunderstore and works even with no detected mods. Inspection
and permanent deletion run on the existing panel worker, keeping rendering
responsive. The confirmation focuses Cancel, accepts Escape/controller B as
Cancel, lists the inspected roots and safety exclusions, and explicitly warns
that deletion cannot be undone. Deletion has activity feedback and no Cancel
action. Logical file sizes include hard links, so progress is indeterminate
rather than reporting an unreliable percentage or guaranteed freed space.

## Service and cleanup scope

- `prepare_factory_reset(game_directory, lease)` performs read-only inspection,
  counts ordinary mod directories regardless of enabled state, estimates logical
  file sizes, records directory identities, and reports exclusions/errors.
- `execute_factory_reset(plan, progress, lease, activity)` revalidates the
  installation and roots, permanently deletes through Windows handles, continues
  independent safe targets after errors, and returns counts, errors and remaining
  paths. A subsequent fresh plan supports retry and idempotency.
- `factory_reset_blocker()` checks all running processes for `Skate.exe`, including
  games launched outside ReSkate. Detection is deliberately conservative: even a
  Skate process belonging to another installation blocks reset.
- `OperationLease` coordinates installation, reset, and launcher workers within
  the process. Local installs and Thunderstore bulk downloads reserve the same
  operation for their entire worker lifetime. Merge/launch workers participate.
- `begin_mod_operation()`, `start_reset_inspection()`, `reset_dialogs()` and
  `collect_install()` connect the service to panel state, confirmation and worker
  completion. The panel destructor joins its worker before destroying its state.

The active root follows `mods_root()` / `mod_data_arguments()`:

1. `<game>/Mods`
2. `<game>/ModData/Default/Mods`

An existing active root is authorised by its exact layout within a validated
game installation. An inactive root also needs a ReSkate metadata/generated
marker or a recognised mod content/manifest marker. Filename checks follow
Windows case-insensitive rules. Unowned inactive roots are excluded explicitly.

Every file and directory inside an authorised root is removed, then the root
itself. This covers enabled/disabled mods, nested assets and mod configuration,
`mods.json`, `.reskate-excluded.json`, their `.tmp` files, `.reskate` merged data
and metadata, `.reskate-install`, and `.reskate-download`. Whole-tree cleanup
also covers unlisted mod folders and other orphaned files within that scope.
Reset does not recreate Mods; normal installation recreates it when needed.

The game directory, `ModData/Default`, game Data/archives, `Skate.exe`,
`ReSkateLauncher.exe`, `ReSkate.dll`, Steam files, savegames, launcher settings,
general logs and personal files outside the confirmed roots are preserved.
Neither `%LOCALAPPDATA%/ReSkate/cache` nor
`%LOCALAPPDATA%/ReSkate/thunderstore/icons` is targeted. The Thunderstore listing,
browsing choices, remote README cache and icon cache remain available.
Custom `-dataPath` locations outside the two known layouts are outside reset scope.

## Filesystem safety and failures

Paths must be absolute, ordinary paths with no parent traversal, device-prefix
aliases, alternate streams or ambiguous trailing dots/spaces. Filesystem/drive
and user-profile roots are refused. Targets must exactly match one of the two
known Mods roots; game/root identities must match the inspected plan. A regular
`Skate.exe` must exist to establish the installation.

Windows handles inspect reparse points without following them. Linked roots or
ancestors stop inspection/deletion. Inside a managed root, links themselves are
removed and their external targets remain untouched. Ancestors and each visited
directory deny write/delete sharing, preventing rename and reparse replacement
during traversal. Deletion uses `SetFileInformationByHandle`, never recursive
shell commands or the Recycle Bin. Extended paths support files beyond MAX_PATH.
Recursion depth is bounded. A read-only exclusive handle on the game executable
and a second process check protect the normal external launch race during cleanup.

Missing entries are harmless. Permission errors, read-only files, locks,
enumeration failures and changed roots produce an incomplete result; no elevated
privileges or forced attribute changes are attempted. Read-only files are
reported for retry rather than changing attributes that might be shared through
a hard link. Successful removals from independent targets are retained.

Completion always rescans the actual active layout, including after failure.
Installed selection, sizes, marks/anchor, pending uninstall/replacement state
and cached local README details are invalidated. Home mod status is refreshed;
successful reset shows an empty list and "No Mods folder yet." Failed reset
shows errors and remaining paths, and can be retried after the cause is resolved.

## Automated coverage

`mod_manager_tests.cpp` keeps all pre-existing installation/order/metadata and
Thunderstore package tests, and adds isolated temporary fixtures for:

- A/B/C: full reset with two enabled mods, one disabled mod, nested assets,
  generated/staging/download data and metadata temporary files; each layout
  separately and both together; game/settings/log/save/personal preservation.
- D/E/F: absent/empty Mods, vanished targets/parents, orphaned generated state
  and malformed ordering/exclusion JSON.
- G: empty, drive/game/parent/unexpected roots, traversal and forged plans.
- H: actual Windows junction escape, reparse root/ancestor rejection, a new
  junction after preflight, changed ordinary roots, rename/write attempts while
  handles are pinned. Junction creation failure fails the suite; it is not a skip.
- I/J: locked and read-only failures, useful remaining paths, sibling/second-root
  cleanup, and successful retries.
- K/L: repeated resets and installation/discovery after reset.
- M: existing ZIP/folder installs, replacement, enabled state, saved order,
  manifests, parks and Thunderstore installation/options regressions remain.
- N: a real installation thread paused in its progress callback blocks reset;
  reset reservations block install, another reset and launcher/merge work.
- O: discarded preflight preserves files. GUI interaction uses the manual
  checklist below because the repository has no ImGui interaction test harness.
- Additional checks: case-insensitive ownership, long paths, external hard-link
  preservation, and an independently launched wait-only copy of the test binary
  named `Skate.exe` blocking both inspection and execution.

All fixtures belong to the test-created temporary tree. No real game or user Mods
directory is used. The process fixture is terminated and joined by the test.

## Commands and results

Executed with the installed Visual Studio 2022 CMake/CTest binaries:

```powershell
& 'C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe' --preset vs2022-x64 -DDINGOSDK_BUILD_LAUNCHER_TESTS=ON

& 'C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe' --build --preset release --target dingosdk_launcher dingosdk_mod_manager_tests -- /v:quiet /nologo

& 'C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe' --build --preset release --target dingosdk_launcher dingosdk_mod_manager_tests dingosdk_thunderstore_tests dingosdk_gamepad_input_tests dingosdk_depot_output_tests -- /v:quiet /nologo /clp:ErrorsOnly

& 'C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe' --test-dir build/vs2022-x64 -C Release -R 'launcher_mod_manager' --output-on-failure

& 'C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe' --test-dir build/vs2022-x64 -C Release -R '^launcher_(mod_manager|thunderstore|gamepad_input|depot_output)$' --output-on-failure
```

Release launcher and all four test targets built. All four launcher suites passed.
The first sandboxed mod-manager run failed because Windows denied the ancestor
handle at the user profile and an early fixture assumption then crashed; the
fixture now guards that assumption. The suite passed after rerunning with
approved access outside the filesystem sandbox. Final regression runs use that
approved access. The initial build reported existing MSB8064 warnings for absent
optional `assets`/`assets/emotes` directories. No game-dependent runtime merge
suite or interactive launcher/game/controller session was run.

## Manual UI checklist (pending interactive verification)

Use a disposable game installation and test mods; do not use personal mod data.

1. Confirm the red Factory Reset button is directly beneath Thunderstore, with
   matching width/spacing and correct hover/active colours. Repeat with zero mods.
2. Click it; verify inspection changes no files. Check counts, estimated size,
   both managed layout paths, and exclusions. Confirm the irreversible deletion
   wording is visible and Cancel receives initial keyboard/controller focus.
3. Cancel by mouse, Escape and controller B separately. Confirm all files remain,
   the popup closes, and focus returns to Factory Reset. Enter/A on the default
   focus must cancel, not delete.
4. Start local ZIP, folder, Thunderstore install and bulk update in turn. Check
   reset and conflicting controls are disabled, including while cancellation is
   waiting for the install worker to finish.
5. Run Skate from this launcher, then independently. Reset must instruct the user
   to close the game. Start Skate after preflight; confirmation execution must
   still refuse cleanup if the game is running.
6. Confirm Delete Everything. Check rendering remains responsive, activity
   updates appear, installation/toggles/order/reset/launch are blocked, and
   deletion offers no ineffective Cancel action.
7. On success, verify Factory Reset Complete, zero installed/enabled mods,
   cleared selection/order/exclusions and "No Mods folder yet." Return home and
   check its mod count/status, then launch Skate without stale mod merge data.
8. Lock a fixture file after preflight. Verify Factory Reset Incomplete names the
   remaining paths, the list matches disk, and other safe roots were cleaned.
   Release the lock and retry without manually deleting any mod directories.
9. Install a ZIP/folder and a Thunderstore package after reset; verify normal
   discovery, toggles, saved order, update/replacement and game launch.
10. Close the launcher during deletion in a disposable fixture; verify its
    destructor waits for the worker without accessing destroyed panel state.
