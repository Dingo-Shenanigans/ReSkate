# Music selection persistence investigation

Investigated and implemented on `investigate/music-favorites-persistence`, starting at
`3d5650e`, on 2026-10-10. Reported symptom: favorited tracks disappear after
map loads and game restarts. This is a static source/binary investigation;
the original symptom was not reproduced in a fresh game session here. After the
song implementation, the user confirmed songs persist across map loads and
restarts, but liked playlists do not. The findings below
describe the starting tree; the implementation section describes the change.

## Finding

**CONFIRMED (source):** the local music path publishes catalog and Favorites
model contexts, but does not persist favorite selections or restore them from
the local profile. `music_ui_initialize_hook` queues local hydration and returns
without calling the original initializer when the local path accepts the
request (`Extension/Music/local_music_ui.cpp:185-221`).
`update_music_catalog` publishes playlist/song DTOs and completes initialization
(`:223-301`); it never reads saved favorite song identities. The `favorites`
member of `MusicUiPending` is a native playlist handle, not a saved song list.
The generated song wire contains identity and presentation metadata, not saved
favorite state (`music_ui_wire`, `:117-142`). Searches of `Extension/Music`,
`Extension/Profile`, and `Extension/Settings` found no local favorite-selection
save/restore implementation.

The existing local profile persists preferences through
`Store::set_user_value`/`user_value`
(`Extension/Profile/local_profile.cpp:345-361`). MusicShuffle uses this local
store, but favorites do not use it.

## Native boundary

Addresses below are virtual addresses in the available
`F:\Games\ReSkate-1.0.0\Skate.exe.i64` database. The initializer's first 32 bytes
at `0x140868760` match the current tree's supported-build fingerprint. Relevant
setter and save-path control flow was checked in disassembly as well as
decompilation. No database annotations or binary patches were applied.

- **CONFIRMED:** `0x140868F80` takes a Boolean and two nonzero context handles,
  and forwards to the music manager at `0x1471F5ED0` (IDA's symbol names the
  overlapping storage `qword_1471F5EC0`). It moves the second argument into
  `r9` and retains the third in `r8`.
- **CONFIRMED:** `0x1406F51A0` first calls `0x1406F5C60`, which queues a
  24-byte change in the manager's vector at `+0x200/+0x208/+0x210`, then calls
  `0x1406F51E0` to update the live model.
- **CONFIRMED:** with the additional context present, `0x1406F51E0` changes
  field 2 of the selected model and the list addressed through manager
  `+0x130`; it also adds/removes the song through the native playback manager
  (`0x140433AF0`/`0x14043E860`) using the Favorites handle at `+0x1F8`.
  The local initializer sets `+0x130` to the Favorites song-list field and
  `+0x1F8` to its native playlist handle.
- **CONFIRMED:** playlist-like wrapper `0x140868F50` calls the same queued
  setter with `r9 = 0`. The apply-only setter uses field 7 and the Liked list
  at manager `+0x110` for playlists, versus field 2 and `+0x130` for songs
  (`0x1406F5206`, `0x1406F5238`). Playlist changes skip song playback
  membership updates (`0x1406F5310`).
- **CONFIRMED:** the separate flush routine `0x1406F53D0`, reached through
  `0x140868F30`, requires a nonempty queue, the object returned by
  `0x1407466B0`, a playback manager, and two service-object lookups before
  processing queued changes. It ultimately calls `0x140275900` at
  `0x1406F5B87` and clears the queued vector. This is distinct from the
  immediate model update.
- **LIKELY:** this flush is the original service-backed persistence route.
  Its object lookup and request/callback flow support this interpretation;
  its complete transport and durable server-side behavior were not traced.
  There is no ReSkate local profile write in the corresponding source path.
- **CONFIRMED:** original initialization `0x140868760` performs the same
  service-object lookup and calls `0x1406E0530`. That function installs
  playlist/song service callback machinery via `0x140274EF0` and two calls
  to `0x1402B7740`. Accepted local initialization bypasses this machinery.
