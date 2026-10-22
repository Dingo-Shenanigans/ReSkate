#requires -Version 7.0
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'PoseComparisonEvidence.ps1')
$script:checks=0
function Require($Value,[string]$Why){$script:checks++;if(-not $Value){throw $Why}}
function Reject([scriptblock]$Body,[string]$Why){$rejected=$false;try{& $Body|Out-Null}catch{$rejected=$true};Require $rejected $Why}
$good=@{Mode='RenderOnly';Phase=$true;Producer=$true;PostFall=$true;GetUp=$true;Impact='side';NativeChassis=$true;Unattended=$true;Profile='bmx-user-data-chassis-probe'}
Assert-BmxPoseComparisonConfiguration @good;Require $true 'exact private RenderOnly scope accepted'
$none=$good.Clone();$none.Mode='None';Assert-BmxPoseComparisonConfiguration @none;Require $true 'exact private None scope accepted'
Assert-BmxPoseComparisonConfiguration Default $false $false $false $false '' $false $false '';Require $true 'ordinary default unchanged'
foreach($field in @('Phase','Producer','PostFall','GetUp','NativeChassis','Unattended')){
    $bad=$good.Clone();$bad[$field]=$false;Reject {Assert-BmxPoseComparisonConfiguration @bad} ('missing '+$field+' refused')
}
foreach($field in @('Impact','Profile','Mode')){
    $bad=$good.Clone();$bad[$field]='wrong';Reject {Assert-BmxPoseComparisonConfiguration @bad} ('wrong '+$field+' refused')
}
$bad=$good.Clone();$bad.Mode='renderonly';Reject {Assert-BmxPoseComparisonConfiguration @bad} 'case-folded mode refused'
$started=[DateTime]::SpecifyKind([DateTime]'2026-10-20 01:00:00',[DateTimeKind]::Local)
$created=$started.AddSeconds(1);$ended=$started.AddSeconds(10)
function New-Line([string]$Mode='RenderOnly',[string]$Scope='true',[string]$Refused='false',[string]$Poses='4',[string]$Renders='4',[string]$Animation='9',[string]$Render='0',[string]$AnimationDispatches='0',[string]$RenderDispatches='0',[int]$ProcessId=123,[int]$Seconds=3){
    '['+$started.AddSeconds($Seconds).ToString('yyyy-MM-dd HH:mm:ss.fff')+'] ['+$ProcessId+':7] SDK: [Skater] [INFO] Native velocity updates: 3; rider pose updates='+$Poses+'; pose listener=ready, rider render updates='+$Renders+', private rider comparison='+$Mode+', comparison scope='+$Scope+', comparison refused='+$Refused+', suppressed animation applies='+$Animation+', suppressed render applies='+$Render+', animation apply dispatches='+$AnimationDispatches+', render apply dispatches='+$RenderDispatches
}
function Evidence([string]$Text,[string]$Mode='RenderOnly',[bool]$Scope=$true){Get-BmxPoseComparisonEvidence $Text $Mode $Scope 123 $created $started $ended}
$r=Evidence ((New-Line)+"`n"+(New-Line -Poses 8 -Renders 8 -Animation 18 -Seconds 4))
Require ($r.rows.Count -eq 2 -and $r.mode -ceq 'RenderOnly' -and -not $r.custom_rider_application_counters_zero) 'owned RenderOnly counters proved without animation success'
$r=Evidence (New-Line -Mode None -Poses 0 -Renders 0 -Animation 9 -Render 8) None
Require $r.custom_rider_application_counters_zero 'None records zero custom rider applications with independent suppressions'
$r=Evidence (New-Line -Mode Default -Animation 0) Default
Require ($r.rows.Count -eq 1) 'qualified Default control accepted'
$r=Evidence (New-Line -Mode Default -Scope false -Animation 0) Default $false
Require (-not $r.qualified_scope) 'ordinary Default evidence remains unqualified'
Reject {Evidence (New-Line -Mode Default)} 'wrong observed mode'
Reject {Evidence (New-Line -Scope false)} 'scope refusal'
Reject {Evidence (New-Line -Refused true)} 'explicit refusal'
Reject {Evidence (New-Line -AnimationDispatches 1)} 'animation apply dispatched in RenderOnly'
Reject {Evidence (New-Line -Render 1)} 'render suppressed in RenderOnly'
Reject {Evidence (New-Line -Animation 0)} 'no observed suppression'
Reject {Evidence (New-Line -Mode None -Poses 1 -Renders 1 -Render 9) None} 'None cannot claim completed application'
Reject {Evidence (New-Line -Mode None -Poses 0 -Renders 0 -Render 0) None} 'None needs observed render suppression'
Reject {Evidence (New-Line -Mode Default -Animation 1) Default} 'Default cannot suppress'
$r=Evidence (New-Line -Poses 4 -Renders 5 -RenderDispatches 6)
Require ($r.rows[0].renders -gt $r.rows[0].poses) 'separately sampled total/render counters do not invent an atomic invariant'
Reject {Evidence (New-Line -Mode None -Poses 0 -Renders 0 -Animation 9 -Render 9 -RenderDispatches 1) None} 'None cannot dispatch render apply'
Reject {Evidence (New-Line -Mode None -Poses 0 -Renders 0 -Animation 9 -Render 9 -AnimationDispatches 1) None} 'None cannot dispatch animation apply'
Reject {Evidence (New-Line -ProcessId 456)} 'other PID cannot provide evidence'
Reject {Evidence (New-Line -Seconds 0)} 'stale PID log excluded'
Reject {Evidence (New-Line -Seconds 11)} 'future log excluded'
Reject {Evidence ((New-Line -Animation 10)+"`n"+(New-Line -Animation 9 -Seconds 4))} 'counter regression rejected'
Reject {Evidence ((New-Line -Seconds 4)+"`n"+(New-Line -Seconds 3))} 'wall observation regression rejected'
Reject {Evidence (New-Line -Poses '18446744073709551616' -Renders '18446744073709551616')} 'UInt64 overflow rejected'
Reject {Evidence ((New-Line).Replace('comparison scope=true','comparison scope=TRUE'))} 'exact boolean shape required'
Reject {Evidence ((New-Line).Replace('rider pose updates=4;','rider pose updates=-1;'))} 'negative count rejected'
Reject {Evidence ((New-Line).Replace('private rider comparison=RenderOnly','private rider comparison=Unknown'))} 'unknown mode rejected'
Reject {Evidence ((New-Line)+', private rider comparison=RenderOnly')} 'duplicate mode evidence rejected'
Reject {Get-BmxPoseComparisonEvidence (New-Line) RenderOnly $true 123 $created $started $created.AddSeconds(-1)} 'invalid interval rejected'
$r=Evidence ((New-Line -ProcessId 456)+"`n"+(New-Line))
Require ($r.rows.Count -eq 1) 'unowned status ignored when owned evidence exists'

