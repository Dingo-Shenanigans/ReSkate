# Music playback order

The music selector used a random candidate scan for both stock and mod playlists. The selector now resolves its queue channel to the registered native playlist and applies `PlaylistPlaybackMode::Sequential` to that native node when Shuffle is off. The registered native playlist already carries the published or authored GUID order, so this change reuses that membership and order. Queue channels and playlist IDs are looked up at runtime; neither is fixed in the hook. Only `MusicPlaylistType::SelectionBehavior` queues with a valid native channel/playlist binding are handled.

Open **Settings → Interface → Music Playback → Shuffle playlists**. Shuffle defaults to off. On each game launch, the first automatic selection on each music channel starts at a uniformly random member of its selected playlist, then continues in playlist order and wraps. No setting change is needed, including for profiles with Shuffle already saved as off. Turning Shuffle on restores the engine's default random selection. The value is stored as the local profile preference `ReSkate.MusicShuffle`.

On an active playlist, turning Shuffle off seeds the sequential cursor from the current song's GUID so the next selection follows that song. A queued manual song request retains native GUID synchronization, including at startup. After the opening selection, a newly selected playlist starts at its first entry; rebinding or changing maps does not randomize that channel again. The opening choice uses the system random source and is limited to the verified native playlist's member count. A one-song playlist still starts at zero, empty or unavailable playlists retain the existing fallback, and a random-source failure falls back to zero and is logged. Consecutive launches can legitimately pick the same song. The engine's existing per-update admission-latch reset remains responsible for allowing the following track; the hook does not clear that latch itself.

External stations (Spotify, YouTube Music and External Audio) keep their native carrier song first; their synthetic rows are not random opening candidates.

The code targets the supported 2026-09-29 game build. Selector and mode-setter fingerprints gate installation. The selector hook returns the native signed candidate index (`-1` on failure) and forwards unchanged whenever the queue or native playlist mapping is not verified.

## Verification

Static/build checks:

- Configure: `cmake --preset vs2022-x64 -DDINGOSDK_BUILD_LAUNCHER_TESTS=ON`
- Build: `cmake --build build/vs2022-x64 --config Release --parallel 8 --target dingosdk_runtime dingosdk_music_playback_policy_tests`
- Existing focused test executables: `cmake --build build/vs2022-x64 --config Release --parallel 8 --target dingosdk_mod_music_tests dingosdk_music_safety_tests dingosdk_music_shelf_lifetime_tests`
- Tests: `ctest --test-dir build/vs2022-x64 -C Release -R "^(music_shelf_lifetime|music_safety|mod_music|music_playback_policy)$" --output-on-failure`

Before startup randomization, configuration and both build commands completed successfully. The focused CTest selection passed 4/4 tests (`mod_music`, `music_safety`, `music_playback_policy`, and `music_shelf_lifetime`).

For startup randomization, the expanded playback-policy tests passed as a standalone C++20 executable with Zig 0.14.1 (`-Wall -Wextra -Werror`). The changed `local_music_playback.cpp` compiled to a Windows object with the same warning flags and MS extensions. A separate smoke check using the production random-selection function and Windows BCrypt passed 1,000 samples each for member counts 0, 1, 2, 6 and 8,192, checking bounds and varying openings. A full MSVC runtime build remains pending because MSVC is unavailable in the validation environment.

Live acceptance remains pending. Across repeated launches with Shuffle off, check San Van Soundtrack opens with different members and then follows their published successors; Your Mixtape follows its authored order from the opening member and wraps; Next and natural completion use the same order; manual song selection and Previous retain their expected behavior; toggling Shuffle on and off mid-playlist continues from the current song; the setting survives relaunch; and changing playlists or maps does not randomize the opening again or carry the prior playlist's cursor into the next one. Also check Favorites, a one-song playlist, and Shuffle on. The historical build results above predate the startup-randomization change.
