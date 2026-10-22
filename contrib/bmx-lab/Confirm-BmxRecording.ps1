param(
    [Parameter(Mandatory=$true)][string]$ControlPath,
    [Parameter(Mandatory=$true)][string]$CaptureManifestPath
)
# Data acknowledgement only. This script never records, sends input, launches
# or stops a process. Run only after the recorder has returned an actual frame.
function Read-BmxRecordingJson {
    param([string]$Path)
    $text=Get-Content -LiteralPath $Path -Raw
    if ((Get-Command ConvertFrom-Json).Parameters.ContainsKey('DateKind')) { return ConvertFrom-Json -InputObject $text -DateKind String }
    return ConvertFrom-Json -InputObject $text
}
function Assert-BmxRecordingFile {
    param([string]$Workspace,[string]$Path,[switch]$Directory)
    $checked=[IO.Path]::GetFullPath($Path)
    $buildRoot=Join-Path $Workspace 'build'
    if (-not $checked.StartsWith($buildRoot+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Recording acknowledgement path escaped the workspace build folder.' }
    $checkedItem=Get-Item -LiteralPath $checked -ErrorAction Stop
    if ([bool]$checkedItem.PSIsContainer -ne [bool]$Directory) { throw 'Recording acknowledgement path has an unexpected file type.' }
    $cursor=$checkedItem
    while ($cursor.FullName.Length -ge $Workspace.Length) {
        if (($cursor.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Recording acknowledgement does not follow reparse points.' }
        if ($cursor.FullName.Equals($Workspace,[StringComparison]::OrdinalIgnoreCase)) { break }
        $cursor=if($cursor.PSIsContainer){$cursor.Parent}else{$cursor.Directory}
    }
    return $checked
}
function Get-BmxRecordingUtc {
    param($Value,[string]$Field)
    if ($Value -isnot [string] -or $Value -cnotmatch 'Z$') { throw ('UTC timestamp unavailable: '+$Field) }
    return [DateTimeOffset]::Parse($Value,[Globalization.CultureInfo]::InvariantCulture,[Globalization.DateTimeStyles]::RoundtripKind).UtcDateTime
}
function Get-BmxRecordingWaitingIdentity {
    param([string]$Workspace,[string]$ControlFile)
    $control=Read-BmxRecordingJson (Assert-BmxRecordingFile $Workspace $ControlFile)
    if ($control.schema_version -ne 1 -or $control.event -cne 'bmx_recording_control' -or $control.gate_state -cne 'waiting' -or
        $control.gate_nonce -cnotmatch '^[0-9a-f]{32}$' -or $control.process_creation_filetime -cnotmatch '^[1-9][0-9]{0,19}$' -or
        $control.owned_pid -isnot [long] -and $control.owned_pid -isnot [int]) { throw 'An owned, waiting recording control is required.' }
    if ($control.owned_pid -le 0 -or $control.owned_pid -gt [uint32]::MaxValue) { throw 'Recording control PID is invalid.' }
    $gateDirectory=Assert-BmxRecordingFile $Workspace $control.gate_directory -Directory
    $statusPath=Assert-BmxRecordingFile $Workspace $control.gate_status_file
    $readyPath=[IO.Path]::GetFullPath($control.gate_ready_file)
    if (-not $statusPath.Equals((Join-Path $gateDirectory 'recording-gate.status.json'),[StringComparison]::OrdinalIgnoreCase) -or
        -not $readyPath.Equals((Join-Path $gateDirectory 'recording-gate.ready'),[StringComparison]::OrdinalIgnoreCase)) { throw 'Recording gate paths disagree with the dedicated gate directory.' }
    if (Test-Path -LiteralPath $readyPath) { throw 'A recording acknowledgement already exists; refusing overwrite.' }
    $status=Read-BmxRecordingJson $statusPath
    if ($status.schema_version -ne 1 -or $status.event -cne 'bmx_recording_startup_gate' -or $status.state -cne 'waiting' -or
        $status.nonce -cne $control.gate_nonce -or $status.process_id -ne $control.owned_pid -or
        $status.process_creation_filetime -cne $control.process_creation_filetime) { throw 'Native recording gate and supervisor identity disagree or are no longer waiting.' }
    $created=[DateTime]::FromFileTimeUtc([Int64]::Parse($status.process_creation_filetime,[Globalization.CultureInfo]::InvariantCulture))
    $controlCreated=Get-BmxRecordingUtc $control.owned_created_utc 'owned_created_utc'
    $statusObserved=Get-BmxRecordingUtc $status.observed_utc 'native gate observed_utc'
    if ([Math]::Abs(($controlCreated-$created).TotalMilliseconds) -gt 2 -or $statusObserved -lt $created.AddMilliseconds(-2) -or $statusObserved -gt [DateTime]::UtcNow.AddSeconds(2)) {
        throw 'Recording gate creation/waiting timestamps are inconsistent.'
    }
    return [pscustomobject]@{control=$control;status=$status;created=$created;ready=$readyPath;directory=$gateDirectory}
}
function Assert-BmxRecordingOwnedProcess {
    param([string]$Executable,[uint32]$ProcessId,[DateTime]$Created,[string]$RuntimeHash)
    $processes=@(Get-CimInstance Win32_Process -Filter "ProcessId=$ProcessId" -ErrorAction Stop)
    if ($processes.Count -ne 1 -or -not $processes[0].ExecutablePath -or
        -not $processes[0].ExecutablePath.Equals($Executable,[StringComparison]::OrdinalIgnoreCase) -or
        -not $processes[0].CreationDate -or [Math]::Abs(($processes[0].CreationDate.ToUniversalTime()-$Created).TotalMilliseconds) -gt 2) {
        throw 'Live authoritative process identity does not match the recording.'
    }
    $installed=Join-Path ([IO.Path]::GetDirectoryName($Executable)) 'ReSkate.dll'
    if ((Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash -cne $RuntimeHash.ToUpperInvariant()) { throw 'The live isolated runtime differs from the recorded runtime identity.' }
}
function Assert-BmxRecordingFrame {
    param([string]$Workspace,[string]$CaptureDirectory,$Frame,$Window)
    if ($Frame.file -isnot [string] -or [IO.Path]::GetFileName($Frame.file) -cne $Frame.file -or $Frame.file -cnotmatch '^frame-[0-9]{6}\.(png|jpg)$' -or
        $Frame.sha256 -cnotmatch '^[a-fA-F0-9]{64}$' -or $Frame.bytes -le 0 -or $Frame.bytes -gt 33554432 -or
        $Frame.window_id -ne $Window.id -or $Frame.app -cne $Window.app) { throw 'Referenced captured frame has invalid path, size or window identity.' }
    $file=Assert-BmxRecordingFile $Workspace (Join-Path $CaptureDirectory $Frame.file)
    $info=Get-Item -LiteralPath $file
    if ($info.Length -ne $Frame.bytes -or (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -cne $Frame.sha256.ToUpperInvariant()) { throw 'Referenced captured frame bytes/hash disagree with the recording manifest.' }
    $started=Get-BmxRecordingUtc $Frame.capture_started_utc 'frame capture_started_utc'
    $ended=Get-BmxRecordingUtc $Frame.capture_ended_utc 'frame capture_ended_utc'
    if ($started -gt $ended) { throw 'Captured frame interval is reversed.' }
    return [pscustomobject]@{started=$started;ended=$ended;sha256=$Frame.sha256;file=$file}
}
function Confirm-BmxRecording {
    param([string]$ControlPath,[string]$CaptureManifestPath,[string]$Workspace=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..')))
    $identity=Get-BmxRecordingWaitingIdentity $Workspace $ControlPath
    $manifestPath=Assert-BmxRecordingFile $Workspace $CaptureManifestPath
    $captureDirectory=Assert-BmxRecordingFile $Workspace ([IO.Path]::GetDirectoryName($manifestPath)) -Directory
    $manifest=Read-BmxRecordingJson $manifestPath
    if ($manifest.schema -ne 1 -or $manifest.event -cne 'bmx_sampled_live_window_capture' -or $manifest.sampled_live -isnot [bool] -or $manifest.sampled_live -ne $true -or
        $null -eq $manifest.PSObject.Properties['failure'] -or $null -ne $manifest.failure -or
        $manifest.completed -isnot [bool] -or $manifest.completed -ne $false -or $null -ne $manifest.capture_ended_utc -or $null -ne $manifest.termination_reason -or
        $null -eq $manifest.audit -or $manifest.audit.sample_count -lt 1 -or $manifest.audit.sample_count -ne @($manifest.samples).Count) {
        throw 'An active, successful sampled capture with at least one completed frame is required.'
    }
    $exe=Join-Path $Workspace 'build\bmx-game\Skate.exe'
    $expectedApp='process:'+$exe
    $window=$manifest.source_window;$owned=$manifest.owned_process
    if ($window.app -isnot [string] -or -not $window.app.Equals($expectedApp,[StringComparison]::OrdinalIgnoreCase) -or $window.id -le 0 -or
        $owned.process_id -ne $identity.control.owned_pid -or $owned.game_exe -isnot [string] -or
        -not ([IO.Path]::GetFullPath($owned.game_exe)).Equals($exe,[StringComparison]::OrdinalIgnoreCase) -or $owned.runtime_sha256 -cnotmatch '^[a-fA-F0-9]{64}$') {
        throw 'Captured source window and owned process must identify this exact isolated Skate executable.'
    }
    $captureCreated=Get-BmxRecordingUtc $owned.process_created_utc 'capture owned process_created_utc'
    $captureStarted=Get-BmxRecordingUtc $manifest.capture_started_utc 'capture_started_utc'
    if ([Math]::Abs(($captureCreated-$identity.created).TotalMilliseconds) -gt 2 -or $captureStarted -lt $identity.created.AddMilliseconds(-2) -or
        (Get-Item -LiteralPath $captureDirectory).CreationTimeUtc -lt $identity.created.AddMilliseconds(-2)) { throw 'Capture directory/session does not belong to this fresh owned process.' }
    $frames=@($manifest.samples)
    if ($frames[0].index -ne 0 -or $frames[-1].index -ne ($frames.Count-1)) { throw 'Captured frame indices are incomplete.' }
    $first=Assert-BmxRecordingFrame $Workspace $captureDirectory $frames[0] $window
    $last=Assert-BmxRecordingFrame $Workspace $captureDirectory $frames[-1] $window
    if ($first.started -lt $captureStarted.AddMilliseconds(-2) -or $first.ended -gt $last.ended -or
        $last.ended -gt [DateTime]::UtcNow.AddSeconds(2) -or $last.ended -lt [DateTime]::UtcNow.AddSeconds(-15)) { throw 'The latest completed captured frame is stale or its time is inconsistent.' }
    Assert-BmxRecordingOwnedProcess $exe $identity.control.owned_pid $identity.created $owned.runtime_sha256
    $temporary=Join-Path $identity.directory ('recording-gate.ready.'+[Guid]::NewGuid().ToString('N')+'.tmp')
    try {
        $receipt="RESKATE_BMX_CAPTURE_READY_V1`n$($identity.control.gate_nonce)`n$($identity.control.owned_pid)`n$($identity.status.process_creation_filetime)`n"
        $bytes=[Text.UTF8Encoding]::new($false).GetBytes($receipt)
        $stream=[IO.File]::Open($temporary,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
        try{$stream.Write($bytes,0,$bytes.Length);$stream.Flush($true)}finally{$stream.Dispose()}
        # Final checks occur immediately before atomic no-overwrite publication.
        $current=Get-BmxRecordingWaitingIdentity $Workspace $ControlPath
        if ($current.control.gate_nonce -cne $identity.control.gate_nonce -or $current.control.owned_pid -ne $identity.control.owned_pid -or
            $current.status.process_creation_filetime -cne $identity.status.process_creation_filetime -or $current.ready -cne $identity.ready) { throw 'Recording control changed before acknowledgement.' }
        Assert-BmxRecordingOwnedProcess $exe $current.control.owned_pid $current.created $owned.runtime_sha256
        if ($last.ended -lt [DateTime]::UtcNow.AddSeconds(-15)) { throw 'Captured frame became stale before acknowledgement.' }
        [IO.File]::Move($temporary,$identity.ready)
    } finally { if(Test-Path -LiteralPath $temporary){Remove-Item -LiteralPath $temporary} }
    [ordered]@{schema_version=1;event='bmx_recording_acknowledged';acknowledged_utc=[DateTime]::UtcNow.ToString('o')
        process_id=$identity.control.owned_pid;process_creation_filetime=$identity.status.process_creation_filetime;nonce=$identity.control.gate_nonce
        ready_file=$identity.ready;capture_manifest=$manifestPath;first_frame=$first.file;last_frame=$last.file;pixel_quality_verified=$false;full_scenario_coverage_verified=$false} | ConvertTo-Json -Depth 5
}
Confirm-BmxRecording -ControlPath $ControlPath -CaptureManifestPath $CaptureManifestPath
