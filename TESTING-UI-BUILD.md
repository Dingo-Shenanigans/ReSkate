# Installing the ReSkate UI test build

The green **Code > Download ZIP** button downloads source code, not an installable mod.
Use a ZIP attached under **Releases** when a test build is published.

1. Install and run the official ReSkate release once.
2. Close Skate and the ReSkate launcher completely.
3. Back up the `ReSkate.dll` beside `ReSkateLauncher.exe`.
4. Download the UI test-build ZIP from this repository's **Releases** page.
5. Extract its `ReSkate.dll` beside `ReSkateLauncher.exe`, replacing the existing DLL.
6. Run `ReSkateLauncher.exe`. Never launch `Skate.exe` directly.

The test build must match the supported Skate/ReSkate game build. If the launcher offers an
update, decline it while testing because an official update can replace the test DLL. Restore
the backup DLL, or run the official installer again, to return to the public version.

Until a Release ZIP is attached, this repository is for developers: build with CMake as
described in `README.md`, then copy the resulting `ReSkate.dll` into the ReSkate folder.
