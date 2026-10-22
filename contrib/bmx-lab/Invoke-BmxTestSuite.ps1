[CmdletBinding()]
param(
    [ValidateSet('Flat','Tricks','Catalog','Ramp','Equipment','FallSide','FallInverted','All')]
    [string[]]$Scenario = @('Flat'),
    [ValidateRange(1,10)][int]$Repeat = 1,
    [ValidateRange(148,1000)][int]$FirstRunNumber = 148,
    [string]$BuildLog = '',
    [string]$OutputDirectory = 'build/bmx/test-suites',
    [string]$CaptureReceiptDirectory = '',
    [switch]$WaitForRecording,
    [ValidateRange(30,600)][int]$CaptureGateTimeoutSeconds = 180,
    [ValidateRange(0,300)][int]$CaptureReceiptTimeoutSeconds = 0,
    [switch]$RequireCapture,
    [switch]$ContinueOnFailure,
    [switch]$PlanOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'BmxTestSuite.psm1') -Force -ErrorAction Stop
$suiteRepo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$suitePlan = Get-BmxSuitePlan -RepoRoot $suiteRepo -Scenario $Scenario -Repeat $Repeat -FirstRunNumber $FirstRunNumber `
    -OutputDirectory $OutputDirectory -BuildLog $BuildLog -CaptureReceiptDirectory $CaptureReceiptDirectory -RequireCapture ([bool]$RequireCapture) `
    -WaitForRecording ([bool]$WaitForRecording) -CaptureGateTimeoutSeconds $CaptureGateTimeoutSeconds -CaptureReceiptTimeoutSeconds $CaptureReceiptTimeoutSeconds
