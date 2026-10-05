<#
.SYNOPSIS
Reapply the Steam privacy changes to ReSkate source after an upstream update.
.DESCRIPTION
Removes Steam sign-in and depot downloads, preserves installed-game checks,
and revises the launcher's antivirus troubleshooting advice.
Checks the whole patch before changing files. Already-patched source is left
alone. Incompatible updates stop with an error and need a refreshed patch.
This changes source files; rebuild the launcher and DLL afterward.
.EXAMPLE
.\patches\Apply-SteamPrivacyPatch.ps1 -SourcePath D:\Source\ReSkate
.EXAMPLE
.\patches\Apply-SteamPrivacyPatch.ps1 -SourcePath D:\Source\ReSkate -CheckOnly
#>
[CmdletBinding()]
param(
    [string]$SourcePath = (Split-Path -Parent $PSScriptRoot),
    [switch]$CheckOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$gitExecutable = (Get-Command git -ErrorAction Stop).Source
$patchPath = Join-Path $PSScriptRoot 'disable-steam-downloads.patch'
if (!(Test-Path -LiteralPath $patchPath -PathType Leaf)) {
    throw "Patch file is missing: $patchPath"
}
$sourceDirectory = (Resolve-Path -LiteralPath $SourcePath -ErrorAction Stop).ProviderPath

function Invoke-GitCommand {
    param([string[]]$Arguments)
    # Capture failed checks as data, including on Windows PowerShell 5.1.
    $ErrorActionPreference = 'Continue'
    $PSNativeCommandUseErrorActionPreference = $false
    $output = @(& $gitExecutable @Arguments 2>&1 | ForEach-Object { $_.ToString() })
    [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = $output -join [Environment]::NewLine }
}

$repository = Invoke-GitCommand -Arguments @('-C', $sourceDirectory, 'rev-parse', '--show-toplevel')
if ($repository.ExitCode -ne 0) {
    throw "SourcePath must be inside a ReSkate Git checkout.`n$($repository.Output)"
}
$sourceDirectory = (Resolve-Path -LiteralPath $repository.Output.Trim()).ProviderPath
if (!(Test-Path -LiteralPath (Join-Path $sourceDirectory 'Launcher/main.cpp') -PathType Leaf)) {
    throw 'The selected checkout does not contain ReSkate Launcher/main.cpp.'
}

$alreadyApplied = Invoke-GitCommand -Arguments @('-C', $sourceDirectory, 'apply', '--check', '--reverse', '--', $patchPath)
if ($alreadyApplied.ExitCode -eq 0) {
    Write-Host 'Steam privacy patch is already applied. No files changed.'
    return
}

$compatible = Invoke-GitCommand -Arguments @('-C', $sourceDirectory, 'apply', '--check', '--', $patchPath)
if ($compatible.ExitCode -ne 0) {
    throw "This source version does not match the patch. No files changed. Refresh the patch for this update.`n$($compatible.Output)"
}
if ($CheckOnly) {
    Write-Host 'Steam privacy patch can be applied. No files changed.'
    return
}

$applied = Invoke-GitCommand -Arguments @('-C', $sourceDirectory, 'apply', '--', $patchPath)
if ($applied.ExitCode -ne 0) {
    throw "Git could not apply the patch.`n$($applied.Output)"
}
Write-Host 'Steam sign-in and depot downloads removed. Installed-game checks preserved.'
Write-Host 'Rebuild ReSkateLauncher.exe and ReSkate.dll, then copy both beside Skate.exe.'
