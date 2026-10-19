> Current source: official ReSkate v2.1.0 (ba1245a5), protocol 52, composed from the clean user-tested protocol-50 source. Commit/release paused; new gameplay validation pending. Older historical notes below do not override this baseline.

# DLL to native implementation changes

This local rebuild uses the official ReSkate v2.1.0 release and audits Jish 1.34.0 with MSC reporting excluded. See DEVELOPMENT.md and SOURCE_MAP.md for the current implementation; FEATURE_PARITY_AUDIT.md records the pre-pass audit, not current missing-feature status.

| DLL area | Native adaptation and reason |
|---|---|
| Bootstrap / companion renderer | Existing ReSkate initialization and literal INS menu. No second swapchain or panel. |
| Activation / binds | Existing native First Person action plus adjacent held glance/modifier Action Binds. No Camera bind recorders. |
| Camera values | Existing native FOV, Spring, offsets, strengths, stabilization/head-motion/cycle values remain authoritative. |
| Profiles | Native Trainer directory, shared JSON read/write helpers, separate First Person catalog. Native bindings remain separately owned. |
| Bail/get-up | Existing native temporary hand-back extended with upstream verified bail/get-up state and latches. Native saved cycle preference is retained while its effective use pauses. |
| Bail head / collision | Existing native head camera temporarily bypasses stabilization/Spring. Upstream sphere query and collision resolver are reused, with bounded return to the normal target. |
| Held skating input | Latch moved to physical modifier edge in existing input hooks to avoid the late camera-tick sample. Immediate physical release remains. |
| Excluded features | No custom mount/jump/hippy attachment, body-flip locking, jump/climbing look overrides or MSC presence. |
| Deferred | DirectInput stick filtering and a proposed original-ReSkate camera mode. |

The test package is local and requires in-game verification. No claim of complete upstream DLL parity is made: exclusions are deliberate, and hardware/camera lifecycle scenarios remain to be tested.

Shipping status (2026-10-18): unfinished double-tap emote coordination and its Other category were removed, including saved option, input filtering and dedicated policy tests. Native game emote input is unchanged. Spectator render refinement remains included. See RESKATE_INS_MENU_HANDOVER.md for current source/version and menu architecture.
