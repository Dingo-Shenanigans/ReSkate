# Native BMX loading investigation history

Historical evidence from the isolated startup investigation and early riding tests. Statements below describe their recorded run, not the current runtime. See [native-prototype.md](native-prototype.md) for current setup and limits.

### Native loading investigation, October 16

The Release runtime builds successfully and all 15 BMX CTest suites pass. Through run 23,
isolated native tests loaded the supported game's world; the city and MPR both reached native
level state 13. The first run was visibly still on the loading screen. No instrumented BMX
run has recorded a controllable local skater, a camera update, or a successful BMX rehearsal.
The stable controls below use lifecycle logs and have no BMX counters. The recorded
movement, bike draw and rider pose counters remain zero.

Read-only source inventory in the fourth run found two
`CharacterBlueprint_Physics_Skater_RSP_CAS` sources, both with local ID 255 and no entity.
The native character-readiness online call site ran. The preparation and post-spawn
online call sites did not run; those call-site counters alone do not establish whether
their containing functions executed. A separate passthrough observer in run 10 verified
that native character preparation itself was never entered. Its fingerprints and binding
checks passed, while unrelated configuration lookups continued normally. The repeated Flow online query maps to
`activities/onboarding/manager/onboardingmanager__oncheckforcompensationcomplete`
(resource key `0xafde90d2`). Its false values include `BootflowDataModel.LoadComplete` and
`LoadScreenDataModel.HasExitedLoadScreen`, with successful model queries. The authored
`Ui_Core.OnUI_ShowWorld` and `OnUI_ExitLoadscreen` graphs publish those completion values.
They describe the incomplete load; forcing them would not establish a working native spawn.
The passive server inventory reports one ready server player, enabled spawners with
available capacity, and no pending player references. A separately validated passthrough
observer recorded no calls to the native ServerSpawner event handler. These are classic
spawners: an authored prefab resolves its seven references to the AI skater Blueprint.
Their silence does not establish where the human player's startup failed. The authored
`Flow_SpawnPlayer` and `PlayerRecipeManager.OnGameCoreStartup_InitialSpawnPlayer` route
imports the actual player Blueprint and is the relevant next investigation.
Flow interpreter coverage recorded 296,643 matching entry/exit boundaries in run 10,
without unreadable identities or inactive boundaries. Neither the asynchronous preparation
constructor nor its callback was seen. No loading-completion flag, player entity pointer,
or spawner selection is fabricated to bypass this missing startup work.

Run logs and rehearsal reports are preserved as `build/bmx-native-run1` through `run10`
with `.log` and `.json` extensions. `bmx diagnose` reports the actual source inventory,
offline call-site counts and, in an explicitly opted-in development launch, Flow identities.
Rehearsal failures now retain the last readiness check in `failure_reason`. The test copy
and profile remain inside `build/`; the normal Steam install and production profile are
separate from these runs.

`Start-BmxGame.ps1 -NativeTestSkater` is a separate diagnostic experiment. It records a
baseline, sets the exposed native `DingoTest.SpawnLocalSkater` boolean through the normal
settings API, loads MPR, records the result and restores the captured setting before the BMX
rehearsal. The override belongs to that process and is discarded on exit. This option is
not an established loading fix or the default launch procedure. Run 8 verified that the
setting changed from 0 to 1, loaded MPR and restored 0; it still produced no local rider.

`Start-BmxGame.ps1 -NativeStartupDelay` is a second process-only experiment, disabled
unless a validated development data root and the exact opt-in environment value are
present. It delays the shared San Vansterdam queue for 45 seconds, including the
session-failure fallback, while preserving the native failure modal. Run 11 logged the
delay from 21:19:05 to 21:19:50 and reached city state 13 at 21:19:56. Three diagnostic
snapshots, before and after the delay, still recorded none of the four watched startup
routines and no native character-preparation entry. No local rider or camera appeared.
This did not fix startup. The owned test window was closed after observations completed.
Its evidence is `build/bmx-native-run11.log` and
`build/bmx-native-run11-startup-observations.json`; this run did not start a riding rehearsal
or generate a new `bmx-selftest.json`. Script waits depend on loader readiness; use the
logged delay timestamps when comparing observations.

