# Offline policy fixtures. No native supervisor, OS input, capture or game is run.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'BmxTestSuite.psm1') -Force
$testRepo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$testRoot = Get-BmxSuiteWorkspacePath $testRepo ('build/bmx/suite-fixtures/' + [Guid]::NewGuid().ToString('N'))
$testCount = 0
function Assert-BmxFixture([bool]$Condition,[string]$Message) {
    ++$script:testCount
    if (-not $Condition) { throw ('Fixture failed: ' + $Message) }
}
function Assert-BmxThrows([scriptblock]$Action,[string]$Message) {
    $rejected=$false
    try { & $Action | Out-Null } catch { $rejected=$true }
    Assert-BmxFixture $rejected $Message
}
function Copy-BmxFixture($Value) {
    $arguments=@{}; if ((Get-Command ConvertFrom-Json).Parameters.ContainsKey('DateKind')) { $arguments.DateKind='String' }
    return $Value | ConvertTo-Json -Depth 15 | ConvertFrom-Json @arguments
}
function New-BmxRunFixture($Step) {
    [pscustomobject]@{
        owned_pid=12345; owned_created_utc='2026-10-19T12:00:01.0000000Z'
        started_utc='2026-10-19T12:00:00.0000000Z'; ended_utc='2026-10-19T12:01:00.0000000Z'
        game_exe=$script:testExpected.game_exe; profile=$Step.profile
        runtime_sha256=$script:testExpected.runtime_sha256; launcher_sha256=$script:testExpected.launcher_sha256; game_sha256=$script:testExpected.game_sha256
        outcome=$Step.accepted_outcome; process_teardown_verified=$true; source_unchanged_after_run=$true; teardown_absence_observations=2
        report_process_identity='verified'; report_completed=$true; report_passed=$true
        lifecycle_report_passed=$true; completed=$true; prepared=$true; report_identity_verified=$true; ramp_ride=$true; ride_passed=$true
        native_fall_passed=$true; pulse_passed=$true; standability_observed=$true; passed=$true; impact_variant=$Step.variant
        trick_report_passed=$true; catalog_report_passed=$true
    }
}
function Test-BmxFixtureRun($Value,$Step) {
    Test-BmxSuiteRun -Run $Value -Step $Step -Expected $script:testExpected `
        -InvocationStartedUtc ([DateTime]::Parse('2026-10-19T11:59:59Z').ToUniversalTime()) `
        -InvocationEndedUtc ([DateTime]::Parse('2026-10-19T12:01:01Z').ToUniversalTime())
}
try {
    New-Item -ItemType Directory -Path $testRoot -ErrorAction Stop | Out-Null
    foreach ($path in @('BmxTestSuite.psm1','Invoke-BmxTestSuite.ps1',$PSCommandPath)) {
        $full=if ([IO.Path]::IsPathRooted($path)) { $path } else { Join-Path $PSScriptRoot $path }
        $tokens=$null; $errors=$null
        [System.Management.Automation.Language.Parser]::ParseFile($full,[ref]$tokens,[ref]$errors) | Out-Null
        Assert-BmxFixture ($errors.Count -eq 0) ('PowerShell parse: ' + $path)
    }
    $plan=Get-BmxSuitePlan -RepoRoot $testRoot -Scenario All -Repeat 2
    Assert-BmxFixture ($plan.steps.Count -eq 14) 'All x2 expands to fourteen serial native trials.'
    Assert-BmxFixture (@($plan.steps.run_number | Select-Object -Unique).Count -eq 14) 'Fresh run IDs are unique across scenario namespaces.'
    Assert-BmxFixture (-not (Test-Path -LiteralPath $plan.output_directory)) 'Planning does not create output.'
    Assert-BmxFixture (-not $plan.recording_implemented_by_runner -and -not $plan.os_input_injected) 'Plan exposes recorder and OS-input boundaries.'
    $gated=Get-BmxSuitePlan -RepoRoot $testRoot -Scenario Flat,Equipment -WaitForRecording $true -CaptureReceiptTimeoutSeconds 60
    Assert-BmxFixture ($gated.wait_for_recording -and $gated.capture_receipt_timeout_seconds -eq 60) 'Recording gate and receipt wait are explicit opt-ins.'
    Assert-BmxFixture ($gated.steps[0].capture_gate_nonce -match '^[0-9a-f]{32}$' -and
        $gated.steps[0].capture_gate_nonce -ne $gated.steps[1].capture_gate_nonce) 'Every recording gate has a fresh nonce.'
    Assert-BmxFixture (-not (Test-Path -LiteralPath $gated.steps[0].capture_gate_directory)) 'Gate planning creates no folders or release file.'
    Assert-BmxThrows { Get-BmxSuitePlan -RepoRoot $testRoot -Scenario All,Flat } 'All cannot silently duplicate specific scenarios.'
    Assert-BmxThrows { Get-BmxSuitePlan -RepoRoot $testRoot -Scenario Unknown } 'Unknown scenario rejected.'
    Assert-BmxThrows { Get-BmxSuitePlan -RepoRoot $testRoot -OutputDirectory '..\outside' } 'Output traversal rejected.'
    Assert-BmxThrows { Get-BmxSuitePlan -RepoRoot $testRoot -CaptureReceiptDirectory '..\outside' } 'Receipt traversal rejected.'
    Assert-BmxThrows { Get-BmxSuitePlan -RepoRoot $testRoot -Scenario Flat,Flat -FirstRunNumber 1000 } 'Run IDs cannot exceed supervisor limit.'
    New-Item -ItemType Directory -Path (Join-Path $testRoot 'build/bmx/native-getup-run148') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $testRoot 'build/bmx/native-impact-run149') -Force | Out-Null
    $fresh=Get-BmxSuitePlan -RepoRoot $testRoot -Scenario Flat
    Assert-BmxFixture ($fresh.steps[0].run_number -eq 150) 'Allocator preserves other scenario namespaces and prior failed impacts.'
    Assert-BmxFixture ($plan.steps[3].profile.EndsWith('bmx-user-data-ramp-test')) 'Ramp gets its exact private profile.'
    Assert-BmxFixture ($plan.steps[0].profile.EndsWith('bmx-user-data-chassis-probe')) 'Flat uses private probe profile, not working controller profile.'
    Assert-BmxFixture ($plan.steps[1].flags -contains 'Tricks' -and $plan.steps[2].flags -contains 'Catalog') 'Trick and catalog launch flags remain distinct.'
    Assert-BmxFixture ($plan.steps[5].variant -eq 'side' -and $plan.steps[6].variant -eq 'inverted') 'Fall variants stay explicit.'
    $buildPlan=Get-BmxSuitePlan -RepoRoot $testRoot -BuildLog (Join-Path $testRoot 'build/current-native.log')
    Assert-BmxFixture (-not [IO.Path]::IsPathRooted($buildPlan.build_log_argument) -and
        [IO.Path]::GetFullPath((Join-Path $testRoot $buildPlan.build_log_argument)) -eq $buildPlan.build_log) 'BuildLog argument remains relative for existing supervisor Join-Path contract.'

    $script:testExpected=[pscustomobject]@{ game_exe=$plan.game_exe; runtime_sha256=('A'*64); launcher_sha256=('B'*64); game_sha256=('C'*64) }
    foreach ($step in @($plan.steps | Select-Object -First 7)) {
        $run=New-BmxRunFixture $step
        Assert-BmxFixture (Test-BmxFixtureRun $run $step).passed ('Valid contract: ' + $step.scenario)
        $run.outcome='supervisor_wall_timeout'
        Assert-BmxFixture (-not (Test-BmxFixtureRun $run $step).passed) ('Exit-zero terminal failure cannot pass: ' + $step.scenario)
    }
    $step=$plan.steps[0]; $valid=New-BmxRunFixture $step
    Assert-BmxFixture (Test-BmxSuiteTeardown $valid) 'Only verified double absence permits continuation.'
    foreach ($value in @($null,'2',1,$true)) {
        $changed=Copy-BmxFixture $valid; $changed.teardown_absence_observations=$value
        Assert-BmxFixture (-not (Test-BmxSuiteTeardown $changed)) 'Unknown/string/boolean/single absence cannot authorize a next game.'
    }
    foreach ($value in @($null,'true',$false)) {
        $changed=Copy-BmxFixture $valid; $changed.process_teardown_verified=$value
        Assert-BmxFixture (-not (Test-BmxSuiteTeardown $changed)) 'Unknown/string/false teardown cannot authorize a next game.'
    }
    foreach ($mutation in @(
        @('owned_pid',0),@('owned_pid','12345'),@('process_teardown_verified',$false),@('process_teardown_verified','true'),
        @('source_unchanged_after_run',$false),@('source_unchanged_after_run','true'),@('teardown_absence_observations',1),@('teardown_absence_observations','2'),
        @('outcome','native_chassis_switch_passed'),@('runtime_sha256',('D'*64)),@('launcher_sha256',('D'*64)),@('game_sha256',('D'*64)),
        @('profile',(Join-Path $testRoot 'build/bmx-user-data-working-profile')),@('game_exe',(Join-Path $testRoot 'normal/Skate.exe')),
        @('started_utc','2026-10-18T12:00:00Z'),@('ended_utc','2026-10-19T12:02:00Z'),@('owned_created_utc','2026-10-19T11:58:00Z'),
        @('owned_created_utc','2026-10-19T12:01:02Z'),@('started_utc','2026-10-19T12:00:00'),@('report_completed',$false),@('report_passed','true'),@('report_process_identity','legacy_report_without_process_id')
    )) {
        $changed=Copy-BmxFixture $valid; $changed.($mutation[0])=$mutation[1]
        Assert-BmxFixture (-not (Test-BmxFixtureRun $changed $step).passed) ('Native invalid field rejected: ' + $mutation[0])
    }
    foreach ($scenarioRequired in @(@(1,'trick_report_passed'),@(2,'catalog_report_passed'),@(3,'ride_passed'),@(4,'lifecycle_report_passed'),@(5,'pulse_passed'),@(6,'standability_observed'))) {
        $specific=$plan.steps[$scenarioRequired[0]]; $changed=New-BmxRunFixture $specific; $changed.($scenarioRequired[1])=$false
        Assert-BmxFixture (-not (Test-BmxFixtureRun $changed $specific).passed) ('Scenario-specific evidence required: ' + $specific.scenario)
    }

    # Archive validation reads actual fixture files, not a self-reported pass.
    New-Item -ItemType Directory -Path (Join-Path $step.archive 'source/Extension/BMX') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $testRoot 'Extension/BMX') -Force | Out-Null
    $sourcePath=Join-Path $testRoot 'Extension/BMX/fixture.cpp'
    [IO.File]::WriteAllText($sourcePath,'SYNTHETIC fixture source')
    $archivedSource=Join-Path $step.archive 'source/Extension/BMX/fixture.cpp'
    Copy-Item -LiteralPath $sourcePath -Destination $archivedSource
    $sourceHash=(Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash
    # Explicit array spelling also works on WindowsPowerShell5.
    [IO.File]::WriteAllText((Join-Path $step.archive 'source-manifest.json'),'[{"path":"Extension/BMX/fixture.cpp","sha256":"' + $sourceHash + '"}]')
    foreach ($name in @('ReSkate.dll','ReSkateLauncher.exe')) { [IO.File]::WriteAllText((Join-Path $step.archive $name),'SYNTHETIC artifact fixture ' + $name) }
    $archiveRun=Copy-BmxFixture $valid
    $archiveRun.runtime_sha256=(Get-FileHash -LiteralPath (Join-Path $step.archive 'ReSkate.dll') -Algorithm SHA256).Hash
    $archiveRun.launcher_sha256=(Get-FileHash -LiteralPath (Join-Path $step.archive 'ReSkateLauncher.exe') -Algorithm SHA256).Hash
    $primaryPath=Join-Path $step.archive 'bmx-selftest.json'
    $primary=[pscustomobject]@{process_id=$valid.owned_pid;completed=$true;passed=$true;physical_chassis_mode=$true;physical_chassis_passed=$true}
    $primary | ConvertTo-Json | Set-Content -LiteralPath $primaryPath
    Assert-BmxSuiteArchive -RepoRoot $testRoot -Step $step -Run $archiveRun
    Assert-BmxFixture $true 'Matching source/artifact/report files validate.'
    [IO.File]::AppendAllText($sourcePath,' modified')
    Assert-BmxThrows { Assert-BmxSuiteArchive -RepoRoot $testRoot -Step $step -Run $archiveRun } 'Live source mutation invalidates archive.'
    [IO.File]::WriteAllText($sourcePath,'SYNTHETIC fixture source')
    [IO.File]::AppendAllText($archivedSource,' modified')
    Assert-BmxThrows { Assert-BmxSuiteArchive -RepoRoot $testRoot -Step $step -Run $archiveRun } 'Archived source mutation invalidates archive.'
    Copy-Item -LiteralPath $sourcePath -Destination $archivedSource -Force
    $primary.process_id=999; $primary | ConvertTo-Json | Set-Content -LiteralPath $primaryPath
    Assert-BmxThrows { Assert-BmxSuiteArchive -RepoRoot $testRoot -Step $step -Run $archiveRun } 'Wrong terminal report PID rejected.'
    $primary.process_id=$valid.owned_pid; $primary.passed='true'; $primary | ConvertTo-Json | Set-Content -LiteralPath $primaryPath
    Assert-BmxThrows { Assert-BmxSuiteArchive -RepoRoot $testRoot -Step $step -Run $archiveRun } 'String terminal pass rejected.'
    $primary.passed=$true; $primary.physical_chassis_mode=$false; $primary | ConvertTo-Json | Set-Content -LiteralPath $primaryPath
    Assert-BmxThrows { Assert-BmxSuiteArchive -RepoRoot $testRoot -Step $step -Run $archiveRun } 'Nonphysical terminal report rejected.'
    $primary.physical_chassis_mode=$true; $primary | ConvertTo-Json | Set-Content -LiteralPath $primaryPath
    [IO.File]::AppendAllText((Join-Path $step.archive 'ReSkate.dll'),' modified')
    Assert-BmxThrows { Assert-BmxSuiteArchive -RepoRoot $testRoot -Step $step -Run $archiveRun } 'Archived DLL mutation rejected.'

    # A deliberately synthetic non-video file tests only receipt/file metadata.
    # The validator must continue to label it unreviewed, never a visual pass.
    $video=Join-Path $testRoot 'synthetic-metadata-only.mp4'
    [IO.File]::WriteAllText($video,'SYNTHETIC FIXTURE: not encoded video; metadata validation is not pixel validation.')
    [IO.File]::SetLastWriteTimeUtc($video,[DateTime]::Parse('2026-10-19T12:01:00Z').ToUniversalTime())
    $capture=[pscustomobject]@{
        schema=1; scenario=$step.scenario; run_number=$step.run_number; process_id=$valid.owned_pid
        process_created_utc=$valid.owned_created_utc; runtime_sha256=$valid.runtime_sha256; game_exe=$valid.game_exe
        capture_started_utc='2026-10-19T12:00:02Z'; capture_ended_utc='2026-10-19T12:00:59Z'
        capture_method='synthetic offline metadata fixture'; video_path=$video; video_sha256=(Get-FileHash -LiteralPath $video -Algorithm SHA256).Hash
    }
    $captureResult=Test-BmxSuiteCaptureReceipt -Receipt $capture -Step $step -Run $valid -RepoRoot $testRoot
    Assert-BmxFixture ($captureResult.receipt_validated -and $captureResult.status -eq 'receipt_validated') 'Exact capture receipt metadata accepted.'
    Assert-BmxFixture (-not $captureResult.visual_pixels_reviewed -and -not $captureResult.visual_acceptance -and -not $captureResult.recording_implemented_by_runner) 'Metadata-only acceptance cannot imply real video, recording implementation or visual quality.'
    Assert-BmxFixture (-not $captureResult.recording_coverage_verified) 'Ungated metadata overlap cannot pass RequireCapture coverage.'
    $coverage=[pscustomobject]@{gate_validated=$true;release_utc='2026-10-19T12:00:03Z';terminal_utc='2026-10-19T12:00:58Z'}
    Assert-BmxFixture (Test-BmxSuiteCaptureReceipt -Receipt $capture -Step $step -Run $valid -RepoRoot $testRoot -CoverageContext $coverage).recording_coverage_verified 'Gated capture spanning release to terminal evidence validates interval coverage.'
    $late=Copy-BmxFixture $capture; $late.capture_started_utc='2026-10-19T12:00:59Z';$late.capture_ended_utc='2026-10-19T12:01:00Z'
    Assert-BmxFixture (-not (Test-BmxSuiteCaptureReceipt -Receipt $late -Step $step -Run $valid -RepoRoot $testRoot -CoverageContext $coverage).recording_coverage_verified) 'One-second post-scenario clip cannot pass recorded coverage.'
    $early=Copy-BmxFixture $capture; $early.capture_ended_utc='2026-10-19T12:00:57Z'
    Assert-BmxFixture (-not (Test-BmxSuiteCaptureReceipt -Receipt $early -Step $step -Run $valid -RepoRoot $testRoot -CoverageContext $coverage).recording_coverage_verified) 'Clip ending before terminal evidence cannot pass recorded coverage.'
    $unknownCoverage=Copy-BmxFixture $coverage;$unknownCoverage.gate_validated='true'
    Assert-BmxFixture (-not (Test-BmxSuiteCaptureReceipt -Receipt $capture -Step $step -Run $valid -RepoRoot $testRoot -CoverageContext $unknownCoverage).recording_coverage_verified) 'String gate validity cannot authorize coverage.'
    $gateStep=$gated.steps[0];$gateRun=New-BmxRunFixture $gateStep
    $gateRun | Add-Member -NotePropertyName recording_gate_enabled -NotePropertyValue $true
    $gateRun | Add-Member -NotePropertyName capture_trace_settle_completed -NotePropertyValue $true
    $gateRun | Add-Member -NotePropertyName recording_gate_state -NotePropertyValue 'released'
    $gateRun | Add-Member -NotePropertyName recording_gate_directory -NotePropertyValue $gateStep.capture_gate_directory
    $gateRun | Add-Member -NotePropertyName recording_gate_nonce -NotePropertyValue $gateStep.capture_gate_nonce
    $nativeFileTime=(([DateTime]::Parse($gateRun.owned_created_utc).ToUniversalTime()).ToFileTimeUtc()+3).ToString([Globalization.CultureInfo]::InvariantCulture)
    $gateStatus=[pscustomobject]@{schema_version=1;event='bmx_recording_startup_gate';state='released';nonce=$gateStep.capture_gate_nonce;process_id=$gateRun.owned_pid;process_creation_filetime=$nativeFileTime;observed_utc='2026-10-19T12:00:03Z';elapsed_ms=2000}
    $gateControl=[pscustomobject]@{owned_pid=$gateRun.owned_pid;owned_created_utc=$gateRun.owned_created_utc;gate_directory=$gateStep.capture_gate_directory;gate_nonce=$gateStep.capture_gate_nonce;gate_status_file=(Join-Path $gateStep.capture_gate_directory 'recording-gate.status.json');gate_ready_file=(Join-Path $gateStep.capture_gate_directory 'recording-gate.ready');gate_state='released';process_creation_filetime=$nativeFileTime}
    Assert-BmxFixture (Test-BmxSuiteRecordingGate -Status $gateStatus -Control $gateControl -Step $gateStep -Run $gateRun).passed 'Exact native FILETIME preserved while CIM precision is independently bounded.'
    foreach ($mutation in @(@('recording_gate_enabled','true'),@('capture_trace_settle_completed',$false),@('recording_gate_state','timed_out'),@('recording_gate_nonce',('0'*32)),@('recording_gate_directory','wrong'))) {
        $changed=Copy-BmxFixture $gateRun;$changed.($mutation[0])=$mutation[1]
        Assert-BmxFixture (-not (Test-BmxSuiteRecordingGate -Status $gateStatus -Control $gateControl -Step $gateStep -Run $changed).passed) ('Bad native gate run field rejected: '+$mutation[0])
    }
    foreach ($mutation in @(@('schema_version','1'),@('event','wrong'),@('state','waiting'),@('nonce',('0'*32)),@('process_id',999),@('process_creation_filetime',([long]::Parse($nativeFileTime)+100000).ToString()),@('observed_utc','2026-10-19T12:02:00Z'))) {
        $changed=Copy-BmxFixture $gateStatus;$changed.($mutation[0])=$mutation[1]
        Assert-BmxFixture (-not (Test-BmxSuiteRecordingGate -Status $changed -Control $gateControl -Step $gateStep -Run $gateRun).passed) ('Bad native gate status rejected: '+$mutation[0])
    }
    foreach ($mutation in @(@('owned_pid',999),@('owned_created_utc','2026-10-19T12:00:02Z'),@('gate_nonce',('0'*32)),@('gate_state','waiting'),@('process_creation_filetime',([long]::Parse($nativeFileTime)+10).ToString()))) {
        $changed=Copy-BmxFixture $gateControl;$changed.($mutation[0])=$mutation[1]
        Assert-BmxFixture (-not (Test-BmxSuiteRecordingGate -Status $gateStatus -Control $changed -Step $gateStep -Run $gateRun).passed) ('Bad supervisor gate control rejected: '+$mutation[0])
    }
    Assert-BmxFixture ((Test-BmxSuiteCaptureReceipt -Receipt $null -Step $step -Run $valid -RepoRoot $testRoot).status -eq 'unavailable') 'Missing capture is explicitly unavailable.'
    foreach ($mutation in @(
        @('schema',2),@('schema','1'),@('scenario','Equipment'),@('run_number',999),@('process_id',999),@('process_id','12345'),
        @('process_created_utc','2026-10-19T12:00:02Z'),@('runtime_sha256',('F'*64)),@('game_exe',(Join-Path $testRoot 'normal/Skate.exe')),
        @('capture_started_utc','2026-10-19T11:59:00Z'),@('capture_ended_utc','2026-10-19T12:02:00Z'),
        @('capture_ended_utc','2026-10-19T12:00:01Z'),@('capture_method',''),@('video_path','relative.mp4'),
        @('video_path',(Join-Path $testRoot '../outside.mp4')),@('video_sha256',('0'*64))
    )) {
        $changed=Copy-BmxFixture $capture; $changed.($mutation[0])=$mutation[1]
        Assert-BmxFixture (-not (Test-BmxSuiteCaptureReceipt -Receipt $changed -Step $step -Run $valid -RepoRoot $testRoot).receipt_validated) ('Mismatched capture rejected: ' + $mutation[0])
    }

    $traceRoot=Join-Path $step.profile 'bmx-traces'; New-Item -ItemType Directory -Path $traceRoot -Force | Out-Null
    $traceGood=Join-Path $traceRoot 'ride-12345-1.json'
    $traceWrongPid=Join-Path $traceRoot 'ride-12345-2.json'
    $traceStale=Join-Path $traceRoot 'fall-12345-3.json'
    $traceBad=Join-Path $traceRoot 'fall-12345-4.json'
    $traceOther=Join-Path $traceRoot 'ride-999-1.json'
    foreach ($path in @($traceGood,$traceStale,$traceOther)) { [IO.File]::WriteAllText($path,'{"schema":1,"process_id":12345,"event":"bmx_passive_fall_lifecycle"}') }
    [IO.File]::WriteAllText($traceWrongPid,'{"schema":1,"process_id":999}')
    [IO.File]::WriteAllText($traceBad,'{"process_id":12345, invalid JSON')
    foreach ($path in @($traceGood,$traceWrongPid,$traceBad)) { [IO.File]::SetLastWriteTimeUtc($path,[DateTime]::Parse('2026-10-19T12:00:30Z').ToUniversalTime()) }
    [IO.File]::SetLastWriteTimeUtc($traceStale,[DateTime]::Parse('2026-10-18T12:00:30Z').ToUniversalTime())
    $traceArchive=Join-Path $testRoot 'trace-archive'
    $traces=Copy-BmxSuiteTraces -RepoRoot $testRoot -Step $step -Run $valid -Destination $traceArchive
    Assert-BmxFixture ($traces.artifacts.Count -eq 1 -and $traces.artifacts[0].path -eq 'bmx-traces/ride-12345-1.json') 'Archive contains only current PID-bound fresh valid traces.'
    Assert-BmxFixture ($traces.rejected.Count -eq 3) 'Wrong PID, stale and malformed traces remain explicitly rejected.'
    Assert-BmxFixture ((Get-FileHash -LiteralPath (Join-Path $traceArchive 'bmx-traces/ride-12345-1.json') -Algorithm SHA256).Hash -eq $traces.artifacts[0].sha256) 'Trace copy hash verified.'

    # Exercise actual entrypoint PlanOnly with process/launch commands poisoned.
    # If planning accidentally invokes one, this fixture fails before any action.
    function global:Get-CimInstance { throw 'PlanOnly attempted CIM.' }
    function global:Start-Process { throw 'PlanOnly attempted launch.' }
    function global:Stop-Process { throw 'PlanOnly attempted stop.' }
    try {
        $dryJson=& (Join-Path $PSScriptRoot 'Invoke-BmxTestSuite.ps1') -PlanOnly -Scenario Flat -OutputDirectory (Join-Path $testRoot 'dry-output')
        $dry=$dryJson | ConvertFrom-Json
        Assert-BmxFixture ($dry.event -eq 'bmx_test_suite_plan' -and -not (Test-Path -LiteralPath $dry.output_directory)) 'Actual PlanOnly makes no process query or output directory.'
    } finally {
        Remove-Item Function:\Get-CimInstance,Function:\Start-Process,Function:\Stop-Process -ErrorAction SilentlyContinue
    }
    Write-Output ('BMX suite offline fixtures PASS: ' + $testCount + ' checks. No game, OS input or capture was run.')
} finally {
    # Only this freshly created synthetic fixture directory can be removed.
    $checked = Get-BmxSuiteWorkspacePath $testRepo $testRoot
    $fixtureParent = [IO.Path]::GetFullPath((Join-Path $testRepo 'build/bmx/suite-fixtures'))
    if ([IO.Path]::GetDirectoryName($checked) -ne $fixtureParent) { throw 'Fixture cleanup escaped its dedicated parent.' }
    if (Test-Path -LiteralPath $checked) { Remove-Item -LiteralPath $checked -Recurse -Force -ErrorAction Stop }
}
