# S.K.A.T.E. trick detector

This is the S.K.A.T.E. throwdown as ReSkate ships it. Each client identifies its own
player's tricks, and the existing throwdown relay carries the result. ReSkate's native
setup, areas, invitations, turns, target HUD, letters and eliminations are unchanged.

## Playing

Open a S.K.A.T.E. drop from the game's normal throwdown menu. Every player in the session
sees it and can join it, and the game starts with the host's setup.

- **No client check:** nobody is turned away because of the client they run, and nothing
  extra is installed.
- **Setup carried over:** the host's eight setup switches (Flips, Grabs, Grinds, Ground
  tricks, Slams, leaving the board, session markers, cannonball) travel with the offer and the
  start. Every player judges the match with that same setup.
- **Unread setup stays local:** if the host's client cannot read its setup, it says so
  and keeps the drop to itself. It never offers a match it cannot describe.

Full trick recording is off. To collect a recording for a bug report, use these console
commands: `trick-log on full`, `trick-log status` and `trick-log off`.

## Network messages

S.K.A.T.E. uses the ordinary throwdown messages (`offer`, `join`, `start`, `attempt`).
Three of them carry extra S.K.A.T.E. fields:

| Message   | Extra S.K.A.T.E. fields |
| --------- | ----------------------- |
| `offer`   | setup mask |
| `start`   | the host's final settings and setup mask |
| `attempt` | setup mask, and the detector's normalized result as JSON (at most 3500 bytes) |

A dedicated server built from this source decodes these messages like any other
throwdown. S.K.A.T.E. drops therefore appear in its activity log and announcements, and they
apply throwdown tool locks.

**Breaking change:** players, and servers that decode throwdowns, must update together
with this release. There is no compatibility with the earlier detector test builds or with
earlier ReSkate releases:

- the separate detector message kinds (16 to 19) are gone;
- no detector version is sent or compared.

`skate::rules_version` survives only as a label in logs and replay reports.

## Source map

| Path | Role |
| --- | --- |
| `Extension/Skater/skate_detector.cpp`, `.h` | Capture, active turn, native eligibility, pending result, target and native judging |
| `Extension/Skater/skate_rules.h` | Setup permissions, omission and normalization |
| `Extension/Skater/trick_set.h`, `trick_attempt.h`, `trick_match.h`, `trick_metadata.h` | Phases, bounded combinations, rotation and comparison |
| `Extension/Skater/native_skate_filter.cpp` | Native ignored-observation and category adapter |
| `Extension/Skater/trick_logger.cpp` | Opt-in JSONL and readable recording |
| `Extension/Throwdowns/native_throwdowns.cpp`, `native_event_delivery.h` | Native send hook and sender attribution |
| `Extension/Throwdowns/skate_trick_rule.cpp` | Native result-comparison hook and unconsumed-result safeguard |
| `Extension/Throwdowns/throwdown_relay.cpp`, `throwdown_wire.cpp` | Relay integration and message format |

## Tests

These suites cover the mode:

- `skate_rules`
- `trick_logger`
- `throwdown_relay`
- `multiplayer_protocol`
- `server_activity`
- `server_locks`

`throwdown_relay` covers both sides:

- **Guest:** joins, starts and replays attempts from a host.
- **Host:** offers a drop, lets a player join and starts with the setup.

`verify-native-skate.py` checks the native scoring bytecode against the supported RES file.
The file names under this folder that end in `-skate.N.md` are the history of the
detector test builds. They describe the old test-client requirement, which no longer applies.
