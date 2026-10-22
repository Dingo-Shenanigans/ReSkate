# Data-only selection/source-pin helpers. Dot-sourcing never launches a process,
# injects input, changes a profile, or mutates the caller's environment.
function Get-BmxBoardHandoffSelection {
    param([string]$Mode='None',[string]$RepoRoot,[string]$ProfileName,[hashtable]$Options=@{})
    if($Options.DeferNativeBoardArm -and ($Mode -cne 'NativeOnly' -or $Options.NativeBoardControl)){throw 'DeferNativeBoardArm requires passive NativeOnly without NativeBoardControl.'}
    if($Mode -ceq 'None') { if($Options.NativeBoardControl){throw 'NativeBoardControl requires explicit NativeOnly.'};return [pscustomobject]@{trace='0';mode='';native_control=$false;native_board_control='0'} }
    if($Mode -cne 'QuickSwitch' -and $Mode -cne 'NativeOnly'){throw 'Unknown board-handoff diagnostic mode.'}
    if([IO.Path]::GetFullPath($RepoRoot) -cne 'C:\Users\Jeffr\Desktop\reskate' -or $ProfileName -cne 'bmx-user-data-chassis-probe') {
        throw 'Board handoff requires the exact private workspace and chassis-probe profile.'
    }
    if(-not $Options.NativeChassis){throw 'Board handoff requires explicit NativeChassis.'}
    foreach($key in @('UnattendedTest','GripProbe','IntentActivation','ImpactRecovery','GetUpPulse','PostFallRemountTest','PhaseProbe','FirstMountProducerProbe','IntentObserver','BindingObserver','CatalogTest','NativeFallTest','ProfilePerformance','TrickTest','RampTest','RampRideTest','ChassisProbe','ChassisPulse','ChassisCalibration','SmokeTest','NativeTestSkater','NativeStartupDelay','NativePlayerStartup','NativeObserveOnly','UseSteam','UseLooseFiles','DisableNativeLocalProfile')) {
        if($Options[$key]){throw ('Board handoff conflicts with '+$key)}
    }
    foreach($key in @('RiderPoseMode','MotionBasisMode')) {
        if($Options.ContainsKey($key) -and $Options[$key] -cne 'Default'){throw ('Board handoff requires Default '+$key)}
    }
    if($Options.ImpactTest){throw 'Board handoff cannot include an impact test.'}
    if($Options.Map -and $Options.Map -cne 'MPR'){throw 'Board handoff requires the matched MPR venue.'}
    if($Options.NativeBoardControl -and $Mode -cne 'NativeOnly'){throw 'NativeBoardControl conflicts with every non-NativeOnly mode.'}
    if($Mode -ceq 'QuickSwitch') {
        if(-not $Options.SwitchTest -or -not $Options.QuickSwitchTest -or $Options.Ride){throw 'QuickSwitch handoff requires only SwitchTest and QuickSwitchTest.'}
        return [pscustomobject]@{trace='1';mode='quick-switch';native_control=$false;native_board_control='0'}
    }
    if(-not $Options.Ride -or $Options.SwitchTest -or $Options.QuickSwitchTest){throw 'NativeOnly handoff requires only Ride; the later Y mount is an active manual control.'}
    return [pscustomobject]@{trace='1';mode='native-only';native_control=$true;native_board_control=$(if($Options.NativeBoardControl){'1'}else{'0'});defer_native_board_arm=[bool]$Options.DeferNativeBoardArm}
}
function Set-BmxBoardHandoffEnvironment {
    param($Environment,$Selection)
    # Explicitly overwrite diagnostic keys even for ordinary/other launches.
    $Environment['RESKATE_BMX_BOARD_HANDOFF_TRACE']=[string]$Selection.trace
    $Environment['RESKATE_BMX_BOARD_HANDOFF_MODE']=[string]$Selection.mode
    $Environment['RESKATE_BMX_NATIVE_BOARD_CONTROL']=[string]$Selection.native_board_control
    $Environment['RESKATE_BMX_NATIVE_BOARD_DEFERRED_ARM']=$(if($Selection.defer_native_board_arm){'1'}else{'0'})
}
function Test-BmxBoardHandoffCaptureGateAllowed {
    param($Selection,[bool]$Ride,[bool]$UnattendedTest,[bool]$SwitchTest,[bool]$RampTest,[string]$ProfileName)
    if($Selection.native_control -and $Selection.trace -ceq '1' -and $Selection.mode -ceq 'native-only') {return $true}
    return -not $Ride -and ($UnattendedTest -or $SwitchTest -or $RampTest) -and
        ($ProfileName -ceq 'bmx-user-data-chassis-probe' -or $ProfileName -ceq 'bmx-user-data-ramp-test')
}
function Get-BmxBoardHandoffNativeControlCommands {
    param([bool]$AutomatedNativeBoard=$false,[bool]$DeferNativeBoardArm=$false)
    if($AutomatedNativeBoard -and $DeferNativeBoardArm){throw 'Deferred arming cannot schedule an active software native-board request.'}
    # Startup wait scheduling is specific to startup_commands, not the console.
    # Deferred startup queues only no-bail setup/status; console arm is explicit.
    if($DeferNativeBoardArm){return 'wait 15;nobail 0;wait 1;nobail;bmx handoff-trace status'}
    if($AutomatedNativeBoard){return 'wait 15;nobail 0;wait 1;nobail;bmx handoff-trace native-board;wait 1;bmx handoff-trace status'}
    # Passive arming only. Y/Triangle without LB must be a later explicit ACTIVE
    # control action. No BMX mount, native mount request, recovery or synthetic input.
    return 'wait 15;nobail 0;wait 1;nobail;bmx handoff-trace arm;wait 1;bmx handoff-trace status'
}
function Get-BmxBoardHandoffSourcePins {
    return @(
        'Extension\BMX\bmx_board_handoff_trace.h',
        'Extension\BMX\bmx_prop_transform_trace.h','Extension\BMX\bmx_world.cpp','Extension\BMX\bmx_world.h',
        'Extension\BMX\bmx_world_transform.h','Extension\Multiplayer\Remote\native_prop.cpp','Extension\Multiplayer\Remote\native_prop.h',
        'Extension\Skater\no_bail.cpp','Extension\Skater\no_bail.h',
        'Extension\BMX\bmx_native_motion.cpp','Extension\BMX\bmx_native_motion.h',
        'Engine\Game\BMX\bmx_board_handoff_policy.h',
        'Engine\Game\BMX\Test\bmx_board_handoff_policy_tests.cpp',
        'Extension\BMX\bmx_rider.cpp','Extension\BMX\bmx_rider.h',
        'Extension\BMX\bmx_runtime.cpp','Extension\BMX\bmx_commands.cpp',
        'Engine\Game\BMX\bmx_quick_switch_rehearsal.h','cmake\BmxTests.cmake',
        'contrib\bmx-lab\BoardHandoffEvidence.ps1',
        'contrib\bmx-lab\Test-BoardHandoffSelector.ps1',
        'contrib\bmx-lab\Test-BoardHandoffArchive.ps1',
        'contrib\bmx-lab\Start-BmxGame.ps1','contrib\bmx-lab\Run-BmxQuickSwitch.ps1'
    )
}

