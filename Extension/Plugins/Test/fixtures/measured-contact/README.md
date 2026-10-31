# Measured-contact diagnostic

A small Luau mod that checks three measured contacts under one owned capsule body, and the
fixture `Extension/Plugins/Test/measured_contact_host_fixture.inc` runs against it. The fixture
reads this manifest and entry as required files, so a missing file fails the plugin tests
rather than being skipped.

Loading only reads its stored report and registers the **Measured contacts** page. It makes no
force, torque, impulse or input request. Its manifest asks for `world.physics`, `world.raycast`,
`skater.read`, `input` (focus), `menu` and `storage`.

## What it does

After **Start measured contacts** is pressed and the menu is closed, it creates one 12 kg capsule
two metres from the skater and 1.5 metres up, reads a measured sample, and asks
`reskate.world.physics.probe_batch` for one three-ray batch with `require_contact = true`. The rays
start inside the capsule, above it through its centre, and 0.6 metres to the side. Each accepted
contact must lie below the capsule; the first two points must agree within 25 mm and the third
height within 0.15 m. The script never makes up a floor or normal, flips a normal or assumes a
zero surface velocity.

Only the refusal "waiting for a fresh qualified 60 Hz physics step" allows one more press to retry
creation; any other failure stops it for good. It makes at most two create calls and one batch,
and stops on watchdogs (120 s or 36,000 callbacks overall, 6 s or 1,800 with a body), a focus or
menu change while it holds a body, a changed sample, a missing or malformed measurement, or 0.35 m
of drift.

## Its report

`measured_contact_trial_v1` in the mod's storage records the requests, counts, the samples, the
normals and surface velocities, which values were available, the placement checks and the
failures. It releases the body once and saves once; an accepted release is the SDK's release
request (the body is retained and parked), not proof that the game destroyed it. A stored report
stops later runs; use a fresh mod folder for another one.

## In the game

Copy this folder into a mod folder of a ReSkate install, open **Measured contacts**, press
**Start measured contacts** and close the menu, standing on roughly flat ground. The fixture checks
this mod against a constructed game; only a run in the game says what the native contacts are.