Runs 12–16 tested further startup differences in the isolated copy:

| Run | Controlled change | Observed result |
| --- | --- | --- |
| 12 | Normal Steam interface available through `-UseSteam`, replacing the offline Steam stand-in | World state 13; no attached local actor |
| 13 | Published the exact authored initial-spawn event once, with `ForceSpawnSkater=false` | Publication logged at 21:37:40.309; watched recipe-handler key `2050444214` remained unseen, and no actor appeared |
| 14 | Copied the five existing native account-save files into the isolated data root | World state 13; no attached local actor |
| 15 | Enabled loose Lua/config files for the test copy | World state 13; no attached local actor |
| 16 | Copied exactly the two existing `CompleteFTUE:singular` and `unlock_quickdrop:singular` play-event rows into the isolated profile clone | World state 13; no attached local actor |

Each run's retained `.json` report has `completed=false` and `passed=false`, with zero
native velocity updates, native distance/turn/air-height measurements, bike draw frames and
rider pose updates. No camera update was observed. These results show that those individual
experiments did not repair startup; event publication alone does not prove a receiver ran.
The exact evidence is `build/bmx-native-run12.log` / `.json` through
`build/bmx-native-run16.log` / `.json`. Only isolated copies were changed in these experiments.

The new passive ECS receiver probe is compiled, independently reviewed and covered by the
passing `bmx_ecs_diagnostics` fixture. It reads the initial-event hash bucket with a 64-node
limit, cycle detection and coherence checks, reports observer vtable RVAs, and captures the
first exact `InitRecipeTemplates` boundary's TLS/context/queue identity. It requires the
supported image, a validated development root and the exact Flow-diagnostics opt-in. It adds
no hooks, engine calls or TLS writes. A complete empty result applies only to the sampled
queue; other contexts/realms and the separate global-monitor path remain outside coverage.
Runs 17 and 18 were blocked before runtime initialization by Windows Smart App Control.
The loader returned zero; a separate load check returned Windows error 4551, and Code
Integrity events 3077/3033 identify the `VerifiedAndReputableDesktop` policy. Neither run
provides receiver-count or riding evidence. Exact artifact hashes, retained logs and
historical signing/development-environment information are recorded in
[application-control-block.md](application-control-block.md). No security policy was changed.

Runs 19–23 continued on this computer under that unchanged policy. The stable control and
the later functional runtime loaded successfully; the earlier blocked DLL was preserved.
The functional artifact used in runs 21 and 22 has SHA-256
`d542489b563f65d233f60973df4cd6b34862850f48cc8f6bec23a31ab4f9ecfd`.
This is a successful loader observation, not a player-startup or riding result. A VM/signing
decision is not blocking tests of this accepted build. The user asked to continue on the
current computer and authorized necessary settings; no Windows policy was changed.

| Run | Controlled comparison | Observed result and retained evidence |
| --- | --- | --- |
| 19 | Frozen v2.0.7 source plus data isolation only, normal Steam and loose files, fresh isolated profile, no mods or BMX probes | Runtime initialized and city reached state 13; no positive local-binding/entity lifecycle log. `build/bmx-native-run19-stable.log` and `build/bmx-native-run19-stable-observations.json`. No BMX rehearsal was run. |
| 20 | Same frozen stable DLL with CameraLight alone | City reached state 13; no positive local-actor lifecycle log. `build/bmx-native-run20-stable-camera-light.log`. This control has no BMX rehearsal or new probes. |
| 21 | Functional runtime with eight startup-entry watches and passive ECS receiver/context diagnostics | All eight watched keys remained unseen. A complete, coherent scan of the sampled client event bucket visited four nodes and found zero matching initial-spawn observers. Native recipe initialization captured the same client context and queue with active TLS. Sampled input focus/readiness were both 1. No local actor appeared; the rehearsal timed out at the actor-identity guard. `build/bmx-native-run21.log` and `.json`. |
| 22 | Same functional runtime with the entire saved SDK database and five native account-save files in a new isolated profile | All eight watched keys remained unseen and the sampled client bucket again found zero matches. No positive local-actor lifecycle log appeared. The rehearsal timed out while mount readiness was queued; focus/input samples included 0, so this does not test BMX movement. Later focus/readiness returned to 1 without a positive actor observation. `build/bmx-native-run22.log` and `.json`. |
| 23 | Ten startup watches plus read-only capability and level-class-reference observations | Root OnBegin ran once; `C8FAABC_CG` was seen once at entry/exit, but both scalar captures were rejected. The other eight watches stayed at zero. The last ECS snapshot was coherent, with four bucket nodes and zero matching initial-spawn observers; recipe initialization used the same active client context/queue. No positive local-actor observation appeared. Focus/input were 0 in the final snapshot, and the timed-out rehearsal did not test movement. `build/bmx-native-run23.log` and `.json`. |

