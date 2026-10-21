<#
.SYNOPSIS
Encode an actual @oai/sky demo-window recording to a verified MJPEG AVI.
.DESCRIPTION
Capture source frames first from node_repl with record_demo.mjs and @oai/sky.
This script only encodes those source frames. It never drives apps or captures
the desktop. Pillow is required; no FFmpeg, driver installation, or network.
.EXAMPLE
.\tools\record_demo.ps1 -Manifest .\recordings\demo-001\capture-manifest.json
#>
param(
    [Parameter(Mandatory=$true)][string]$Manifest,
    [string]$Python = 'python',
    [ValidateRange(1,30)][int]$Fps = 10
)
$ErrorActionPreference='Stop'
$encoder=Join-Path $PSScriptRoot 'encode_recording.py'
& $Python $encoder $Manifest --fps $Fps
if ($LASTEXITCODE -ne 0) { throw "Recording encoding failed with exit code $LASTEXITCODE" }
