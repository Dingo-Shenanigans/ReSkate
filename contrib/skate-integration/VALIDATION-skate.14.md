# skate.14 body-flip category correction

Base: skate.13.1 commit b4e9134. Product 2.1.0-skate.14, detector rules 14.

## Finding

Native ticker observations carry front/backflip evidence in body_flip_direction
and body_flip_rotation on an underlying pop, board flip or grab. Prior filtering
used the underlying trick's category only. Disabling Grabs could omit its body
flip too. Foundational-pop display also omitted the body-flip modifier even when
the matching identity retained it, showing Ollie for a more specific requirement.

The earlier recorded landed Seatbelt Front Flip fixture demonstrates the native
modifier on a grab carrier. The latest user's Flips-only report has no new full
trick recording supplied; native runtime submission/eligibility in that exact
reported attempt has not been established from a complete recording.

## Change

For a verified airborne phase and known native flip/grab carrier, split known
ID_FLIPDIRECTION_FRONTFLIP / ID_FLIPDIRECTION_BACKFLIP modifiers into a distinct
flip-category matching component. Use those native keys as stable identities;
retain count and phase ownership. Filter the carrier and body component separately.
Keep body components after their phase's core trick rows so attachment to a
different carrier cannot reorder the body's requirement. Collapse repeated
identical body modifiers within the phase. Original raw input remains diagnostic
evidence; horizontal spin measurement is unchanged.

Gameplay capture can retain verified body-flip evidence on a disabled grab.
Native ignored-observation, category/area adapters and eligibility decisions are
not relaxed. In particular, disabled grabs alone still cannot seed native attempt
initialization; a result still waits for genuine native eligibility and landing.
Do not treat source tests that supply native outcomes as live graph acceptance.

Rules and existing envelope validation advance to version 14 to prevent comparison
with older clients. Server source, relay architecture, area logic, visual layout,
installed files and native result safeguard are unchanged. Logging stays opt-in.

## Validation

Release DLL/launcher build passed. All six suites passed: skate_rules,
trick_logger, throwdown_relay, multiplayer_protocol, multiplayer_native_layout
and server_activity (test only, no server executable delivery).
Native-bytecode verification passed against the supported scoring RES, SHA256
5a0e7db9d5dbe0b4f4a6051c9ddf8346bf9d7809a3226ca9eeeac78dd94ba722.
Existing MSB8064 missing assets/assets-emotes dependency warnings occurred;
compilation/linking succeeded. Live two-client gameplay acceptance has not been
performed, including native eligibility for the newly reported Flips-only case.

Coverage adds front/backflips under Flips-only, Grabs-only, both and neither;
different grab/pop carriers; front/back and count distinctions; grounded and
plain-grab negatives; repeated modifier observations; enabled board flips; and
gameplay capture of an eligible modifier on a disabled grab. The actual recorded
landed frontflip fixture is normalized under Flips-only. Existing native lifecycle,
sender-queue, manual/recovery, bounded-combination and matching suites remain.