The run-22 clone was staged from the closed `build/bmx-user-data-stable` control. SQLite's
backup API read the existing workspace `working.sqlite3` snapshot together with its WAL;
all 16 table schemas and logical row multisets matched, `quick_check` passed and the foreign-key
check had no violations. The five existing native account files, 8,459 bytes total, came
from the earlier workspace snapshot. There were no SQL value edits or production writes.
Only the new clone's main profile DB/WAL/SHM and those five native files differed from the
closed control. No working-install launcher, favorites or Trainer policies were transferred.
No startup/autoconnect fields or command strings were found in the profile extensions.
`build/bmx-working-profile-clone-report.json` records this **pre-launch** verification;
the running test subsequently owned and could change that clone. Independent review checked
the script/report and unchanged source hashes, without claiming post-launch target equality.
The earlier native-only and two-event-only negative comparisons did not cover this combined
state. The combined negative result still does not identify the missing native startup gate.

The four added authored OnBegin breadcrumbs distinguish InitLevel, the async prepare proxy,
StartupComplete and PreSpawnTransition. Their resource identities/layouts and binding words
match retained bytes and the contract fixture passes. They were present in runs 21 and 22,
bringing the watched startup total to eight; each watched key recorded zero entries/exits.
They observe entry boundaries and do not reveal the ESM's current active state. The ECS scan
likewise describes only the sampled client queue. A zero matching-observer count does not
establish that no receiver exists in another realm or the separate global-monitor path.

Run 23's artifact, SHA-256
`b803d89fa767e638da179f1fb29d4256b383288d6fc4569ecc02cf9f0aba9f99`, also loaded under the
unchanged Windows policy. Its **last** diagnostic snapshot reports a complete, coherent
41-entry capability set containing DataModel and Schematics capabilities, and a coherent
opaque level-class reference with presence 1. These presence checks do not decode the
class, establish a live `SpawningEnabled`/suffix value, or prove receiver registration.
The first snapshot occurred just as BAM reached state 13, before recipe-initializer capture;
the later snapshot captured active TLS and matching client context/queue correctly.

The new Root OnBegin watch, key `0x74faadb7`, recorded one accepted entry and exit.
`Flow_Root/C8FAABC_CG` recorded one key-seen entry and exit, but `binding_rejected=2`;
neither scalar sample is trusted. Its condition result remains unknown while the actual
binding bytes and the Root-to-CoreWait dependency are investigated. This result narrows
the startup trace without identifying a repair or exercising BMX motion. The run-23
report has zero native velocity/distance/turn/air-height, bike-draw and rider-pose counters.

Run 24 used artifact `B4BC` with Steam and loose files enabled, but ended during BAM loading
at state 8. Its last retained log entry is 22:43:39; the process was absent at 22:44:30.
The exit cause was not observed, and no matching Application-event crash record was found.
No diagnostic snapshot or BMX self-test completed. This incomplete run supplies no startup
gate counters or movement result; see `build/bmx-native-run24-steam-loose-incomplete.log`
and its `.json` report.

The earlier working installation had a controllable local actor at 13:04:07.014, before
multiplayer connection began at 13:04:14.328. Its 28 package folders include CameraLight 1.1.0,
which changes `ui/foundations/ui_core` and the profile-settings-manager configuration.
Across the audited global/BAM/root/MPR/splash startup bundles, no package changes a named
Flow, GameCore or RecipeManager asset. The isolated built-in drone has no matching startup
asset changes. CameraLight alone did not repair startup in run 20. After the comparison,
the isolated game's previous `Mods` tree was restored; the comparison tree is retained as
`build/bmx-game/Mods-camera-light-comparison-saved`. Production mods were not moved.