- **CONFIRMED:** the manager constructor `0x1406CA9A0` starts with empty
  selection containers and an empty pending-change vector. Construction is
  reached through `0x1406D5E60` from `0x14094E328`; destruction through
  `0x1406D75C0` from `0x14095A186` frees the manager and clears its global.
- **LIKELY:** manager/model recreation at a map load loses the session-only
  selection, explaining the reported map-load symptom. The lifecycle call
  sites are statically confirmed, but their execution during the user's
  specific transition has not been logged.

## Cause and bounded next change

The credible mechanism is a missing local persistence bridge: the native
favorite action changes session state, while ReSkate's replacement catalog
initialization does not recover saved selections. Restart necessarily discards
that session state; manager/model recreation can do the same during travel.
This investigation does not establish which original service prerequisite
fails during a live save attempt.

A focused fix should capture successful favorite/unfavorite actions, save
stable song identities in the existing local profile, then restore available
selections after song publication on each new UI generation. Song/context
handles and pointers must not be stored. Restoration should not produce new
save requests, should retain identities for temporarily absent mod songs, and
should leave shuffle behavior alone. Verify the exact
native mutation boundary and all Favorites/model/playback bookkeeping before
choosing a restore call; publishing a checked icon alone is insufficient.

Required proof for the fix: persistence after closing/reopening the store,
unfavorite persistence, stable identity across recreated handles, unavailable
mod-song retention, and live favorite/unfavorite behavior after a map change
and full game relaunch.

## Implementation

The local provider intercepts the queued selection setter at `0x1406F51A0`
only for a song or playlist context belonging to its currently hydrated manager/model.
The first implementation handled only songs; playlist likes still used the
original service route, explaining the user's follow-up report.
The extended implementation calls the apply-only setter at `0x1406F51E0`,
checks the identity and selected flag afterwards, and saves stable identities
under `ReSkate.MusicFavoriteSongs` or `ReSkate.MusicLikedPlaylists`.
The settings codec already stores JSON
arrays, but its gameplay validator allowed only scalar values. The change permits
an ordered array of valid identities for these two keys, leaving validation
of other gameplay settings intact. Its native return value is preserved:
native add success is `-1`, while unchanged returns the manager's cached status
(initially `-2`); a conventional nonnegative-success check would be incorrect.
Generations outside the local provider continue through the original setter.
Both native entry points are fingerprint-checked before enabling the bridge.

After publishing the catalog, each initialization resolves saved identities
through the new song/playlist context maps and calls the apply-only setter
with the corresponding song flag.
Disassembly confirms that operation updates the song's field 2, Favorites list,
and native playback membership, without calling the service change queue.
For playlists it updates field 7 and Liked shelf membership. Restoration also
suppresses persistence for reentrant selection callbacks.
Selection order is retained. Missing mod songs and playlists remain saved for a later initialization. Invalid saved
data is reported rather than overwritten. The generation identity retains a
weak owner; native calls take a strong lease so the saved identity does not
keep an unloaded manager alive.

Logs: `music_favorite_saved`, `music_favorite_save_failed`,
`music_favorites_restored` (saved/restored counts),
`music_favorites_restore_failed`, and `music_favorites_contract_mismatch`.
Playlist events: `music_playlist_like_saved`, `music_playlist_like_save_failed`,
`music_playlist_likes_restored`, and `music_playlist_likes_restore_failed`.

The `music_favorites` regression uses a scratch SQLite profile and tests
favorite/unfavorite across reopen, failed native readback, unchanged writes,
new model handles, missing/returning mod songs and playlists, independent song
and playlist selections, ordered playlist restoration, and preservation of
unrelated settings. Its native apply callbacks are test doubles; it does not demonstrate
in-game model or audio behavior.

## Validation

Release `dingosdk_runtime` build passed, producing
`build/vs2022-x64/Release/ReSkate.dll`. CTest passed 4/4:
`music_favorites`, `profile_changes`, `music_playback_policy`, and
`music_shelf_lifetime`. `git diff --check` passed.

The user confirmed both song favorites and playlist likes work in-game,
including persistence across map loads and game restarts. This is user-reported
live validation of the implementation before rebasing onto current upstream
main; the rebased build is validated separately with the focused checks above.
