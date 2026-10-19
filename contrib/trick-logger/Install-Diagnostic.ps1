param([string]$GameRoot = 'C:\Games\ReSkate')
$ErrorActionPreference = 'Stop'
if (Get-Process -Name Skate,ReSkateLauncher -ErrorAction SilentlyContinue) { throw 'Close Skate and the ReSkate launcher first.' }
$game = Join-Path $GameRoot 'Skate.exe'
if ((Get-FileHash -LiteralPath $game -Algorithm SHA256).Hash -ne 'FBCE74D5E28EF525DBBA2CB4ADBEBC13405BDBD88F31BC940BCA45E4AE88B8F9') {
    throw 'This diagnostic client requires the supported Skate executable. No files changed.'
}
$files = @('ReSkate.dll', 'ReSkateLauncher.exe')
foreach ($name in $files) {
    if (-not (Test-Path -LiteralPath (Join-Path $PSScriptRoot $name))) { throw "Package is missing $name" }
}
$backup = Join-Path $GameRoot ('diagnostic-backups\' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $backup | Out-Null
foreach ($name in $files) {
    $destination = Join-Path $GameRoot $name
    if (Test-Path -LiteralPath $destination) { Copy-Item -LiteralPath $destination -Destination (Join-Path $backup $name) }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $destination -Force
}
Write-Output "Installed diagnostic client. Original binaries saved to $backup. Restore them with the game closed to revert."
