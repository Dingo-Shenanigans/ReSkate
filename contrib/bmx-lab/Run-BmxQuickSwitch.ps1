param(
    [Parameter(Mandatory=$true)][ValidateRange(58,1000)][int]$RunNumber,
    [ValidateSet('None','QuickSwitch')][string]$BoardHandoffMode='None',
    [string]$CaptureGateDirectory='',
    [ValidatePattern('^$|^[0-9a-f]{32}$')][string]$CaptureGateNonce='',
    [ValidateRange(30,600)][int]$CaptureGateTimeoutSeconds=180,
    [string]$BuildLog='build\bmx-chassis-ride-build.log'
)
$ErrorActionPreference='Stop'
$bmxQuickRepo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
# Reuse the established exact-PID/creation, source/archive and teardown-two
# supervisor. The new branch validates its native sidecar, not legacy F6 logs.
& (Join-Path $bmxQuickRepo 'build\bmx\run_native_chassis_ride.ps1') -RunNumber $RunNumber -Switch -QuickSwitch -BoardHandoffMode $BoardHandoffMode `
    -CaptureGateDirectory $CaptureGateDirectory -CaptureGateNonce $CaptureGateNonce `
    -CaptureGateTimeoutSeconds $CaptureGateTimeoutSeconds -BuildLog $BuildLog
