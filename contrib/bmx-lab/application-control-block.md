# Windows Application Control observations

On 2026-10-16 at 21:53 and 21:55 EDT, the isolated game could not load
the newly compiled development runtime. These attempts never initialized ReSkate
and provide no BMX riding or startup-handler evidence.

- Runtime: `build/vs2022-x64/Release/ReSkate.dll`
- Flat SHA-256: `e54f330bada98dbb758006fa539e7cb9c9661d5f49d89399763e4ed2949706f8`
- Windows error: `4551`, "An Application Control policy has blocked this file."
- Code Integrity events: `3077` and `3033`.
- Policy name: `VerifiedAndReputableDesktop` (Smart App Control).
- Policy GUID: `{0283ac0f-fff1-49ae-ada1-8a933130cad6}`.
- Retained evidence: `build/bmx-native-run17-loader-failure.log`,
  `build/bmx-native-run18-loader-failure.log`, and
  `build/bmx-code-integrity-blocks.xml`.

The runtime compiled successfully. The three focused CTest suites for player
startup, Flow observation and ECS diagnostics passed. The operating system's
execution decision is a separate prerequisite for native testing. No installed
code-signing certificate with a private key was found in the current-user or
local-machine personal certificate stores.

No policy, antivirus setting, trust store, registry value or signing configuration
has been changed. The blocked artifact and its evidence were preserved. No file
was altered for the purpose of evading that execution decision.

Later planned comparison and diagnostic builds loaded successfully on this same
computer under the unchanged Windows policy:

- Run 19 loaded the stable v2.0.7 source control with data-path isolation only,
  SHA-256 `9184ed75a36e1c10bceb6107f5440b4d3784f517daeb58644fb138684f914c55`.
  Runtime initialization and world state 13 were observed; no positive local-actor
  lifecycle log appeared. Evidence: `build/bmx-native-run19-stable.log` and
  `build/bmx-native-run19-stable-observations.json`.
- Run 20 loaded the same frozen control with CameraLight alone. It also reached
  world state 13 without a positive local-actor log. This control has no BMX
  rehearsal or new diagnostic probes. Evidence:
  `build/bmx-native-run20-stable-camera-light.log`.
- Runs 21 and 22 loaded the functional BMX runtime with eight startup watches and
  passive ECS diagnostics, SHA-256
  `d542489b563f65d233f60973df4cd6b34862850f48cc8f6bec23a31ab4f9ecfd`.
  Their retained `.log` and `.json` files show runtime initialization but no
  attached local rider or successful BMX movement.
- Run 23 loaded the next diagnostic build, SHA-256
  `b803d89fa767e638da179f1fb29d4256b383288d6fc4569ecc02cf9f0aba9f99`.
  Runtime and startup observations were collected; the Root OnBegin watch ran
  once. No positive local-actor lifecycle observation or successful movement
  followed. Evidence: `build/bmx-native-run23.log` and `.json`.

These are per-artifact observations; they do not establish why Windows accepted
one build and blocked another, or guarantee acceptance of future builds. The user
asked to continue testing on the current computer and authorized necessary
settings, but no Windows security or application-control setting was changed.
A VM or signing decision is not a current prerequisite for testing the functional
build that Windows accepted. A newly blocked artifact must be handled according
to its actual Windows result, without bypassing the policy.

Microsoft documents [trusted code signing for Smart App Control](https://learn.microsoft.com/en-us/windows/apps/develop/smart-app-control/code-signing-for-smart-app-control).
Its [FAQ](https://support.microsoft.com/en-us/windows/security/threat-malware-protection/smart-app-control-frequently-asked-questions)
states that individual-app exceptions are unavailable. A self-signed certificate
is not a supported substitute for the required publicly trusted signer.

The stable comparison and its CameraLight variant did not repair initial player
startup. The full SDK/native-save comparison in run 22 also produced no local
actor. The run-22 and run-23 rehearsals encountered input-focus gating and did
not test movement.
These startup failures remain separate from the historical loader blocks. See
[native-prototype.md](native-prototype.md) for the scoped runtime evidence and
remaining loading investigation.
