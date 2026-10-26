# Studio 1.0.9 release acceptance

Audit baseline: `studio/ux-round-2` at `d9a3fc5` (PR #12 merged), on ReSkate 2.1.2. At the audit snapshot, main was still `c2b4551` / Studio 1.0.8 and PR #11 was open. Merge status must be checked again when choosing the final public source commit.

This is an acceptance record, not a claim that the game has already passed it. Tick a runtime item only after observing the exact candidate DLL and record its source commit/hash, tester and date in the PR or release notes.

## Automated evidence for the candidate baseline

- [x] Windows Release runtime and launcher compile with warnings treated as errors.
- [x] Eight CTest suites pass, including pack file rollback/cancellation and Fixed/Alternate priority regressions.
- [x] Native Python tests pass (21); packaging tests pass (13 run, one platform-dependent skip).
- [x] An existing saved custom animation loads with the production reader.
- [x] All 4,004 clips in the pinned native preview fixture load and have finite poses.
- [x] Inventory and readable-name checks pass for 19,339 assets; 25,511 inspector documents fit the production reader.
- [x] Current tracked-tree scan found no obvious GitHub-token/private-key/AWS-key patterns or tracked market account files (1,825 text files checked). This is not an exhaustive history/security audit.
- [ ] Validate the final packaged inventory/documents/names if distributing the optional prebuilt library; checks against a fixture are not a hash check of a different archive.
- [ ] Confirm successful upload and download of the final distribution. A quota-blocked CI run is not a successful artifact delivery.

Latest baseline CI: [run 40361192408](https://github.com/FourthStudio/ReSkate-Animator-Studio/actions/runs/40361192408) builds and passes regressions, then fails at artifact upload with the GitHub storage-quota error. The previous 23 generated ZIP artifacts have been removed; current storage being empty does not erase accumulated billing usage. Delivery remains blocked until a successful upload or another explicitly verified release distribution is available.

## In-game acceptance

- [ ] Clean install: official ReSkate 2.1.2 first, install the Studio DLL, launch, open/close F8 repeatedly, and allow the first library build to finish.
- [ ] Old data: back up the library, open an existing trick, edit/save/reopen, undo/redo, and restart without losing edits or selection.
- [ ] Preview: 360 Flip high and 360 Hardflip high; play, pause, scrub and change speed. Compare body, board, feet and landing with the intended animation.
- [ ] Gameplay: an obvious body edit and board edit on low/mid/high; verify the affected limb, board heading, onset and end timing.
- [ ] Stances: normal, fakie, switch and switch fakie; test Exact stance and Front foot separately.
- [ ] Selection: ACTIVE, no playlist on, switching the one active playlist, Fixed pack priority above/below Alternate, In turns and Shuffle over repeated attempts.
- [ ] Pop changes: ANY, an explicit pop with fallbacks, copied pop versions, a refined game grade, flat ground, stairs, gaps and landings.
- [ ] Online/AI: repeat selection and pop tests with other skaters present, including near-simultaneous pops. Check what a remote player actually sees.
- [ ] UI: custom key bindings, previews/back navigation, opening/closing the studio room during respawn/menu changes, and a large saved library.
- [ ] Market: browse and preview without linking an account, install/reinstall/update a pack, activate it through a playlist, and verify ownership after relinking a publishing team.
- [ ] File safety: use a disposable copied library for pack deletion/cancellation and update failure testing. Confirm retained files, Recycle Bin recovery and recovery error messages.
- [ ] Publishing: use an explicitly approved test pack/team; inspect the package and verify the real server result. Automated tests do not perform a live publish.
- [ ] Diagnostics: REPORT A BUG creates an inspectable ZIP with the relevant build/trick/stance context; no account files are bundled.

## Public delivery

- [ ] PR #11 and required follow-up fixes are merged into the intended public source branch; its tree matches the built DLL.
- [ ] Gameplay acceptance above is recorded. No unverified feature is described as stable or implemented.
- [ ] README, generated package instructions and release notes match the shipped controls and limitations.
- [ ] Publish a persistent release download with source commit, version and SHA256. Include license/third-party notices as appropriate to the distribution.
- [ ] Keep the game, user data, credentials and development research mods out of the distribution. Review which native data is included if offering the optional library.
- [ ] Check that a fresh user can obtain the download and corresponding project source with the access intended for the public release.

Until these gates are complete, call the build a release candidate. Keep a known working DLL/library backup for rollback.