The broader manifest audit covers all changed/added EBX and RES entries in the audited
global/BAM/root/MPR/splash TOCs, including the original generated `.reskate` layer. Parsed
root-level structures explain the working installation's 13 server-map entries versus six
in the isolated control: seven custom-map registrations were added. The effective merged
root adds seven `OnDemandBundles` entries, seven `DetachedSubLevelInfos` entries and seven
corresponding `SubWorldReferenceObjectData` objects, each with `AutoLoad=0`. All existing
parsed fields and retained-array order are unchanged; `StartLocalServer` remains 1.
This accounts for the catalog count and is not evidence that the added maps cause player
startup. See `build/bmx/mod-level-structural-diff.json` and the retained parsed dumps.

The physical base Data trees were also compared in full: all 100 relative member paths,
sizes and SHA-256 hashes match between `D:/Games/ReSkate/Data` and `build/bmx-game/Data`,
14,437,644,810 bytes per tree. File sizes/mtimes stayed stable while hashing.
`build/bmx-data-identity.json` retains each hash. Separately, the run-19 production-data
before/after audit counted 919 files with zero metadata differences
(`build/bmx-production-data-after-run19-audit.json`); that audit is a metadata comparison,
not a content hash of all production user data. These checks do not equate installed mod
layers, live state, Steam services or Windows caches with the base Data tree.

### Primary local profile startup investigation

After the approved Windows development setting was applied and verified on
2026-10-17, run 27 loaded the previously blocked DLL normally. Runs 27 and 28
(offline and Steam respectively) both reached the same native settings gate:
group-zero query type `608fc38`, incomplete, storage absent, idle load state 5,
and no deferred load. The real index-zero native user had Guest flag `+1b8=1`,
no provider at `+148`, and no global EA/Origin provider at `74cbc50`. The account
initializer's weak references and once flag were also absent. This identifies
an offline account/settings mismatch; it does not establish a BMX regression.

`bmx_local_profile.cpp` adds an explicitly requested, isolated offline experiment
(`Start-BmxGame.ps1`, enabled for this isolated offline launcher; use
`-DisableNativeLocalProfile` for a comparison). It calls the original native predicate
once and only adapts its false result at the inspected load/update call sites,
for the coherently verified installed group-zero query and real primary Guest.
Constructor, save, all other callers, normal profiles, and Steam launches keep
the native predicate. Native storage performs its real list/read and completion
callbacks. No account flag or query completion byte is written by the adapter.
Native code proves that the PC storage accepts this genuine Guest user and uses
its own user name for the local directory. Guard fixtures cover native first-match
selection, type identity, bounds, missing providers, and concurrent changes.
The PC storage configuration function is also guarded before its first filesystem
operation: a stable, bounded native path must resolve inside the validated test
root. Invalid, changed, or escaping paths return native invalid-path result 11;
accepted paths call the original function once. The client `/persistent` alias
normally resolves through `/user/profile/data/` and the Win32 Local AppData
setting, but the guard checks the actual path rather than assuming those defaults.
Runs 29/30 exposed and recorded an implementation error in the initial storage
guard: the native path is UTF-8 `char*`, not UTF-16. Rejecting it caused the native
storage initializer to clean up its interface, and its caller then dereferenced
that null interface at `41ac0a4`. The corrected guard uses bounded byte snapshots,
strict UTF-8 conversion for Windows path validation, and regression fixtures.
Native configuration failure is therefore capable of stopping this development
test; it must not be described as a successful settings completion.

Run 31 validated the corrected native route: storage configuration returned 0;
the resolved root was exactly the isolated `Game/Skate/data/` directory; the real
group-zero query completed after an actual list request; the ScreenManager
dependency predicate became true; and all watched authored startup actions,
including AsyncPrepare and Spawn OnBegin, ran. At 08:14:01 EDT the game assigned
a controllable local skater at `(627.1, 199.9, 1062.8)`. The old recipe event
observer remained unseen despite that successful native spawn, so it was not a
necessary startup receiver for this route. The production profile audit still
showed 919 files and zero path/size/mtime differences after this run.