function Get-BmxBoardHandoffArchiveIdentity {
    param($Control,[int]$ProcessId,[DateTime]$CreatedUtc,[string]$GateDirectory,[string]$GateNonce)
    if($ProcessId -le 0 -or -not $GateDirectory -or $GateNonce -cnotmatch '^[0-9a-f]{32}$' -or
       $Control.schema_version -ne 1 -or $Control.event -cne 'bmx_recording_control' -or
       $Control.owned_pid -ne $ProcessId -or $Control.gate_state -cne 'released' -or
       $Control.gate_nonce -cne $GateNonce -or $Control.gate_directory -cne $GateDirectory -or
       $Control.process_creation_filetime -cnotmatch '^[1-9][0-9]{0,19}$') {
        throw 'Trace archive lacks the validated recording-control identity.'
    }
    # PowerShell7 ConvertFrom-Json already converts ISO dates to DateTime.
    # Casting that value to string drops fractional ticks in its display format.
    $controlCreated=if($Control.owned_created_utc -is [DateTime]){$Control.owned_created_utc}elseif($Control.owned_created_utc -is [DateTimeOffset]){
        $Control.owned_created_utc.UtcDateTime
    }else{
        [DateTime]::Parse([string]$Control.owned_created_utc,[Globalization.CultureInfo]::InvariantCulture,[Globalization.DateTimeStyles]::RoundtripKind)
    }
    if($controlCreated.ToUniversalTime().Ticks -ne $CreatedUtc.ToUniversalTime().Ticks){throw 'Trace control owner creation differs.'}
    # Preserve this exact native GetProcessTimes value. CIM is only the existing
    # <=2ms association check, never the source of an invented precise FILETIME.
    $exact=[string]$Control.process_creation_filetime
    $nativeCreated=[DateTime]::FromFileTimeUtc([Int64]::Parse($exact,[Globalization.CultureInfo]::InvariantCulture))
    if([Math]::Abs(($nativeCreated-$CreatedUtc.ToUniversalTime()).TotalMilliseconds) -gt 2){throw 'Trace native/CIM creation association differs.'}
    return $exact
}
function Test-BmxBoardHandoffArchiveBytes {
    param([byte[]]$Bytes,[int]$ProcessId,[string]$CreationFiletime,[ValidateSet(1,2)][int]$ExpectedMode=1)
    if(-not $Bytes.Length -or $Bytes.Length -gt 2MB){throw 'Trace final JSON exceeds the bounded byte size.'}
    $report=[Text.UTF8Encoding]::new($false,$true).GetString($Bytes) | ConvertFrom-Json
    if($report.schema -ne 1 -or $report.mode -ne $ExpectedMode -or $report.process_id -ne $ProcessId -or
       [string]$report.creation_filetime -cne $CreationFiletime -or
       $report.private_profile -cne 'bmx-user-data-chassis-probe' -or
       $report.private_root -cne 'C:\Users\Jeffr\Desktop\reskate\build\bmx-user-data-chassis-probe' -or
       $report.capacity -ne 512 -or $report.window_limit_ms -ne 8000 -or
       $null -eq $report.count -or $report.count -lt 0 -or $report.count -gt 512 -or
       @($report.rows).Count -ne $report.count -or -not ([string]$report.stop)) {
        throw 'Trace final JSON has mismatched identity, private scope or bounded header.'
    }
    return $report
}
function Write-BmxBoardHandoffArchiveBytes {
    param([byte[]]$Bytes,[string]$RunDirectory)
    if(-not $Bytes.Length -or $Bytes.Length -gt 2MB){throw 'Trace archive bytes exceed bound.'}
    $directory=Get-Item -LiteralPath $RunDirectory -ErrorAction Stop
    if(-not $directory.PSIsContainer -or ($directory.Attributes -band [IO.FileAttributes]::ReparsePoint)){throw 'Trace archive directory is unavailable or a reparse point.'}
    $path=Join-Path $directory.FullName 'bmx-board-handoff.json'
    $temporary=$path+'.'+[Guid]::NewGuid().ToString('N')+'.tmp'
    $stream=[IO.File]::Open($temporary,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
    try {$stream.Write($Bytes,0,$Bytes.Length);$stream.Flush($true)}finally{$stream.Dispose()}
    [IO.File]::Move($temporary,$path) # Atomic, no overwrite; retain temp on error.
    $hash=[Security.Cryptography.SHA256]::Create()
    try {$expected=([BitConverter]::ToString($hash.ComputeHash($Bytes))).Replace('-','')}finally{$hash.Dispose()}
    $actual=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    if($actual -cne $expected){throw 'Trace exact-byte archive hash differs.'}
    return [pscustomobject]@{path=$path;sha256=$actual;bytes=$Bytes.Length}
}
function Save-BmxBoardHandoffArchive {
    param([string]$ProfileDirectory,[string]$RunDirectory,$Control,[int]$ProcessId,[DateTime]$CreatedUtc,[string]$GateDirectory,[string]$GateNonce,[ValidateSet(1,2)][int]$ExpectedMode=1)
    $result=[ordered]@{present=$false;identity_verified=$false;archived=$false;archive_file=$null;sha256=$null;bytes=0;source_file=$null;creation_filetime=$null;stop=$null;issue='not_archived';coverage='not_assessed'}
    try {
        $exact=Get-BmxBoardHandoffArchiveIdentity $Control $ProcessId $CreatedUtc $GateDirectory $GateNonce
        $result.creation_filetime=$exact
        $profile=[IO.Path]::GetFullPath($ProfileDirectory)
        if($profile -cne 'C:\Users\Jeffr\Desktop\reskate\build\bmx-user-data-chassis-probe'){throw 'Trace source profile is not exact private root.'}
        $path=Join-Path $profile ('bmx-board-handoff-'+$ProcessId+'-'+$exact+'.json')
        $result.source_file=$path
        if(-not(Test-Path -LiteralPath $path -PathType Leaf)){$result.issue='missing_final_trace';return [pscustomobject]$result}
        $result.present=$true
        $item=Get-Item -LiteralPath $path
        if(($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $item.Length -le 0 -or $item.Length -gt 2MB){throw 'Trace final file is a reparse point or outside byte bound.'}
        $stream=[IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
        try {
            if($stream.Length -le 0 -or $stream.Length -gt 2MB){throw 'Trace final stream is outside byte bound.'}
            $bytes=[byte[]]::new([int]$stream.Length);$offset=0
            while($offset -lt $bytes.Length){$read=$stream.Read($bytes,$offset,$bytes.Length-$offset);if(-not $read){throw 'Trace final stream truncated.'};$offset+=$read}
            if($stream.ReadByte() -ne -1){throw 'Trace final stream grew.'}
        }finally{$stream.Dispose()}
        $report=Test-BmxBoardHandoffArchiveBytes $bytes $ProcessId $exact $ExpectedMode
        $result.identity_verified=$true;$result.stop=[string]$report.stop
        $archived=Write-BmxBoardHandoffArchiveBytes $bytes $RunDirectory
        $result.archived=$true;$result.archive_file=[IO.Path]::GetFileName($archived.path);$result.sha256=$archived.sha256;$result.bytes=$archived.bytes;$result.issue='archived_identity_only'
    } catch {$result.issue=$_.Exception.Message}
    return [pscustomobject]$result
}

# Acceptance for the separately selected ACTIVE software-board baseline only.
# Archival identity and retained CPU coverage are distinct; neither rates pixels.
function Test-BmxNativeBoardControlEvidence {
    param($Report)
    $uint={param($v) if($null -eq $v -or $v -is [bool] -or $v -is [string] -or $v -isnot [ValueType]){return $false};try{$n=[decimal]$v;return $n -ge 0 -and $n%1 -eq 0 -and $n -le [uint64]::MaxValue}catch{return $false}}
    $finite={param($v) if($null -eq $v -or $v -is [bool] -or $v -is [string] -or $v -isnot [ValueType]){return $false};try{$n=[double]$v;return -not [double]::IsNaN($n) -and -not [double]::IsInfinity($n) -and $n -ge 0}catch{return $false}}
    if($Report.mode -ne 2 -or $Report.stop -cne 'observed_board100_dwell_complete_not_a_pose_pass' -or $Report.contention_drops -ne 0){return $false}
    $a=$Report.active_control
    if($a.enabled -isnot [bool] -or -not $a.enabled -or $a.kind -cne 'existing_native_board_mount_request' -or
       $a.ordinary_Y_or_hardware_test -isnot [bool] -or $a.ordinary_Y_or_hardware_test -or
       -not (& $uint $a.attempts) -or $a.attempts -ne 1 -or $a.accepted -isnot [bool] -or -not $a.accepted -or
       $a.failed -isnot [bool] -or $a.failed -or $a.baseline_retained -isnot [bool] -or -not $a.baseline_retained){return $false}
    if(@($a.baseline_owner).Count -ne 6){return $false};foreach($v in $a.baseline_owner){if(-not (& $uint $v) -or $v -eq 0){return $false}}
    foreach($key in @('baseline_serial','baseline_wall_ms','request_serial','request_wall_ms')){if(-not (& $uint $a.$key) -or $a.$key -eq 0){return $false}}
    foreach($key in @('baseline_native_time','request_native_time')){if(-not (& $finite $a.$key)){return $false}}
    if($a.request_serial -le $a.baseline_serial -or $a.request_native_time -le $a.baseline_native_time -or $a.request_wall_ms -lt $a.baseline_wall_ms){return $false}
    $rows=@($Report.rows);if($rows.Count -lt 12 -or $rows.Count -gt 512 -or $Report.count -ne $rows.Count){return $false}
    $owner=($a.baseline_owner -join ',');$lastSerial=0;$lastNative=0.0;$lastWall=0
    foreach($r in $rows){
        if(-not (& $uint $r.kind) -or $r.kind -gt 2 -or -not (& $uint $r.serial) -or $r.serial -eq 0 -or
           -not (& $uint $r.wall_ms) -or -not (& $finite $r.native_time) -or -not (& $uint $r.physics) -or
           @($r.owner).Count -ne 6 -or ($r.owner -join ',') -cne $owner -or $r.same_owner_clock_after -isnot [bool] -or -not $r.same_owner_clock_after){return $false}
        if($r.serial -lt $lastSerial -or $r.native_time -lt $lastNative -or $r.wall_ms -lt $lastWall -or
           ($lastSerial -and (($r.serial -eq $lastSerial) -ne ($r.native_time -eq $lastNative)))){return $false}
        $lastSerial=$r.serial;$lastNative=$r.native_time;$lastWall=$r.wall_ms
        foreach($key in @('client_motion_updates','client_pose_updates','client_render_updates','client_board_hide_updates','chassis_allocations','chassis_mounts','parts')){if(-not (& $uint $r.$key) -or $r.$key -ne 0){return $false}}
        foreach($key in @('capture','lease_active','bmx_active','board_pending','stepping_off')){if($r.$key -isnot [bool] -or $r.$key){return $false}}
        if($r.native_control_input_allowed -isnot [bool] -or -not $r.native_control_input_allowed -or $r.physics -notin @(504,200,201,100)){return $false}
        if($r.kind -ne 0){
            foreach($key in @('publication_pose_updates','publication_render_updates','custom_board_hide_updates')){if(-not (& $uint $r.$key) -or $r.$key -ne 0){return $false}}
            if($r.sdk_pose_publication_active -isnot [bool] -or $r.sdk_pose_publication_active -or $r.sdk_pose_publication_entity -ne 0 -or
               $r.published_pointer_matches -isnot [bool] -or -not $r.published_pointer_matches){return $false}
            foreach($key in @('holder','buffer','render_data')){if(-not (& $uint $r.$key) -or $r.$key -eq 0){return $false}}
            if(($r.kind -eq 1 -and $r.bone_count -ne 395) -or ($r.kind -eq 2 -and $r.bone_count -ne 16)){return $false}
        }
    }
    $clients=@($rows|Where-Object{$_.kind -eq 0})
    $baseline=@($clients|Where-Object{$_.serial -eq $a.baseline_serial -and $_.native_time -eq $a.baseline_native_time -and $_.wall_ms -eq $a.baseline_wall_ms})
    $request=@($clients|Where-Object{$_.serial -eq $a.request_serial -and $_.native_time -eq $a.request_native_time -and $_.wall_ms -eq $a.request_wall_ms})
    if($baseline.Count -ne 1 -or $request.Count -ne 1){return $false}
    foreach($r in @($baseline[0],$request[0])){if($r.physics -ne 504 -or $r.offboard -isnot [bool] -or -not $r.offboard -or
        $r.standable -isnot [bool] -or -not $r.standable -or $r.motion_supported -isnot [bool] -or -not $r.motion_supported){return $false}}
    foreach($r in $clients){
        if($r.serial -lt $a.request_serial -and $r.physics -ne 504){return $false}
        if($r.physics -eq 504 -and ($r.serial -gt $a.request_serial) -and
           ($r.wall_ms-$a.request_wall_ms -ge 250 -or $r.native_time-$a.request_native_time -ge .5 -or -not $r.offboard -or -not $r.standable -or -not $r.motion_supported)){return $false}
        if($r.physics -in @(100,200,201) -and ($r.offboard -isnot [bool] -or $r.offboard)){return $false}
    }
    foreach($kind in 0,1,2){
        $group=@($rows|Where-Object{$_.kind -eq $kind});if($group.Count -lt 4){return $false}
        $ground=@($group|Where-Object{$_.physics -eq 100});if($ground.Count -lt 20){return $false}
        $first=$ground[0];$last=$ground[-1]
        if($first.serial -le $a.request_serial -or $last.native_time-$first.native_time -lt 2.25 -or $last.wall_ms-$first.wall_ms -lt 2250){return $false}
        $previous=$null;foreach($r in $group){
            if($previous -and ($r.serial -le $previous.serial -or $r.native_time -le $previous.native_time -or $r.wall_ms-$previous.wall_ms -lt 50 -or $r.wall_ms-$previous.wall_ms -gt 250)){return $false}
            if($r.serial -ge $first.serial -and $r.physics -ne 100){return $false};$previous=$r
        }
        if($kind -ne 0 -and @($group|ForEach-Object{[string]$_.holder+'/'+$_.buffer+'/'+$_.render_data+'/'+$_.bone_count}|Sort-Object -Unique).Count -ne 1){return $false}
    }
    return $true
}
