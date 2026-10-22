#Requires -RunAsAdministrator
param([ValidateSet('Disable', 'Restore')][string]$Action = 'Disable', [string]$BackupDirectory = '')
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$history = Join-Path $repo 'build\bmx-windows-development'
$key = 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy'
$name = 'VerifiedAndReputablePolicyState'
$reg = Join-Path ([Environment]::SystemDirectory) 'reg.exe'
$ci = Join-Path ([Environment]::SystemDirectory) 'CiTool.exe'
if ($Action -eq 'Restore') {
    $backup = [IO.Path]::GetFullPath($BackupDirectory)
    if (-not $backup.StartsWith($history + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Restore backup must be inside this task history.' }
    $saved = Get-Content -LiteralPath (Join-Path $backup 'before.json') -Raw | ConvertFrom-Json
    if (-not ($saved.PSObject.Properties.Name -contains 'RegistryState') -or
        ($saved.RegistryState -isnot [int] -and $saved.RegistryState -isnot [long])) { throw 'Restore backup requires an integer RegistryState.' }
    if ($saved.SmartAppControlState -notin @('Off', 'On', 'Evaluation')) { throw 'Restore backup requires a known live Smart App Control state.' }
    $value = [int]$saved.RegistryState
    if ($value -notin @(0, 1, 2)) { throw 'Unexpected saved Smart App Control state.' }
} else {
    $backup = Join-Path $history ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $backup -Force | Out-Null
    & $reg export 'HKLM\SYSTEM\CurrentControlSet\Control\CI\Policy' (Join-Path $backup 'ci-policy-before.reg') /y
    if ($LASTEXITCODE -ne 0) { throw 'Cannot back up the existing application-control registry key.' }
    $status = Get-MpComputerStatus
    [ordered]@{ Time = (Get-Date -Format o); RegistryState = (Get-ItemPropertyValue -LiteralPath $key -Name $name);
        SmartAppControlState = [string]$status.SmartAppControlState; AntivirusEnabled = $status.AntivirusEnabled;
        RealTimeProtectionEnabled = $status.RealTimeProtectionEnabled } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $backup 'before.json') -Encoding utf8
    & $ci -lp -json | Set-Content -LiteralPath (Join-Path $backup 'policies-before.json') -Encoding utf8
    if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect existing application-control policies.' }
    $value = 0
}
Start-Transcript -LiteralPath (Join-Path $backup ($Action.ToLowerInvariant() + '.log')) -Force | Out-Null
try {
    Set-ItemProperty -LiteralPath $key -Name $name -Type DWord -Value $value
    & $ci -r -json | Set-Content -LiteralPath (Join-Path $backup ($Action.ToLowerInvariant() + '-refresh.json')) -Encoding utf8
    if ($LASTEXITCODE -ne 0) { throw 'Windows rejected the policy refresh; inspect the saved refresh result.' }
    $status = Get-MpComputerStatus
    $after = [ordered]@{ Time = (Get-Date -Format o); Action = $Action; BackupDirectory = $backup;
        RegistryState = (Get-ItemPropertyValue -LiteralPath $key -Name $name); SmartAppControlState = [string]$status.SmartAppControlState;
        AntivirusEnabled = $status.AntivirusEnabled; RealTimeProtectionEnabled = $status.RealTimeProtectionEnabled }
    $after | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $backup ($Action.ToLowerInvariant() + '-after.json')) -Encoding utf8
    $after | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $history 'latest-result.json') -Encoding utf8
    if ($Action -eq 'Disable' -and [string]$status.SmartAppControlState -ne 'Off') { throw 'Registry updated, but live Smart App Control is not Off. No restart was requested.' }
    if ($Action -eq 'Restore' -and [string]$status.SmartAppControlState -ne [string]$saved.SmartAppControlState) { throw 'Registry restored, but live Smart App Control does not match its saved state.' }
    Write-Output ($after | ConvertTo-Json)
} finally { Stop-Transcript | Out-Null }
