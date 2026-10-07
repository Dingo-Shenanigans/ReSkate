# Music playback order

The music selector used a random candidate scan for both stock and mod playlists. The selector now resolves its queue channel to the registered native playlist and applies `PlaylistPlaybackMode::Sequential` to that native node when Shuffle is off. The registered native playlist already carries the published or authored GUID order, so this change reuses that membership and order. Queue channels and playlist IDs are looked up at runtime; neither is fixed in the hook. Only `MusicPlaylistType::SelectionBehavior` queues with a valid native channel/playlist binding are handled.

Open **Settings → Interface → Music Playback → Shuffle playlists**. Shuffle defaults to off, so playback begins at the playlist's first entry and continues in playlist order. Turning it on restores the engine's default random selection. The value is stored as the local profile preference `ReSkate.MusicShuffle`.

On an active playlist, turning Shuffle off seeds the sequential cursor from the current song's GUID so the next selection follows that song. A queued manual song request retains native GUID synchronization. A newly selected playlist starts at its first entry. The engine's existing per-update admission-latch reset remains responsible for allowing the following track; the hook does not clear that latch itself.

The code targets the supported 2026-09-29 game build. Selector and mode-setter fingerprints gate installation. The selector hook returns the native signed candidate index (`-1` on failure) and forwards unchanged whenever the queue or native playlist mapping is not verified.

## Verification

Static/build checks:

- Configure: `cmake --preset vs2022-x64 -DDINGOSDK_BUILD_LAUNCHER_TESTS=ON`
- Build: `cmake --build build/vs2022-x64 --config Release --parallel 8 --target dingosdk_runtime dingosdk_music_playback_policy_tests`
- Existing focused test executables: `cmake --build build/vs2022-x64 --config Release --parallel 8 --target dingosdk_mod_music_tests dingosdk_music_safety_tests dingosdk_music_shelf_lifetime_tests`
- Tests: `ctest --test-dir build/vs2022-x64 -C Release -R "^(music_shelf_lifetime|music_safety|mod_music|music_playback_policy)$" --output-on-failure`

Configuration and both build commands completed successfully. The focused CTest selection passed 4/4 tests (`mod_music`, `music_safety`, `music_playback_policy`, and `music_shelf_lifetime`).

Live acceptance remains pending. Check San Van Soundtrack starts with its first three published tracks; Your Mixtape follows its two authored tracks and wraps; Next and natural completion use the same order; manual song selection and Previous retain their expected behavior; toggling Shuffle on and off mid-playlist continues from the current song; the setting survives relaunch; and changing playlists or maps does not carry the prior playlist's cursor into the next one.
