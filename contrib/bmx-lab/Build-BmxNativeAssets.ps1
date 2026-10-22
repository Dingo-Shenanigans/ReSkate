param([string]$GameRoot = 'D:\Games\ReSkate')
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$gameExe = Join-Path $GameRoot 'Skate.exe'
if ((Get-FileHash -LiteralPath $gameExe -Algorithm SHA256).Hash -ne 'FBCE74D5E28EF525DBBA2CB4ADBEBC13405BDBD88F31BC940BCA45E4AE88B8F9') {
    throw 'Use the separate supported game build as the read-only asset source.'
}
$builder = Join-Path $repoRoot 'build\vs2022-x64\Release\dingosdk_bmx_native_pack_builder.exe'
$sourceGeometry = Join-Path $repoRoot 'assets\models\bmx\reference\bike_geometry.bin'
if (-not (Test-Path -LiteralPath $builder)) { throw 'Build the dingosdk_bmx_native_pack_builder CMake target first.' }
if (-not (Test-Path -LiteralPath $sourceGeometry)) { throw 'The private local reference geometry has not been prepared.' }
$artifactRoot = Join-Path $repoRoot ('build\bmx-native-assets\generated-' + [guid]::NewGuid().ToString('N'))
$template = Join-Path $artifactRoot 'template'
$inspection = Join-Path $artifactRoot 'inspection'
$parts = Join-Path $artifactRoot 'parts'
$pack = Join-Path $artifactRoot 'pack'
New-Item -ItemType Directory -Path $template -Force | Out-Null
$zipPath = Join-Path $repoRoot 'assets\builtin\fpv_drone.zip'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($zipPath)
try {
    $total = 0L
    foreach ($entry in $zip.Entries) {
        $total += $entry.Length
        $target = [IO.Path]::GetFullPath((Join-Path $template $entry.FullName))
        if (-not $target.StartsWith($template + '\', [StringComparison]::OrdinalIgnoreCase) -or $total -gt 64MB) {
            throw 'Native template archive exceeds its local extraction boundary.'
        }
    }
} finally { $zip.Dispose() }
[IO.Compression.ZipFile]::ExtractToDirectory($zipPath, $template)
& $builder --extract-template $template $inspection $GameRoot
if ($LASTEXITCODE -ne 0) { throw 'Native template validation failed.' }
& python (Join-Path $repoRoot 'assets\models\bmx\prepare_native_parts.py') --source $sourceGeometry --output $parts --inspection $inspection
if ($LASTEXITCODE -ne 0) { throw 'Bike part preparation failed.' }
& $builder $template $parts $pack $GameRoot
if ($LASTEXITCODE -ne 0) { throw 'Native bike pack generation failed.' }
Write-Output "Native BMX pack ready: $pack"
Write-Output 'Install this private pack only in the separate test game Mods folder, with the game closed.'