This establishes native settings completion and rider creation. The sampled
native menu remained open, so BMX mounting, physical movement, rendering, and
rider pose still require a successful focused rehearsal.

Run 32 repeated native storage/rider success with the extracted, tested narrow
path helper (`5DF4E4F227841C58D8664B5ED6D9B48E1658B5B8DC5A1D76327FCE1B67564CFA`).
The automatic rehearsal stopped before mounting because the native menu was open:
distance/height/turn, velocity updates, bike frames and pose updates were all zero,
and speed measurements remained null. This is not riding evidence.
The reflected UI dump identifies an active, focused `ID_MENU_HUB` stack and
`DingoScreenFlowType.Menu`; loading-transition and persistent-HUD stacks were
inactive, `HasExitedLoadScreen` and `Bootflow.LoadComplete` were true. A passive
native `FlowSpawn` lookup also returned ready because no such request remained.
Thus Run 32's observed blocker was the ordinary Hub menu. Its authored Back
action is `UI_Back` -> `ToggleMenu` (Escape/controller B), with no callable
delegate or public SDK close API proven. Keep the normal menu guard intact.
Run 33 resolved this through the normal `load MPR` transition, without changing native UI
flags or invoking a guessed navigation function. At 09:30:55 EDT, the native menu was closed
and BMX mounted. At 09:31:24 the focused scripted rehearsal passed: native displacement
11.1412 m, ground-relative hop height 0.5633 m, native yaw change 0.2438 rad, and measured
braking from 4.9106 m/s to zero. There were 541 native velocity updates, 1,363 nonempty bike
triangle submissions and 541 rider pose updates. The process subsequently closed normally
with exit code 0. Evidence: `build/bmx-native-run33-{selftest.json,log,process-status.json}`.
These are native rider measurements; draw/pose counts do not establish correct visual
appearance. Terrain/obstacle handling, long sessions, physical pad input and airborne trick
execution remain unverified. Future smoke runs dump only the screen schema.

Run 35 repeated the ride using `-UnattendedTest` and runtime DLL SHA-256
`B16C1E85407791ADDDB18CA8ED10B1DFE05104EEE754E354E3285DF6F75BF64D`.
It passed the same native distance/hop/yaw/braking measurements at 09:38:56 EDT, with
541 native velocity updates, 971 bike submissions and 541 rider pose updates. It used
9.0167 native simulation seconds across 18.672 wall seconds and closed normally with
exit code 0. The window stayed focused: `unattended=true` demonstrates no human action
was required for this run, while `background_native_seconds=0` means actual background
operation remains unverified. All 18 BMX fixture suites and the native MSVC build passed.

The intervening Run 34 stopped before the rehearsal during MPR loading. A bounded local
hang dump records the native `SwapChainPcDx12.cpp` error: `Present` returned `E_ABORT`.
Its dump has no original exception context and is not a BMX motion failure measurement.
The owned test required termination; the identical runtime subsequently passed Run 35.
Retain `build/bmx-native-run34-{stalled.dmp,stalled.json,render-error.json,log}` for future
graphics stability work. No driver or Windows graphics settings were changed.

Run 36 verified the normal `-Ride` launch with the same DLL: automatic MPR load,
mount at 09:40:13 EDT and grounded idle status after 20 seconds, with 1,202 native
velocity updates, 1,119 bike submissions and 1,201 rider pose updates. No synthetic
rehearsal was requested in that process. It closed normally with exit code 0.
The final production-profile audit still found 919 files and zero path/size/mtime
differences. The test game was closed after verification; use `-Ride` to launch it again.

### Authored initial-spawn publisher contract

