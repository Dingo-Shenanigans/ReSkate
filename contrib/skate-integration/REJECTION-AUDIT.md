# Rejected-defense audit — skate.8

All four supplied archives were inspected. Duplicate sessions were not double-counted. The fixture aligns 111 local outcomes and 44 unique failed defenses; both original landed signatures are available for 13 failures. Clock offsets were calculated from shared native events, including the five-hour offset on one client.

Offline replay checks capture and matching only. It does not replay native area eligibility, turn timing, or establish live acceptance. Missing evidence is not treated as a false rejection.

Classification counts: corrected false mismatch: 4, insufficient evidence: 27, recorded trick difference: 13.

| Case | Time (reference client) | Original target | Replayed defense | Finding |
| --- | --- | --- | --- | --- |
| 1 | 18:37:24 | Hospital Flip  /  BS 180 (normal) | BS 180 Kickflip | recorded trick difference; Trick component differs: base: "ID_TRICK_HOSPITALFLIP" vs "ID_TRICK_KICKFLIP" |
| 2 | 18:37:41 | Kickflip Underflip  /  BS 180 (normal) | BS 180 Kickflip | recorded trick difference; Trick component differs: underflip: "ID_TRICK_UNDERFLIP" vs "" |
| 3 | 18:40:40 | Kickflip  /  BS 360 (normal) | BS 360 Kickflip | corrected false mismatch |
| 4 | 18:41:04 | Ollie | Unavailable | insufficient evidence; Missing replay evidence |
| 5 | 18:42:09 | Nollie Late Kickflip  /  BS 180 (normal) | BS 180 Ollie | recorded trick difference; Trick component differs: base: "ID_TRICK_KICKFLIP" vs "ID_TRICK_OLLIE", late: "Late" vs "" |
| 6 | 18:47:02 | 360 Flip  /  BS 360 (normal) | BS 180 360 Flip | recorded trick difference; Spin, direction or stance differs: target spin:normal:360:BS, attempt spin:normal:180:BS |
| 7 | 18:47:23 | Hospital Flip | BS 180 Hospital Flip | recorded trick difference; Spin, direction or stance differs: target spin:normal:0:, attempt spin:normal:180:BS |
| 8 | 18:48:21 | Kickflip Underflip  /  BS 540 (normal) | BS 360 Kickflip Underflip | recorded trick difference; Spin, direction or stance differs: target spin:normal:540:BS, attempt spin:normal:360:BS |
| 9 | 18:48:40 | Kickflip  /  BS 540 (normal) | BS 360 Ollie | recorded trick difference; Spin, direction or stance differs: target spin:normal:540:BS, attempt spin:normal:360:BS |
| 10 | 18:50:00 | Nollie Laserflip  /  FS 360 (nollie) | BS 360 Nollie Laserflip | recorded trick difference; Spin, direction or stance differs: target spin:nollie:360:FS, attempt spin:nollie:360:BS |
| 11 | 18:52:18 | BS Pop-Shuvit  /  BS 360 (normal) | BS 360 BS 360 Pop-Shuvit | recorded trick difference; Trick component differs: held_rotation: "" vs "360" |
| 12 | 18:53:02 | Switch 360 Inward Heelflip  /  BS 360 (switch) | Unavailable | insufficient evidence; Missing replay evidence |
| 13 | 18:53:09 | Switch 360 Inward Heelflip  /  BS 360 (switch) | Unavailable | insufficient evidence; Missing replay evidence |
| 14 | 18:54:20 | No Comply | FS 180 No Comply | recorded trick difference; Spin, direction or stance differs: target spin:normal:0:, attempt spin:normal:180:FS |
| 15 | 18:54:48 | Ollie  /  FS 180 (normal) | FS 180 Ollie | corrected false mismatch |
| 16 | 18:54:58 | Ollie  /  FS 180 (normal) | FS 180 Ollie | corrected false mismatch |
| 17 | 18:55:21 | Hospital Flip  /  BS 360 (normal) | Unavailable | insufficient evidence; Missing replay evidence |
| 18 | 18:56:56 | Nollie FS 360 Pop-Shuvit | Nollie FS 360 Pop-Shuvit | corrected false mismatch |
| 19 | 18:57:09 | Nollie FS 360 Pop-Shuvit | Nollie BS 360 Pop-Shuvit | recorded trick difference; Trick component differs: direction: "ID_TRICK_FS" vs "ID_TRICK_BS" |
| 20 | 18:57:51 | Nollie  /  BS 180 (nollie) | FS 180 Switch Ollie | recorded trick difference; Spin, direction or stance differs: target spin:nollie:180:BS, attempt spin:switch:180:FS |
| 21 | 18:58:15 | Ollie  /  BS 360 (normal) | Unavailable | insufficient evidence; Missing replay evidence |
| 22 | 18:58:46 | Late Kickflip + Late Kickflip  /  BS 180 (normal) | BS 180 Heelflip + Late Kickflip | recorded trick difference; Trick component differs: base: "ID_TRICK_KICKFLIP" vs "ID_TRICK_HEELFLIP", late: "Late" vs "" |
| 23 | 19:08:32 | Hardflip  /  BS 180 (normal) | Unavailable | insufficient evidence |
| 24 | 19:08:56 | 360 Inward Heelflip  /  BS 360 (normal) | Unavailable | insufficient evidence |
| 25 | 19:09:24 | Nollie Varial Kickflip  /  BS 180 (nollie) | Unavailable | insufficient evidence |
| 26 | 19:10:12 | Nollie 360 Hardflip  /  BS 180 (nollie) | Unavailable | insufficient evidence |
| 27 | 19:10:38 | Heelflip + FS Shifty Seatbelt | Unavailable | insufficient evidence |
| 28 | 19:11:56 | Ollie  /  BS 180 (normal) | Unavailable | insufficient evidence |
| 29 | 19:13:11 | Nollie Kickflip  /  BS 360 (nollie) | Unavailable | insufficient evidence |
| 30 | 19:13:39 | Nollie BS 360 Pop-Shuvit  /  BS 180 (nollie) | Unavailable | insufficient evidence |
| 31 | 19:14:20 | FS Pop-Shuvit  /  BS 180 (normal) | Unavailable | insufficient evidence |
| 32 | 19:16:20 | BS Pop-Shuvit Underflip  /  BS 180 (normal) | Unavailable | insufficient evidence |
| 33 | 19:19:39 | Kickflip + Fakie FS Bluntslide  /  BS 180 (normal) | Unavailable | insufficient evidence |
| 34 | 19:22:59 | Ollie + FS Smith + Fakie Ollie  /  FS 180 (normal)  /  BS 180 (normal) | Unavailable | insufficient evidence |
| 35 | 19:23:45 | 360 Flip + FS Bluntslide + Heelflip  /  FS 180 (normal) | Unavailable | insufficient evidence |
| 36 | 19:24:25 | 360 Flip + FS Bluntslide + Kickflip  /  BS 180 (normal)  /  FS 180 (normal) | Unavailable | insufficient evidence |
| 37 | 19:24:47 | Heelflip + FS Boardslide | Unavailable | insufficient evidence |
| 38 | 19:25:08 | Kickflip + FS Overcrook + Nollie  /  BS 180 (normal) | Unavailable | insufficient evidence |
| 39 | 19:25:34 | BS Pop-Shuvit + FS 5-0 + Kickflip | Unavailable | insufficient evidence |
| 40 | 19:26:33 | Kickflip + FS Nose Grind + Nollie Kickflip + FS 5-0 + FS 5-0 Stall + FS Tail Stall + 360 Flip | Unavailable | insufficient evidence |
| 41 | 19:26:55 | Kickflip + BS 5-0 + Ollie | Unavailable | insufficient evidence |
| 42 | 19:27:28 | Kickflip + BS Noseslide + BS Crooked | Unavailable | insufficient evidence |
| 43 | 19:29:07 | Kickflip + BS Crooked + Nollie Back Foot Impossible  /  BS 180 (normal) | Unavailable | insufficient evidence |
| 44 | 19:29:39 | BS 360 Pop-Shuvit + BS Tailslide | Unavailable | insufficient evidence |

The corrected matches include Boned Ollie/pop equivalence and ignored manual observations. Recorded differences such as Hospital Flip versus Kickflip, missing Underflip, board-spin counts, or opposite directions remain mismatches.

The later rail session lacks the opponent’s raw capture. Its original rows visibly drifted into later segment identifiers and included stalls; the new comparison evidence is intended to make subsequent tests diagnosable from either client.

Anonymized fixture: Extension/Skater/Test/fixtures/skate7-rejections.json. audit-playtest8.py reproduces extraction from the original four archives.
