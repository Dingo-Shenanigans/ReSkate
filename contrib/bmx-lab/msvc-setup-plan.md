# Native BMX build prerequisites: setup record

Status: approved, installed and verified on 2026-10-16. Native Release runtime/launcher builds
and all three BMX suites pass. No reboot was required.

The portable BMX lab and all three tests build with the existing MinGW toolchain. The native
project explicitly requires MSVC x64. Before installation, `cmake --preset vs2022-x64` failed
because it could not find a Visual Studio instance. The same preset now configures successfully.

## Installation scope

- Install Visual Studio **2022 Build Tools** into the new `C:\BuildTools\VS2022` directory.
- Select `Microsoft.VisualStudio.Workload.VCTools` with its recommended components: MSVC v143
  x64/x86, Windows SDK and C++ CMake tools. Use the VS 2022 package, matching generator 17/v143.
- Microsoft Installer also manages its system-wide installer, shared components and Windows
  SDK under the standard Program Files / Windows Kits locations, plus temporary package data.
- Use unattended install with no automatic restart. If the installer requires elevation or a
  reboot, report that state. Allow the vendor to calculate required space before installation;
  before installation C: had approximately 282 GiB free.
- At approval, the requested install directory did not exist. No existing toolchain/configuration
  was replaced, so there was no existing target directory to back up. Preserve the current compiler
  and use a scoped developer environment for builds.
- Build output stays under `C:\Users\Jeffr\Desktop\reskate\build\vs2022-x64`.
  This setup does not copy binaries into either game installation or change player saves.

Approved command executed successfully:

```powershell
winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --source winget --override '--quiet --wait --norestart --nocache --installPath C:\BuildTools\VS2022 --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended' --accept-package-agreements --accept-source-agreements --disable-interactivity
```

## Verification and next work

Verified installation:

- Visual Studio Build Tools 2022 **17.14.41**, installed at `C:\BuildTools\VS2022`.
- `vswhere` reports complete/launchable, with no reboot required; required C++ and CMake
  component checks passed.
- MSVC compiler **19.44.35229.0**, v143 tools directory `14.44.35207`.
- Windows SDK **10.0.26100.0** headers, desktop/UCRT libraries and resource compiler present.
- Standalone MSVC Release lab builds with warnings treated as errors. All three CTest suites
  pass; its offscreen BMP export also succeeds. This does not prove in-game riding.
- Native test registration was corrected: `enable_testing()` now runs in the root CMake
  directory rather than only inside the shared helper function.
- Sandboxed parallel MSBuild workers stalled before compilation. The approved build runs with
  working worker IPC outside that sandbox and `/nr:false`; build outputs stay in the workspace.
- The native Release `dingosdk_runtime` target and its launcher dependency build successfully:
  `build/vs2022-x64/Release/ReSkate.dll` and `ReSkateLauncher.exe`, with matching PDBs.
- Native CTest discovers and passes `bmx_input`, `bmx_model` and `bmx_controls_model`.
- Asset directory dependencies that produced MSB8064 warnings were replaced with exact-file
  configure watches. The final incremental runtime build reports **zero warnings and errors**.
- Local launcher binary updates are disabled in the generated configuration. No built DLL or
  launcher was copied into a game folder and no in-game BMX validation was performed.

Reproducible native commands (after toolchain installation):

```powershell
cmake --preset vs2022-x64 -DDINGOSDK_BUILD_BMX_TESTS=ON -DDINGOSDK_LAUNCHER_AUTO_UPDATE=OFF
cmake --build --preset release --target dingosdk_runtime dingosdk_bmx_input_tests dingosdk_bmx_model_tests dingosdk_bmx_controls_model_tests --parallel 4 -- /nr:false
ctest --test-dir build/vs2022-x64 -C Release -R '^bmx_' --no-tests=error --output-on-failure
```

Local evidence: `build/native-build-msvc.log`, `build/native-build-msvc-incremental.log`,
`build/vs2022-x64/Testing/Temporary/LastTest.log` and
`build/bmx-native-build-verification.json` (artifact sizes and SHA-256 hashes).

Remaining verification/work:

1. Continue M0 test-data isolation, collision and rider-pose experiments. Native runtime testing
   requires verified data isolation before copying an experimental DLL into a test game folder.
2. Keep compile/link/test evidence distinct from gameplay feel, native contact stability,
   controller-device testing and rider animation, which have not been verified in game.

Uninstall through Visual Studio Installer if needed; avoid manually deleting shared SDK or
component directories.

Sources: [Microsoft Build Tools workload/component IDs](https://learn.microsoft.com/en-us/visualstudio/install/workload-component-id-vs-build-tools?view=vs-2022)
and [Microsoft installer command-line parameters](https://learn.microsoft.com/en-us/visualstudio/install/use-command-line-parameters-to-install-visual-studio?view=vs-2022).
