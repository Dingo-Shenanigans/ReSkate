# Python virtual-controller setup plan

State: explicitly approved by the user and installed on 2026-10-21 UTC. The
official installer completed with exit code 0, without a restart. ViGEmBus is
running from `C:\Windows\System32\drivers\ViGEmBus.sys`; the installed driver is
version 1.21.442.0 (the version included in the 1.22.0 installer), with a valid
Authenticode signature. Receipts are under `build/bmx/gamepad-setup`.

The requested Python harness sends Xbox 360 controller reports through `vgamepad`.
Python alone cannot create a Windows game-controller device. A read-only system
driver inventory on 2026-10-21 found no ViGEm/vJoy/HidHide/ScpVBus driver.

## Approved setup and actual changes

- Run the official Nefarius ViGEmBus 1.22.0 installer already downloaded to
  `C:\Users\Jeffr\Desktop\reskate\build\bmx\gamepad-setup\ViGEmBus_1.22.0_x64_x86_arm64.exe`.
  SHA-256: `89220A7865076B342892F98865F3499FB7C4CFD673159E89D352C360FD014C6A`.
  Windows reports a valid Authenticode signature from Nefarius Software Solutions
  e.U.; signing-certificate thumbprint is
  `1F431092EC96A80B41AB5317F53AC02EA6F9B89B`.
- This installs a **system-wide kernel driver**, its ViGEmBus service and normal
  Windows Driver Store/device registration. It is not confined to the repository.
  Use the installer's normal program location and `/norestart`; record the actual
  installed version, service path, package identity, exit code and setup log.
  Do not change Windows security settings or restart Windows automatically.
- Create only the Python dependency environment under
  `C:\Users\Jeffr\Desktop\reskate\build\bmx-gamepad-runtime`.
  The published `vgamepad` 0.1.0 source package does **not** honor
  `VGAMEPAD_SKIP_VIGEMBUS_INSTALL`. Its setup script was never executed. Instead,
  five reviewed Python files and the x64 ViGEmClient DLL were copied from the
  hash-verified source archive into this private environment. No bundled MSI or
  global Python installation was used. The exact six-file inventory is pinned in
  `dependency-verification.json` and checked before every live harness import.
- Use the virtual controller only during bounded, explicitly executed tests of
  `C:\Users\Jeffr\Desktop\reskate\build\bmx-game\Skate.exe`, with exact process
  identity, foreground checks, an input-event ledger and neutral/disconnect
  cleanup. Physical controllers may need disconnecting to avoid controller-slot
  selection ambiguity; do not disable or hide them programmatically.

## Material limitation

ViGEmBus and its client libraries are archived and receive no further updates.
The final 1.22.0 installer removes the old automatic updater. Its valid signature
does not make it an actively maintained driver. This setup requires explicit
approval before installation on another machine. The user approved this install.
No HidHide, additional device-hiding driver, custom
unsigned driver or Windows policy change is included.

## Verification and rollback

After approval, reverify the installer hash/signature, install without reboot,
check the actual driver/service and device, then test a virtual controller's
neutral/button/release/disconnect states. A local driver test is not proof the
game consumes an input; verify that separately with a fresh isolated game trace
and recorded pixels. Stop if Windows requires a restart or driver readiness fails.

Installation and device-only checks are complete. The first invocation used
incorrect installer switches and installed nothing; the retry used the documented
Advanced Installer switches `/exenoui /qn /norestart /L*V` and succeeded. Both
attempts and logs are retained. The installed driver SHA-256 is
`B6D6FA5CA8334368FC366A3E78552EFB74EEF657061371B2DE407AA158B0A11C`.

The device-only test exposed a driver startup packet with nonzero stick values:
an initial all-zero update can be suppressed as a duplicate. One raw left-stick
unit followed by neutral flushed that cached startup packet. Independent XInput
readback then showed every button, trigger and stick value zero; device removal
was also observed. No game was running during this check. The receipt is
`neutral-probe-retry3-result.json`. Live harness execution must log and guard this
bootstrap and verify neutral before any scenario; this check alone does not prove
that Skate consumes the virtual controller.

Separate live runs 232 and 233 now show Skate consuming a virtual Y press and
mounting its normal skateboard. Run 233 joins that input ledger with a native
observer trace and sampled game-window images. Both runs preserve the isolated
runtime/source identities and two process-teardown observations. These results
verify this mount-input case, not all BMX controls, smooth motion or physics
quality. The harness's cleanup flags mean the SDK call returned; independent
device disappearance was observed in the earlier device-only test.

Rollback is to remove the specific **Nefarius Virtual Gamepad Emulation Bus
Driver / ViGEmBus** installation through its registered uninstall entry, verify
its service/device removal, and remove the repository-local dependency folder
only if requested. Preserve installer, logs and test evidence. The normal Steam
Skate installation is outside this setup.

Sources: [vgamepad API and installation](https://github.com/yannbouteiller/vgamepad),
[installer source behavior](https://github.com/yannbouteiller/vgamepad/blob/main/setup.py),
[official final release](https://github.com/nefarius/ViGEmBus/releases/tag/v1.22.0),
[maintainer retirement notice](https://docs.nefarius.at/projects/ViGEm/End-of-Life/).

The exact download/signature receipt is saved under
`build/bmx/gamepad-setup/installer-verification.json`.
