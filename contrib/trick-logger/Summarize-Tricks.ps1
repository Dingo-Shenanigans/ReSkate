param(
    [Parameter(Mandatory=$true)][string]$LogPath,
    [string]$OutputPath
)
$ErrorActionPreference = 'Stop'
if (-not $OutputPath) { $OutputPath = [IO.Path]::ChangeExtension($LogPath, '.csv') }
$groups = [ordered]@{}
$names = [ordered]@{}
foreach ($line in [IO.File]::ReadLines((Resolve-Path -LiteralPath $LogPath).Path)) {
    if (-not $line.Trim()) { continue }
    $record = $line | ConvertFrom-Json
    $key = [string]$record.marker
    if (-not $groups.Contains($key)) {
        $groups[$key] = [pscustomobject]@{ Marker=$record.marker; Label='unlabelled'; Samples=0; NativeAttempts=0; Landed=$null; StanceRaw=$null; DirectionRaw=$null; SpinsRaw=$null; ScorableId=$null; DroppedRecords=0 }
    }
    $row = $groups[$key]
    $row.DroppedRecords = $record.dropped_records
    if ($record.kind -in @('trick_name','trick_name_update')) {
        $names[[string]$record.observation_id] = [pscustomobject]@{
            Observation=$record.observation_id; Revision=$record.revision; Name=$record.display_name;
            BaseName=$record.name; Resolved=$record.name_resolved; AlreadyVisible=$record.preexisting;
            Stance=$record.labels.stance; Direction=$record.labels.direction; BodyRotation=$record.labels.body_rotation;
            HeldRotation=$record.labels.held_rotation; ComparisonKey=$record.comparison_key;
            Marker=$record.marker; Landed='unverified'; DroppedRecords=$record.dropped_records
        }
    }
    switch ($record.kind) {
        'marker' { $row.Label = $record.label }
        'sample' { $row.Samples++ }
        'skate_attempt' {
            $row.NativeAttempts++
            $row.Landed = $record.landed
            $row.StanceRaw = $record.stance_raw
            $row.DirectionRaw = $record.direction_raw
            $row.SpinsRaw = $record.spins_raw
            $row.ScorableId = $record.scorable_id
        }
    }
}
if ($names.Count) {
    $names.Values | Export-Csv -LiteralPath $OutputPath -NoTypeInformation -Encoding UTF8
    Write-Output "Wrote $OutputPath with the latest name for each native ticker row. Repeated names in separate rows remain separate. Landing is not established by a ticker name."
} else {
    $groups.Values | Export-Csv -LiteralPath $OutputPath -NoTypeInformation -Encoding UTF8
    Write-Output "No native names were captured. Wrote legacy marker/sample summary to $OutputPath. Check trick-log status and enable the normal scoring ticker. Keep the JSONL."
}
