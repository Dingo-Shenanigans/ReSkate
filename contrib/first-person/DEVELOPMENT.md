> Current source: official ReSkate v2.1.0 (ba1245a5), protocol 52, composed from the clean user-tested protocol-50 source. Commit/release paused; new gameplay validation pending. Older historical notes below do not override this baseline.

# Local native First Person development

Local branch: JishFPMNative. Source: Development/JishIntegration. Base: published ReSkate v2.0.7, protocol 50. Latest Jish source audited: c0d2c4788fcbc1f970d4b3dc013a70f46b43bd6f (1.34.0). MSC presence is excluded.

The literal INS > Skater > Camera > First Person section owns the UI. Existing ReSkate values and actions take precedence. Main toggles/profiles are above Advanced tuning. All added bindings live immediately below First Person in Settings > Controls > Action Binds.

Profiles use the same directory and shared JSON file helpers as Trainer physics profiles: %LOCALAPPDATA%/ReSkate/trainer/first-person-profiles.json. Current add-on preferences use first-person-settings.json there. Native values and bindings retain their existing native persistence owner. Recommended is a shipped scripts/Custom/FirstPersonNative/presets/bundled/recommended.json resource. No profile conversion is performed.


Excluded: custom mount/jump/hippy attachment, body-flip head lock, jump/climbing look overrides, MSC reporting. Their configuration switches are removed. ReSkate's own Follow flips remains native-owned. DirectInput stick adaptation and an optional original-ReSkate camera mode are deferred.


Bail/get-up uses Jish's verified animation/get-up evidence and latches, routed through ReSkate's existing board-only hand-back. The active first-person bail view temporarily bypasses native stabilization/Spring; saved values remain intact. Collision reuses Jish's fingerprinted engine sphere query and resolver. No trace or logging subsystem is added. Invalid query contracts leave collision unavailable and visible in the menu/log.

Held skating input is latched by the existing game-facing input hook on the physical modifier edge; the camera tick can no longer replace it with a later centered sample. Camera movement still waits for neutral. Release immediately passes the physical axes. XInput and Sony HID/raw routes reuse existing native hooks; DirectInput is unchanged.

Source-level checks and compilation do not establish in-game camera contracts or controller-driver behavior. Test this build locally before any commit/push. No automatic deployment is performed. INS controller navigation remains a separate add-on, not part of this branch.

## Profile/camera bugfix pass

Profile Save is always visible, selection survives refresh/save, and native stabilization, head-motion and camera-cycle values now round-trip through profiles without duplicating live preference ownership. Reset to defaults shares the native setting queue; each slider has a Trainer-style trailing Reset control. Camera handoff math is restricted to mount/dismount rather than ordinary animation/physics-state changes. Marker teleports disarm the native cached head pose and reset stabilization/Spring before a fresh destination pose is sampled. Optional remembered glances remain governed by their explicit setting. Bail telemetry is sampled once per client tick.

Startup reader follows Trainer unknown-field handling: only recognized values are consumed and validated. Unrecognized catalog entries do not prevent initialization. No key-specific imports or conversions are used.




Local first person already refines its camera from this frame's evaluated animation at render handoff. Jish spectator_runtime.cpp does the same. Our initial native spectator path sampled only at client tick: the visible head can move during the later animation/render phase, causing a one-frame discrepancy. Native first_person::FrameSpring matches Jish's vendored reskate_spectator math; configured smoothing is intentional, but the missing render refinement was a real adaptation difference.




Shipping status (2026-10-18): unfinished double-tap emote coordination and its Other category were removed, including saved option, input filtering and dedicated policy tests. Native game emote input is unchanged. Spectator render refinement remains included. See RESKATE_INS_MENU_HANDOVER.md for current source/version and menu architecture.
