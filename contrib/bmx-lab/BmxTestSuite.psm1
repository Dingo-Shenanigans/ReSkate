Set-StrictMode -Version Latest

# The suite orchestrates existing private native rehearsals. It never sends OS
# input, records a desktop, integrates physics time, or kills a process itself.
function Get-BmxSuiteProperty($Object, [string]$Name) {
    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -ne $property) { return $property.Value }
    return $null
}

function Test-BmxSuiteInteger($Value, [long]$Minimum = 0, [long]$Maximum = [int]::MaxValue) {
    return ($Value -is [int] -or $Value -is [long] -or $Value -is [double] -or $Value -is [decimal]) -and
        -not [double]::IsNaN([double]$Value) -and -not [double]::IsInfinity([double]$Value) -and
        $Value -ge $Minimum -and $Value -le $Maximum -and
        [math]::Truncate([double]$Value) -eq [double]$Value
}

function ConvertTo-BmxSuiteUtc($Value) {
    if ($Value -isnot [string] -or $Value -notmatch '(Z|\+00:00)$') { throw 'Expected an explicit UTC timestamp.' }
    return [DateTimeOffset]::Parse($Value, [Globalization.CultureInfo]::InvariantCulture).UtcDateTime
}

function Read-BmxSuiteJson([string]$Path) {
    $jsonArguments = @{ ErrorAction='Stop' }
    # PowerShell 7.5+ otherwise converts timestamps to DateTime and loses the
    # original explicit UTC spelling required by the evidence contract.
    if ((Get-Command ConvertFrom-Json).Parameters.ContainsKey('DateKind')) { $jsonArguments.DateKind='String' }
    $value = Get-Content -LiteralPath $Path -Raw -ErrorAction Stop | ConvertFrom-Json @jsonArguments
    if ($null -eq $value -or $value -isnot [pscustomobject]) { throw ('Expected one JSON object: ' + $Path) }
    return $value
}

function Get-BmxSuiteWorkspacePath([string]$RepoRoot, [string]$Path) {
    $root = [IO.Path]::GetFullPath($RepoRoot).TrimEnd('\', '/')
    $full = if ([IO.Path]::IsPathRooted($Path)) { [IO.Path]::GetFullPath($Path) } else { [IO.Path]::GetFullPath((Join-Path $root $Path)) }
    if (-not $full.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw ('Suite path must stay inside the repository: ' + $Path)
    }
    # Refuse junctions/symlinks in existing ancestors before copying evidence or
    # creating output. A lexical prefix alone does not constrain a junction.
    $cursor = $full
    while ($cursor.Length -gt $root.Length) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force -ErrorAction Stop
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw ('Reparse path is not supported: ' + $cursor) }
        }
        $cursor = [IO.Path]::GetDirectoryName($cursor)
    }
    return $full
}

function Get-BmxSuiteDefinitions {
    @(
        [pscustomobject]@{ name='Flat'; supervisor='build/bmx/run_native_chassis_ride.ps1'; flags=@(); profile='bmx-user-data-chassis-probe'; archive_prefix='chassis-ride-run'; outcome='selftest_passed'; coverage='Pedal, steer, brake and hop through the BMX mapper and native chassis.' },
        [pscustomobject]@{ name='Tricks'; supervisor='build/bmx/run_native_chassis_ride.ps1'; flags=@('Tricks'); profile='bmx-user-data-chassis-probe'; archive_prefix='chassis-ride-run'; outcome='selftest_passed'; coverage='Authored barspin/tailwhip combo and short pitch response.' },
        [pscustomobject]@{ name='Catalog'; supervisor='build/bmx/run_native_chassis_ride.ps1'; flags=@('Tricks','Catalog'); profile='bmx-user-data-chassis-probe'; archive_prefix='chassis-ride-run'; outcome='selftest_passed'; coverage='One authored Superman catalog gesture, not the whole catalog.' },
        [pscustomobject]@{ name='Ramp'; supervisor='build/bmx/run_native_ramp_probe.ps1'; flags=@('Ride'); profile='bmx-user-data-ramp-test'; archive_prefix='native-ramp-run'; outcome='native_ramp_ride_passed'; coverage='Prepared measured ramp, natural flight, supported landing and braking.' },
        [pscustomobject]@{ name='Equipment'; supervisor='build/bmx/run_native_chassis_ride.ps1'; flags=@('Switch'); profile='bmx-user-data-chassis-probe'; archive_prefix='chassis-ride-run'; outcome='native_chassis_switch_passed'; coverage='Console-requested bike/board, dismount, carry, setdown and remount with neutral BMX input.' },
        [pscustomobject]@{ name='FallSide'; supervisor='build/bmx/run_native_getup_test.ps1'; flags=@(); variant='side'; profile='bmx-user-data-chassis-probe'; archive_prefix='native-getup-run'; outcome='native_getup_lab_passed'; coverage='Private side fall and typed GetUp pulse/reset; ordinary controller GetUp and pickup/remount are not tested.' },
        [pscustomobject]@{ name='FallInverted'; supervisor='build/bmx/run_native_getup_test.ps1'; flags=@(); variant='inverted'; profile='bmx-user-data-chassis-probe'; archive_prefix='native-getup-run'; outcome='native_getup_lab_passed'; coverage='Private inverted fall and typed GetUp pulse/reset; ordinary controller GetUp and pickup/remount are not tested.' }
    )
}

