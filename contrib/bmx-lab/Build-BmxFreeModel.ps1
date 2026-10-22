param(
    [Parameter(Mandatory=$true)][string]$Python,
    [string]$GameRoot = 'D:\Games\ReSkate',
    [string]$Intermediate = 'build\bmx\free-model-inspection\intermediate',
    [double]$WheelRadius = 0.34
)
$ErrorActionPreference = 'Stop'
if ([double]::IsNaN($WheelRadius) -or [double]::IsInfinity($WheelRadius) -or $WheelRadius -le 0) { throw 'WheelRadius must be a finite positive number of metres.' }
$bmxRadiusArgument = $WheelRadius.ToString('R', [Globalization.CultureInfo]::InvariantCulture)
$bmxRepo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if (Get-Process -Name Skate -ErrorAction SilentlyContinue) { throw 'Close the isolated game before running asset preparation.' }
if ((Get-FileHash -LiteralPath (Join-Path $GameRoot 'Skate.exe') -Algorithm SHA256).Hash -ne 'FBCE74D5E28EF525DBBA2CB4ADBEBC13405BDBD88F31BC940BCA45E4AE88B8F9') {
    throw 'Use the separate supported game build as the read-only source.'
}
$bmxBuilder = Join-Path $bmxRepo 'build\vs2022-x64\Release\dingosdk_bmx_native_pack_builder.exe'
$bmxTemplate = Join-Path $bmxRepo 'build\bmx-native-assets\template'
$bmxInput = [IO.Path]::GetFullPath((Join-Path $bmxRepo $Intermediate))
if (-not $bmxInput.StartsWith($bmxRepo+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Intermediate must be inside this workspace.' }
$bmxArtifact = Join-Path $bmxRepo ('build\bmx-native-assets\free-model-'+[guid]::NewGuid().ToString('N'))
$bmxInspection = Join-Path $bmxArtifact 'inspection'
$bmxPrepared = Join-Path $bmxArtifact 'prepared'
$bmxTextures = Join-Path $bmxArtifact 'native-textures'
$bmxPack = Join-Path $bmxArtifact 'pack'
& $bmxBuilder --extract-template $bmxTemplate $bmxInspection $GameRoot
if ($LASTEXITCODE -ne 0) { throw 'Native template validation failed.' }
Write-Output "Selected maximum wheel radius: ${bmxRadiusArgument}m; reference scale factor: $($WheelRadius/0.34)"
& $Python (Join-Path $bmxRepo 'assets\models\bmx\prepare_free_native_parts.py') --intermediate $bmxInput --inspection $bmxInspection --output $bmxPrepared --wheel-radius $bmxRadiusArgument
if ($LASTEXITCODE -ne 0) { throw 'Source geometry conversion failed.' }
& $Python (Join-Path $bmxRepo 'assets\models\bmx\validate_free_native_parts.py') --intermediate $bmxInput --prepared $bmxPrepared --negative-fixtures --wheel-radius $bmxRadiusArgument
if ($LASTEXITCODE -ne 0) { throw 'Independent native geometry validation failed.' }
& $Python (Join-Path $bmxRepo 'assets\models\bmx\measure_free_contacts.py') --intermediate $bmxInput --output (Join-Path $bmxPrepared 'source-contacts.json') --wheel-radius $bmxRadiusArgument
if ($LASTEXITCODE -ne 0) { throw 'Source contact measurement failed.' }
& $Python (Join-Path $bmxRepo 'assets\models\bmx\prepare_free_native_textures.py') --atlas (Join-Path $bmxPrepared 'atlas') --output $bmxTextures
if ($LASTEXITCODE -ne 0) { throw 'Native texture preparation failed.' }
& $Python (Join-Path $bmxRepo 'assets\models\bmx\validate_free_native_textures.py') --textures $bmxTextures --atlas (Join-Path $bmxPrepared 'atlas')
if ($LASTEXITCODE -ne 0) { throw 'Independent native texture validation failed.' }
& $bmxBuilder $bmxTemplate (Join-Path $bmxPrepared 'parts') $bmxPack $GameRoot $bmxTextures
if ($LASTEXITCODE -ne 0) { throw 'Native model pack assembly failed.' }
Write-Output "Private eight-part BMX pack prepared: $bmxPack"
Write-Output 'Nothing was installed. Preserve the existing isolated pack before replacing it with this one.'
