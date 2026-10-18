# Integration branch preparation

Prepared on 2026-10-18 for a future upstream pull request. No pull request or release was submitted.

Source: `ReSkate-Addons-JishFPMNative.zip` supplied by the fork owner.
Archive SHA-256: `4ef0fb38e76244138259145703a7209c9a0013e07f4663b741123c28e5f85692`.

The archive was imported against its stated official ReSkate v2.1.0 base,
`ba1245a5af51892e90f2be8637dd69a7e20f5b77`, then merged with the fork's main
at `db99ed4`. This retains the later upstream fixes without reverting them.
One trailing blank line in first_person_controls.cpp was removed for diff hygiene.

The other documents in this directory are supplied development history. Their
older references to paused publication, local paths, build results and gameplay
validation describe the source author's environment and do not establish current
validation for this branch.

Gameplay validation of the merged branch remains outstanding.

## Validation in this workspace

- Visual Studio 2026 x64 / MSVC 14.51, Release configuration.
- CMake configure succeeded with DINGOSDK_BUILD_FIRST_PERSON_HOST_TESTS=ON.
- dingosdk_runtime built successfully, producing ReSkate.dll.
- All 14 first_person_* CTest tests passed (0 failed).
- MSBuild emitted MSB8064 warnings about asset-directory dependencies during
  incremental-build checks; these did not prevent compilation or linking.
- No game installation was modified and no in-game validation was performed.
