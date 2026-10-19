param([Parameter(Mandatory=$true)][string]$LogPath, [string]$OutPath)
$ErrorActionPreference = 'Stop'
if (!$OutPath) { $OutPath = [IO.Path]::ChangeExtension($LogPath, '.rotation.csv') }
$previous=$null; $total=0.0; $segment=0; $label=''; $lastName=''; $touchdown=$null; $wasAir=$false
$rows = foreach ($line in [IO.File]::ReadLines((Resolve-Path -LiteralPath $LogPath))) {
    $r = $line | ConvertFrom-Json
    if ($r.kind -eq 'marker') { $label=$r.label }
    if ($r.kind -in @('trick_name','trick_name_update')) { $lastName=$r.display_name }
    if ($r.kind -ne 'sample') { continue }
    $yaw=$r.body_forward_yaw_deg
    $known=$r.ready -and $r.riding_body_candidate_known -and $null -ne $yaw -and $null -ne $r.offboard -and !$r.offboard
    $delta=$null; $gap=$null
    if ($previous) { $gap=[double]$r.uptime_ms-[double]$previous.uptime_ms }
    if (!$known) { $previous=$null; $touchdown=$null; $wasAir=$false; continue }
    if (!$previous -or $gap -gt 250 -or $gap -le 0 -or $previous.map -ne $r.map) {
        $segment++; $total=0.0; $touchdown=$null; $wasAir=$false
    } else {
        $delta=[double]$yaw-[double]$previous.body_forward_yaw_deg
        while ($delta -gt 180) { $delta-=360 }
        while ($delta -lt -180) { $delta+=360 }
        $total+=$delta
    }
    if ($wasAir -and !$r.riding_airborne) { $touchdown=[double]$r.uptime_ms }
    if ($r.riding_airborne) { $touchdown=$null }
    $since=$null; if ($null -ne $touchdown) { $since=[double]$r.uptime_ms-$touchdown }
    [pscustomobject]@{
        UptimeMs=$r.uptime_ms; Segment=$segment; Marker=$label; LatestTickerName=$lastName
        PhysicsState=$r.selected_physics_state; Airborne=$r.riding_airborne; MsSinceAirEnded=$since
        BodyForwardYawDeg=$yaw; BodyRightYawDeg=$r.body_right_yaw_deg
        DeltaDeg=$delta; ContinuousYawDeg=$total; SampleGapMs=$gap
        BodyForwardHorizontalLength=$r.body_forward_horizontal_length
    }
    $previous=$r; $wasAir=[bool]$r.riding_airborne
}
if (!$rows) { throw 'No valid riding-body samples. Record with trick-log on full using tricklog.3 or later.' }
$rows | Export-Csv -LiteralPath $OutPath -NoTypeInformation -Encoding utf8
Write-Output "Saved $OutPath. Angles are diagnostic axis yaw, not validated FS/BS or accepted trick rotations. LatestTickerName is temporal context, not an attempt association."