if ($PlanOnly) {
    # Pure planning: no process queries, launch, profile/output creation, capture
    # or game input. Existing archive names are read to avoid suggesting reuse.
    $suitePlan | ConvertTo-Json -Depth 12
    return
}
if (-not $BuildLog) { throw 'Supply -BuildLog with the exact successful native build log. Use -PlanOnly to inspect scenarios without launching.' }
foreach ($suiteRequired in @($suitePlan.game_exe,$suitePlan.runtime,$suitePlan.launcher,$suitePlan.build_log) + @($suitePlan.steps.supervisor)) {
    if (-not (Test-Path -LiteralPath $suiteRequired -PathType Leaf)) { throw ('Suite prerequisite missing: ' + $suiteRequired) }
}
if ($WaitForRecording) {
    foreach ($suiteSupervisor in @($suitePlan.steps.supervisor | Select-Object -Unique)) {
        $suiteCommand = Get-Command -Name $suiteSupervisor -ErrorAction Stop
        foreach ($suiteParameter in @('CaptureGateDirectory','CaptureGateNonce','CaptureGateTimeoutSeconds')) {
            if (-not $suiteCommand.Parameters.ContainsKey($suiteParameter)) { throw ('Supervisor lacks recording-gate support: ' + $suiteSupervisor) }
        }
    }
}
if ((Get-FileHash -LiteralPath $suitePlan.game_exe -Algorithm SHA256).Hash -cne 'FBCE74D5E28EF525DBBA2CB4ADBEBC13405BDBD88F31BC940BCA45E4AE88B8F9') { throw 'The isolated game executable is not the supported build.' }
$suiteExpected = [pscustomobject]@{
    game_exe=$suitePlan.game_exe
    runtime_sha256=(Get-FileHash -LiteralPath $suitePlan.runtime -Algorithm SHA256).Hash
    launcher_sha256=(Get-FileHash -LiteralPath $suitePlan.launcher -Algorithm SHA256).Hash
    game_sha256=(Get-FileHash -LiteralPath $suitePlan.game_exe -Algorithm SHA256).Hash
}
$suiteResult = [ordered]@{
    schema=1; event='bmx_test_suite'; suite_id=$suitePlan.suite_id; started_utc=[DateTime]::UtcNow.ToString('o'); ended_utc=$null
    plan_only=$false; completed=$false; passed=$false; native_passed=$false; require_capture=[bool]$RequireCapture
    recording_implemented_by_runner=$false; visual_acceptance=$false; physical_controller_tested=$false
    runtime_sha256=$suiteExpected.runtime_sha256; launcher_sha256=$suiteExpected.launcher_sha256; game_sha256=$suiteExpected.game_sha256
    boundary=$suitePlan.boundary; scenarios=@(); error=$null
}
$suiteLock = $null; $suiteOutputCreated = $false
try {
    # A failed CIM query is an error, never evidence that the game is absent.
    Assert-BmxSuiteNoGame
    $suiteLockDirectory = Get-BmxSuiteWorkspacePath $suiteRepo 'build/bmx'
    if (-not (Test-Path -LiteralPath $suiteLockDirectory)) { New-Item -ItemType Directory -Path $suiteLockDirectory | Out-Null }
    $suiteLockPath = Get-BmxSuiteWorkspacePath $suiteRepo 'build/bmx/native-suite.lock'
    $suiteLock = [IO.File]::Open($suiteLockPath,[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    if (Test-Path -LiteralPath $suitePlan.output_directory) { throw 'Suite output already exists; refusing to overwrite evidence.' }
    New-Item -ItemType Directory -Path $suitePlan.output_directory -ErrorAction Stop | Out-Null
    $suiteOutputCreated = $true
    $suitePlan | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $suitePlan.output_directory 'plan.json') -Encoding UTF8
    $suiteOwnSources = @()
    $suiteSourceInputs = @($PSCommandPath,(Join-Path $PSScriptRoot 'BmxTestSuite.psm1'),(Join-Path $PSScriptRoot 'Start-BmxGame.ps1')) + @($suitePlan.steps.supervisor | Select-Object -Unique)
    $suiteGateHelper = Join-Path $PSScriptRoot 'RecordingGate.ps1'
    if ($WaitForRecording -and (Test-Path -LiteralPath $suiteGateHelper -PathType Leaf)) { $suiteSourceInputs += $suiteGateHelper }
    foreach ($suiteSource in $suiteSourceInputs) {
        $suiteSource = Get-BmxSuiteWorkspacePath $suiteRepo $suiteSource
        $suiteSourceRelative = $suiteSource.Substring($suiteRepo.TrimEnd('\','/').Length + 1)
        $suiteSourceCopy = Get-BmxSuiteWorkspacePath $suiteRepo (Join-Path (Join-Path $suitePlan.output_directory 'orchestrator-source') $suiteSourceRelative)
        New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($suiteSourceCopy)) -Force | Out-Null
        $suiteSourceHash = (Get-FileHash -LiteralPath $suiteSource -Algorithm SHA256).Hash
        Copy-Item -LiteralPath $suiteSource -Destination $suiteSourceCopy -ErrorAction Stop
        if ((Get-FileHash -LiteralPath $suiteSourceCopy -Algorithm SHA256).Hash -ine $suiteSourceHash) { throw 'Orchestrator archive differs from its input.' }
        $suiteOwnSources += [pscustomobject]@{ path=$suiteSource; archive=('orchestrator-source/' + $suiteSourceRelative); sha256=$suiteSourceHash }
    }
    $suiteOwnSources | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $suitePlan.output_directory 'orchestrator-manifest.json') -Encoding UTF8
    foreach ($suiteStep in $suitePlan.steps) {
        Assert-BmxSuiteNoGame
        if (Test-Path -LiteralPath $suiteStep.archive) { throw 'A planned native run ID was taken; preserve its archive and plan a new suite.' }
        foreach ($suiteSource in $suiteOwnSources) { if ((Get-FileHash -LiteralPath $suiteSource.path -Algorithm SHA256).Hash -ine $suiteSource.sha256) { throw 'A suite/supervisor source changed while the suite was running.' } }
        foreach ($suitePair in @(@($suitePlan.runtime,$suiteExpected.runtime_sha256),@($suitePlan.launcher,$suiteExpected.launcher_sha256),@($suitePlan.game_exe,$suiteExpected.game_sha256))) {
            if ((Get-FileHash -LiteralPath $suitePair[0] -Algorithm SHA256).Hash -ine $suitePair[1]) { throw 'A native artifact changed while the suite was running.' }
        }
        $suiteStepDirectory = Get-BmxSuiteWorkspacePath $suiteRepo (Join-Path $suitePlan.output_directory ($suiteStep.scenario + '-' + $suiteStep.run_number))
        New-Item -ItemType Directory -Path $suiteStepDirectory -ErrorAction Stop | Out-Null
        $suiteArguments = @{ RunNumber=$suiteStep.run_number; BuildLog=$suitePlan.build_log_argument }
        foreach ($suiteFlag in $suiteStep.flags) { $suiteArguments[$suiteFlag]=$true }
        if ($suiteStep.variant) { $suiteArguments.Variant=$suiteStep.variant }
        if ($WaitForRecording) {
            $suiteGateDirectory = Get-BmxSuiteWorkspacePath $suiteRepo $suiteStep.capture_gate_directory
            New-Item -ItemType Directory -Path $suiteGateDirectory -ErrorAction Stop | Out-Null
            $suiteArguments.CaptureGateDirectory=$suiteGateDirectory
            $suiteArguments.CaptureGateNonce=$suiteStep.capture_gate_nonce
            $suiteArguments.CaptureGateTimeoutSeconds=$suitePlan.capture_gate_timeout_seconds
        }
        $suiteStarted = [DateTime]::UtcNow
        Write-Host ('BMX suite: ' + $suiteStep.scenario + ' / native run ' + $suiteStep.run_number)
        $suiteStepError = $null
        try { & $suiteStep.supervisor @suiteArguments *>&1 | Tee-Object -FilePath (Join-Path $suiteStepDirectory 'supervisor.log') | Out-Host }
        catch { $suiteStepError=$_.Exception.Message }
        $suiteEnded = [DateTime]::UtcNow
        $suiteRun = $null; $suiteValidation = $null; $suiteTraceArtifacts = $null; $suiteGate = $null; $suiteCoverage = $null
        $suiteCapture = Test-BmxSuiteCaptureReceipt -Receipt $null -Step $suiteStep -Run $null -RepoRoot $suiteRepo
        try {
            $suiteRunPath = Get-BmxSuiteWorkspacePath $suiteRepo (Join-Path $suiteStep.archive 'run.json')
            $suiteRun = Read-BmxSuiteJson $suiteRunPath
            Copy-Item -LiteralPath $suiteRunPath -Destination (Join-Path $suiteStepDirectory 'native-run.json') -ErrorAction Stop
            $suiteValidation = Test-BmxSuiteRun -Run $suiteRun -Step $suiteStep -Expected $suiteExpected -InvocationStartedUtc $suiteStarted -InvocationEndedUtc $suiteEnded
            if ($suiteValidation.passed) {
                try { Assert-BmxSuiteArchive -RepoRoot $suiteRepo -Step $suiteStep -Run $suiteRun }
                catch { $suiteValidation=[pscustomobject]@{ passed=$false; issues=@($_.Exception.Message) } }
            }
            if ($WaitForRecording) {
                try {
                    $suiteGateStatusPath = Get-BmxSuiteWorkspacePath $suiteRepo (Join-Path $suiteStep.capture_gate_directory 'recording-gate.status.json')
                    $suiteGateStatus = Read-BmxSuiteJson $suiteGateStatusPath
                    $suiteGateControl = Read-BmxSuiteJson $suiteStep.supervisor_control
                    Copy-Item -LiteralPath $suiteGateStatusPath -Destination (Join-Path $suiteStepDirectory 'recording-gate-status.json')
                    Copy-Item -LiteralPath $suiteStep.supervisor_control -Destination (Join-Path $suiteStepDirectory 'recording-control.json')
                    $suiteGate = Test-BmxSuiteRecordingGate -Status $suiteGateStatus -Control $suiteGateControl -Step $suiteStep -Run $suiteRun
                    if (-not $suiteGate.passed) { throw ($suiteGate.issues -join ' ') }
                    $suitePrimary = Get-Item -LiteralPath (Get-BmxSuitePrimaryReportPath $suiteStep) -ErrorAction Stop
                    $suiteCoverage = [pscustomobject]@{ gate_validated=$true; release_utc=$suiteGate.release_utc; terminal_utc=$suitePrimary.LastWriteTimeUtc.ToString('o') }
                } catch { $suiteValidation=[pscustomobject]@{ passed=$false; issues=@($suiteValidation.issues) + @($_.Exception.Message) } }
            }
            # Even a failed native trial can carry useful traces, but only an
            # owned, fully terminated process may supply this archival window.
            if ((Get-BmxSuiteProperty $suiteRun 'process_teardown_verified') -is [bool] -and $suiteRun.process_teardown_verified -and
                (Get-BmxSuiteProperty $suiteRun 'owned_pid') -gt 0) {
                $suiteTraceArtifacts = Copy-BmxSuiteTraces -RepoRoot $suiteRepo -Step $suiteStep -Run $suiteRun -Destination $suiteStepDirectory
            }
            if ($suitePlan.capture_receipt_directory) {
                $suiteReceiptPath = Get-BmxSuiteWorkspacePath $suiteRepo (Join-Path $suitePlan.capture_receipt_directory $suiteStep.capture_receipt_name)
                $suiteReceiptDeadline = [DateTime]::UtcNow.AddSeconds($suitePlan.capture_receipt_timeout_seconds)
                while (-not (Test-Path -LiteralPath $suiteReceiptPath -PathType Leaf) -and [DateTime]::UtcNow -lt $suiteReceiptDeadline) {
                    Start-Sleep -Milliseconds 250
                }
                if (Test-Path -LiteralPath $suiteReceiptPath -PathType Leaf) {
                    try {
                        $suiteReceipt = Read-BmxSuiteJson $suiteReceiptPath
                        $suiteCapture = Test-BmxSuiteCaptureReceipt -Receipt $suiteReceipt -Step $suiteStep -Run $suiteRun -RepoRoot $suiteRepo -CoverageContext $suiteCoverage
                        if ($suiteCapture.receipt_validated) {
                            $suiteVideoCopy = Join-Path $suiteStepDirectory ('capture' + [IO.Path]::GetExtension($suiteCapture.video_path))
                            Copy-Item -LiteralPath $suiteCapture.video_path -Destination $suiteVideoCopy -ErrorAction Stop
                            if ((Get-FileHash -LiteralPath $suiteVideoCopy -Algorithm SHA256).Hash -ine $suiteReceipt.video_sha256) { throw 'Video changed during archival.' }
                            Copy-Item -LiteralPath $suiteReceiptPath -Destination (Join-Path $suiteStepDirectory 'capture-receipt.json') -ErrorAction Stop
                            $suiteCapture | Add-Member -NotePropertyName archived_video -NotePropertyValue $suiteVideoCopy
                        }
                    } catch { $suiteCapture=[pscustomobject]@{ status='receipt_rejected'; receipt_validated=$false; recording_coverage_verified=$false; issues=@($_.Exception.Message); recording_implemented_by_runner=$false; visual_pixels_reviewed=$false; visual_acceptance=$false } }
                }
            }
        } catch { $suiteValidation=[pscustomobject]@{ passed=$false; issues=@($_.Exception.Message) } }
        if ($suiteStepError) { $suiteValidation=[pscustomobject]@{ passed=$false; issues=@($suiteStepError) + @($suiteValidation.issues) } }
        $suiteStepResult = [pscustomobject]@{
            scenario=$suiteStep.scenario; iteration=$suiteStep.iteration; run_number=$suiteStep.run_number
            invocation_started_utc=$suiteStarted.ToString('o'); invocation_ended_utc=$suiteEnded.ToString('o')
            native_archive=$suiteStep.archive; native_passed=[bool]$suiteValidation.passed
            passed=([bool]$suiteValidation.passed -and (-not $RequireCapture -or $suiteCapture.recording_coverage_verified))
            validation=$suiteValidation; trace_archive=$suiteTraceArtifacts; recording_gate=$suiteGate; capture=$suiteCapture; visual_acceptance=$false
        }
        $suiteResult.scenarios += $suiteStepResult
        $suiteStepResult | ConvertTo-Json -Depth 14 | Set-Content -LiteralPath (Join-Path $suiteStepDirectory 'result.json') -Encoding UTF8
        # Do not infer safe continuation from the supervisor's shell exit code.
        # Unknown teardown always ends the suite, even with ContinueOnFailure.
        if (-not (Test-BmxSuiteTeardown $suiteRun)) { throw 'Native teardown was not verified; no next game will be started.' }
        Assert-BmxSuiteNoGame
        foreach ($suiteSource in $suiteOwnSources) { if ((Get-FileHash -LiteralPath $suiteSource.path -Algorithm SHA256).Hash -ine $suiteSource.sha256) { throw 'A suite/supervisor source changed during the native trial.' } }
        if (-not $suiteStepResult.passed -and -not $ContinueOnFailure) { break }
    }
    $suiteResult.completed = $suiteResult.scenarios.Count -eq $suitePlan.steps.Count
    $suiteResult.native_passed = $suiteResult.completed -and @($suiteResult.scenarios | Where-Object { -not $_.native_passed }).Count -eq 0
    $suiteResult.passed = $suiteResult.completed -and @($suiteResult.scenarios | Where-Object { -not $_.passed }).Count -eq 0
} catch { $suiteResult.error=$_.Exception.Message; $suiteResult.passed=$false; $suiteResult.native_passed=$false }
finally {
    $suiteResult.ended_utc = [DateTime]::UtcNow.ToString('o')
    if ($suiteOutputCreated) { $suiteResult | ConvertTo-Json -Depth 18 | Set-Content -LiteralPath (Join-Path $suitePlan.output_directory 'suite.json') -Encoding UTF8 }
    if ($null -ne $suiteLock) { $suiteLock.Dispose() }
}
$suiteResult | ConvertTo-Json -Depth 18
if (-not $suiteResult.passed) { throw ('BMX suite did not pass. Evidence: ' + $suitePlan.output_directory + '. ' + $suiteResult.error) }
