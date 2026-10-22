#requires -Version 7.0
# PowerShell 7 is required for System.Text.Json duplicate-property validation.
param(
    [Parameter(Mandatory)][ValidateRange(1,10000)][int]$RunNumber,
    [ValidateSet('side')][string]$Variant = 'side',
    [switch]$PhaseProbe,
    [switch]$FirstMountProducerProbe,
    [ValidateSet('Default','RenderOnly','None')][string]$RiderPoseMode = 'Default',
    [ValidateSet('Default','PreserveSuppliedBasis')][string]$MotionBasisMode = 'Default',
    [string]$CaptureGateDirectory = '',
    [ValidatePattern('^$|^[0-9a-f]{32}$')][string]$CaptureGateNonce = '',
    [ValidateRange(30,600)][int]$CaptureGateTimeoutSeconds = 180,
    [Parameter(Mandatory)][string]$BuildLog
)
# ROOT-OWNED EXECUTION ONLY. This distinct lab follows one private typed GetUp
# with a later explicit post-fall remount; the passive GetUp lab is unchanged.
# Parser fixtures load only function ASTs, never execute this supervisor.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'PoseComparisonEvidence.ps1')
Assert-BmxPoseComparisonConfiguration $RiderPoseMode ([bool]$PhaseProbe) ([bool]$FirstMountProducerProbe) $true $true $Variant $true $true 'bmx-user-data-chassis-probe'
Assert-BmxMotionBasisConfiguration $MotionBasisMode $RiderPoseMode ([bool]$PhaseProbe) ([bool]$FirstMountProducerProbe) $true $true $Variant $true $true 'bmx-user-data-chassis-probe'
if ($FirstMountProducerProbe -and -not $PhaseProbe) { throw 'FirstMountProducerProbe requires explicit PhaseProbe in the private PostFallRemount supervisor.' }
$bmxRepoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
. (Join-Path $bmxRepoRoot 'contrib\bmx-lab\RecordingGate.ps1')
Assert-BmxCaptureGateConfiguration $bmxRepoRoot $CaptureGateDirectory $CaptureGateNonce $CaptureGateTimeoutSeconds
$bmxCaptureLaunchArgs=@{}
if ($CaptureGateDirectory) {
    $CaptureGateDirectory=[IO.Path]::GetFullPath($CaptureGateDirectory)
    $bmxCaptureLaunchArgs=@{CaptureGateDirectory=$CaptureGateDirectory;CaptureGateNonce=$CaptureGateNonce;CaptureGateTimeoutSeconds=$CaptureGateTimeoutSeconds}
}
$bmxCaptureExtraSeconds=if ($CaptureGateDirectory) { $CaptureGateTimeoutSeconds } else { 0 }
$bmxGameExe = [IO.Path]::GetFullPath((Join-Path $bmxRepoRoot 'build\bmx-game\Skate.exe'))
$bmxProfile = Join-Path $bmxRepoRoot 'build\bmx-user-data-chassis-probe'
$bmxReport = Join-Path $bmxProfile 'bmx-selftest.json'
$bmxImpactReport = Join-Path $bmxProfile 'bmx-impact-test.json'
$bmxActionReport = Join-Path $bmxProfile 'bmx-getup-action-test.json'
$bmxPhaseReport = Join-Path $bmxProfile 'bmx-skeleton-phase-observation.json'
$bmxProducerReport = Join-Path $bmxProfile 'bmx-first-mount-producer-observation.json'
$bmxLog = Join-Path $bmxProfile 'logs\ReSkate.log'
$bmxRunDirectory = Join-Path $bmxRepoRoot ('build\bmx\native-postfall-remount-run' + $RunNumber)
$bmxArtifacts = Join-Path $bmxRepoRoot 'build\vs2022-x64\Release'
$bmxBuildLog = [IO.Path]::GetFullPath((Join-Path $bmxRepoRoot $BuildLog))
foreach ($bmxPath in @($bmxGameExe,$bmxProfile,$bmxReport,$bmxImpactReport,$bmxActionReport,$bmxPhaseReport,$bmxRunDirectory,$bmxBuildLog)) {
    if (-not [IO.Path]::GetFullPath($bmxPath).StartsWith($bmxRepoRoot + '\',[StringComparison]::OrdinalIgnoreCase)) {
        throw 'Native chassis supervisor path escaped the workspace.'
    }
}
if (Test-Path -LiteralPath $bmxRunDirectory) { throw 'Preserve the existing run directory; choose a new run number.' }
if (@(Get-CimInstance Win32_Process -Filter "Name='Skate.exe'" -ErrorAction Stop).Count) { throw 'Skate is already running.' }
foreach ($bmxRequired in @($bmxGameExe,$bmxBuildLog,(Join-Path $bmxArtifacts 'ReSkate.dll'),(Join-Path $bmxArtifacts 'ReSkateLauncher.exe'))) {
    if (-not (Test-Path -LiteralPath $bmxRequired -PathType Leaf)) { throw ('Required artifact missing: ' + $bmxRequired) }
}
New-Item -ItemType Directory -Path $bmxRunDirectory | Out-Null
$bmxSourceDirectory = Join-Path $bmxRunDirectory 'source'
New-Item -ItemType Directory -Path $bmxSourceDirectory | Out-Null
$bmxDllModified = (Get-Item -LiteralPath (Join-Path $bmxArtifacts 'ReSkate.dll')).LastWriteTimeUtc
$bmxSources = @(
    'Engine\Game\BMX\bmx_grip_probe_policy.h',
    'Extension\BMX\bmx_grip_probe.h',
    'Extension\Multiplayer\Remote\native_prop.h',
    'Extension\Multiplayer\Remote\native_prop.cpp',
    'Extension\Multiplayer\Remote\native_skater.h','Engine\Game\BMX\Test\bmx_pose_listener_tests.cpp',
    'Engine\Game\BMX\mount_readiness.h','Engine\Game\BMX\Test\bmx_mount_readiness_tests.cpp',
    'Extension\BMX\bmx_skeleton_phase_probe.h','Extension\BMX\bmx_skeleton_phase_probe.cpp','Engine\Game\BMX\bmx_skeleton_phase_policy.h','Engine\Game\BMX\Test\bmx_skeleton_phase_policy_tests.cpp','contrib\bmx-lab\PhaseProbeEvidence.ps1','contrib\bmx-lab\Test-PhaseProbeEvidence.ps1',
    'Engine\Game\BMX\bmx_owner_observation.h','Engine\Game\BMX\Test\bmx_owner_observation_tests.cpp',
    'Engine\Game\BMX\bmx_geometry.h','Engine\Game\BMX\Test\bmx_geometry_tests.cpp',
    'Engine\Game\BMX\bmx_automated_ride.h','Engine\Game\BMX\Test\bmx_automated_ride_tests.cpp',
    'Engine\Game\BMX\bmx_automated_switch.h','Engine\Game\BMX\Test\bmx_automated_switch_tests.cpp',
    'Engine\Game\BMX\bmx_mount_pose.h','Engine\Game\BMX\Test\bmx_mount_pose_tests.cpp',
    'Engine\Game\BMX\bmx_pool_admission.h','Engine\Game\BMX\Test\bmx_pool_admission_tests.cpp',
    'Engine\Game\BMX\bmx_mount_transition.h','Engine\Game\BMX\Test\bmx_mount_transition_tests.cpp',
    'Extension\BMX\bmx_binding_observer.h','Extension\BMX\bmx_binding_observer.cpp',
    'Extension\BMX\bmx_binding_snapshot.h','Extension\BMX\Test\bmx_binding_snapshot_tests.cpp','Runtime\bootstrap.cpp',
    'Extension\BMX\bmx_intent_observer.h','Extension\BMX\bmx_intent_observer.cpp','Extension\BMX\bmx_intent_snapshot.h',
    'Extension\BMX\bmx_intent_contracts.h','Extension\BMX\Test\bmx_intent_snapshot_tests.cpp',
    'Extension\BMX\Test\bmx_world_transform_tests.cpp',
    'Extension\BMX\Test\bmx_world_lifecycle_tests.cpp',
    'Extension\BMX\bmx_crash_recovery.h','Extension\BMX\Test\bmx_crash_recovery_tests.cpp',
    'Extension\BMX\bmx_intent_pulse.h',
    'Extension\BMX\bmx_ride_trace.h','Extension\BMX\Test\bmx_ride_trace_tests.cpp',
    'Extension\BMX\bmx_impact_receipt.h','Extension\BMX\Test\bmx_impact_receipt_tests.cpp',
    'Extension\BMX\bmx_snapshot_retry.h','Extension\BMX\Test\bmx_snapshot_retry_tests.cpp',
    'Extension\BMX\bmx_fall_lifecycle.h','Extension\BMX\Test\bmx_fall_lifecycle_tests.cpp',
    'Extension\BMX\bmx_bounded_fifo.h','Extension\BMX\Test\bmx_bounded_fifo_tests.cpp',
    'Extension\BMX\bmx_getup_observation.h','Extension\BMX\Test\bmx_getup_observation_tests.cpp',
    'Extension\BMX\bmx_getup_action.h','Extension\BMX\Test\bmx_getup_action_tests.cpp',
    'Extension\BMX\bmx_getup_callback_audit.h','Extension\BMX\Test\bmx_getup_callback_audit_tests.cpp',
    'Extension\BMX\bmx_getup_lab.h','Extension\BMX\Test\bmx_getup_lab_tests.cpp',
    'build\bmx\impact-recovery-guard-parser-tests.ps1',
    'Engine\Game\BMX\equipment_switch.h','Engine\Game\BMX\Test\bmx_equipment_switch_tests.cpp',
    'Extension\ReplayKeyframes\replay_keyframes.cpp',
    'Extension\UI\Overlay\gpu_diagnostics.cpp',
    'Extension\BMX\bmx_bail.h',
    'Extension\BMX\bmx_bail.cpp',
    'Extension\BMX\bmx_body_guard.h',
    'Extension\BMX\bmx_collision_envelope.h',
    'Extension\BMX\bmx_impact_test.h',
    'Extension\BMX\bmx_catalog_test.h',
    'Extension\BMX\bmx_rider_motion.h',
    'Extension\BMX\Test\bmx_body_guard_tests.cpp',
    'Engine\Game\BMX\bmx_bail_policy.h',
    'Engine\Game\BMX\Test\bmx_bail_policy_tests.cpp',
    'Engine\Game\BMX\bmx_interaction.h',
    'Engine\Game\BMX\Test\bmx_interaction_tests.cpp',
    'Engine\Game\BMX\bmx_trick_catalog.h',
    'Engine\Game\BMX\Test\bmx_trick_tests.cpp',
    'Extension\BMX\Test\bmx_rider_pose_tests.cpp',
    'Extension\Skater\no_bail.cpp',
    'Extension\Skater\no_bail.h',

    'Extension\BMX\Test\bmx_ramp_rehearsal_tests.cpp','Engine\Game\BMX\bmx_quick_switch_rehearsal.h','Extension\BMX\bmx_runtime.cpp','Extension\BMX\bmx_runtime.h',
    'Extension\BMX\bmx_native_chassis.cpp','Extension\BMX\bmx_native_chassis.h',
    'Extension\BMX\bmx_chassis_ride.cpp','Extension\BMX\bmx_chassis_ride.h',
    'Extension\BMX\bmx_ramp_test.cpp','Extension\BMX\bmx_ramp_test.h',
    'Extension\BMX\bmx_wheel_surface.h','Extension\BMX\Test\bmx_wheel_surface_tests.cpp','Extension\BMX\bmx_ramp_rehearsal.h',
    'Extension\BMX\bmx_native_motion.cpp','Extension\BMX\bmx_native_motion.h',
    'Extension\BMX\bmx_commands.cpp','Extension\BMX\bmx_selftest.h',
    'Extension\BMX\bmx_visual.cpp','Extension\BMX\bmx_visual.h',
    'Extension\BMX\bmx_world.cpp','Extension\BMX\bmx_world.h','Extension\BMX\bmx_world_transform.h',
    'Extension\BMX\bmx_rider.cpp','Extension\BMX\bmx_rider.h','Extension\BMX\bmx_rider_pose.h',
    'Extension\BMX\bmx_pose_write.h','Extension\BMX\Test\bmx_pose_write_tests.cpp',
    'Engine\Game\BMX\Test\bmx_input_tests.cpp','Engine\Game\Input\Test\bmx_input_sample_tests.cpp',
    'Engine\Game\BMX\Test\bmx_chassis_control_tests.cpp',
    'Engine\Game\BMX\bmx_coupled_brake.h','Engine\Game\BMX\Test\bmx_coupled_brake_tests.cpp',
    'Engine\Game\BMX\Test\bmx_controls_model_tests.cpp','Extension\BMX\Test\bmx_selftest_tests.cpp',
    'Extension\BMX\bmx_bike_contacts.h','Extension\BMX\bmx_grip_anchors.inc',
    'Engine\Game\BMX\bmx_landing_presentation.h','Engine\Game\BMX\Test\bmx_landing_presentation_tests.cpp',
    'Engine\Game\BMX\bmx_landing_approach.h','Engine\Game\BMX\Test\bmx_landing_approach_tests.cpp','Engine\Game\BMX\bmx_crank_presentation.h','Engine\Game\BMX\Test\bmx_crank_presentation_tests.cpp','Engine\Game\BMX\bmx_chassis_control.h','Engine\Game\BMX\bmx_tire_dynamics.h',
    'Engine\Game\BMX\bmx_contact_patch.h',
    'Engine\Game\BMX\chassis_transform.h','Engine\Game\BMX\bmx_input.h','Engine\Game\BMX\bmx_tricks.h',
    'Engine\Game\Input\bmx_input_sample.h','Extension\UI\Overlay\input_capture.cpp',
    'Extension\UI\Overlay\input_capture.h','Extension\UI\Overlay\overlay_input.cpp',
    'Extension\UI\Overlay\overlay.h','Extension\UI\Overlay\overlay_notices.cpp','Engine\Game\Input\playstation_report.h',
    'Extension\Multiplayer\Remote\native_skater.cpp','Extension\Skater\client_noclip.cpp',
    'Engine\Game\Build\20260929\remote_collision.h','Engine\Game\Build\20260929\engine.h',
    'Engine\Game\Build\20260929\no_bail.h','Engine\Game\Build\addresses.h',
    'Runtime\client_tick.cpp','Runtime\startup_recording_gate.h','Runtime\Test\startup_recording_gate_tests.cpp','contrib\bmx-lab\RecordingGate.ps1','contrib\bmx-lab\Test-RecordingGate.ps1','cmake\Runtime.cmake','cmake\BmxTests.cmake','CMakeLists.txt',
    'Launcher\main.cpp','Launcher\launch.cpp','Launcher\launch.h',
    'contrib\bmx-lab\PostFallRemountEvidence.ps1','contrib\bmx-lab\Test-PostFallRemount.ps1','contrib\bmx-lab\Run-BmxPostFallRemount.ps1','contrib\bmx-lab\Start-BmxGame.ps1','build\bmx\run_native_chassis_ride.ps1','build\bmx\run_native_impact_test.ps1','build\bmx\run_native_getup_test.ps1','build\bmx\getup-parser-tests.ps1',
    'Engine\Game\Build\20260929\offboard_flight.h','Engine\Game\Build\20260929\skater_state.h',
    'Extension\Skater\offboard_flight.cpp','Extension\Skater\offboard_flight.h'
    'Engine\Game\BMX\bmx_rider_posture_presentation.h','Engine\Game\BMX\bmx_pose_comparison_policy.h','contrib\bmx-lab\PoseComparisonEvidence.ps1','contrib\bmx-lab\Test-PoseComparisonEvidence.ps1','contrib\bmx-lab\FirstMountProducerEvidence.ps1','contrib\bmx-lab\Test-FirstMountProducerEvidence.ps1'
)
$bmxManifest = @()
. (Join-Path $bmxRepoRoot 'contrib\bmx-lab\BoardHandoffEvidence.ps1')
$bmxSources += @(Get-BmxBoardHandoffSourcePins)
foreach ($bmxRelative in $bmxSources) {
    $bmxSource = Join-Path $bmxRepoRoot $bmxRelative
    $bmxInfo = Get-Item -LiteralPath $bmxSource
    # Launcher and supervisor scripts are interpreted at launch, not compiled into the DLL.
    if ($bmxInfo.Extension -ne '.ps1' -and $bmxInfo.LastWriteTimeUtc -gt $bmxDllModified) {
        throw ('Source is newer than the runtime: ' + $bmxRelative)
    }
    $bmxDestination = Join-Path $bmxSourceDirectory $bmxRelative
    New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($bmxDestination)) | Out-Null
    Copy-Item -LiteralPath $bmxSource -Destination $bmxDestination
    $bmxSha = (Get-FileHash -LiteralPath $bmxSource -Algorithm SHA256).Hash
    if ((Get-FileHash -LiteralPath $bmxDestination -Algorithm SHA256).Hash -ne $bmxSha) { throw 'Archived source differs from input.' }
    $bmxManifest += [ordered]@{ path=$bmxRelative; bytes=$bmxInfo.Length; sha256=$bmxSha; modified_utc=$bmxInfo.LastWriteTimeUtc.ToString('o') }
}
$bmxManifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $bmxRunDirectory 'source-manifest.json')
$bmxArtifactManifest = @()
foreach ($bmxName in @('ReSkate.dll','ReSkateLauncher.exe')) {
    $bmxSource = Join-Path $bmxArtifacts $bmxName
    $bmxDestination = Join-Path $bmxRunDirectory $bmxName
    Copy-Item -LiteralPath $bmxSource -Destination $bmxDestination
    $bmxSha = (Get-FileHash -LiteralPath $bmxSource -Algorithm SHA256).Hash
    if ((Get-FileHash -LiteralPath $bmxDestination -Algorithm SHA256).Hash -ne $bmxSha) { throw 'Archived artifact differs from input.' }
    $bmxArtifactManifest += [ordered]@{ name=$bmxName; bytes=(Get-Item -LiteralPath $bmxSource).Length; sha256=$bmxSha }
}
$bmxBuildLogHash=(Get-FileHash -LiteralPath $bmxBuildLog -Algorithm SHA256).Hash
Copy-Item -LiteralPath $bmxBuildLog -Destination (Join-Path $bmxRunDirectory 'native-build.log')
if((Get-FileHash -LiteralPath (Join-Path $bmxRunDirectory 'native-build.log') -Algorithm SHA256).Hash -ne $bmxBuildLogHash){throw 'Archived build log differs from supplied provenance.'}
if (Test-Path -LiteralPath $bmxReport) {
    Move-Item -LiteralPath $bmxReport -Destination (Join-Path $bmxRunDirectory 'previous-bmx-selftest.json')
}
if (Test-Path -LiteralPath $bmxImpactReport) {
    Move-Item -LiteralPath $bmxImpactReport -Destination (Join-Path $bmxRunDirectory 'previous-bmx-impact-test.json')
}
if (Test-Path -LiteralPath $bmxActionReport) {
    Move-Item -LiteralPath $bmxActionReport -Destination (Join-Path $bmxRunDirectory 'previous-bmx-getup-action-test.json')
}
foreach($phaseOld in @(@($bmxPhaseReport,'previous-bmx-skeleton-phase-observation.json'),@(($bmxPhaseReport+'.tmp'),'previous-bmx-skeleton-phase-observation.json.tmp'))){if(Test-Path -LiteralPath $phaseOld[0]){Move-Item -LiteralPath $phaseOld[0] -Destination (Join-Path $bmxRunDirectory $phaseOld[1])}}
foreach($producerOld in @(@($bmxProducerReport,'previous-bmx-first-mount-producer-observation.json'),@(($bmxProducerReport+'.tmp'),'previous-bmx-first-mount-producer-observation.json.tmp'))){if(Test-Path -LiteralPath $producerOld[0]){Move-Item -LiteralPath $producerOld[0] -Destination (Join-Path $bmxRunDirectory $producerOld[1])}}
$bmxOwnedPid = 0
$bmxOwnedCreation = $null
$bmxLaunchAttempted = $false
$bmxStart = Get-Date
$bmxMonitoringSeconds=260+$bmxCaptureExtraSeconds
$bmxMonitorDeadline = $bmxStart.AddSeconds($bmxMonitoringSeconds)
$bmxHardDeadline = $bmxStart.AddSeconds($bmxMonitoringSeconds+10)
$bmxExpectedVariant = if ($Variant -eq 'inverted') { 1 } else { 2 }
$bmxRunInfo = [ordered]@{
    recording_gate_enabled=[bool]$CaptureGateDirectory;recording_gate_directory=$CaptureGateDirectory;recording_gate_nonce=$CaptureGateNonce;recording_gate_state=if($CaptureGateDirectory){'awaiting_native_world'}else{'disabled'};capture_trace_settle_completed=$false
    upstream_git_head=(git rev-parse HEAD); branch=(git branch --show-current)
    started_utc=$bmxStart.ToUniversalTime().ToString('o'); game_exe=$bmxGameExe; profile=$bmxProfile
    runtime_sha256=$bmxArtifactManifest[0].sha256; launcher_sha256=$bmxArtifactManifest[1].sha256
    build_log_path=$bmxBuildLog;build_log_sha256=$bmxBuildLogHash
    game_sha256=(Get-FileHash -LiteralPath $bmxGameExe -Algorithm SHA256).Hash
    native_chassis=$true; unattended=$true; impact_variant=$Variant; impact_variant_id=$bmxExpectedVariant; intent_observer=$false
    intent_activation=$false; getup_pulse_lab=$true; postfall_remount_lab=$true
    phase_probe_requested=[bool]$PhaseProbe;phase_observation=$null;phase_checkpoint=$null;phase_observation_error=$null
    first_mount_producer_requested=[bool]$FirstMountProducerProbe;first_mount_producer_observation=$null;first_mount_producer_checkpoint=$null;first_mount_producer_observation_error=$null
    rider_pose_comparison_mode=$RiderPoseMode;rider_pose_comparison_observation=$null;rider_pose_comparison_error=$null
    motion_basis_mode=$MotionBasisMode;motion_basis_observation=$null;motion_basis_observation_error=$null
    ordinary_riding_falls=$true; ride_trace=$true
    wall_limit_seconds=($bmxMonitoringSeconds+10); monitoring_limit_seconds=$bmxMonitoringSeconds
    owned_pid=0; owned_created_utc=$null; outcome='launching'; report_process_identity='no_report'
    report_passed=$null; process_teardown_verified=$false; source_unchanged_after_run=$false
    native_fall_passed=$null; pulse_passed=$null; standability_observed=$null; remount_passed=$null; passed=$false
    recovery_mapping_verified=$false; fall_trace_artifacts=@()
    refusal_terminal=$null
    native_terminal_file='native-terminal.json';native_terminal_written=$false;native_terminal_sha256=$null;native_terminal_utc=$null;native_terminal_passed=$false
    teardown_absence_observations=0; source_manifest='source-manifest.json'; artifacts=$bmxArtifactManifest
    ended_utc=$null
}
function Get-BmxOwnedCandidate {
    @(Get-CimInstance Win32_Process -Filter "Name='Skate.exe'" -ErrorAction Stop | Where-Object {
        $_.ExecutablePath -and $_.ExecutablePath.Equals($bmxGameExe,[StringComparison]::OrdinalIgnoreCase) -and
        $_.CreationDate -and $_.CreationDate.ToUniversalTime() -ge $bmxStart.ToUniversalTime().AddSeconds(-1)
    })
}
function Assert-BmxOwnedProcess($bmxProcess) {
    if (-not $bmxProcess.ExecutablePath -or -not $bmxProcess.ExecutablePath.Equals($bmxGameExe,[StringComparison]::OrdinalIgnoreCase) -or
        -not $bmxProcess.CreationDate -or $bmxProcess.CreationDate.ToUniversalTime().Ticks -ne $bmxOwnedCreation.Ticks) {
        throw 'PID identity changed; refusing process termination.'
    }
}
function Get-BmxOwnedStopDisposition($bmxProcess) {
    if (-not $bmxProcess) { return 'absent' }
    # A dying CIM record can lose path/creation fields before disappearing.
    # Missing identity is presence, never authority to terminate or count absence.
    if ($bmxProcess.CreationDate -and $bmxProcess.CreationDate.ToUniversalTime().Ticks -ne $bmxOwnedCreation.Ticks) {
        throw 'PID creation identity changed; refusing process termination.'
    }
    if ($bmxProcess.ExecutablePath -and -not $bmxProcess.ExecutablePath.Equals($bmxGameExe,[StringComparison]::OrdinalIgnoreCase)) {
        throw 'PID executable identity changed; refusing process termination.'
    }
    if (-not $bmxProcess.ExecutablePath -or -not $bmxProcess.CreationDate) { return 'wait' }
    return 'stop'
}
function Get-BmxPostStopAbsenceCount($bmxProcess,[int]$bmxPreviousAbsences) {
    if (-not $bmxProcess) { return ($bmxPreviousAbsences + 1) }
    # A dying process can retain a CIM record after ExecutablePath disappears.
    # Presence, including missing identity fields, never counts as absence.
    # A newly observed different creation time means PID reuse, not our process.
    if ($bmxProcess.CreationDate -and $bmxProcess.CreationDate.ToUniversalTime().Ticks -ne $bmxOwnedCreation.Ticks) {
        throw 'PID creation identity changed after termination; refusing teardown verification.'
    }
    return 0
}
. (Join-Path $PSScriptRoot 'PostFallRemountEvidence.ps1')
. (Join-Path $PSScriptRoot 'PhaseProbeEvidence.ps1')
. (Join-Path $PSScriptRoot 'FirstMountProducerEvidence.ps1')
$bmxImpactArchived=$false
$bmxActionArchived=$false
$bmxGetupProgress=$null
try {
    $bmxLaunchAttempted=$true
    & (Join-Path $bmxRepoRoot 'contrib\bmx-lab\Start-BmxGame.ps1') @bmxCaptureLaunchArgs -PostFallRemountTest -PhaseProbe:$PhaseProbe -FirstMountProducerProbe:$FirstMountProducerProbe -RiderPoseMode $RiderPoseMode -MotionBasisMode $MotionBasisMode -GetUpPulse -NativeChassis -UnattendedTest -ImpactTest $Variant -ProfileName bmx-user-data-chassis-probe
    while((Get-Date) -lt $bmxMonitorDeadline){
        $bmxProcesses=@(Get-BmxOwnedCandidate)
        if($bmxProcesses.Count -gt 1){throw 'Multiple isolated game processes appeared; ownership is ambiguous.'}
        if($bmxProcesses.Count -eq 1 -and -not $bmxOwnedPid){
            $bmxOwnedPid=[int]$bmxProcesses[0].ProcessId;$bmxOwnedCreation=$bmxProcesses[0].CreationDate.ToUniversalTime()
            $bmxRunInfo.owned_pid=$bmxOwnedPid;$bmxRunInfo.owned_created_utc=$bmxOwnedCreation.ToString('o')
            foreach($bmxInstalledArtifact in $bmxArtifactManifest){
                if((Get-FileHash -LiteralPath (Join-Path ([IO.Path]::GetDirectoryName($bmxGameExe)) $bmxInstalledArtifact.name) -Algorithm SHA256).Hash -ne $bmxInstalledArtifact.sha256){throw 'Installed artifact differs from archived build.'}
            }
            Write-Output "Owned isolated Skate PID $bmxOwnedPid"
        }
        if($bmxOwnedPid -and $bmxProcesses.Count -eq 1){
            if([int]$bmxProcesses[0].ProcessId -ne $bmxOwnedPid){throw 'A different isolated process replaced the owned run.'}
            Assert-BmxOwnedProcess $bmxProcesses[0]
        }
        if($bmxOwnedPid -and $bmxProcesses.Count -eq 0){$bmxRunInfo.outcome='game_exited_before_terminal_report';break}
        if ($bmxOwnedPid -and $CaptureGateDirectory) {
            $bmxRunInfo.recording_gate_state=Update-BmxCaptureGateControl $CaptureGateDirectory $CaptureGateNonce $bmxOwnedPid $bmxOwnedCreation (Join-Path $bmxRunDirectory 'control.json')
            if ($bmxRunInfo.recording_gate_state -eq 'timed_out') { $bmxRunInfo.outcome='recording_gate_timeout'; break }
        }
        if($PhaseProbe -and -not $bmxRunInfo.phase_observation -and $bmxOwnedPid -and (Test-Path -LiteralPath $bmxPhaseReport)){
            $phaseFile=Get-Item -LiteralPath $bmxPhaseReport
            $phaseText=Get-Content -Raw -LiteralPath $bmxPhaseReport
            $phase=ConvertFrom-BmxStrictJson $phaseText
            $phaseIdentity=Test-BmxPhaseEnvelope $phase $bmxOwnedPid $bmxOwnedCreation $bmxStart.ToUniversalTime() $phaseFile.LastWriteTimeUtc
            $phaseHash=(Get-FileHash -LiteralPath $bmxPhaseReport -Algorithm SHA256).Hash
            $phaseTarget=Join-Path $bmxRunDirectory 'bmx-skeleton-phase-observation.json'
            Copy-Item -LiteralPath $bmxPhaseReport -Destination $phaseTarget
            if((Get-FileHash -LiteralPath $phaseTarget -Algorithm SHA256).Hash -ne $phaseHash -or (Get-FileHash -LiteralPath $bmxPhaseReport -Algorithm SHA256).Hash -ne $phaseHash){throw 'Phase report changed during archive.'}
            $bmxRunInfo.phase_observation=[ordered]@{path='bmx-skeleton-phase-observation.json';sha256=$phaseHash;bytes=$phaseFile.Length;source_modified_utc=$phaseFile.LastWriteTimeUtc.ToString('o');identity=$phaseIdentity}
        }
        if($FirstMountProducerProbe -and -not $bmxRunInfo.first_mount_producer_observation -and $bmxOwnedPid -and (Test-Path -LiteralPath $bmxProducerReport)){
            $producerFile=Get-Item -LiteralPath $bmxProducerReport
            $producerRead=Read-BmxFirstMountProducerReport $bmxProducerReport
            $producer=ConvertFrom-BmxStrictJson $producerRead.text
            $producerIdentity=Test-BmxFirstMountProducerEnvelope $producer $bmxOwnedPid $bmxOwnedCreation $bmxStart.ToUniversalTime() $producerFile.LastWriteTimeUtc
            $producerTarget=Join-Path $bmxRunDirectory 'bmx-first-mount-producer-observation.json'
            Copy-Item -LiteralPath $bmxProducerReport -Destination $producerTarget
            if((Get-FileHash -LiteralPath $producerTarget -Algorithm SHA256).Hash -cne $producerRead.sha256 -or (Get-FileHash -LiteralPath $bmxProducerReport -Algorithm SHA256).Hash -cne $producerRead.sha256){throw 'Producer report changed between bounded parse and archive.'}
            $bmxRunInfo.first_mount_producer_observation=[ordered]@{path='bmx-first-mount-producer-observation.json';sha256=$producerRead.sha256;bytes=$producerRead.bytes;source_modified_utc=$producerFile.LastWriteTimeUtc.ToString('o');validator_completed_utc=[DateTime]::UtcNow.ToString('o');identity=$producerIdentity}
        }
        if(-not $bmxImpactArchived -and (Test-Path -LiteralPath $bmxImpactReport)){
            $bmxImpact=$null;try{$bmxImpact=ConvertFrom-BmxStrictJson (Get-Content -Raw -LiteralPath $bmxImpactReport)}catch{}
            if($bmxImpact -and $null -ne $bmxImpact.PSObject.Properties['passed']){
                if(-not $bmxOwnedPid -or (Get-Item -LiteralPath $bmxImpactReport).LastWriteTimeUtc -lt $bmxStart.ToUniversalTime()){throw 'Impact report lacks fresh owned process identity.'}
                $bmxRunInfo.native_fall_passed=Test-BmxImpactReport $bmxImpact $bmxOwnedPid $bmxExpectedVariant
                $bmxRunInfo.report_process_identity='verified';$bmxRunInfo.report_passed=$bmxRunInfo.native_fall_passed
                Copy-Item -LiteralPath $bmxImpactReport -Destination (Join-Path $bmxRunDirectory 'bmx-impact-test.json')
                $bmxImpactArchived=$true;$bmxRunInfo.outcome='awaiting_getup_action_and_full_lifecycle'
            }
        }
        if($bmxImpactArchived -and (Test-Path -LiteralPath $bmxActionReport)){
            $bmxAction=$null;try{$bmxAction=ConvertFrom-BmxStrictJson (Get-Content -Raw -LiteralPath $bmxActionReport)}catch{}
            if($bmxAction -and $bmxAction.completed -is [bool] -and $bmxAction.completed){
                if((Get-Item -LiteralPath $bmxActionReport).LastWriteTimeUtc -lt $bmxStart.ToUniversalTime() -or
                   $bmxAction.event -ne 'bmx_getup_action_test' -or -not (Test-BmxUnsignedInteger $bmxAction.process_id) -or $bmxAction.process_id -ne $bmxOwnedPid){throw 'Completed GetUp report lacks fresh owned identity.'}
                $bmxFreshFalls=@(Get-BmxFreshFallReports (Join-Path $bmxProfile 'bmx-traces') $bmxOwnedPid $bmxStart.ToUniversalTime())
                if($bmxFreshFalls.Count -gt 1){throw 'Multiple fresh owned fall reports are ambiguous.'}
                if($bmxFreshFalls.Count -eq 1 -and $bmxFreshFalls[0].report.window_complete -is [bool] -and $bmxFreshFalls[0].report.window_complete -and (Test-Path -LiteralPath $bmxLog)){
                    if($bmxAction.fall_trace -cne $bmxFreshFalls[0].file.Name){throw 'GetUp report is not bound to the fresh owned fall trace filename.'}
                    Copy-Item -LiteralPath $bmxActionReport -Destination (Join-Path $bmxRunDirectory 'bmx-getup-action-test.json')
                    $bmxActionArchived=$true
                    $bmxGetupProgress=Get-BmxPostFallRemountProgress (Get-Content -Raw -LiteralPath $bmxLog) $bmxOwnedPid $bmxExpectedVariant $bmxImpact $bmxAction $bmxFreshFalls[0].report $bmxStart
                    $bmxRunInfo.outcome=if($bmxGetupProgress.complete){'native_postfall_remount_evidence_observed'}else{'awaiting_postfall_remount: '+$bmxGetupProgress.issue}
                    if ($bmxGetupProgress.complete) {
                        Write-Output ('Completed post-fall remount evidence: '+$bmxGetupProgress.issue)
                        break
                    }
                    $bmxRefusalTerminal=Get-BmxPostFallRefusalTerminal (Get-Content -Raw -LiteralPath $bmxLog) $bmxOwnedPid $bmxExpectedVariant $bmxImpact $bmxAction $bmxFreshFalls[0].report $bmxStart
                    if($bmxRefusalTerminal.terminal_failure){
                        $bmxRunInfo.refusal_terminal=$bmxRefusalTerminal
                        $bmxRunInfo.outcome='native_postfall_recovery_refused'
                        Write-Output ('Completed failed post-fall trial: '+$bmxRefusalTerminal.issue)
                        break
                    }
                }
            }
        }
        Start-Sleep -Milliseconds 500
    }
    $bmxRunInfo.outcome=Get-BmxPostFallMonitorOutcome $bmxRunInfo.outcome ([bool]($bmxGetupProgress -and $bmxGetupProgress.complete)) (Get-Date) $bmxMonitorDeadline
}catch{
    $bmxRunInfo.outcome='supervisor_exception: '+$_.Exception.Message;Write-Output $bmxRunInfo.outcome
}finally{
    try {
        if ($bmxLaunchAttempted -and -not $bmxOwnedPid) {
            $bmxCandidates=@(Get-BmxOwnedCandidate)
            if ($bmxCandidates.Count -eq 1) {
                $bmxOwnedPid=[int]$bmxCandidates[0].ProcessId;$bmxOwnedCreation=$bmxCandidates[0].CreationDate.ToUniversalTime()
                $bmxRunInfo.owned_pid=$bmxOwnedPid;$bmxRunInfo.owned_created_utc=$bmxOwnedCreation.ToString('o')
            }
        }
    } catch { $bmxRunInfo.outcome+='; ownership_recovery_error: '+$_.Exception.Message }
    try {
        $bmxNativePassed=[bool]($bmxGetupProgress -and $bmxGetupProgress.complete -and $bmxRunInfo.native_fall_passed -eq $true -and
            (-not $CaptureGateDirectory -or $bmxRunInfo.recording_gate_state -eq 'released'))
        $bmxTerminalAt=[DateTime]::UtcNow
        $bmxTerminalIssue=if($bmxGetupProgress){[string]$bmxGetupProgress.issue}else{''}
        $bmxTerminal=New-BmxNativeTerminalReceipt $bmxOwnedPid $bmxOwnedCreation $bmxTerminalAt $bmxNativePassed $bmxRunInfo.outcome $bmxRunInfo.runtime_sha256 $bmxTerminalIssue
        $bmxTerminalWritten=Write-BmxNativeTerminalReceipt $bmxRunDirectory $bmxTerminal
        $bmxRunInfo.native_terminal_written=$true;$bmxRunInfo.native_terminal_sha256=$bmxTerminalWritten.sha256
        $bmxRunInfo.native_terminal_utc=$bmxTerminal.terminal_utc;$bmxRunInfo.native_terminal_passed=$bmxTerminal.passed
        Write-Output ('Native terminal receipt published before capture settle: '+$bmxTerminalWritten.path)
        # Give the continuous recorder time to observe the atomic terminal
        # receipt and capture its final frame before owned process termination.
        # The hard deadline retains >=2 seconds for identity-checked teardown.
        if ($CaptureGateDirectory -and $bmxRunInfo.recording_gate_state -eq 'released' -and $bmxOwnedPid -and
            (Get-Date).AddMilliseconds(7500) -lt $bmxHardDeadline) {
            Start-Sleep -Milliseconds 5500
            $bmxRunInfo.capture_trace_settle_completed=$true
        }
    } catch { $bmxRunInfo['native_terminal_error']=$_.Exception.Message }
    try {
        if ($bmxOwnedPid) {
            $bmxEmptyObservations=0; $bmxStopRequested=$false
            do {
                # Failed queries throw. A present identity-incomplete dying record
                # resets absence count and never authorizes Stop-Process.
                $bmxStillPresent=Get-CimInstance Win32_Process -Filter "ProcessId=$bmxOwnedPid" -ErrorAction Stop
                $bmxDisposition=Get-BmxOwnedStopDisposition $bmxStillPresent
                if ($bmxDisposition -eq 'stop' -and -not $bmxStopRequested) {
                    $bmxStopRequested=$true
                    try { Stop-Process -Id $bmxOwnedPid -Force -ErrorAction Stop }
                    catch { $bmxRunInfo['teardown_stop_error']=$_.Exception.Message }
                    # Even a failed/racing stop does not establish absence; use
                    # only subsequent successful CIM observations below.
                }
                $bmxEmptyObservations=Get-BmxPostStopAbsenceCount $bmxStillPresent $bmxEmptyObservations
                $bmxRunInfo.teardown_absence_observations=$bmxEmptyObservations
                if ($bmxEmptyObservations -ge 2) { $bmxRunInfo.process_teardown_verified=$true; break }
                Start-Sleep -Milliseconds 250
            } while ((Get-Date) -lt $bmxHardDeadline)
        }
    } catch { $bmxRunInfo.outcome+='; teardown_error: '+$_.Exception.Message }
    foreach($bmxEntry in @(@($bmxReport,'bmx-selftest-final.json'),@($bmxImpactReport,'bmx-impact-test-final.json'),@($bmxActionReport,'bmx-getup-action-test-final.json'),@($bmxLog,'ReSkate.log'))){
        try{if(Test-Path -LiteralPath $bmxEntry[0]){Copy-Item -LiteralPath $bmxEntry[0] -Destination (Join-Path $bmxRunDirectory $bmxEntry[1])}}
        catch{$bmxRunInfo['evidence_archive_error']=$_.Exception.Message}
    }
    try{
        if($bmxOwnedPid){
            $bmxTraceArchive=Join-Path $bmxRunDirectory 'bmx-traces';New-Item -ItemType Directory -Path $bmxTraceArchive -Force|Out-Null
            foreach($bmxFreshFall in @(Get-BmxFreshFallReports (Join-Path $bmxProfile 'bmx-traces') $bmxOwnedPid $bmxStart.ToUniversalTime())){
                $bmxFallPath=Join-Path $bmxTraceArchive $bmxFreshFall.file.Name;$bmxFallHash=(Get-FileHash -LiteralPath $bmxFreshFall.file.FullName -Algorithm SHA256).Hash
                Copy-Item -LiteralPath $bmxFreshFall.file.FullName -Destination $bmxFallPath
                if((Get-FileHash -LiteralPath $bmxFallPath -Algorithm SHA256).Hash -ne $bmxFallHash){throw 'Fall trace changed during archival.'}
                $bmxRunInfo.fall_trace_artifacts+=[ordered]@{path=('bmx-traces/'+$bmxFreshFall.file.Name);sha256=$bmxFallHash;bytes=(Get-Item -LiteralPath $bmxFallPath).Length;source_modified_utc=$bmxFreshFall.file.LastWriteTimeUtc.ToString('o')}
            }
        }
    }catch{$bmxRunInfo.fall_trace_artifacts=@();$bmxRunInfo['fall_trace_archive_error']=$_.Exception.Message}
    try{
    if($PhaseProbe -and -not $bmxRunInfo.phase_observation -and $bmxOwnedPid -and (Test-Path -LiteralPath $bmxPhaseReport)){
        $phaseFile=Get-Item -LiteralPath $bmxPhaseReport
        $phaseText=Get-Content -Raw -LiteralPath $bmxPhaseReport
        $phase=ConvertFrom-BmxStrictJson $phaseText
        $phaseIdentity=Test-BmxPhaseEnvelope $phase $bmxOwnedPid $bmxOwnedCreation $bmxStart.ToUniversalTime() $phaseFile.LastWriteTimeUtc
        $phaseHash=(Get-FileHash -LiteralPath $bmxPhaseReport -Algorithm SHA256).Hash
        $phaseTarget=Join-Path $bmxRunDirectory 'bmx-skeleton-phase-observation.json'
        Copy-Item -LiteralPath $bmxPhaseReport -Destination $phaseTarget
        if((Get-FileHash -LiteralPath $phaseTarget -Algorithm SHA256).Hash -ne $phaseHash -or (Get-FileHash -LiteralPath $bmxPhaseReport -Algorithm SHA256).Hash -ne $phaseHash){throw 'Phase report changed during archive.'}
        $bmxRunInfo.phase_observation=[ordered]@{path='bmx-skeleton-phase-observation.json';sha256=$phaseHash;bytes=$phaseFile.Length;source_modified_utc=$phaseFile.LastWriteTimeUtc.ToString('o');identity=$phaseIdentity}
    }
    }catch{$bmxRunInfo.phase_observation_error=$_.Exception.Message}
    try{
        if($FirstMountProducerProbe -and -not $bmxRunInfo.first_mount_producer_observation -and $bmxOwnedPid -and (Test-Path -LiteralPath $bmxProducerReport)){
            $producerFile=Get-Item -LiteralPath $bmxProducerReport
            $producerRead=Read-BmxFirstMountProducerReport $bmxProducerReport
            $producer=ConvertFrom-BmxStrictJson $producerRead.text
            $producerIdentity=Test-BmxFirstMountProducerEnvelope $producer $bmxOwnedPid $bmxOwnedCreation $bmxStart.ToUniversalTime() $producerFile.LastWriteTimeUtc
            $producerTarget=Join-Path $bmxRunDirectory 'bmx-first-mount-producer-observation.json'
            Copy-Item -LiteralPath $bmxProducerReport -Destination $producerTarget
            if((Get-FileHash -LiteralPath $producerTarget -Algorithm SHA256).Hash -cne $producerRead.sha256 -or (Get-FileHash -LiteralPath $bmxProducerReport -Algorithm SHA256).Hash -cne $producerRead.sha256){throw 'Producer report changed between bounded parse and archive.'}
            $bmxRunInfo.first_mount_producer_observation=[ordered]@{path='bmx-first-mount-producer-observation.json';sha256=$producerRead.sha256;bytes=$producerRead.bytes;source_modified_utc=$producerFile.LastWriteTimeUtc.ToString('o');validator_completed_utc=[DateTime]::UtcNow.ToString('o');identity=$producerIdentity}
        }
    }catch{$bmxRunInfo.first_mount_producer_observation_error=$_.Exception.Message}
    $bmxRunInfo.source_unchanged_after_run=$true
    try{foreach($bmxEntry in $bmxManifest){if((Get-FileHash -LiteralPath (Join-Path $bmxRepoRoot $bmxEntry.path) -Algorithm SHA256).Hash -ne $bmxEntry.sha256){$bmxRunInfo.source_unchanged_after_run=$false}}}
    catch{$bmxRunInfo.source_unchanged_after_run=$false;$bmxRunInfo['source_verification_error']=$_.Exception.Message}
    $bmxRunInfo.ended_utc=(Get-Date).ToUniversalTime().ToString('o')
    # Acceptance is recomputed from archived evidence after owned teardown.
    $bmxGetupProgress=$null
    try{
        if($bmxImpactArchived -and $bmxActionArchived -and $bmxRunInfo.fall_trace_artifacts.Count -eq 1){
            $bmxArchivedImpact=ConvertFrom-BmxStrictJson (Get-Content -Raw -LiteralPath (Join-Path $bmxRunDirectory 'bmx-impact-test.json'))
            $bmxArchivedAction=ConvertFrom-BmxStrictJson (Get-Content -Raw -LiteralPath (Join-Path $bmxRunDirectory 'bmx-getup-action-test.json'))
            $bmxArchivedTrace=ConvertFrom-BmxStrictJson (Get-Content -Raw -LiteralPath (Join-Path $bmxRunDirectory $bmxRunInfo.fall_trace_artifacts[0].path))
            if($bmxArchivedAction.fall_trace -cne [IO.Path]::GetFileName($bmxRunInfo.fall_trace_artifacts[0].path)){throw 'Archived action names a different fall trace.'}
            $bmxGetupProgress=Get-BmxPostFallRemountProgress (Get-Content -Raw -LiteralPath (Join-Path $bmxRunDirectory 'ReSkate.log')) $bmxOwnedPid $bmxExpectedVariant $bmxArchivedImpact $bmxArchivedAction $bmxArchivedTrace $bmxStart
        }
    }catch{$bmxGetupProgress=[pscustomobject]@{complete=$false;pulse_passed=$false;standability_observed=$false;issue=('Archived evidence unreadable: '+$_.Exception.Message)}}
    if($PhaseProbe){
        try{
            if(-not $bmxRunInfo.phase_observation){throw 'Requested phase report was not observed before owned teardown.'}
            if((Get-FileHash -LiteralPath (Join-Path $bmxRunDirectory $bmxRunInfo.phase_observation.path) -Algorithm SHA256).Hash -ne $bmxRunInfo.phase_observation.sha256){throw 'Archived phase report hash changed.'}
            $phaseArchived=ConvertFrom-BmxStrictJson (Get-Content -Raw -LiteralPath (Join-Path $bmxRunDirectory $bmxRunInfo.phase_observation.path))
            [void](Test-BmxPhaseEnvelope $phaseArchived $bmxOwnedPid $bmxOwnedCreation $bmxStart.ToUniversalTime() ([DateTime]::Parse($bmxRunInfo.phase_observation.source_modified_utc)))
            if($bmxImpactArchived -and $bmxActionArchived -and $bmxRunInfo.fall_trace_artifacts.Count -eq 1){
                $bmxRunInfo.phase_checkpoint=Get-BmxPhaseCheckpoint (Get-Content -Raw -LiteralPath (Join-Path $bmxRunDirectory 'ReSkate.log')) $bmxOwnedPid $bmxExpectedVariant $bmxArchivedImpact $bmxArchivedAction $bmxArchivedTrace $bmxStart ([DateTime]::Parse($bmxRunInfo.phase_observation.source_modified_utc))
            }
        }catch{$bmxRunInfo.phase_observation_error=$_.Exception.Message}
    }
    if($FirstMountProducerProbe){
        try{
            if(-not $bmxRunInfo.first_mount_producer_observation){throw 'Requested producer report was not observed before owned teardown.'}
            $producerArchivePath=Join-Path $bmxRunDirectory 'bmx-first-mount-producer-observation.json'
            $producerRead=Read-BmxFirstMountProducerReport $producerArchivePath
            if($producerRead.sha256 -cne $bmxRunInfo.first_mount_producer_observation.sha256){throw 'Archived producer report hash changed.'}
            $producerArchived=ConvertFrom-BmxStrictJson $producerRead.text
            [void](Test-BmxFirstMountProducerEnvelope $producerArchived $bmxOwnedPid $bmxOwnedCreation $bmxStart.ToUniversalTime() ([DateTime]::Parse($bmxRunInfo.first_mount_producer_observation.source_modified_utc)))
            $bmxRunInfo.first_mount_producer_checkpoint=Get-BmxPhaseCheckpoint (Get-Content -Raw -LiteralPath (Join-Path $bmxRunDirectory 'ReSkate.log')) $bmxOwnedPid $bmxExpectedVariant $bmxArchivedImpact $bmxArchivedAction $bmxArchivedTrace $bmxStart ([DateTime]::Parse($bmxRunInfo.first_mount_producer_observation.source_modified_utc))
            if(-not $bmxRunInfo.first_mount_producer_checkpoint.dump_before_interaction -or -not $bmxRunInfo.first_mount_producer_checkpoint.file_written_before_interaction){throw 'Producer export lacks the owned phase-dump-before-interaction ordering checkpoint.'}
            # This checks report write ordering after the fact. Existing queued
            # startup commands are unchanged; validator timing is NOT an input
            # barrier and can be later than the interaction. Coverage may be0.
        }catch{$bmxRunInfo.first_mount_producer_observation_error=$_.Exception.Message}
    }
    try{
        $comparisonLog=Join-Path $bmxRunDirectory 'ReSkate.log'
        $bmxRunInfo.rider_pose_comparison_observation=Get-BmxPoseComparisonEvidence (Get-Content -Raw -LiteralPath $comparisonLog) $RiderPoseMode ([bool]($PhaseProbe -and $FirstMountProducerProbe)) $bmxOwnedPid $bmxOwnedCreation $bmxStart ([DateTime]::UtcNow)
    }catch{$bmxRunInfo.rider_pose_comparison_error=$_.Exception.Message}
    try{
        $bmxRunInfo.motion_basis_observation=Get-BmxMotionBasisEvidence (Get-Content -Raw -LiteralPath (Join-Path $bmxRunDirectory 'ReSkate.log')) $MotionBasisMode $bmxOwnedPid $bmxOwnedCreation $bmxStart ([DateTime]::UtcNow)
    }catch{$bmxRunInfo.motion_basis_observation_error=$_.Exception.Message}
    [ordered]@{event='bmx_private_motion_basis_measurement';mode=$MotionBasisMode;process_id=$bmxOwnedPid;owned_created_utc=$bmxRunInfo.owned_created_utc;
        runtime_sha256=$bmxRunInfo.runtime_sha256;measurement_valid=(Test-BmxMotionBasisMeasurement $bmxRunInfo $MotionBasisMode);
        observation=$bmxRunInfo.motion_basis_observation;issue=$bmxRunInfo.motion_basis_observation_error;
        native_completion_proved=$false;physics_quality_proved=$false;visual_acceptance_proved=$false;
        boundary='Private basis-selection measurement only. The ordinary PostFall acceptance below is unchanged and None still requires its original pose criteria.'
    }|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $bmxRunDirectory 'bmx-native-motion-basis-measurement.json')
    $bmxRunInfo.pulse_passed=$null -ne $bmxGetupProgress -and $bmxGetupProgress.pulse_passed
    $bmxRunInfo.standability_observed=$null -ne $bmxGetupProgress -and $bmxGetupProgress.standability_observed
    $bmxRunInfo.remount_passed=$null -ne $bmxGetupProgress -and $bmxGetupProgress.remount_passed
    $bmxRunInfo.passed=$null -ne $bmxGetupProgress -and $bmxGetupProgress.complete -and
        $bmxRunInfo.native_fall_passed -eq $true -and $bmxRunInfo.process_teardown_verified -and $bmxRunInfo.source_unchanged_after_run -and $bmxRunInfo.native_terminal_written -and
        (-not $CaptureGateDirectory -or $bmxRunInfo.recording_gate_state -eq 'released') -and
        (-not $PhaseProbe -or ($bmxRunInfo.phase_observation -and -not $bmxRunInfo.phase_observation_error)) -and
        (-not $FirstMountProducerProbe -or ($bmxRunInfo.first_mount_producer_observation -and -not $bmxRunInfo.first_mount_producer_observation_error)) -and
        ($bmxRunInfo.rider_pose_comparison_observation -and -not $bmxRunInfo.rider_pose_comparison_error)
    if($bmxRunInfo.passed){$bmxRunInfo.outcome='native_postfall_remount_passed'}
    [ordered]@{event='bmx_native_postfall_remount_test';completed=$true;passed=[bool]$bmxRunInfo.passed;process_id=$bmxOwnedPid;owned_created_utc=$bmxRunInfo.owned_created_utc
        runtime_sha256=$bmxRunInfo.runtime_sha256;native_fall_passed=$bmxRunInfo.native_fall_passed;pulse_passed=$bmxRunInfo.pulse_passed;standability_observed=$bmxRunInfo.standability_observed
        phase_observation=$bmxRunInfo.phase_observation;phase_checkpoint=$bmxRunInfo.phase_checkpoint;phase_observation_error=$bmxRunInfo.phase_observation_error
        first_mount_producer_observation=$bmxRunInfo.first_mount_producer_observation;first_mount_producer_checkpoint=$bmxRunInfo.first_mount_producer_checkpoint;first_mount_producer_observation_error=$bmxRunInfo.first_mount_producer_observation_error
        rider_pose_comparison_mode=$RiderPoseMode;rider_pose_comparison_observation=$bmxRunInfo.rider_pose_comparison_observation;rider_pose_comparison_error=$bmxRunInfo.rider_pose_comparison_error
        proof=$bmxGetupProgress;fall_trace_artifacts=$bmxRunInfo.fall_trace_artifacts;outcome=$bmxRunInfo.outcome;source_unchanged_after_run=$bmxRunInfo.source_unchanged_after_run
        process_teardown_verified=$bmxRunInfo.process_teardown_verified;teardown_absence_observations=$bmxRunInfo.teardown_absence_observations
        boundary='One private typed GetUp followed by explicit console interaction and independently observed native remount. Production controller input, full GetUp script guards, carry lifecycle and visual quality are not established.'
    }|ConvertTo-Json -Depth 14|Set-Content -LiteralPath (Join-Path $bmxRunDirectory 'bmx-native-postfall-remount-test.json')
    $bmxRunInfo|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $bmxRunDirectory 'run.json')
    Write-Output ($bmxRunInfo|ConvertTo-Json -Depth 8 -Compress)
}


