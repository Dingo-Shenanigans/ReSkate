# Dot-source this script in each CI step that invokes MSVC or the Rust linker.
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $installation) { throw 'MSVC x64 build tools were not found' }
& (Join-Path $installation 'Common7/Tools/Launch-VsDevShell.ps1') -SkipAutomaticLocation -Arch amd64 -HostArch amd64
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw 'MSVC developer shell initialization failed' }
