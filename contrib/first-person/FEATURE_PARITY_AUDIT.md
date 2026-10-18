> Current source: official ReSkate v2.1.0 (ba1245a5), protocol 52, composed from the clean user-tested protocol-50 source. Commit/release paused; new gameplay validation pending. Older historical notes below do not override this baseline.

> This is the pre-pass audit. The accepted native feature pass is described in DEVELOPMENT.md and DLL_TO_NATIVE_CHANGES.md; the missing-feature table below is historical.

# First Person feature parity audit — 2026-10-15

Compared the active local native rebuild in Development/JishIntegration against Jish2637/JishsFirstPersonMod origin/main c0d2c4788fcbc1f970d4b3dc013a70f46b43bd6f (1.34.0). This audits runtime call sites and menu wiring, not just version numbers or the presence of configuration fields. Source-level implementation is not proof of in-game parity.

## Excluded: MSC First Person nametag

The diff from 974e91e05b622c5b4b7ea0b78e09be0f18c8edb3 (1.33.2) to c0d2c47 introduces the MSC bridge, presence policy, Steam capability discovery, bootstrap call, associated test/docs and release metadata. Its only unrelated code adjustment is test-host shutdown after assertions. No other camera feature was added in this commit.

Searched the active tree for msc_presence, msc_server, start_msc, jish-fp, jfp1 and Midnight Skate. No implementation, bootstrap call, command dispatch, discovery filter or build entry was found. Nothing needs removing from the native integration. Ordinary ReSkate multiplayer server tags are unrelated and retained. The pristine upstream reference repository is retained as reference; it is not linked into the native build.

Upstream commit: https://github.com/Jish2637/JishsFirstPersonMod/commit/c0d2c4788fcbc1f970d4b3dc013a70f46b43bd6f

## Missing or partially connected features

| Feature | Native state | Evidence and required work |
|---|---|---|
| First-person spectator view | Missing runtime and menu | spectator_first_person exists in Config, codec and command handler only. No spectator runtime is compiled. Port watched-head view, target/session ownership, render/head visibility, FOV/offset handling and activation-binding switch between follow/head view. Upstream spectator_runtime.cpp, spectator_view.h, spectator_head.h, spectator_pose.h and spectator_look.h. |
| Third person while bailing | Missing runtime and menu | third_person_bailing is stored, but native integration never consumes it for camera switching. Upstream comfort.cpp / comfort_policy.h implement temporary switching and restoration of the master request. |
| Third person while getting up | Missing runtime and menu | third_person_recovery is stored but no recovery camera latch or live switching is connected. Need upstream verified recovery/get-up classification, not a generic bail flag. |
| First-person bail/get-up head camera | Missing special policy | Native camera remains available as the baseline; that does not provide Jish's direct head attachment, stabilization/Follow flips suppression, bail view suspension and controlled return. Upstream head_attachment.cpp, bail_view.h, stock_camera_feed.cpp and comfort.cpp. |
| Camera collision during bails | Missing runtime and menu | bail_camera_collision is stored only. No bail trace/collision runtime is compiled. Upstream bail_collision.cpp / bail_camera_trace.cpp. |
| Comfort camera-cycle preference override | Missing runtime | Native camera-cycle control exists, but no temporary suppression/restoration while bail/recovery automatic switching owns the camera. cycle_restore fields are not evidence of a connected override. Upstream camera_cycle_override.h / comfort.cpp. |
| Attach head during mounting / on-foot jumping | Partial data only; missing behavior/menu | lock_mount_jump exists in settings. Native telemetry detects mounting, but native adapter pauses look and never invokes action_camera_locked or head_position_attached. Need upstream jump episode tracking, attachment and temporary effective native settings. |
| Attach head during hippy jumps | Missing behavior/menu | lock_hippy_jump stored; no live hippy detection/episode or attachment. Upstream hippy_jump.h, jump_episode.h and stick_runtime.cpp. |
| Lock head during on-board body flips | Missing behavior/menu | lock_body_flips stored; no body-flip detector/blend/runtime is compiled. Native Follow flips is a separate baseline setting, not an equivalent feature. Upstream body_flip.h / body_flip_runtime.cpp blend head ownership over 250 ms. |
| Allow looking during jumps and climbing | Missing option and policy | look_during_parkour stored but not read by live adapter. controls_context.cpp uses fixed eligibility, explicitly excludes falling/mounting; runtime never calls camera_look_allowed. Need supported-state policy and camera-only control for these episodes. |
| DirectInput right-stick filtering | Missing dedicated path | Native XInput/Sony filtering is wired. Upstream also filters DirectInput state/buffered axes; no corresponding First Person adapter is connected locally. This affects hardware coverage. |
| Compatibility / look-controls-only mode | Partial | look_only exists and is read by foot Spring suppression, character turning and transitions, but has no current menu toggle. Full upstream mode semantics depend on the missing attachment/comfort policies. Decide its place after native ownership is complete; merely exposing it does not close parity. |
| Profile global ownership | Difference needing review | Native apply_preset preserves look_only, while current personal-profile serialization still writes it. That creates a saved value that Load does not apply. Resolve ownership consistently; no user data conversion is required. |

