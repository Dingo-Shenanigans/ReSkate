#requires -Version 7.0
# Synthetic data-only fixtures: never execute a supervisor or game.
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if($env:BMX_PRODUCER_TEST_REPO){$repo=$env:BMX_PRODUCER_TEST_REPO}
. (Join-Path $repo 'contrib\bmx-lab\PostFallRemountEvidence.ps1')
. (Join-Path $PSScriptRoot 'FirstMountProducerEvidence.ps1')
$script:checks=0
function Require($Value,[string]$Why){$script:checks++;if(-not $Value){throw $Why}}
$created=[DateTime]::UtcNow.AddMinutes(-1);$started=$created.AddSeconds(-1);$modified=$created.AddSeconds(20)
function New-Raw([int]$Count) {
    if($Count -eq 4){return [pscustomobject]@{raw_words=@(0,0,0,0);xyz_finite=$true;all_finite=$true}}
    return [pscustomobject]@{raw_words=@(1..16|ForEach-Object{0});translation_xyz_finite=$true;all_finite=$true}
}
function New-Owner {[pscustomobject]@{base=5368709120L;entity=2;core=3;context=4;rig=5}}
function New-Clock {[pscustomobject]@{available=$true;pre_original_listener_serial=100;native_time=1.0}}
function New-World {[pscustomobject]@{sdk_available=$false;published_owner_matches=$false;fresh_entity_context_available=$false;client=0;client_context=0;world=0;native_state=0;body_token=0;canonical_native_world_proved=$false}}
function New-Snapshot {
    [pscustomobject]@{qpc_begin=1;qpc_end=2;owner_available=$true;final_owner_available=$true;owner_before=(New-Owner);owner_after=(New-Owner)
        clock_before=(New-Clock);clock_after=(New-Clock);world_publication_before=(New-World);world_publication_after=(New-World)
        same_owner_and_listener_clock=$true;component_proved=$true;atomic_native_snapshot=$false;full_canonical_world_coherence_proved=$false;copy_branch_execution_proved=$false
        root_words=@(0,0,0);context_13cc=0;context_13cc_bit8000=$false;context_830=(New-Raw 4);context_7c0=(New-Raw 4);rig_57f0=(New-Raw 4)
        rig_4720=(New-Raw 16);rig_4760=(New-Raw 16);cached_35c0_record23_raw_basis_unknown=(New-Raw 16);physical_record23_matrix=(New-Raw 16)
        physical_rig=0;parts=0;physical_typed=$false;physical_record_owner=$false;physical_chain_stable=$false;physical_flags=$null
        rig_4630_dispatch_target_evidence=[pscustomobject]@{object_raw=$null;object_pointer_bounds=$false;vtable_raw=$null;vtable_in_verified_readable_data_section=$false
            slot_d8_target_raw=$null;target_in_verified_readable_executable_section=$false;target_first_byte_readable=$false;call_performed=$false;subobject_atomic_snapshot_proved=$false;completion_contract_proved=$false}
        rig_4638_raw_pointer_lane_words=$null;raw_5860=(New-Raw 4);raw_5870=(New-Raw 4)
        raw_context_17ec=[pscustomobject]@{available=$true;raw_word=1015580809;finite=$true}}
}
function New-Row([string]$Kind='motion_original',[int]$Slot=0,[int]$Epoch=100,[switch]$NoPost) {
    $writer=$Kind -ceq 'before_bmx_pose_write';$motion=$Kind -ceq 'motion_original';$index=if($motion){0}elseif($writer){32}else{16}
    [pscustomobject]@{record_index=$index+$Slot;invocation=$index+$Slot+1;thread_id=7;kind=$Kind;slot_in_kind=$Slot;listener_serial_at_reservation=$Epoch
        component=if($motion){0}else{6};rendering=if($writer){$true}else{$null};frame_entity=if($writer){2}else{0};mount_token=if($writer){1}else{0};frame_revision=if($writer){1}else{0}
        flags=if($motion){0}else{$null};selected_target_words=if($motion){@(1..16|ForEach-Object{0})}else{$null};before=(New-Snapshot)
        post_observed=(-not $writer -and -not $NoPost);original_returned=if(-not $writer -and -not $NoPost){$true}else{$null}
        after=if(-not $writer -and -not $NoPost){New-Snapshot}else{$null};pair_same_owner_and_listener_clock=(-not $writer -and -not $NoPost)}
}
function New-Report([switch]$Empty) {
    $report=[pscustomobject]@{schema_version=1;event='bmx_first_mount_producer_probe';process_id=123;process_creation_filetime=[string]$created.ToFileTimeUtc()
        read_only_native_state=$true;explicit_producer_opt_in=$true;source_sha256='fbce74d5e28ef525dbba2cb4adbebc13405bdbd88f31bc940bca45e4ae88b8f9'
        qpc_frequency=10000000;armed=$false;sealed=$false;record_capacity=48;snapshot_capacity=80;epoch_window=8;per_kind_per_epoch=2
        anchor_listener_serial=100;stored_snapshots=if($Empty){0}else{2};static_storage_bytes=120000;contention=0;unavailable=0;dropped=0;bad_cookies=0;owner_changes=0
        completion_boundary='existing_call_return_only_not_native_physics_or_animation_completion';clock_boundary='SDK_listener_advances_before_original_and_is_not_a_completion_fence';mount_token_continuity_proved=$false
        rows=@()}
    if(-not $Empty){$report.rows=@(New-Row);$report.armed=$true};return $report
}
function Verify($Report){$data=ConvertFrom-BmxStrictJson ($Report|ConvertTo-Json -Depth 20 -Compress);Test-BmxFirstMountProducerEnvelope $data 123 $created $started $modified}
function Reject([scriptblock]$Mutation,[string]$Why){$r=New-Report;& $Mutation $r;$bad=$false;try{[void](Verify $r)}catch{$bad=$true};Require $bad $Why}
Require (Verify (New-Report)).valid 'valid paired motion fixture'
Require (Verify (New-Report -Empty)).valid 'zero coverage/unarmed/unsealed valid; no minimum/outcome is imposed'
$r=New-Report;$r.rows=@(New-Row 'local_animation_original');Require (Verify $r).valid 'animation pair accepted'
$r=New-Report;$r.rows=@(New-Row 'before_bmx_pose_write');$r.stored_snapshots=1;Require (Verify $r).valid 'writer-only before is accepted'
$r=New-Report;$r.rows=@(New-Row -NoPost);$r.stored_snapshots=1;Require (Verify $r).valid 'unavailable post is valid missing coverage'
$r=New-Report;$r.rows[0].before.owner_available=$false;$r.rows[0].before.component_proved=$false;$r.rows[0].before.same_owner_and_listener_clock=$false;$r.rows[0].pair_same_owner_and_listener_clock=$false;Require (Verify $r).valid 'unqualified motion snapshot may lack component proof'
$r=New-Report;$r.rows[0].listener_serial_at_reservation=$null;$r.anchor_listener_serial=$null;Require (Verify $r).valid 'unknown reservation/anchor accepted without borrowed epoch proof'
$r=New-Report -Empty;$r.armed=$true;$r.rows=@(foreach($k in @('motion_original','local_animation_original','before_bmx_pose_write')){for($i=0;$i -lt 16;$i++){New-Row $k $i (100+[int][math]::Floor($i/2))}});$r.stored_snapshots=80;Require (Verify $r).valid 'all48 records/80 immutable halves fill exact8epoch/perkind2 budget'
Reject {param($r)$r.schema_version=1.0} 'fractional numeric schema rejected'
Reject {param($r)$r.process_id='123'} 'string PID rejected'
Reject {param($r)$r.process_id=124} 'foreign PID rejected'
Reject {param($r)$r.process_creation_filetime=[string]$created.AddSeconds(2).ToFileTimeUtc()} 'foreign creation rejected'
Reject {param($r)$r.source_sha256='wrong'} 'foreign source rejected'
Reject {param($r)$r.explicit_producer_opt_in=$false} 'explicit producer opt-in required'
Reject {param($r)$r.read_only_native_state='true'} 'read-only Boolean type enforced'
Reject {param($r)$r.completion_boundary='native_completed'} 'false completion boundary rejected'
Reject {param($r)$r.clock_boundary='completion_fence'} 'false clock fence rejected'
Reject {param($r)$r.mount_token_continuity_proved=$true} 'raw writer token cannot become continuity proof'
Reject {param($r)$r.record_capacity=49} 'fixed record bound enforced'
Reject {param($r)$r.snapshot_capacity=81} 'fixed snapshot bound enforced'
Reject {param($r)$r.static_storage_bytes=131073} 'static storage bound enforced'
Reject {param($r)$r.stored_snapshots=1} 'stored-half count exact'
Reject {param($r)$r.contention=$true} 'counter Boolean rejected'
Reject {param($r)$r.rows[0].record_index=48} 'record index bound enforced'
Reject {param($r)$r.rows[0].record_index=16} 'record index must match kind16stride storage'
Reject {param($r)$r.armed=$false} 'nonempty report cannot claim unarmed storage'
Reject {param($r)$r.rows+=,(New-Row 'motion_original' 1);$r.rows[1].invocation=$r.rows[0].invocation;$r.stored_snapshots=4} 'invocation IDs unique across records'
Reject {param($r)$r.rows+=($r.rows[0]);$r.stored_snapshots=4} 'duplicate record/slot rejected'
Reject {param($r)$r.rows[0].kind='Motion_original'} 'kind case exact'
Reject {param($r)$r.rows[0].slot_in_kind=16} 'kind-slot bound enforced'
Reject {param($r)$r.rows[0].invocation=0} 'zero invocation rejected'
Reject {param($r)$r.rows[0].thread_id=0} 'zero thread rejected'
Reject {param($r)$r.rows[0].listener_serial_at_reservation=108} 'arithmetic epoch window upper bound enforced'
Reject {param($r)$r.rows[0].listener_serial_at_reservation=99} 'arithmetic epoch window lower bound enforced'
Reject {param($r)$r.anchor_listener_serial=$null} 'known reservation cannot borrow missing anchor'
Reject {param($r)$r.rows=@((New-Row 'motion_original' 0),(New-Row 'motion_original' 1),(New-Row 'motion_original' 2));$r.stored_snapshots=6} 'more than2 same-kind same-epoch rejected'
Reject {param($r)$r.rows[0].flags=256} 'motion byte flags bound enforced'
Reject {param($r)$r.rows[0].selected_target_words=@(0)} 'motion target exact16 raw words'
Reject {param($r)$r.rows[0].selected_target_words[0]='0'} 'raw word type strict'
Reject {param($r)$r.rows[0].component=1} 'motion component fixed zero'
Reject {param($r)$r.rows=@(New-Row 'local_animation_original');$r.rows[0].flags=0} 'animation cannot borrow motion metadata'
Reject {param($r)$r.rows=@(New-Row 'local_animation_original');$r.rows[0].component=0} 'animation requires nonzero component identity'
Reject {param($r)$r.rows=@(New-Row 'before_bmx_pose_write');$r.stored_snapshots=1;$r.rows[0].component=0} 'writer requires nonzero component identity'
Reject {param($r)$r.rows=@(New-Row 'before_bmx_pose_write');$r.stored_snapshots=1;$r.rows[0].original_returned=$true} 'writer cannot claim original return'
Reject {param($r)$r.rows[0].frame_entity=2} 'non-writer frame fields fixed zero'
Reject {param($r)$r.rows[0].post_observed=$false} 'unobserved post cannot retain after/return/pair'
Reject {param($r)$r.rows[0].after=$null} 'observed post requires snapshot'
Reject {param($r)$r.rows[0].before.atomic_native_snapshot=$true} 'atomic snapshot proof forbidden'
Reject {param($r)$r.rows[0].before.full_canonical_world_coherence_proved=$true} 'canonical whole-world proof forbidden'
Reject {param($r)$r.rows[0].after.copy_branch_execution_proved=$true} 'copy execution proof forbidden'
Reject {param($r)$r.rows[0].before.world_publication_before.canonical_native_world_proved=$true} 'SDK publication cannot become native-world proof'
Reject {param($r)$r.rows[0].before.rig_4630_dispatch_target_evidence.call_performed=$true} 'target invocation proof forbidden'
Reject {param($r)$r.rows[0].before.rig_4630_dispatch_target_evidence.completion_contract_proved=$true} 'target completion proof forbidden'
Reject {param($r)$r.rows[0].before.raw_5860.raw_words[0]=4294967296} 'raw uint32 overflow rejected'
Reject {param($r)$r.rows[0].before.raw_5860.raw_words[0]=2139095040} 'raw finite flag cannot hide infinity bits'
Reject {param($r)$r.rows[0].before.raw_context_17ec.available=$false} 'unavailable denominator cannot retain word/finite claim'
Reject {param($r)$r.rows[0].before.raw_context_17ec.raw_word=2139095040} 'denominator finite claim checked against raw bits'
Reject {param($r)$r.rows[0].before.qpc_end=0} 'reversed QPC rejected'
Reject {param($r)$r.rows[0].before.owner_after.rig=6} 'qualified snapshot cannot hide owner change'
Reject {param($r)$r.rows[0].before.owner_available=$false;$r.rows[0].before.same_owner_and_listener_clock=$false;$r.rows[0].pair_same_owner_and_listener_clock=$false} 'component proof needs available owner even without qualified clock'
Reject {param($r)$r.rows[0].before.owner_after.rig=6;$r.rows[0].before.same_owner_and_listener_clock=$false;$r.rows[0].pair_same_owner_and_listener_clock=$false} 'component proof cannot hide owner change on unqualified clock'
Reject {param($r)$r.rows[0].after.clock_after.native_time=2} 'qualified snapshot cannot hide clock change'
Reject {param($r)$r.rows[0].after.owner_before=(New-Owner);$r.rows[0].after.owner_before.rig=6;$r.rows[0].after.owner_after.rig=6} 'pair cannot bridge different qualified owners'
$bad=$false;try{[void](Test-BmxFirstMountProducerEnvelope (New-Report) 123 $created $started $started.AddSeconds(-1))}catch{$bad=$true};Require $bad 'stale report mtime rejected'
$bad=$false;try{[void](Test-BmxFirstMountProducerEnvelope (New-Report) 123 $created $started ([DateTime]::UtcNow.AddMinutes(1)))}catch{$bad=$true};Require $bad 'future report mtime rejected'
$bad=$false;try{[void](ConvertFrom-BmxStrictJson '{"schema_version":1,"SCHEMA_VERSION":1}')}catch{$bad=$true};Require $bad 'case-colliding JSON properties rejected before type validation'
# Bounded UTF8 read uses temporary fixture files in this test script's folder.
$tmp=Join-Path $PSScriptRoot ('producer-fixture-'+[guid]::NewGuid().ToString('N')+'.json')
try{
    $text=(New-Report|ConvertTo-Json -Depth 20 -Compress);[IO.File]::WriteAllText($tmp,$text,[Text.UTF8Encoding]::new($false))
    $read=Read-BmxFirstMountProducerReport $tmp;Require ($read.bytes -eq [Text.Encoding]::UTF8.GetByteCount($text) -and $read.text -ceq $text) 'bounded read returns exact parsed text/byte count'
    Require ($read.sha256 -ceq (Get-FileHash -LiteralPath $tmp -Algorithm SHA256).Hash) 'bounded read hash describes exact bytes parsed'
    [IO.File]::WriteAllBytes($tmp,[byte[]]::new(2097153));$bad=$false;try{[void](Read-BmxFirstMountProducerReport $tmp)}catch{$bad=$true};Require $bad 'oversized report read rejected'
    [IO.File]::WriteAllBytes($tmp,[byte[]]@(255));$bad=$false;try{[void](Read-BmxFirstMountProducerReport $tmp)}catch{$bad=$true};Require $bad 'invalid UTF8 rejected'
}finally{if(Test-Path -LiteralPath $tmp){Remove-Item -LiteralPath $tmp}}
# Parse staged integration, then execute ONLY extracted data-only guards/Boolean
# acceptance expression. Never dot-source or invoke Start/Run supervisors.
$stagedRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$startPath=Join-Path $stagedRoot 'contrib\bmx-lab\Start-BmxGame.ps1';$runPath=Join-Path $stagedRoot 'contrib\bmx-lab\Run-BmxPostFallRemount.ps1'
if((Test-Path -LiteralPath $startPath) -and (Test-Path -LiteralPath $runPath)){
    $tokens=$null;$errors=$null;$startAst=[Management.Automation.Language.Parser]::ParseFile($startPath,[ref]$tokens,[ref]$errors);Require ($errors.Count -eq 0) 'Start integration parses without executing'
    $runAst=[Management.Automation.Language.Parser]::ParseFile($runPath,[ref]$tokens,[ref]$errors);Require ($errors.Count -eq 0) 'Run integration parses without executing'
    $startIf=$startAst.FindAll({param($n)$n -is [Management.Automation.Language.IfStatementAst] -and $n.Extent.Text.StartsWith('if ($FirstMountProducerProbe -and ')},$true)
    $runIf=$runAst.FindAll({param($n)$n -is [Management.Automation.Language.IfStatementAst] -and $n.Extent.Text.StartsWith('if ($FirstMountProducerProbe -and ')},$true)
    Require ($startIf.Count -eq 1 -and $runIf.Count -eq 1) 'one explicit early opt-in guard at each entry point'
    function Check-StartGuard([bool]$FirstMountProducerProbe,[bool]$PhaseProbe,[bool]$PostFallRemountTest,[bool]$ExpectedReject){
        $bad=$false;try{& ([scriptblock]::Create($startIf[0].Extent.Text))}catch{$bad=$true};Require ($bad -eq $ExpectedReject) 'Start opt-in requires both private mode and phase probe'
    }
    Check-StartGuard $false $false $false $false;Check-StartGuard $true $false $false $true;Check-StartGuard $true $true $false $true;Check-StartGuard $true $false $true $true;Check-StartGuard $true $true $true $false
    function Check-RunGuard([bool]$FirstMountProducerProbe,[bool]$PhaseProbe,[bool]$ExpectedReject){
        $bad=$false;try{& ([scriptblock]::Create($runIf[0].Extent.Text))}catch{$bad=$true};Require ($bad -eq $ExpectedReject) 'Run opt-in requires phase probe'
    }
    Check-RunGuard $false $false $false;Check-RunGuard $true $false $true;Check-RunGuard $true $true $false
    $envAssignment=$startAst.FindAll({param($n)$n -is [Management.Automation.Language.AssignmentStatementAst] -and $n.Left.Extent.Text -ceq "`$start.Environment['RESKATE_BMX_FIRST_MOUNT_PRODUCER_PROBE']"},$true)
    Require ($envAssignment.Count -eq 1) 'explicit environment override is present'
    function Check-Environment([bool]$FirstMountProducerProbe,[string]$Expected){$value=& ([scriptblock]::Create($envAssignment[0].Right.Extent.Text));Require ($value -ceq $Expected) 'new mode environment flag explicitly resets inherited state'}
    Check-Environment $false '0';Check-Environment $true '1'
    $passAssignment=$runAst.FindAll({param($n)$n -is [Management.Automation.Language.AssignmentStatementAst] -and $n.Left.Extent.Text -ceq '$bmxRunInfo.passed'},$true)
    Require ($passAssignment.Count -eq 1) 'one final strict acceptance assignment'
    function Check-FinalGate([bool]$FirstMountProducerProbe,$Observation,$ObservationError,[bool]$Expected,
        $ComparisonObservation=([pscustomobject]@{identity='synthetic validated routing report'}),$ComparisonError=$null){
        $bmxGetupProgress=[pscustomobject]@{complete=$true};$CaptureGateDirectory='';$PhaseProbe=$false
        $bmxRunInfo=[pscustomobject]@{passed=$false;native_fall_passed=$true;process_teardown_verified=$true;source_unchanged_after_run=$true;native_terminal_written=$true
            recording_gate_state='disabled';phase_observation=$null;phase_observation_error=$null;first_mount_producer_observation=$Observation;first_mount_producer_observation_error=$ObservationError
            rider_pose_comparison_observation=$ComparisonObservation;rider_pose_comparison_error=$ComparisonError}
        . ([scriptblock]::Create($passAssignment[0].Extent.Text));Require ([bool]$bmxRunInfo.passed -eq $Expected) 'final acceptance requires requested producer evidence and independently valid routing evidence'
    }
    Check-FinalGate $false $null $null $true;Check-FinalGate $true $null $null $false;Check-FinalGate $true ([pscustomobject]@{identity='synthetic validated report'}) $null $true;Check-FinalGate $true ([pscustomobject]@{}) 'synthetic validation error' $false
    Check-FinalGate $true ([pscustomobject]@{identity='synthetic validated report'}) $null $false $null $null
    Check-FinalGate $true ([pscustomobject]@{identity='synthetic validated report'}) $null $false ([pscustomobject]@{identity='synthetic routing report'}) 'synthetic routing validation error'
}
. (Join-Path $repo 'contrib\bmx-lab\PhaseProbeEvidence.ps1')
function Get-BmxGetupProgress {param($text,$processId,$variant,$impact,$action,$trace,$earliestAt);[pscustomobject]@{complete=$false;issue='synthetic unknown native outcome'}}
$log="[2026-10-19 12:00:02.000] [123:1] Startup command queued: bmx phase-dump`n[2026-10-19 12:00:04.000] [123:1] Startup command queued: bmx interact"
$early=[DateTime]::Parse('2026-10-19T11:00:00');$written=[DateTime]::Parse('2026-10-19T12:00:03')
$order=Get-BmxPhaseCheckpoint $log 123 2 $null $null $null $early $written
Require ($order.dump_before_interaction -and $order.file_written_before_interaction -and -not $order.native_getup_and_standing_before_dump) 'file ordering remains distinct from native physics outcome'
$order=Get-BmxPhaseCheckpoint $log 123 2 $null $null $null $early ([DateTime]::Parse('2026-10-19T12:00:05'))
Require (-not $order.file_written_before_interaction) 'late file export cannot establish before-remount ordering'
$order=Get-BmxPhaseCheckpoint $log 124 2 $null $null $null $early $written
Require (-not $order.dump_before_interaction) 'foreign process commands cannot establish export ordering'
Write-Output ('First-mount producer data-only fixtures passed ('+$script:checks+' checks).')
