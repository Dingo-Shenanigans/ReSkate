#requires -Version 7.0
# PowerShell 7 is required for System.Text.Json duplicate-property validation.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'PostFallRemountEvidence.ps1')
# Synthetic fixtures only. No supervisor, launcher, game, capture or OS input runs.
function Assert-Guard([bool]$value,[string]$message){if(-not $value){throw $message}}
function Clone-Guard($value){ConvertFrom-BmxStrictJson ($value | ConvertTo-Json -Depth 40 -Compress)}
function Guard-Status([string]$when){
    '['+$when+'] [1234:77] SDK: [Console] [INFO] BMX crash. Native velocity updates: 33; native bike parts: 8/8; placements: 2000; native physics state: 504; BMX capture: released; chassis mounts=1; allocations=1; active=false; parked=false; force applications=33; body world=999; rider pose updates=66; bike mode=crashed; rider mode=none; setdown verified=false; body token=1. pose listener=ready, rider render updates=33, hand/foot contact=0/0'
}
function New-GuardScenario {
    $identity=[ordered]@{client=1;entity=2;core=3;context=4;rig=5;world=999;native_state=7;body_index=8;body_generation=9;body_token=1}
    $counts=[ordered]@{velocity_updates=33;rider_pose_updates=66;rider_render_updates=33;force_applications=33}
    $impact=[ordered]@{process_id=1234;completed=$true;variant=2;bail=$true;reason=2;region=1;body_token=1;serial=34
        com=@(300,270,-470);quaternion=@(0,0,0,1);velocity=@(0,-4,0);body_alignment=.04;closing_speed=5;guard_rays=74
        native_fall_consumed=$true;native_ragdoll_observed=$true;native_calls=0;issue='verified';chassis_active=$false;elapsed_ms=31;passed=$true
        accepted_request=$true;request_matches=$true;activation_enabled=$true;intent_status_available=$true;request_id=1;expected_request_id=1;intent_generation=1;expected_generation=1
        intent_armed=$true;intent_prior_false=$true;intent_true_returned=$true;intent_true_readback=$true;intent_written=$true;intent_reset=$true;intent_reset_readback=$true
        intent_reset_pending=$false;intent_owner_expired=$false;intent_native_cleared=$false;intent_true_serial=34;intent_reset_serial=35;intent_true_calls=1;intent_reset_calls=1;intent_native_calls=2}
    $guard=[ordered]@{process_id=1234;decision=1;rider_available=$true;body_available=$true;coherent=$true;standable=$false;offboard=$true;detached=$true
        drive_active=$false;board_timer=$false;bmx_capture=$false;interaction_capture=$false;rider_motion_lease=$false
        serial=934;native_time=(34/60.0+15);step_seconds=(1/60.0);parts=8;mounts=1;allocations=1
        velocity_updates=33;rider_pose_updates=66;rider_render_updates=33;force_applications=33;identity=$identity;expected=(Clone-Guard $identity)}
    $samples=for($index=0;$index -le 300;++$index){
        [ordered]@{serial=(34+6*$index);native_time=(34/60.0+$index/10.0);step_seconds=(1/60.0);elapsed=($index/10.0);snapshot_issue=0
            rider=[ordered]@{offboard=$true;standable=$false;getup_allowed_value=$null;getup_intent_value=$null}
            body=[ordered]@{com=@(300,270,-470);velocity=@(0,0,0);angular_velocity=@(0,0,0);quaternion=@(0,0,0,1)}
            completed_counters=(Clone-Guard $counts);capture_leases=[ordered]@{bmx=$false;interaction=$false;rider_motion=$false;bike_drive=$false;board_switch_pending=$false}}
    }
    $trace=[ordered]@{event='bmx_passive_fall_lifecycle';process_id=1234;window_complete=$true;reason_id=1;native_elapsed=30
        identity=(Clone-Guard $identity);request=[ordered]@{attempted=$true;accepted=$true;status_fresh=$true;status_available=$true;typed=$true;entity=2;id=1;generation=1;binding=1}
        counter_regression=$false;overwritten_samples=0;initial=[ordered]@{serial=34;native_time=(34/60.0)};samples=@($samples)}
    Clone-Guard ([ordered]@{impact=$impact;guard=$guard;trace=$trace;prefix_pid=1234;duplicate_guard=$false;raw_guard=$null;tail=''
        status_lines=@((Guard-Status '2026-10-19 02:00:19.000'),(Guard-Status '2026-10-19 02:00:21.000'))})
}
function New-GetupScenario {
    $s=New-GuardScenario
    foreach($row in $s.trace.samples){$row.rider|Add-Member native_protection $false;$row.rider|Add-Member known_ragdoll_substate $true;$row.rider|Add-Member native_cached_velocity @(0,0,0)}
    $preflight=Clone-Guard $s.trace.samples[120]
    $preflight|Add-Member eligible $true;$preflight|Add-Member issue 0
    $s.trace.initial=Clone-Guard $s.trace.samples[0]
    $s.trace|Add-Member canonical_getup_observation ([pscustomobject]@{entity=2;entity_handle=4611686018427396096;owner_changed=$false})
    for($index=130;$index -lt $s.trace.samples.Count;++$index){$s.trace.samples[$index].rider.standable=$true;$s.trace.samples[$index].rider.known_ragdoll_substate=$false;$s.trace.samples[$index].capture_leases.interaction=$true}
    $action=[ordered]@{event='bmx_getup_action_test';process_id=1234;completed=$true;accepted_request=$true;status_fresh=$true;status_available=$true
        request_id=1;fall_request_id=1;fall_generation=1;entity_handle=4611686018427396096;identity=(Clone-Guard $s.guard.identity)
        requested_serial=754;native_time=(34/60.0+12);allowed_serial=755;allowed_native_time=(34/60.0+12+1/60.0)
        down_before_action=$true;allowed_before_action=$true;pulse_passed=$true;standability_observed=$true
        baseline=[ordered]@{parts=8;mounts=1;allocations=1;velocity_updates=33;rider_pose_updates=66;rider_render_updates=33;force_applications=33}
        preflight=$preflight;pulse=[ordered]@{generation=1;prior_false=$true;true_returned=$true;true_readback=$true;intent_written=$true;intent_reset=$true;reset_readback=$true
            reset_pending=$false;owner_changed=$false;owner_expired=$false;native_cleared=$false;true_serial=755;reset_serial=756;true_calls=1;reset_calls=1}}
    Clone-Guard ([ordered]@{impact=$s.impact;action=$action;trace=$s.trace;status_lines=$s.status_lines;prefix_pid=1234;duplicate_request=$false;tail='';raw_json=$null})
}
function Mount-Status([string]$when,[int]$count){
    '['+$when+'] [1234:77] SDK: [Console] [INFO] BMX mounted. Native velocity updates: '+$count+'; native bike parts: 8/8; placements: '+(2000+$count)+'; native physics state: 504; BMX capture: held; chassis mounts=2; allocations=1; active=true; parked=false; force applications='+$count+'; body world=999; rider pose updates='+(66+$count)+'; bike mode=mounted; rider mode=riding; setdown verified=true; body token=2. pose listener=ready, rider render updates='+$count+', hand/foot contact=2/2'
}
function New-RemountScenario {
    $s=New-GetupScenario
    $s.status_lines=@((Guard-Status '2026-10-19 02:00:44.000'),(Guard-Status '2026-10-19 02:00:46.000'))
    $s|Add-Member complete_line '[2026-10-19 02:00:40.000] [1234:77] Native(C): [Skater] [INFO] BMX GetUp lab complete: pulse=true; native standing suffix=17s; report=test'
    $s|Add-Member recovery_line '[2026-10-19 02:01:00.000] [1234:77] Native(C): [Skater] [INFO] BMX crash recovery verified: body token=1; serial=2434; native elapsed=40s; grounded placement accepted.'
    $s|Add-Member begin_line '[2026-10-19 02:01:00.100] [1234:77] Native(C): [Skater] [INFO] BMX natural mount begin: token=2; duration=1s; root distance=1m; rotation delta=0rad.'
    $s|Add-Member mounted @((Mount-Status '2026-10-19 02:01:08.000' 100),(Mount-Status '2026-10-19 02:01:11.000' 130))
    $s
}
function Assess-Remount($s){
    $line='[2026-10-19 02:00:16.000] ['+$s.prefix_pid+':77] Native(C): [Skater] [INFO] BMX GetUp action requested: request=1; fall request=1; fall generation=1; serial=754;'
    $lines=@($line)+@($s.complete_line)+@($s.status_lines)+@($s.recovery_line)+@($s.begin_line)+@($s.mounted)+@($s.tail)
    Get-BmxPostFallRemountProgress ($lines -join "`n") 1234 2 $s.impact $s.action $s.trace ([datetime]'2026-10-19 01:59:00')
}
$base=New-RemountScenario;$positive=Assess-Remount $base
Assert-Guard $positive.complete ('Valid post-fall remount rejected: '+$positive.issue)
Assert-Guard ($positive.remount_passed -and $positive.pulse_passed -and $positive.standability_observed -and $positive.mounts -eq 2 -and $positive.allocations -eq 1) 'Pulse, standing and remount proofs must remain distinct.'
$mutations=[ordered]@{
    no_recovery={param($s)$s.recovery_line=''}
    refusal={param($s)$s.recovery_line=$s.recovery_line.Replace('BMX crash recovery verified:','BMX crash recovery waiting: reason=6;')}
    duplicate_recovery={param($s)$s.recovery_line+="`n"+$s.recovery_line}
    wrong_recovery_pid={param($s)$s.recovery_line=$s.recovery_line.Replace('[1234:','[4321:')}
    early_recovery={param($s)$s.recovery_line=$s.recovery_line.Replace('serial=2434; native elapsed=40','serial=1234; native elapsed=20')}
    recovery_epoch={param($s)$s.recovery_line=$s.recovery_line.Replace('elapsed=40s','elapsed=31s')}
    recovery_token={param($s)$s.recovery_line=$s.recovery_line.Replace('body token=1','body token=2')}
    quiet_before_flush={param($s)$s.status_lines=@((Guard-Status '2026-10-19 02:00:19.000'),(Guard-Status '2026-10-19 02:00:21.000'))}
    no_lab_flush={param($s)$s.complete_line=''}
    rejected_lab_pulse={param($s)$s.complete_line=$s.complete_line.Replace('pulse=true','pulse=false')}
    late_lab_flush={param($s)$s.tail=$s.complete_line;$s.complete_line=''}
    no_mount_begin={param($s)$s.begin_line=''}
    reused_lease={param($s)$s.begin_line=$s.begin_line.Replace('token=2','token=1')}
    duplicate_mount={param($s)$s.begin_line+="`n"+$s.begin_line}
    wrong_status_pid={param($s)$s.mounted=@($s.mounted|ForEach-Object{$_.Replace('[1234:','[4321:')})}
    missing_status={param($s)$s.mounted=@($s.mounted[0])}
    new_world={param($s)$s.mounted[1]=$s.mounted[1].Replace('body world=999','body world=998')}
    extra_allocation={param($s)$s.mounted[1]=$s.mounted[1].Replace('allocations=1','allocations=2')}
    no_new_mount={param($s)$s.mounted[1]=$s.mounted[1].Replace('mounts=2','mounts=1')}
    extra_mount={param($s)$s.mounted[1]=$s.mounted[1].Replace('mounts=2','mounts=3')}
    missing_capture={param($s)$s.mounted[1]=$s.mounted[1].Replace('capture: held','capture: released')}
    old_token={param($s)$s.mounted[1]=$s.mounted[1].Replace('body token=2','body token=1')}
    rider_unmounted={param($s)$s.mounted[1]=$s.mounted[1].Replace('rider mode=riding','rider mode=none')}
    blend_not_finished={param($s)$s.mounted[1]=$s.mounted[1].Replace('bike mode=mounted','bike mode=mounting')}
    board_state={param($s)$s.mounted[1]=$s.mounted[1].Replace('state: 504','state: 100')}
    bike_missing={param($s)$s.mounted[1]=$s.mounted[1].Replace('parts: 8/8','parts: 7/8')}
    frozen_velocity={param($s)$s.mounted[1]=$s.mounted[1].Replace('updates: 130;','updates: 100;')}
    frozen_forces={param($s)$s.mounted[1]=$s.mounted[1].Replace('applications=130;','applications=100;')}
    frozen_pose={param($s)$s.mounted[1]=$s.mounted[1].Replace('pose updates=196;','pose updates=166;')}
    frozen_render={param($s)$s.mounted[1]=$s.mounted[1].Replace('render updates=130,','render updates=100,')}
    duplicate_status_field={param($s)$s.mounted[1]=$s.mounted[1].Replace('allocations=1;','allocations=1; allocations=1;')}
    insufficient_status_time={param($s)$s.mounted[1]=$s.mounted[1].Replace('02:01:11.000','02:01:08.500')}
    duplicate_status_clock={param($s)$s.mounted[1]=$s.mounted[1].Replace('02:01:11.000','02:01:08.000')}
    native_down={param($s)$s.trace.samples[-1].rider.standable=$false;$s.action.standability_observed=$false}
    pulse_reset_failed={param($s)$s.action.pulse.reset_readback=$false}
    fall_rejected={param($s)$s.impact.accepted_request=$false}
    extra_getup={param($s)$s.tail='[2026-10-19 02:01:12.000] [1234:77] Native(C): [Skater] [INFO] BMX GetUp action requested: request=2; fall request=1; fall generation=1; serial=2700;'}
    dismount_after={param($s)$s.tail='[2026-10-19 02:01:12.000] [1234:77] Native(C): [Skater] [INFO] BMX dismounted.'}
}
foreach($name in $mutations.Keys){$s=Clone-Guard $base;& $mutations[$name] $s;$r=Assess-Remount $s;Assert-Guard (-not $r.complete) ('Adversarial remount fixture accepted: '+$name)}
# The original passive contract continues to refuse all post-GetUp recovery.
$passive=Get-BmxGetupProgress ((@('[2026-10-19 02:00:16.000] [1234:77] Native(C): [Skater] [INFO] BMX GetUp action requested: request=1; fall request=1; fall generation=1; serial=754;')+@($base.status_lines)+@($base.recovery_line)) -join "`n") 1234 2 $base.impact $base.action $base.trace ([datetime]'2026-10-19 01:59:00')
Assert-Guard (-not $passive.complete) 'Passive GetUp must continue to reject remount actions.'
$tokens=$null;$errors=$null
$launcher=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'Start-BmxGame.ps1'),[ref]$tokens,[ref]$errors)
Assert-Guard ($errors.Count -eq 0) 'Launcher must parse.'
$guard=$launcher.Find({param($node)$node -is [Management.Automation.Language.IfStatementAst] -and $node.Extent.Text.StartsWith('if ($PostFallRemountTest -and')},$true)
Assert-Guard ($null -ne $guard) 'Distinct remount launch-mode guard required.'
function Test-RemountGuard($overrides){
    $PostFallRemountTest=$true;$GetUpPulse=$true;$ImpactTest='side';$NativeChassis=$true;$UnattendedTest=$true;$ProfileName='bmx-user-data-chassis-probe'
    foreach($key in $overrides.Keys){Set-Variable -Name $key -Value $overrides[$key]}
    try{. ([scriptblock]::Create($guard.Extent.Text));$true}catch{$false}
}
Assert-Guard (Test-RemountGuard @{}) 'Exact explicit remount mode must pass its guard.'
foreach($bad in @(@{GetUpPulse=$false},@{ImpactTest='inverted'},@{ImpactTest=''},@{NativeChassis=$false},@{UnattendedTest=$false},@{ProfileName='bmx-user-data-working-profile'},@{ProfileName='BMX-user-data-chassis-probe'})){Assert-Guard (-not (Test-RemountGuard $bad)) 'Incomplete/conflicting remount mode admitted.'}
foreach($file in @('Run-BmxPostFallRemount.ps1','PostFallRemountEvidence.ps1','Test-PostFallRemount.ps1')){
    $tokens=$null;$errors=$null;$null=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot $file),[ref]$tokens,[ref]$errors)
    Assert-Guard ($errors.Count -eq 0) ($file+' did not parse: '+($errors|Out-String))
}
Write-Output ('Post-fall remount assessor PASS: positive, '+$mutations.Count+' refusal/failure fixtures, passive contract and private launch guards. No game executed.')
# Portable data-only capture handoff fixtures. No process query or game launch.
$receiptAt=[datetime]::SpecifyKind([datetime]'2026-10-19 22:00:00',[DateTimeKind]::Utc)
$createdAt=$receiptAt.AddSeconds(-30);$fakeHash=('A'*64)
$receipt=New-BmxNativeTerminalReceipt 1234 $createdAt $receiptAt $true 'native_postfall_remount_evidence_observed' $fakeHash 'observed'
Assert-Guard ($receipt.owned_pid -eq 1234 -and $receipt.process_identity_verified -and $receipt.owned_created_utc -ceq $createdAt.ToString('o') -and $receipt.terminal_utc -ceq $receiptAt.ToString('o')) 'Terminal receipt must carry exact owned PID/creation and UTC terminal instant.'
Assert-Guard ($receipt.passed -is [bool] -and $receipt.passed -and $receipt.phase -ceq 'native_evidence_complete' -and $receipt.acceptance.Contains('teardown remain pending')) 'Passing receipt is native evidence only, never final teardown acceptance.'
$failedReceipt=New-BmxNativeTerminalReceipt 1234 $createdAt $receiptAt $false 'game_exited_before_terminal_report' $fakeHash 'owned game exited'
Assert-Guard (-not $failedReceipt.passed -and $failedReceipt.phase -ceq 'native_evidence_failed' -and $failedReceipt.outcome -ceq 'game_exited_before_terminal_report') 'Failure receipt must preserve process exit reason and false verdict.'
$launchFailure=New-BmxNativeTerminalReceipt 0 $null $receiptAt $false 'supervisor_exception: launch failed' $fakeHash
Assert-Guard (-not $launchFailure.process_identity_verified -and $null -eq $launchFailure.owned_created_utc -and -not $launchFailure.passed) 'Pre-ownership failure must not invent identity.'
foreach($entry in @(
    @{pid=0;created=$null;passed=$true;sha=$fakeHash},
    @{pid=1234;created=$null;passed=$false;sha=$fakeHash},
    @{pid=0;created=$createdAt;passed=$false;sha=$fakeHash},
    @{pid=1234;created=$receiptAt.AddSeconds(1);passed=$false;sha=$fakeHash},
    @{pid=1234;created=$createdAt;passed=$false;sha='BAD'})){
    $rejected=$false;try{New-BmxNativeTerminalReceipt $entry.pid $entry.created $receiptAt $entry.passed 'failure' $entry.sha|Out-Null}catch{$rejected=$true}
    Assert-Guard $rejected 'Incomplete/future identity or malformed runtime provenance must not publish a terminal receipt.'
}
$deadline=$receiptAt.AddSeconds(10)
Assert-Guard ((Get-BmxPostFallMonitorOutcome 'awaiting_postfall_remount: mounting' $false $deadline $deadline) -ceq 'supervisor_wall_timeout: awaiting_postfall_remount: mounting') 'Actual monitor expiration must label its waiting reason.'
Assert-Guard ((Get-BmxPostFallMonitorOutcome 'awaiting_postfall_remount: mounting' $false $receiptAt $deadline) -ceq 'awaiting_postfall_remount: mounting') 'Waiting before deadline is not a timeout.'
Assert-Guard ((Get-BmxPostFallMonitorOutcome 'native_postfall_remount_evidence_observed' $true $deadline.AddSeconds(1) $deadline) -ceq 'native_postfall_remount_evidence_observed') 'Complete native evidence never becomes a timeout.'
foreach($outcome in @('game_exited_before_terminal_report','recording_gate_timeout','supervisor_exception: owned query failed','supervisor_wall_timeout: awaiting_postfall_remount: mounting')){
    Assert-Guard ((Get-BmxPostFallMonitorOutcome $outcome $false $deadline.AddSeconds(1) $deadline) -ceq $outcome) 'Explicit failure reasons cannot receive a misleading/doubled supervisor timeout prefix.'
}
$fixtureDirectory=Join-Path ([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\build\bmx'))) ('terminal-receipt-fixtures-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixtureDirectory|Out-Null
$written=Write-BmxNativeTerminalReceipt $fixtureDirectory $receipt
$decoded=ConvertFrom-BmxStrictJson (Get-Content -LiteralPath $written.path -Raw)
Assert-Guard ($decoded.owned_pid -eq 1234 -and $decoded.terminal -is [bool] -and $decoded.terminal -and $decoded.passed -is [bool] -and $decoded.passed) 'Published terminal file must be complete strict JSON with typed identity/verdict.'
Assert-Guard ($written.sha256 -ceq (Get-FileHash -LiteralPath $written.path -Algorithm SHA256).Hash -and @(Get-ChildItem -LiteralPath $fixtureDirectory -Filter '*.tmp' -File).Count -eq 0) 'Atomic writer must return the final hash and leave no temporary receipt.'
$secondRejected=$false;try{Write-BmxNativeTerminalReceipt $fixtureDirectory $failedReceipt|Out-Null}catch{$secondRejected=$true}
Assert-Guard ($secondRejected -and (Get-FileHash -LiteralPath $written.path -Algorithm SHA256).Hash -ceq $written.sha256) 'Existing receipt must remain unchanged rather than being overwritten.'
$noDirectoryRejected=$false;try{Write-BmxNativeTerminalReceipt (Join-Path $fixtureDirectory 'missing') $receipt|Out-Null}catch{$noDirectoryRejected=$true}
Assert-Guard $noDirectoryRejected 'Writer cannot silently create an unprepared output directory.'
Write-Output ('Terminal receipt PASS: complete/failed/identity/provenance policies, timeout reasons, atomic strict JSON and no-overwrite fixtures. Artifacts: '+$fixtureDirectory)

# Real run153 status layouts are embedded so these regressions stay portable.
# Runtime reports/logs are never rewritten; the compatibility adapter is local.
$actual153Statuses=@(
    '[2026-10-19 18:54:37.323] [33020:16036] SDK: [Console] [INFO] Private GetUp intent armed once; native animation and reset are being observed. Native velocity updates: 33; native bike parts: 8/8; placements: 35656; Native bike entities placed.; native physics state: 504; BMX capture: released; chassis mounts=1; allocations=1; active=false; parked=false; failed=false; blocked=false; observation suspended=false; deferred park=false; chassis issue=native detached bike; drive released; force applications=33; body world=9419500544; rider pose updates=66; bike mode=crashed; rider mode=none; setdown verified=false; body token=1. pose listener=ready, source skeleton=validated, successful pose updates=66, rider render updates=33, stale parent rejections=0, max completed riding hips local XYZ norm (native local units)=0.945908, palm/ankle target error m=0.000000/0.000031, hand/foot contact=1.000000/1.000000, local board render hides=4457',
    '[2026-10-19 18:54:39.381] [33020:16036] SDK: [Console] [INFO] Private GetUp intent armed once; native animation and reset are being observed. Native velocity updates: 33; native bike parts: 8/8; placements: 36600; Native bike entities placed.; native physics state: 504; BMX capture: released; chassis mounts=1; allocations=1; active=false; parked=false; failed=false; blocked=false; observation suspended=false; deferred park=false; chassis issue=native detached bike; drive released; force applications=33; body world=9419500544; rider pose updates=66; bike mode=crashed; rider mode=none; setdown verified=false; body token=1. pose listener=ready, source skeleton=validated, successful pose updates=66, rider render updates=33, stale parent rejections=0, max completed riding hips local XYZ norm (native local units)=0.945908, palm/ankle target error m=0.000000/0.000031, hand/foot contact=1.000000/1.000000, local board render hides=4575',
    '[2026-10-19 18:54:47.478] [33020:16036] SDK: [Console] [INFO] BMX mounted: 0.97 km/h; grounded. Physical chassis; left stick handlebars, right stick rear tricks. Native velocity updates: 403; native bike parts: 8/8; placements: 39568; Native bike entities placed.; native physics state: 504; BMX capture: held; chassis mounts=2; allocations=1; active=true; parked=false; failed=false; blocked=false; observation suspended=false; deferred park=false; chassis issue=native chassis force submitted; force applications=403; body world=9419500544; rider pose updates=806; bike mode=mounted; rider mode=riding; setdown verified=false; body token=2. pose listener=ready, source skeleton=validated, successful pose updates=806, rider render updates=403, stale parent rejections=0, max completed riding hips local XYZ norm (native local units)=0.945908, palm/ankle target error m=0.000043/0.000031, hand/foot contact=1.000000/1.000000, local board render hides=4946',
    '[2026-10-19 18:54:50.551] [33020:16036] SDK: [Console] [INFO] BMX mounted: 1.28 km/h; grounded. Physical chassis; left stick handlebars, right stick rear tricks. Native velocity updates: 544; native bike parts: 8/8; placements: 40696; Native bike entities placed.; native physics state: 504; BMX capture: held; chassis mounts=2; allocations=1; active=true; parked=false; failed=false; blocked=false; observation suspended=false; deferred park=false; chassis issue=native chassis force submitted; force applications=544; body world=9419500544; rider pose updates=1088; bike mode=mounted; rider mode=riding; setdown verified=false; body token=2. pose listener=ready, source skeleton=validated, successful pose updates=1088, rider render updates=544, stale parent rejections=0, max completed riding hips local XYZ norm (native local units)=0.945908, palm/ankle target error m=0.000031/0.000000, hand/foot contact=1.000000/1.000000, local board render hides=5087'
)
$expectedCounts=@(33,33,403,544)
for($i=0;$i -lt $actual153Statuses.Count;++$i){
    $actual=Get-BmxLifecycleStatusValues $actual153Statuses[$i]
    Assert-Guard ($actual.health_diagnostics_present -and $actual.velocity -eq $expectedCounts[$i] -and $actual.force -eq $expectedCounts[$i] -and $actual.world -eq 9419500544) 'Real run153 field/counter extraction must preserve the observed values.'
    Assert-Guard ($actual.token -eq $(if($i -lt 2){1}else{2}) -and $actual.mode -ceq $(if($i -lt 2){'crashed'}else{'mounted'})) 'Real quiet/mounted token and lifecycle mode must remain distinct.'
    if($i -lt 2){
        $legacy=ConvertTo-BmxPassiveStatusLine $actual153Statuses[$i]
        Assert-Guard ($legacy -match 'chassis mounts=1; allocations=1; active=false; parked=false; force applications=33; body world=9419500544;') 'Actual new diagnostics must be adapted into the unchanged passive layout after strict validation.'
        $roundTrip=Get-BmxLifecycleStatusValues $legacy
        foreach($name in @('velocity','parts','placements','state','capture','mounts','allocations','active','parked','force','world','poses','token','mode','rider','render')){
            Assert-Guard ($roundTrip.$name -ceq $actual.$name) ('Compatibility normalization changed native evidence: '+$name)
        }
    }
}
function Add-HealthyStatusDiagnostics([string]$line){
    $line.Replace('; force applications=','; failed=false; blocked=false; observation suspended=false; deferred park=false; chassis issue=native detached bike; drive released; future status field=added; force applications=')
}
$modern=Clone-Guard $base
$modern.status_lines=@($modern.status_lines|ForEach-Object{Add-HealthyStatusDiagnostics $_})
$modern.mounted=@($modern.mounted|ForEach-Object{Add-HealthyStatusDiagnostics $_})
$modernResult=Assess-Remount $modern
Assert-Guard $modernResult.complete ('Healthy additive native status diagnostics rejected: '+$modernResult.issue)
$healthFailures=0
foreach($where in @('status_lines','mounted')){
    foreach($name in @('failed','blocked','observation suspended','deferred park')){
        foreach($bad in @('true','1','FALSE','unknown')){
            $case=Clone-Guard $modern;$case.$where[1]=$case.$where[1].Replace(($name+'=false;'),($name+'='+$bad+';'))
            $r=Assess-Remount $case;Assert-Guard (-not $r.complete) ('Malformed/unhealthy native flag accepted: '+$where+'/'+$name+'/'+$bad);++$healthFailures
        }
    }
}
$modernFailures=[ordered]@{
    quiet_duplicate_allocations={param($s)$s.status_lines[1]=$s.status_lines[1].Replace('allocations=1;','allocations=1; allocations=1;')}
    quiet_duplicate_force={param($s)$s.status_lines[1]=$s.status_lines[1].Replace('applications=33;','applications=33; force applications=33;')}
    quiet_duplicate_health={param($s)$s.status_lines[1]=$s.status_lines[1].Replace('failed=false;','failed=false; failed=false;')}
    mounted_duplicate_health={param($s)$s.mounted[1]=$s.mounted[1].Replace('blocked=false;','blocked=false; blocked=false;')}
    quiet_partial_quartet={param($s)$s.status_lines[1]=$s.status_lines[1].Replace('deferred park=false; ','')}
    mounted_partial_quartet={param($s)$s.mounted[1]=$s.mounted[1].Replace('observation suspended=false; ','')}
    quiet_bad_counter={param($s)$s.status_lines[1]=$s.status_lines[1].Replace('allocations=1;','allocations=invalid;')}
    mounted_bad_counter={param($s)$s.mounted[1]=$s.mounted[1].Replace('force applications=130;','force applications=invalid;')}
}
foreach($name in $modernFailures.Keys){$case=Clone-Guard $modern;& $modernFailures[$name] $case;$r=Assess-Remount $case;Assert-Guard (-not $r.complete) ('Malformed additive status fixture accepted: '+$name)}
Write-Output ('Additive status parser PASS: four actual run153 samples, legacy compatibility, healthy added fields, '+$healthFailures+' health-flag failures and '+$modernFailures.Count+' duplicate/malformed cases. Native game never executed.')

# Real154 status/refusal rows, with only PID/world/time/counter fixture identity
# normalized to the existing synthetic native reports. Raw archived154 is also
# re-assessed separately; this portable fixture cannot rewrite its trial verdict.
$actual154RefusalLines=[ordered]@{
    quiet1='[2026-10-19 19:06:27.040] [23816:14768] SDK: [Console] [INFO] Private GetUp intent armed once; native animation and reset are being observed. Native velocity updates: 33; native bike parts: 8/8; placements: 35848; Native bike entities placed.; native physics state: 504; BMX capture: released; chassis mounts=1; allocations=1; active=false; parked=false; failed=false; blocked=false; observation suspended=false; deferred park=false; chassis issue=native detached bike; drive released; force applications=33; body world=9430201440; rider pose updates=67; bike mode=crashed; rider mode=none; setdown verified=false; body token=1. pose listener=ready, source skeleton=validated, successful pose updates=67, rider render updates=34, stale parent rejections=0, max completed riding hips local XYZ norm (native local units)=0.945908, palm/ankle target error m=0.000000/0.000031, hand/foot contact=1.000000/1.000000, local board render hides=4481'
    quiet2='[2026-10-19 19:06:29.090] [23816:14768] SDK: [Console] [INFO] Private GetUp intent armed once; native animation and reset are being observed. Native velocity updates: 33; native bike parts: 8/8; placements: 36808; Native bike entities placed.; native physics state: 504; BMX capture: released; chassis mounts=1; allocations=1; active=false; parked=false; failed=false; blocked=false; observation suspended=false; deferred park=false; chassis issue=native detached bike; drive released; force applications=33; body world=9430201440; rider pose updates=67; bike mode=crashed; rider mode=none; setdown verified=false; body token=1. pose listener=ready, source skeleton=validated, successful pose updates=67, rider render updates=34, stale parent rejections=0, max completed riding hips local XYZ norm (native local units)=0.945908, palm/ankle target error m=0.000000/0.000031, hand/foot contact=1.000000/1.000000, local board render hides=4601'
    command='[2026-10-19 19:06:29.118] [23816:14768] Native(C): [Level] [INFO] Startup command queued: bmx interact'
    refusal='[2026-10-19 19:06:29.130] [23816:14768] Native(C): [Skater] [INFO] BMX crash recovery waiting: reason=11; distance=12.204600837803296m; height=0.90606689453125m; speed=1.1867010640611894e-08m/s; native elapsed=76.13332879543304s; standable=true; offboard=true.'
    failed1='[2026-10-19 19:06:37.171] [23816:14768] SDK: [Console] [INFO] Wait until you and the bike stop falling, then move beside the bike and tap Y/Triangle. Native velocity updates: 33; native bike parts: 8/8; placements: 40568; Native bike entities placed.; native physics state: 504; BMX capture: released; chassis mounts=1; allocations=1; active=false; parked=false; failed=false; blocked=false; observation suspended=false; deferred park=false; chassis issue=native detached bike; drive released; force applications=33; body world=9430201440; rider pose updates=67; bike mode=crashed; rider mode=none; setdown verified=false; body token=1. pose listener=ready, source skeleton=validated, successful pose updates=67, rider render updates=34, stale parent rejections=0, max completed riding hips local XYZ norm (native local units)=0.945908, palm/ankle target error m=0.000000/0.000031, hand/foot contact=1.000000/1.000000, local board render hides=5071'
    failed2='[2026-10-19 19:06:40.221] [23816:14768] SDK: [Console] [INFO] Wait until you and the bike stop falling, then move beside the bike and tap Y/Triangle. Native velocity updates: 33; native bike parts: 8/8; placements: 42016; Native bike entities placed.; native physics state: 504; BMX capture: released; chassis mounts=1; allocations=1; active=false; parked=false; failed=false; blocked=false; observation suspended=false; deferred park=false; chassis issue=native detached bike; drive released; force applications=33; body world=9430201440; rider pose updates=67; bike mode=crashed; rider mode=none; setdown verified=false; body token=1. pose listener=ready, source skeleton=validated, successful pose updates=67, rider render updates=34, stale parent rejections=0, max completed riding hips local XYZ norm (native local units)=0.945908, palm/ankle target error m=0.000000/0.000031, hand/foot contact=1.000000/1.000000, local board render hides=5252'
}
function Normalize-RefusalFixtureLine([string]$line,[string]$time){
    $normalized=$line.Replace('[23816:','[1234:').Replace('body world=9430201440;','body world=999;').Replace('rider pose updates=67;','rider pose updates=66;').Replace('rider render updates=34,','rider render updates=33,')
    '[2026-10-19 '+$time+']'+$normalized.Substring(25)
}
function New-RefusalScenario {
    $s=Clone-Guard $base
    $s.status_lines=@((Normalize-RefusalFixtureLine $actual154RefusalLines.quiet1 '02:00:44.000'),(Normalize-RefusalFixtureLine $actual154RefusalLines.quiet2 '02:00:46.000'))
    $s|Add-Member command_line (Normalize-RefusalFixtureLine $actual154RefusalLines.command '02:01:00.000')
    $s|Add-Member refusal_line (Normalize-RefusalFixtureLine $actual154RefusalLines.refusal '02:01:00.023')
    $s|Add-Member failed @((Normalize-RefusalFixtureLine $actual154RefusalLines.failed1 '02:01:08.000'),(Normalize-RefusalFixtureLine $actual154RefusalLines.failed2 '02:01:11.000'))
    $s
}
function Assess-Refusal($s){
    $line='[2026-10-19 02:00:16.000] [1234:77] Native(C): [Skater] [INFO] BMX GetUp action requested: request=1; fall request=1; fall generation=1; serial=754;'
    $lines=@($line)+@($s.complete_line)+@($s.status_lines)+@($s.command_line)+@($s.refusal_line)+@($s.failed)+@($s.tail)
    Get-BmxPostFallRefusalTerminal ($lines -join "`n") 1234 2 $s.impact $s.action $s.trace ([datetime]'2026-10-19 01:59:00')
}
$refusalBase=New-RefusalScenario;$refused=Assess-Refusal $refusalBase
Assert-Guard ($refused.terminal_failure -and -not $refused.passed -and $refused.reason -eq 11 -and $refused.post_status_count -eq 2) ('Actual154-shaped refused interaction must stop as failed: '+$refused.issue)
Assert-Guard ($refused.getup_proof.complete -and $refused.getup_proof.pulse_passed -and $refused.getup_proof.standability_observed -and $refused.first_failed.token -eq 1 -and $refused.last_failed.mounts -eq 1 -and $refused.last_failed.allocations -eq 1) 'Negative terminal must preserve complete passive proof and same released pool without inventing mount acceptance.'
$refusalFailures=[ordered]@{
    no_explicit_interact={param($s)$s.command_line=''}
    duplicate_interact={param($s)$s.command_line+="`n"+$s.command_line}
    stale_interact={param($s)$s.command_line=$s.command_line.Replace('2026-10-19 02:01:00.000','2026-10-18 02:01:00.000')}
    wrong_interact_pid={param($s)$s.command_line=$s.command_line.Replace('[1234:','[4321:')}
    no_refusal={param($s)$s.refusal_line=''}
    duplicate_refusal={param($s)$s.refusal_line+="`n"+$s.refusal_line}
    duplicate_refusal_same_line={param($s)$s.refusal_line+=' '+($s.refusal_line -split 'BMX crash recovery waiting:')[1].Insert(0,'BMX crash recovery waiting:')}
    duplicate_refusal_field={param($s)$s.refusal_line+=' reason=11;'}
    wrong_refusal_pid={param($s)$s.refusal_line=$s.refusal_line.Replace('[1234:','[4321:')}
    late_refusal={param($s)$s.refusal_line=$s.refusal_line.Replace('02:01:00.023','02:01:03.000')}
    malformed_measurement={param($s)$s.refusal_line=$s.refusal_line.Replace('12.204600837803296m','NaNm')}
    reason_measurement_mismatch={param($s)$s.refusal_line=$s.refusal_line.Replace('12.204600837803296m','1.0m')}
    not_standing={param($s)$s.refusal_line=$s.refusal_line.Replace('standable=true','standable=false')}
    no_getup_pulse={param($s)$s.action.pulse.reset_readback=$false}
    incomplete_passive_window={param($s)$s.trace.window_complete=$false}
    incomplete_quiet_phase={param($s)$s.status_lines=@($s.status_lines[0])}
    missing_flush={param($s)$s.complete_line=''}
    quiet_before_flush={param($s)$s.complete_line=$s.complete_line.Replace('02:00:40.000','02:00:50.000')}
    quiet_after_interact_clock={param($s)$s.status_lines[1]=$s.status_lines[1].Replace('02:00:46.000','02:01:01.000')}
    one_final_status={param($s)$s.failed=@($s.failed[0])}
    statuses_too_close={param($s)$s.failed[1]=$s.failed[1].Replace('02:01:11.000','02:01:08.500')}
    final_clock_before_refusal={param($s)$s.failed[0]=$s.failed[0].Replace('02:01:08.000','02:00:59.000')}
    still_mounting={param($s)$s.failed[1]=$s.failed[1].Replace('bike mode=crashed','bike mode=mounting')}
    mounted={param($s)$s.failed[1]=$s.failed[1].Replace('bike mode=crashed','bike mode=mounted')}
    native_active={param($s)$s.failed[1]=$s.failed[1].Replace('active=false','active=true')}
    capture_held={param($s)$s.failed[1]=$s.failed[1].Replace('capture: released','capture: held')}
    force_advancing={param($s)$s.failed[1]=$s.failed[1].Replace('force applications=33;','force applications=34;')}
    velocity_advancing={param($s)$s.failed[1]=$s.failed[1].Replace('velocity updates: 33;','velocity updates: 34;')}
    pose_advancing={param($s)$s.failed[1]=$s.failed[1].Replace('rider pose updates=66;','rider pose updates=67;')}
    render_advancing={param($s)$s.failed[1]=$s.failed[1].Replace('rider render updates=33,','rider render updates=34,')}
    different_world={param($s)$s.failed[1]=$s.failed[1].Replace('body world=999;','body world=998;')}
    different_token={param($s)$s.failed[1]=$s.failed[1].Replace('body token=1.','body token=2.')}
    extra_mount={param($s)$s.failed[1]=$s.failed[1].Replace('mounts=1;','mounts=2;')}
    extra_allocation={param($s)$s.failed[1]=$s.failed[1].Replace('allocations=1;','allocations=2;')}
    unhealthy={param($s)$s.failed[1]=$s.failed[1].Replace('failed=false;','failed=true;')}
    duplicate_field={param($s)$s.failed[1]=$s.failed[1].Replace('allocations=1;','allocations=1; allocations=1;')}
    malformed_counter={param($s)$s.failed[1]=$s.failed[1].Replace('force applications=33;','force applications=invalid;')}
    new_interaction_after_refusal={param($s)$s.tail='[2026-10-19 02:01:12.000] [1234:77] Native(C): [Level] [INFO] Startup command queued: bmx carry'}
    natural_mount_after_refusal={param($s)$s.tail='[2026-10-19 02:01:12.000] [1234:77] Native(C): [Skater] [INFO] BMX natural mount begin: token=2; duration=1s;'}
}
foreach($name in $refusalFailures.Keys){$s=Clone-Guard $refusalBase;& $refusalFailures[$name] $s;$r=Assess-Refusal $s;Assert-Guard (-not $r.terminal_failure) ('Pending/ambiguous/advancing refusal fixture classified terminal: '+$name)}
Write-Output ('Early failed terminal PASS: actual154-shaped refusal and '+$refusalFailures.Count+' incomplete/ambiguous/advancing/unhealthy negatives. No game executed.')