$basisGood=@{Mode='PreserveSuppliedBasis';RiderMode='None';Phase=$true;Producer=$true;PostFall=$true;GetUp=$true;Impact='side';NativeChassis=$true;Unattended=$true;Profile='bmx-user-data-chassis-probe'}
Assert-BmxMotionBasisConfiguration @basisGood;Require $true 'qualified None-only basis contrast accepted'
Assert-BmxMotionBasisConfiguration Default Default $false $false $false $false '' $false $false '';Require $true 'default basis needs no experiment flags'
foreach($field in @('Phase','Producer','PostFall','GetUp','NativeChassis','Unattended')){$bad=$basisGood.Clone();$bad[$field]=$false;Reject {Assert-BmxMotionBasisConfiguration @bad} ('basis missing '+$field)}
foreach($field in @('Mode','RiderMode','Impact','Profile')){$bad=$basisGood.Clone();$bad[$field]='wrong';Reject {Assert-BmxMotionBasisConfiguration @bad} ('basis wrong '+$field)}
function New-BasisLine([string]$Mode='PreserveSuppliedBasis',[string]$Scope='true',[string]$Refused='false',[string]$Selected='4',[string]$Preserved='4',[int]$ProcessId=123,[int]$Seconds=3){
    '['+$started.AddSeconds($Seconds).ToString('yyyy-MM-dd HH:mm:ss.fff')+'] ['+$ProcessId+':7] SDK: [Console] [INFO] BMX; motion basis='+$Mode+', basis scope='+$Scope+', basis refused='+$Refused+', selected full rotation targets='+$Selected+', preserved supplied bases='+$Preserved
}
function BasisEvidence([string]$Text,[string]$Mode='PreserveSuppliedBasis'){Get-BmxMotionBasisEvidence $Text $Mode 123 $created $started $ended}
$basis=BasisEvidence ((New-BasisLine)+"`n"+(New-BasisLine -Selected 9 -Preserved 8 -Seconds 4));Require ($basis.rows.Count -eq 2) 'qualified preserved basis selection evidence'
$basisDefault=BasisEvidence (New-BasisLine -Mode Default -Scope false -Preserved 0) Default;Require (-not $basisDefault.qualified_scope) 'Default leaves experiment disabled'
Require ((BasisEvidence (New-BasisLine -Selected 3 -Preserved 4)).rows.Count -eq 1) 'independent counter snapshots are not treated as atomic pairs'
Reject {BasisEvidence (New-BasisLine -Mode Default)} 'wrong observed basis mode'
Reject {BasisEvidence (New-BasisLine -Scope false)} 'private basis scope missing'
Reject {BasisEvidence (New-BasisLine -Refused true)} 'private basis refused'
Reject {BasisEvidence (New-BasisLine -Selected 0 -Preserved 0)} 'variant with zero actual selections does not validate'
Reject {BasisEvidence (New-BasisLine -Mode Default -Scope false -Preserved 1) Default} 'Default cannot preserve experiment basis'
Reject {BasisEvidence (New-BasisLine -ProcessId 456)} 'foreign PID cannot supply basis evidence'
Reject {BasisEvidence (New-BasisLine -Seconds 0)} 'basis stale process interval'
Reject {BasisEvidence (New-BasisLine -Seconds 11)} 'basis future process interval'
Reject {BasisEvidence ((New-BasisLine)+', motion basis=PreserveSuppliedBasis')} 'basis duplicate selector ambiguous'
Reject {BasisEvidence (New-BasisLine -Preserved '18446744073709551616')} 'basis counter overflow rejected'
Reject {BasisEvidence ((New-BasisLine)+"`n"+(New-BasisLine -Preserved 3 -Seconds 4))} 'basis counters cannot regress'
function New-BasisRun {
    [pscustomobject]@{owned_pid=123;owned_created_utc=$created.ToUniversalTime().ToString('o');source_unchanged_after_run=$true;process_teardown_verified=$true;teardown_absence_observations=2;
        motion_basis_observation=(BasisEvidence (New-BasisLine));motion_basis_observation_error=$null;rider_pose_comparison_mode='None';
        phase_observation=[pscustomobject]@{valid=$true};phase_observation_error=$null;first_mount_producer_observation=[pscustomobject]@{valid=$true};first_mount_producer_observation_error=$null;
        rider_pose_comparison_observation=[pscustomobject]@{mode='None';qualified_scope=$true;custom_rider_application_counters_zero=$true};rider_pose_comparison_error=$null}
}
Require (Test-BmxMotionBasisMeasurement (New-BasisRun) PreserveSuppliedBasis) 'distinct measurement validity requires owned variant and None evidence'
foreach($field in @('source_unchanged_after_run','process_teardown_verified')){$run=New-BasisRun;$run.$field=$false;Require (-not (Test-BmxMotionBasisMeasurement $run PreserveSuppliedBasis)) ('measurement requires '+$field)}
foreach($field in @('phase_observation_error','first_mount_producer_observation_error','rider_pose_comparison_error','motion_basis_observation_error')){$run=New-BasisRun;$run.$field='error';Require (-not (Test-BmxMotionBasisMeasurement $run PreserveSuppliedBasis)) ('measurement rejects '+$field)}
$run=New-BasisRun;$run.teardown_absence_observations=1;Require (-not (Test-BmxMotionBasisMeasurement $run PreserveSuppliedBasis)) 'basis measurement requires two owned absence observations'
$run=New-BasisRun;$run.rider_pose_comparison_observation.custom_rider_application_counters_zero=$false;Require (-not (Test-BmxMotionBasisMeasurement $run PreserveSuppliedBasis)) 'basis contrast cannot claim None when custom writes occurred'
$run=New-BasisRun;$run.rider_pose_comparison_mode='Default';Require (-not (Test-BmxMotionBasisMeasurement $run PreserveSuppliedBasis)) 'no full-pose comparison acceptance exception'
$run=New-BasisRun;$run.motion_basis_observation.process_id=456;Require (-not (Test-BmxMotionBasisMeasurement $run PreserveSuppliedBasis)) 'measurement cannot join foreign process'
$run=New-BasisRun;$run.motion_basis_observation.owned_created_utc=$created.AddSeconds(1).ToUniversalTime().ToString('o');Require (-not (Test-BmxMotionBasisMeasurement $run PreserveSuppliedBasis)) 'measurement cannot join reused PID creation'
Write-Output ('Pose comparison data-only checks passed: '+$script:checks)