Several missing fields currently survive in saved settings/profiles without affecting the camera. They must not be counted as implemented or exposed as functional toggles until runtime is connected.

## Connected source-level features

| Feature | Native implementation |
|---|---|
| Activation / First Person binding | ReSkate's existing action and camera request, not a second activation handler. |
| FOV / game default | Native FOV preference; zero sentinel for game default. |
| Spring, positional/rotational offsets, directional strengths, reset | Existing ReSkate values and controls; add-on observes shared state. |
| Disable Spring on foot | Temporary native settings sample suppression; saved Spring preference remains intact. Full upstream special head-attachment behavior is still missing as above. |
| Board-only view and camera-cycle toggle | Existing native settings retained; comfort-related cycle override is separate unfinished work. |
| Glances | Native held action binds, angles, hold/toggle mode, master toggle, linked/independent directional smoothing. |
| Off-board right-stick look | Camera-only and camera+character, sensitivities, deadzone, invert, independent directional smoothing; native motion hook for character facing. Specialized jump/parkour/recovery cases are unfinished. |
| Character-lock bail/dismount recenter | Existing latch and neutral rearming reuse upstream recenter_facing. Does not establish full bail camera parity. |
| Foot/board transition smoothing | Toggle and separate mount/dismount strengths; latest 1.33.2 direction-only handoff for BOTH off-board look modes preserves head translation. Special attachment episodes remain unfinished. |
| On-board free look | Enable, modifier, sensitivities, deadzone, invert, horizontal/vertical limits, look smoothing, held skating input, immediate physical-axis restoration, separate release centering toggles, continuing center until new movement, center strength and pitch. |
| Profiles | Native-style named JSON catalog in the physics profile folder, shared file helpers, Recommended, load/save/replace/delete/refresh. Bindings belong to native Action Binds and are intentionally excluded. |

## Intentional native adaptations, not missing DLL features

- Literal ReSkate INS menu replaces the companion panel, F9 companion visibility and standalone renderer.
- Native Action Binds immediately below First Person replace the DLL's recorders; activation/menu bindings remain native-owned.
- Native profile folder and separate JSON catalog replace DLL INI/preset paths. No data imports or conversions.
- ReSkate logging replaces a second DLL logger.
- Native initialization/camera/input hooks replace stock-binary bridge/bootstrap infrastructure where already exposed.
- Native stabilization, head nod/tilt/bob and Follow flips remain available; they do not substitute for the missing special attachment/body-flip policies.
- INS controller navigation remains a separate add-on branch; it is not automatically included by this integration.

## Audit scope and validation

No camera or input behavior was changed for this audit. No MSC implementation was present to remove. No build was needed for this documentation-only result. Prior runtime build and 12 focused tests passed; those checks do not prove completeness, hardware coverage or special-state game behavior. Current DLL_TO_NATIVE_CHANGES.md describes an older integration milestone and is not the current rebuild's parity authority; this dated audit supersedes its status claims.

Local evidence: cmake/FirstPerson.cmake; Extension/Skater/FirstPerson/integration.cpp, controls_context.cpp, right_stick_runtime.h, glance_runtime.h, native_action_bindings.h; Upstream/src/mod.h, config.cpp, settings.cpp, camera_policy.h; Extension/UI/Overlay/first_person_controls.cpp, first_person_stick_controls.cpp, skate_menu_skater.cpp.

Shipping status (2026-10-18): unfinished double-tap emote coordination and its Other category were removed, including saved option, input filtering and dedicated policy tests. Native game emote input is unchanged. Spectator render refinement remains included. See RESKATE_INS_MENU_HANDOVER.md for current source/version and menu architecture.