The supported image and local game data establish a specific publisher, independently of
the classic AI spawners. Compiled resource `18222288932636478767`,
`flow/statemachines/main/flow_spawnplayer/ba279f7_ag_commandactiondata_0_onbegin`, has
runtime graph key `0x8e8a8b37`. At bytecode PC `0x2c`, native binding `0x575ea096` receives
the addresses of constant registers `K40` and `K100`. The EBX type patch for `K40` uses
fixup entry 9, GUID `42a429c0-fd5c-62da-bf8d-20000ee64968`, which identifies the authored
global event `Ecs/GameStateEAT/GameCoreStartup_InitialSpawnPlayer`. Its only field is
`ForceSpawnSkater`; the compiled one-byte payload at `K100` is **false**. This event is
published before the later Blueprint/teleport action at PC `0x100`.

Binding metadata at RVA `0x8236db0` declares exactly two parameters, `EventType` and
`Payload`, under `SchematicsEcs`. Its native function is RVA `0x4691310`; adapter
`0x5e49280` passes the two operand addresses unchanged. The direct native ABI is
`void(EventTypeRef*, const void*)`: the first argument points to an eight-byte cell
containing the actual runtime `TypeInfo*`, rather than pointing directly to that object.
The wrapper dereferences this cell, derives its ECS dispatch context through native
`0x18b6ae0` and `0x4b257a0`, then calls native typed dispatcher `0x4b30ab0`. Its local
dispatch-options bytes encode a zero float followed by 32-bit values `1` and `0`, and
its fifth dispatcher argument is zero. No caller-supplied ESM context is required by
this publisher. This evidence does not establish that an arbitrary thread has a valid
current game context, or permit retaining context/type objects across level teardown.

The later binding `0x90c8f816` resolves to native `0x562400`, which forwards to the
`TeleportManager` at `0x566140` with the imported player Blueprint and a string named
`BreadcrumbKeySuffix`. Its second operand is not an ESM context; binding metadata and
the native string-combination helper establish the argument type.
The state's OnEnd graph uses the same typed publisher for the separate, empty global
`GameCoreStartup_PlayerSpawned` event, GUID `e762cb3d-5cb1-4b4c-a72e-d32c586cbe74`.
These are separate actions. A controlled
initial-spawn recovery must preserve the publisher's type-reference indirection,
false payload, context lifetime and game-thread ownership, and must report actual
recipe-handler/entity observations. The authored route alone is not evidence that
normal startup or BMX riding has been repaired.

The parent machine is `Flow/StateMachines/Main/Flow_Root`, an approximately 1.3 MiB
`EcsExpressionStateMachineAsset` (file GUID
`6b5d38bd-d967-4c91-9022-1990505971f7`). Its embedded `Spawn` state uses the above
OnBegin/OnEnd action graph. Its success transition uses
`Flow/StateMachines/Main/Flow_Spawn/B4FE97C_CG`, compiled resource
`6632916432930943219`, runtime key `0x7e934eaa`. That predicate requires native
`0x831fd0` to find the selected client player with a non-null field at `+0x98`,
and requires the `FlowSpawn` world-loading request to complete unless the native
`DingoGameplay.GateBootflowOnWorldReadiness` setting disables that additional gate.
That setting's authored default is true. A separate timeout branch exists; callers
must not invoke OnEnd or publish `PlayerSpawned` to imitate success.

The read-only teleport follow-up establishes two further preconditions. The imported
Blueprint is exactly `Gameplay/Characters/CharacterBlueprint_Lite_RSP_Skater`, file GUID
`b6bc87a5-baff-404e-90a1-e2dead52e46e`, instance GUID
`986450c5-57e1-4383-9e24-296268f6404a`. Its native parameter type is the existing
`skater_entities::blueprint_type`; the existing loaded-asset lookup and Blueprint/name
guards provide an appropriate lookup pattern. Native `0x566140` accepts only manager
phase zero and retains that Blueprint through the native reference-assignment helper.
The other argument is the live `DelMarECS_GameManager_Class.BreadcrumbKeySuffix` field,
and the authored graph checks `SpawningEnabled` first. This class has generated GUID
`783e6d56-a82f-5eb5-e3e5-29fe61886736`; its authored `SpawningEnabled` default is true.
The class-getter binding `0xb99cc99d` has not yet been resolved to a supported direct
native lookup contract. A teleport invocation is not implemented because valid live
class/suffix inputs and the complete native phase/ABI guards remain unproven. The event-only
rehearsal's negative result does not justify skipping those preconditions.
