# Local BMX development settings

The user authorized using this PC and changing the settings needed for native
BMX testing. Windows Smart App Control blocked the freshly compiled runtime
`63312c1fba8f589104e81ea973950f3f2e57540e8e09b2e787cfddc9f6b8e9ec`
on 2026-10-16 at 22:55:18 EDT, before runtime initialization. Windows reports
Smart App Control On and both Microsoft Defender Antivirus and real-time
protection enabled at preparation time. The ordinary process has no
administrator token.

The prepared administrator script is `Set-BmxDevelopmentAppControl.ps1` beside
this plan. It backs up the existing CI Policy key and captures policy/protection
state inside `build/bmx-windows-development/<timestamp>/`, changes only
`HKLM\SYSTEM\CurrentControlSet\Control\CI\Policy\VerifiedAndReputablePolicyState`
to DWORD `0`, and invokes `CiTool.exe -r -json` to refresh the policy. It records
the live result and treats a state other than Off as incomplete. This disables
Smart App Control for the PC; it is not an exception for only this DLL.

Microsoft documents this registry value and refresh procedure in
[Application Control for Windows](https://learn.microsoft.com/en-us/windows/security/application-security/application-control/app-control-for-business/appcontrol).
Its [Smart App Control FAQ](https://support.microsoft.com/en-us/windows/security/threat-malware-protection/smart-app-control-frequently-asked-questions)
says recent Windows updates support re-enabling the feature without a clean
installation. The installed version reports 25H2, build 26200.9711. Restoring
must still be verified live rather than inferred from a registry write.

The script supports `-Action Restore -BackupDirectory <the exact saved folder>`
to restore the captured registry value and refresh. It does not change antivirus,
real-time scanning, firewall rules, Secure Boot, BitLocker, signing requirements
for kernel drivers, or other App Control policies. It does not restart Windows.
Windows administrator consent must be supplied at the UAC prompt; Codex's
filesystem escalation does not grant that token.

After a confirmed Off state, test the unchanged blocked DLL through the ordinary
launcher and verify native initialization. Keep the result separate from the
unresolved native rider/startup issue.

## Verified application on 2026-10-17

The user supplied Windows administrator consent for the hidden helper. Its saved
result at `build/bmx-windows-development/latest-result.json` reports Smart App
Control **Off**, Defender Antivirus **enabled**, and real-time protection
**enabled** at 07:46:37 EDT. Independent live readback matched. The exact backup
directory is `build/bmx-windows-development/20261017-074636-5af8553292bf424b8dad3b2d0fc749dd`.
No restart was required. Run 27 then loaded the previously blocked, unchanged
DLL and initialized the native runtime. This resolves the DLL loading block;
the separate loading-screen/rider startup issue remained under investigation.
