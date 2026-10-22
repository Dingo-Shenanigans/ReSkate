# Shared data-only recording barrier helpers. No screen capture or OS input.
function Assert-BmxCaptureGateConfiguration {
    param([string]$Repository,[string]$Directory,[string]$Nonce,[int]$TimeoutSeconds)
    if (-not $Directory -and -not $Nonce) { return }
    if (-not $Directory -or $Nonce -cnotmatch '^[0-9a-f]{32}$' -or $TimeoutSeconds -lt 30 -or $TimeoutSeconds -gt 600) {
        throw 'Capture gate requires a directory, a fresh 32-character lowercase hex nonce, and timeout 30..600 seconds.'
    }
    $gateRoot=[IO.Path]::GetFullPath((Join-Path $Repository 'build'))
    $gatePath=[IO.Path]::GetFullPath($Directory)
    if (-not $gatePath.StartsWith($gateRoot+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Capture gate must be a dedicated directory inside the repository build folder.' }
    if (-not (Test-Path -LiteralPath $gatePath -PathType Container)) { throw 'Create a fresh capture gate directory before starting the supervisor.' }
    $gateCursor=Get-Item -LiteralPath $gatePath
    while ($gateCursor.FullName.Length -ge $gateRoot.Length) {
        if (($gateCursor.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Capture gate cannot use a reparse-point directory.' }
        if ($gateCursor.FullName.Equals($gateRoot,[StringComparison]::OrdinalIgnoreCase)) { break }
        $gateCursor=$gateCursor.Parent
    }
    foreach ($gateName in @('recording-gate.ready','recording-gate.status.json','recording-gate.status.tmp')) {
        if (Test-Path -LiteralPath (Join-Path $gatePath $gateName)) { throw 'Capture gate files already exist; choose a fresh directory and nonce.' }
    }
}
# Data-only private coupled allowance. The existing no-Ride launcher retains
# empty startup commands; no legacy automated mode is coactivated for capture.
function Test-BmxCoupledCaptureGateAllowed {
    param([string]$Repository,[string]$ProfileName,[hashtable]$Options,[hashtable]$CoupledEnvironment)
    if([IO.Path]::GetFullPath($Repository) -cne 'C:\Users\Jeffr\Desktop\reskate' -or $ProfileName -cne 'bmx-user-data-coupled-test') {return $false}
    foreach($key in @('RESKATE_BMX_NATIVE_COUPLED','RESKATE_BMX_COUPLED_RIDE','RESKATE_BMX_COUPLED_CONTROLLER_TRIAL')) {
        if($CoupledEnvironment[$key] -cne '1') {return $false}
    }
    foreach($key in @('RESKATE_BMX_COUPLED_FORCE_PROBE','RESKATE_BMX_COUPLED_CALIBRATION')) {
        if([string]$CoupledEnvironment[$key] -cnotin @('','0')) {return $false}
    }
    foreach($key in @('Ride','NativeChassis','UnattendedTest','SwitchTest','QuickSwitchTest','NativeBoardControl','DeferNativeBoardArm',
        'GripProbe','GripRequestHud','IntentActivation','ImpactRecovery','GetUpPulse','PostFallRemountTest','PhaseProbe','FirstMountProducerProbe',
        'IntentObserver','BindingObserver','CatalogTest','NativeFallTest','ProfilePerformance','TrickTest','RampTest','RampRideTest','ChassisProbe','ChassisPulse',
        'ChassisCalibration','SmokeTest','NativeTestSkater','NativeStartupDelay','NativePlayerStartup','NativeObserveOnly','UseSteam','UseLooseFiles','DisableNativeLocalProfile')) {
        if($Options[$key]) {return $false}
    }
    return $Options.BoardHandoffMode -ceq 'None' -and -not $Options.ImpactTest -and
        $Options.RiderPoseMode -ceq 'Default' -and $Options.MotionBasisMode -ceq 'Default'
}
function Update-BmxCaptureGateControl {
    param([string]$Directory,[string]$Nonce,[int]$ProcessId,[Nullable[DateTime]]$CreatedUtc,[string]$ControlPath)
    if (-not $Directory) { return 'disabled' }
    $gateState='awaiting_native_world';$gateStatus=$null
    $gateStatusPath=Join-Path $Directory 'recording-gate.status.json'
    if (Test-Path -LiteralPath $gateStatusPath -PathType Leaf) {
        $gateStatus=Get-Content -LiteralPath $gateStatusPath -Raw | ConvertFrom-Json
        if ($gateStatus.schema_version -ne 1 -or $gateStatus.event -cne 'bmx_recording_startup_gate' -or
            $gateStatus.nonce -cne $Nonce -or $gateStatus.process_id -ne $ProcessId -or
            $gateStatus.process_creation_filetime -cnotmatch '^[1-9][0-9]{0,19}$' -or $gateStatus.state -cnotin @('waiting','released','timed_out')) {
            throw 'Capture gate status lacks the exact fresh owned process identity.'
        }
        # CIM creation can have reduced precision. Exact FILETIME comes from
        # native GetProcessTimes; compare its UTC instant within CIM precision.
        $gateCreated=[DateTime]::FromFileTimeUtc([Int64]::Parse($gateStatus.process_creation_filetime,[Globalization.CultureInfo]::InvariantCulture))
        if (-not $CreatedUtc -or [Math]::Abs(($gateCreated-[DateTime]$CreatedUtc).TotalMilliseconds) -gt 2) { throw 'Capture gate process creation disagrees with the owned process.' }
        $gateState=$gateStatus.state
    }
    $gateControl=[ordered]@{schema_version=1;event='bmx_recording_control';owned_pid=$ProcessId
        owned_created_utc=if($CreatedUtc){([DateTime]$CreatedUtc).ToUniversalTime().ToString('o')}else{$null}
        gate_directory=$Directory;gate_nonce=$Nonce;gate_status_file=$gateStatusPath;gate_ready_file=(Join-Path $Directory 'recording-gate.ready')
        gate_state=$gateState;process_creation_filetime=if($gateStatus){$gateStatus.process_creation_filetime}else{$null}}
    $gateText=$gateControl | ConvertTo-Json -Depth 5
    $gateOld=if(Test-Path -LiteralPath $ControlPath -PathType Leaf){Get-Content -LiteralPath $ControlPath -Raw}else{''}
    if ($gateText.Trim() -cne $gateOld.Trim()) {
        $gateTemp=$ControlPath+'.tmp'
        [IO.File]::WriteAllText($gateTemp,$gateText,[Text.UTF8Encoding]::new($false))
        Move-Item -LiteralPath $gateTemp -Destination $ControlPath -Force
    }
    return $gateState
}