function Get-BmxSuitePlan {
    param([string]$RepoRoot, [string[]]$Scenario = @('Flat'), [ValidateRange(1,10)][int]$Repeat = 1,
        [ValidateRange(148,1000)][int]$FirstRunNumber = 148, [string]$OutputDirectory = 'build/bmx/test-suites',
        [string]$BuildLog = '', [string]$CaptureReceiptDirectory = '', [bool]$RequireCapture = $false,
        [bool]$WaitForRecording = $false, [ValidateRange(30,600)][int]$CaptureGateTimeoutSeconds = 180,
        [ValidateRange(0,300)][int]$CaptureReceiptTimeoutSeconds = 0)
    $definitions = @(Get-BmxSuiteDefinitions)
    if ($Scenario.Count -eq 0) { throw 'Choose at least one scenario.' }
    if ($Scenario -contains 'All') {
        if ($Scenario.Count -ne 1) { throw 'All cannot be combined with individual scenarios.' }
        $Scenario = @($definitions | ForEach-Object { $_.name })
    }
    foreach ($name in $Scenario) { if ($name -notin $definitions.name) { throw ('Unknown BMX scenario: ' + $name) } }
    $outputRoot = Get-BmxSuiteWorkspacePath $RepoRoot $OutputDirectory
    $suiteId = 'suite-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8)
    $suitePath = Get-BmxSuiteWorkspacePath $RepoRoot (Join-Path $outputRoot $suiteId)
    $receiptRoot = if ($CaptureReceiptDirectory) { Get-BmxSuiteWorkspacePath $RepoRoot $CaptureReceiptDirectory } else { $null }
    $buildPath = if ($BuildLog) { Get-BmxSuiteWorkspacePath $RepoRoot $BuildLog } else { $null }
    $steps = @(); $nextRun = $FirstRunNumber
    for ($iteration=1; $iteration -le $Repeat; ++$iteration) {
        foreach ($name in $Scenario) {
            $definition = @($definitions | Where-Object { $_.name -eq $name })[0]
            do {
                if ($nextRun -gt 1000) { throw 'No unused supervisor run number remains below 1001.' }
                $occupied = $false
                foreach ($prefix in @('chassis-ride-run','native-ramp-run','native-getup-run','native-impact-run','native-fall-observation-run')) {
                    if (Test-Path -LiteralPath (Join-Path $RepoRoot ('build/bmx/' + $prefix + $nextRun))) { $occupied=$true; break }
                }
                $candidate = $nextRun; ++$nextRun
            } while ($occupied)
            $steps += [pscustomobject]@{
                scenario=$name; iteration=$iteration; run_number=$candidate
                supervisor=(Get-BmxSuiteWorkspacePath $RepoRoot $definition.supervisor)
                flags=@($definition.flags); variant=(Get-BmxSuiteProperty $definition 'variant')
                profile=(Get-BmxSuiteWorkspacePath $RepoRoot ('build/' + $definition.profile))
                archive=(Get-BmxSuiteWorkspacePath $RepoRoot ('build/bmx/' + $definition.archive_prefix + $candidate))
                accepted_outcome=$definition.outcome; coverage=$definition.coverage
                capture_receipt_name=($name + '-' + $candidate + '.capture.json')
                capture_gate_directory=$(if ($WaitForRecording) { Join-Path (Join-Path $suitePath ($name + '-' + $candidate)) 'recording-gate' } else { $null })
                capture_gate_nonce=$(if ($WaitForRecording) { [Guid]::NewGuid().ToString('N') } else { $null })
                supervisor_control=(Get-BmxSuiteWorkspacePath $RepoRoot ('build/bmx/' + $definition.archive_prefix + $candidate + '/control.json'))
            }
        }
    }
    return [pscustomobject]@{
        schema=1; event='bmx_test_suite_plan'; suite_id=$suiteId; repo_root=[IO.Path]::GetFullPath($RepoRoot)
        output_directory=$suitePath; build_log=$buildPath
        # Existing supervisors resolve BuildLog with Join-Path repo argument;
        # PowerShell Join-Path does not replace its parent for rooted children.
        build_log_argument=$(if ($buildPath) { $buildPath.Substring([IO.Path]::GetFullPath($RepoRoot).TrimEnd('\','/').Length + 1) } else { $null })
        capture_receipt_directory=$receiptRoot
        require_capture=$RequireCapture; recording_implemented_by_runner=$false; os_input_injected=$false
        wait_for_recording=$WaitForRecording; capture_gate_timeout_seconds=$CaptureGateTimeoutSeconds
        capture_receipt_timeout_seconds=$CaptureReceiptTimeoutSeconds
        game_exe=(Get-BmxSuiteWorkspacePath $RepoRoot 'build/bmx-game/Skate.exe')
        runtime=(Get-BmxSuiteWorkspacePath $RepoRoot 'build/vs2022-x64/Release/ReSkate.dll')
        launcher=(Get-BmxSuiteWorkspacePath $RepoRoot 'build/vs2022-x64/Release/ReSkateLauncher.exe')
        steps=$steps
        boundary='Synthetic game-side BMX input and console lifecycle requests. Physical controller delivery, full terrain/trick coverage and visual quality are separate checks. A capture receipt is metadata evidence, not a visual acceptance score.'
    }
}

function Test-BmxSuiteRun {
    param($Run, $Step, $Expected, [DateTime]$InvocationStartedUtc, [DateTime]$InvocationEndedUtc)
    $issues = [Collections.Generic.List[string]]::new()
    try {
        if ($null -eq $Run -or $Run -isnot [pscustomobject]) { throw 'Missing run JSON object.' }
        if (-not (Test-BmxSuiteInteger (Get-BmxSuiteProperty $Run 'owned_pid') 1)) { throw 'Missing owned PID.' }
        foreach ($field in @('process_teardown_verified','source_unchanged_after_run')) {
            if ((Get-BmxSuiteProperty $Run $field) -isnot [bool] -or $Run.$field -ne $true) { $issues.Add($field + ' must be boolean true.') }
        }
        if (-not (Test-BmxSuiteInteger (Get-BmxSuiteProperty $Run 'teardown_absence_observations') 2)) { $issues.Add('Two successful consecutive absence observations are required.') }
        if ((Get-BmxSuiteProperty $Run 'outcome') -cne $Step.accepted_outcome) { $issues.Add('Scenario outcome did not pass: ' + (Get-BmxSuiteProperty $Run 'outcome')) }
        foreach ($pair in @(@('game_exe',$Expected.game_exe),@('profile',$Step.profile))) {
            $value = Get-BmxSuiteProperty $Run $pair[0]
            if ($value -isnot [string] -or -not [IO.Path]::GetFullPath($value).Equals($pair[1],[StringComparison]::OrdinalIgnoreCase)) { $issues.Add('Wrong ' + $pair[0] + '.') }
        }
        foreach ($field in @('runtime_sha256','launcher_sha256','game_sha256')) {
            $value = Get-BmxSuiteProperty $Run $field
            if ($value -isnot [string] -or $value -notmatch '^[0-9a-fA-F]{64}$' -or $value -ine $Expected.$field) { $issues.Add('Wrong ' + $field + '.') }
        }
        $started = ConvertTo-BmxSuiteUtc (Get-BmxSuiteProperty $Run 'started_utc')
        $ended = ConvertTo-BmxSuiteUtc (Get-BmxSuiteProperty $Run 'ended_utc')
        $created = ConvertTo-BmxSuiteUtc (Get-BmxSuiteProperty $Run 'owned_created_utc')
        if ($started -lt $InvocationStartedUtc.AddSeconds(-1) -or $ended -gt $InvocationEndedUtc.AddSeconds(1) -or
            $ended -lt $started -or $created -lt $started.AddSeconds(-1) -or $created -gt $ended) { $issues.Add('Run identity/timestamps are stale or inconsistent.') }
        $required = switch ($Step.scenario) {
            'Equipment' { @('lifecycle_report_passed') }
            'Ramp' { @('completed','prepared','report_identity_verified','ramp_ride','ride_passed') }
            { $_ -in @('FallSide','FallInverted') } { @('report_passed','native_fall_passed','pulse_passed','standability_observed','passed') }
            default { @('report_completed','report_passed') }
        }
        if ($Step.scenario -eq 'Tricks') { $required += 'trick_report_passed' }
        if ($Step.scenario -eq 'Catalog') { $required += 'catalog_report_passed' }
        foreach ($field in $required) {
            if ((Get-BmxSuiteProperty $Run $field) -isnot [bool] -or $Run.$field -ne $true) { $issues.Add($field + ' must be boolean true.') }
        }
        if ($Step.scenario -notin @('Equipment','Ramp') -and (Get-BmxSuiteProperty $Run 'report_process_identity') -cne 'verified') { $issues.Add('Terminal report PID must be verified.') }
        if ($Step.scenario -in @('FallSide','FallInverted') -and (Get-BmxSuiteProperty $Run 'impact_variant') -cne $Step.variant) { $issues.Add('Wrong fall variant.') }
    } catch { $issues.Add($_.Exception.Message) }
    return [pscustomobject]@{ passed=($issues.Count -eq 0); issues=@($issues.ToArray()) }
}

function Test-BmxSuiteTeardown($Run) {
    return $null -ne $Run -and $Run -is [pscustomobject] -and
        (Test-BmxSuiteInteger (Get-BmxSuiteProperty $Run 'owned_pid') 1) -and
        (Get-BmxSuiteProperty $Run 'process_teardown_verified') -is [bool] -and $Run.process_teardown_verified -eq $true -and
        (Test-BmxSuiteInteger (Get-BmxSuiteProperty $Run 'teardown_absence_observations') 2)
}

function Get-BmxSuitePrimaryReportPath($Step) {
    $name = switch ($Step.scenario) {
        'Equipment' { 'switch-evidence.json' }
        # The native action receipt is written after the full fall window.
        # The supervisor's final aggregate is written after game teardown and
        # cannot be a genuine game-window capture boundary.
        { $_ -in @('FallSide','FallInverted') } { 'bmx-getup-action-test.json' }
        default { 'bmx-selftest.json' }
    }
    return Join-Path $Step.archive $name
}

function Test-BmxSuiteRecordingGate {
    param($Status, $Control, $Step, $Run)
    $issues=[Collections.Generic.List[string]]::new(); $released=$null
    try {
        if ($null -eq $Status -or $null -eq $Control -or -not $Step.capture_gate_directory) { throw 'Gated recording status/control is unavailable.' }
        foreach ($field in @('recording_gate_enabled','capture_trace_settle_completed')) {
            if ((Get-BmxSuiteProperty $Run $field) -isnot [bool] -or $Run.$field -ne $true) { throw ($field + ' must be boolean true.') }
        }
        if ((Get-BmxSuiteProperty $Run 'recording_gate_state') -cne 'released' -or
            (Get-BmxSuiteProperty $Run 'recording_gate_nonce') -cne $Step.capture_gate_nonce -or
            (Get-BmxSuiteProperty $Run 'recording_gate_directory') -cne $Step.capture_gate_directory) { throw 'Native run gate state/nonce/directory mismatch.' }
        if (-not (Test-BmxSuiteInteger (Get-BmxSuiteProperty $Status 'schema_version') 1 1) -or
            (Get-BmxSuiteProperty $Status 'event') -cne 'bmx_recording_startup_gate' -or
            (Get-BmxSuiteProperty $Status 'state') -cne 'released' -or
            (Get-BmxSuiteProperty $Status 'nonce') -cne $Step.capture_gate_nonce -or
            -not (Test-BmxSuiteInteger (Get-BmxSuiteProperty $Status 'process_id') 1) -or $Status.process_id -ne $Run.owned_pid) { throw 'Native gate status identity/state mismatch.' }
        foreach ($pair in @(@('gate_directory',$Step.capture_gate_directory),@('gate_nonce',$Step.capture_gate_nonce),
            @('gate_status_file',(Join-Path $Step.capture_gate_directory 'recording-gate.status.json')),
            @('gate_ready_file',(Join-Path $Step.capture_gate_directory 'recording-gate.ready')),@('gate_state','released'))) {
            if ((Get-BmxSuiteProperty $Control $pair[0]) -cne $pair[1]) { throw ('Supervisor control mismatch: ' + $pair[0]) }
        }
        if (-not (Test-BmxSuiteInteger (Get-BmxSuiteProperty $Control 'owned_pid') 1) -or $Control.owned_pid -ne $Run.owned_pid -or
            (ConvertTo-BmxSuiteUtc (Get-BmxSuiteProperty $Control 'owned_created_utc')) -ne (ConvertTo-BmxSuiteUtc $Run.owned_created_utc)) { throw 'Supervisor control owned identity mismatch.' }
        $fileTime=Get-BmxSuiteProperty $Status 'process_creation_filetime'
        if ($fileTime -isnot [string] -or $fileTime -notmatch '^\d{1,19}$' -or
            (Get-BmxSuiteProperty $Control 'process_creation_filetime') -cne $fileTime) { throw 'Native exact creation FILETIME mismatch.' }
        $nativeCreated=[DateTime]::FromFileTimeUtc([long]::Parse($fileTime,[Globalization.CultureInfo]::InvariantCulture))
        # CIM can truncate native FILETIME to microseconds. Exact native text is
        # compared above; this bounded cross-check does not fabricate that text.
        if ([math]::Abs(($nativeCreated-(ConvertTo-BmxSuiteUtc $Run.owned_created_utc)).Ticks) -gt 10) { throw 'Native gate creation differs from supervisor process creation.' }
        $released=ConvertTo-BmxSuiteUtc (Get-BmxSuiteProperty $Status 'observed_utc')
        if ($released -lt $nativeCreated -or $released -gt (ConvertTo-BmxSuiteUtc $Run.ended_utc) -or
            -not (Test-BmxSuiteInteger (Get-BmxSuiteProperty $Status 'elapsed_ms') 0 600000)) { throw 'Native gate release time is outside this run.' }
    } catch { $issues.Add($_.Exception.Message) }
    return [pscustomobject]@{ passed=($issues.Count -eq 0); issues=@($issues.ToArray()); release_utc=$(if ($released) { $released.ToString('o') } else { $null }) }
}

function Assert-BmxSuiteArchive {
    param([string]$RepoRoot, $Step, $Run)
    $manifestPath = Get-BmxSuiteWorkspacePath $RepoRoot (Join-Path $Step.archive 'source-manifest.json')
    $manifest = @(Get-Content -LiteralPath $manifestPath -Raw -ErrorAction Stop | ConvertFrom-Json -ErrorAction Stop)
    if ($manifest.Count -eq 0) { throw 'Empty source manifest.' }
    foreach ($entry in $manifest) {
        if ($entry.path -isnot [string] -or $entry.sha256 -notmatch '^[a-fA-F0-9]{64}$') { throw 'Invalid source manifest entry.' }
        $live = Get-BmxSuiteWorkspacePath $RepoRoot $entry.path
        $archived = Get-BmxSuiteWorkspacePath $RepoRoot (Join-Path (Join-Path $Step.archive 'source') $entry.path)
        if (-not $archived.StartsWith((Join-Path $Step.archive 'source') + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Archived source escaped its run.' }
        if ((Get-FileHash -LiteralPath $live -Algorithm SHA256).Hash -ine $entry.sha256 -or
            (Get-FileHash -LiteralPath $archived -Algorithm SHA256).Hash -ine $entry.sha256) { throw ('Source archive/live mismatch: ' + $entry.path) }
    }
    foreach ($pair in @(@('ReSkate.dll','runtime_sha256'),@('ReSkateLauncher.exe','launcher_sha256'))) {
        $path = Get-BmxSuiteWorkspacePath $RepoRoot (Join-Path $Step.archive $pair[0])
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $Run.($pair[1])) { throw 'Archived native artifact hash mismatch.' }
    }
    if ($Step.scenario -eq 'Equipment') {
        $primary = Read-BmxSuiteJson (Join-Path $Step.archive 'bmx-chassis-lifecycle.json')
        $switch = Read-BmxSuiteJson (Join-Path $Step.archive 'switch-evidence.json')
        foreach ($field in @('complete','all_phases','scoped_native_board_completion','pause_pass_logged')) {
            if ((Get-BmxSuiteProperty $switch $field) -isnot [bool] -or -not $switch.$field) { throw ('Missing ordered equipment evidence: ' + $field) }
        }
    } elseif ($Step.scenario -in @('FallSide','FallInverted')) {
        $primary = Read-BmxSuiteJson (Join-Path $Step.archive 'bmx-native-getup-lab-test.json')
        if ($primary.event -cne 'bmx_native_getup_lab_test' -or $primary.runtime_sha256 -ine $Run.runtime_sha256 -or
            (ConvertTo-BmxSuiteUtc $primary.owned_created_utc) -ne (ConvertTo-BmxSuiteUtc $Run.owned_created_utc)) { throw 'Private GetUp evidence identity mismatch.' }
    } else {
        $primary = Read-BmxSuiteJson (Join-Path $Step.archive 'bmx-selftest.json')
        if ((Get-BmxSuiteProperty $primary 'physical_chassis_mode') -isnot [bool] -or -not $primary.physical_chassis_mode -or
            (Get-BmxSuiteProperty $primary 'physical_chassis_passed') -isnot [bool] -or -not $primary.physical_chassis_passed) { throw 'Physical chassis evidence missing.' }
        if ($Step.scenario -eq 'Ramp') {
            foreach ($field in @('ramp_rehearsal','ramp_passed')) { if ((Get-BmxSuiteProperty $primary $field) -isnot [bool] -or -not $primary.$field) { throw 'Ramp runtime evidence missing.' } }
        }
    }
    if (-not (Test-BmxSuiteInteger (Get-BmxSuiteProperty $primary 'process_id') 1) -or $primary.process_id -ne $Run.owned_pid) { throw 'Archived terminal report PID mismatch.' }
    foreach ($field in @('completed','passed')) { if ((Get-BmxSuiteProperty $primary $field) -isnot [bool] -or -not $primary.$field) { throw 'Archived terminal report did not complete/pass.' } }
}

function Copy-BmxSuiteTraces {
    param([string]$RepoRoot, $Step, $Run, [string]$Destination)
    $artifacts = @(); $rejected = @()
    $traceRoot = Get-BmxSuiteWorkspacePath $RepoRoot (Join-Path $Step.profile 'bmx-traces')
    $traceDestination = Get-BmxSuiteWorkspacePath $RepoRoot (Join-Path $Destination 'bmx-traces')
    if (-not (Test-Path -LiteralPath $traceRoot)) { return [pscustomobject]@{ artifacts=@(); rejected=@() } }
    $earliest = ConvertTo-BmxSuiteUtc $Run.owned_created_utc
    $latest = ConvertTo-BmxSuiteUtc $Run.ended_utc
    foreach ($file in @(Get-ChildItem -LiteralPath $traceRoot -File -Filter '*.json')) {
        if ($file.Name -notmatch ('^(ride|fall)-' + [regex]::Escape([string]$Run.owned_pid) + '-\d+\.json$')) { continue }
        try {
            if ($file.LastWriteTimeUtc -lt $earliest -or $file.LastWriteTimeUtc -gt $latest) { throw 'Trace timestamp is outside this process lifetime.' }
            $sourcePath = Get-BmxSuiteWorkspacePath $RepoRoot $file.FullName
            $sourceHash = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash
            $trace = Read-BmxSuiteJson $sourcePath
            if (-not (Test-BmxSuiteInteger (Get-BmxSuiteProperty $trace 'process_id') 1) -or $trace.process_id -ne $Run.owned_pid) { throw 'Trace PID mismatch.' }
            if ($file.Name.StartsWith('fall-') -and (Get-BmxSuiteProperty $trace 'event') -cne 'bmx_passive_fall_lifecycle') { throw 'Fall trace event mismatch.' }
            if (-not (Test-Path -LiteralPath $traceDestination)) { New-Item -ItemType Directory -Path $traceDestination -ErrorAction Stop | Out-Null }
            $copyPath = Join-Path $traceDestination $file.Name
            Copy-Item -LiteralPath $sourcePath -Destination $copyPath -ErrorAction Stop
            if ((Get-FileHash -LiteralPath $copyPath -Algorithm SHA256).Hash -ine $sourceHash -or
                (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash -ine $sourceHash) { throw 'Trace changed during archival.' }
            $artifacts += [pscustomobject]@{ path=('bmx-traces/' + $file.Name); sha256=$sourceHash; bytes=$file.Length; source_modified_utc=$file.LastWriteTimeUtc.ToString('o'); process_id=$Run.owned_pid }
        } catch { $rejected += [pscustomobject]@{ file=$file.Name; reason=$_.Exception.Message } }
    }
    return [pscustomobject]@{ artifacts=$artifacts; rejected=$rejected }
}

function Test-BmxSuiteCaptureReceipt {
    param($Receipt, $Step, $Run, [string]$RepoRoot, $CoverageContext = $null)
    $issues = [Collections.Generic.List[string]]::new(); $video = $null; $started=$null; $ended=$null
    try {
        if ($null -eq $Receipt -or $Receipt -isnot [pscustomobject]) { throw 'No capture receipt was supplied.' }
        if (-not (Test-BmxSuiteInteger (Get-BmxSuiteProperty $Receipt 'schema') 1 1)) { throw 'Capture receipt schema must be numeric 1.' }
        if (-not (Test-BmxSuiteInteger (Get-BmxSuiteProperty $Receipt 'run_number') 148 1000) -or $Receipt.run_number -ne $Step.run_number -or
            (Get-BmxSuiteProperty $Receipt 'scenario') -cne $Step.scenario) { throw 'Capture receipt run/scenario mismatch.' }
        if (-not (Test-BmxSuiteInteger (Get-BmxSuiteProperty $Receipt 'process_id') 1) -or $Receipt.process_id -ne $Run.owned_pid) { throw 'Capture receipt PID mismatch.' }
        if ((ConvertTo-BmxSuiteUtc (Get-BmxSuiteProperty $Receipt 'process_created_utc')) -ne (ConvertTo-BmxSuiteUtc $Run.owned_created_utc)) { throw 'Capture receipt process creation mismatch.' }
        if ((Get-BmxSuiteProperty $Receipt 'runtime_sha256') -isnot [string] -or $Receipt.runtime_sha256 -ine $Run.runtime_sha256) { throw 'Capture receipt runtime mismatch.' }
        if ((Get-BmxSuiteProperty $Receipt 'game_exe') -isnot [string] -or -not [IO.Path]::GetFullPath($Receipt.game_exe).Equals($Run.game_exe,[StringComparison]::OrdinalIgnoreCase)) { throw 'Capture receipt executable mismatch.' }
        $started = ConvertTo-BmxSuiteUtc (Get-BmxSuiteProperty $Receipt 'capture_started_utc')
        $ended = ConvertTo-BmxSuiteUtc (Get-BmxSuiteProperty $Receipt 'capture_ended_utc')
        $runStart = ConvertTo-BmxSuiteUtc $Run.owned_created_utc; $runEnd = ConvertTo-BmxSuiteUtc $Run.ended_utc
        if ($ended -le $started -or $started -lt $runStart.AddSeconds(-1) -or $ended -gt $runEnd.AddSeconds(5) -or $started -ge $runEnd -or $ended -le $runStart) { throw 'Capture interval does not match this process lifetime.' }
        if ((Get-BmxSuiteProperty $Receipt 'capture_method') -isnot [string] -or [string]::IsNullOrWhiteSpace($Receipt.capture_method)) { throw 'Capture method must be named.' }
        if ((Get-BmxSuiteProperty $Receipt 'video_path') -isnot [string] -or -not [IO.Path]::IsPathRooted($Receipt.video_path)) { throw 'Capture video path must be absolute.' }
        $video = Get-BmxSuiteWorkspacePath $RepoRoot $Receipt.video_path
        if ([IO.Path]::GetExtension($video) -notin @('.mp4','.mkv','.webm','.avi','.mov')) { throw 'Unsupported capture video extension.' }
        $info = Get-Item -LiteralPath $video -ErrorAction Stop
        if ($info.PSIsContainer -or $info.Length -le 0) { throw 'Capture video is empty or missing.' }
        # Encoding sampled PNGs may finish after native teardown. Absolute
        # capture time is supplied by the receipt/source intervals, not mtime.
        if ($info.LastWriteTimeUtc -lt $started -or
            $info.LastWriteTimeUtc -gt [DateTime]::UtcNow.AddSeconds(1)) { throw 'Capture video modification time is stale, outside the finalization window or in the future.' }
        if ((Get-BmxSuiteProperty $Receipt 'video_sha256') -isnot [string] -or $Receipt.video_sha256 -notmatch '^[0-9a-fA-F]{64}$' -or
            (Get-FileHash -LiteralPath $video -Algorithm SHA256).Hash -ine $Receipt.video_sha256) { throw 'Capture video hash mismatch.' }
    } catch { $issues.Add($_.Exception.Message) }
    $coverageIssues=[Collections.Generic.List[string]]::new()
    try {
        if ($issues.Count) { throw 'Capture receipt metadata did not validate.' }
        if ($null -eq $CoverageContext -or (Get-BmxSuiteProperty $CoverageContext 'gate_validated') -isnot [bool] -or
            $CoverageContext.gate_validated -ne $true) { throw 'Recorded-suite coverage requires a validated native recording gate.' }
        $release=ConvertTo-BmxSuiteUtc $CoverageContext.release_utc
        $terminal=ConvertTo-BmxSuiteUtc $CoverageContext.terminal_utc
        if ($terminal -lt $release -or $terminal -gt (ConvertTo-BmxSuiteUtc $Run.ended_utc) -or $started -gt $release -or $ended -lt $terminal) {
            throw 'Capture does not span native gate release through the terminal scenario evidence.'
        }
    } catch { $coverageIssues.Add($_.Exception.Message) }
    return [pscustomobject]@{
        status=$(if ($issues.Count -eq 0) { 'receipt_validated' } elseif ($null -eq $Receipt) { 'unavailable' } else { 'receipt_rejected' })
        receipt_validated=($issues.Count -eq 0); issues=@($issues.ToArray()); video_path=$video
        recording_coverage_verified=($coverageIssues.Count -eq 0); coverage_issues=@($coverageIssues.ToArray())
        capture_kind=(Get-BmxSuiteProperty $Receipt 'capture_kind')
        sample_rate_hz=(Get-BmxSuiteProperty $Receipt 'sample_rate_hz'); maximum_sample_gap_ms=(Get-BmxSuiteProperty $Receipt 'maximum_sample_gap_ms')
        full_motion_coverage_verified=$false
        recording_implemented_by_runner=$false; visual_pixels_reviewed=$false; visual_acceptance=$false
        boundary='Receipt identity/time/file metadata and optional gate-to-terminal temporal coverage only. This does not decode video, exclude black/loading-only footage, prove full-motion capture or award visual scores.'
    }
}

function Assert-BmxSuiteNoGame {
    $processes = @(Get-CimInstance Win32_Process -Filter "Name='Skate.exe'" -ErrorAction Stop)
    if ($processes.Count -ne 0) { throw 'A Skate process is running. Close it before this isolated suite; the suite will not terminate another session.' }
}

Export-ModuleMember -Function Get-BmxSuiteProperty, Read-BmxSuiteJson, Get-BmxSuiteWorkspacePath, Get-BmxSuiteDefinitions, Get-BmxSuitePlan, Test-BmxSuiteRun, Test-BmxSuiteTeardown, Get-BmxSuitePrimaryReportPath, Test-BmxSuiteRecordingGate, Assert-BmxSuiteArchive, Copy-BmxSuiteTraces, Test-BmxSuiteCaptureReceipt, Assert-BmxSuiteNoGame
