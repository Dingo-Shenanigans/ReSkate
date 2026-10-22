param([string]$CandidateRoot=([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))))
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'BoardHandoffEvidence.ps1')
function Check([bool]$Value,[string]$Reason){if(-not $Value){throw $Reason}}
$repo='C:\Users\Jeffr\Desktop\reskate';$profile='bmx-user-data-chassis-probe'
$quick=@{NativeChassis=$true;SwitchTest=$true;QuickSwitchTest=$true;RiderPoseMode='Default';MotionBasisMode='Default'}
$native=@{NativeChassis=$true;Ride=$true;RiderPoseMode='Default';MotionBasisMode='Default'}
$positive=0;$negative=0
$active=$native.Clone();$active.NativeBoardControl=$true
foreach($pair in @(@('QuickSwitch',$quick,'quick-switch'),@('NativeOnly',$native,'native-only'))){
    $s=Get-BmxBoardHandoffSelection $pair[0] $repo $profile $pair[1]
    Check ($s.trace -ceq '1' -and $s.mode -ceq $pair[2]) 'Explicit diagnostic selector mismatch.';++$positive
}
foreach($options in @(@{},@{Ride=$true},@{SwitchTest=$true},@{RampTest=$true},@{UnattendedTest=$true},@{QuickSwitchTest=$true})){
    $environment=@{RESKATE_BMX_BOARD_HANDOFF_TRACE='1';RESKATE_BMX_BOARD_HANDOFF_MODE='quick-switch';RESKATE_BMX_NATIVE_BOARD_CONTROL='1'}
    $s=Get-BmxBoardHandoffSelection 'None' $repo 'ordinary-profile' $options
    Set-BmxBoardHandoffEnvironment $environment $s
    Check ($environment.RESKATE_BMX_BOARD_HANDOFF_TRACE -ceq '0' -and $environment.RESKATE_BMX_BOARD_HANDOFF_MODE -ceq '' -and $environment.RESKATE_BMX_NATIVE_BOARD_CONTROL -ceq '0') 'Inherited selector leaked into ordinary/default launch.';++$positive
}
function Reject([string]$Mode,[string]$Root,[string]$Profile,[hashtable]$Options){
    $rejected=$false;try{Get-BmxBoardHandoffSelection $Mode $Root $Profile $Options|Out-Null}catch{$rejected=$true}
    Check $rejected ('Rejected configuration accepted: '+$Mode+'/'+$Profile);$script:negative++
}
foreach($mode in @('QuickSwitch','NativeOnly')){
    $valid=if($mode -ceq 'QuickSwitch'){$quick}else{$native}
    foreach($badProfile in @('bmx-user-data','bmx-user-data-working-profile','bmx-user-data-chassis-probe-extra','BMX-USER-DATA-CHASSIS-PROBE')){Reject $mode $repo $badProfile $valid}
    foreach($badRoot in @('C:\Users\Jeffr\Desktop\other','C:\Users\Jeffr\Desktop\reskate-copy')){Reject $mode $badRoot $profile $valid}
    foreach($key in @('UnattendedTest','GripProbe','IntentActivation','ImpactRecovery','GetUpPulse','PostFallRemountTest','PhaseProbe','FirstMountProducerProbe','IntentObserver','BindingObserver','CatalogTest','NativeFallTest','ProfilePerformance','TrickTest','RampTest','RampRideTest','ChassisProbe','ChassisPulse','ChassisCalibration','SmokeTest','NativeTestSkater','NativeStartupDelay','NativePlayerStartup','NativeObserveOnly','UseSteam','UseLooseFiles','DisableNativeLocalProfile')){
        $bad=$valid.Clone();$bad[$key]=$true;Reject $mode $repo $profile $bad
    }
    foreach($pair in @(@('RiderPoseMode','RenderOnly'),@('MotionBasisMode','PreserveSuppliedBasis'),@('ImpactTest','side'),@('Map','other'))){$bad=$valid.Clone();$bad[$pair[0]]=$pair[1];Reject $mode $repo $profile $bad}
    $bad=$valid.Clone();$bad.NativeChassis=$false;Reject $mode $repo $profile $bad
}
foreach($key in @('SwitchTest','QuickSwitchTest')){$bad=$quick.Clone();$bad[$key]=$false;Reject 'QuickSwitch' $repo $profile $bad}
$bad=$quick.Clone();$bad.Ride=$true;Reject 'QuickSwitch' $repo $profile $bad
foreach($key in @('SwitchTest','QuickSwitchTest')){$bad=$native.Clone();$bad[$key]=$true;Reject 'NativeOnly' $repo $profile $bad}
$bad=$native.Clone();$bad.Ride=$false;Reject 'NativeOnly' $repo $profile $bad
$activeSelection=Get-BmxBoardHandoffSelection NativeOnly $repo $profile $active
Check ($activeSelection.native_board_control -ceq '1') 'Explicit active control did not select its own flag.';++$positive
foreach($mode in @('None','QuickSwitch')){Reject $mode $repo $profile $active}
$s=Get-BmxBoardHandoffSelection NativeOnly $repo $profile $native
Check ($s.native_board_control -ceq '0') 'Manual NativeOnly default changed.';++$positive
Check ((Get-BmxBoardHandoffNativeControlCommands $true) -ceq 'wait 15;nobail 0;wait 1;nobail;bmx handoff-trace native-board;wait 1;bmx handoff-trace status') 'Active baseline command must remain explicitly labeled and contain no BMX mount.';++$positive
Check (Test-BmxBoardHandoffCaptureGateAllowed $s $true $false $false $false $profile) 'Exact native control capture gate rejected.'
$none=Get-BmxBoardHandoffSelection None $repo $profile @{}
Check (-not(Test-BmxBoardHandoffCaptureGateAllowed $none $true $false $false $false $profile)) 'Ordinary Ride capture gate widened.'
Check (Test-BmxBoardHandoffCaptureGateAllowed $none $false $false $true $false $profile) 'Existing private switch capture gate regressed.'
$commands=Get-BmxBoardHandoffNativeControlCommands
Check ($commands -ceq 'wait 15;nobail 0;wait 1;nobail;bmx handoff-trace arm;wait 1;bmx handoff-trace status' -and $commands -notmatch 'bmx mount|bmx toggle|bmx interact|selftest') 'NativeOnly command stream injects a mount or BMX action.'
# Parse actual launcher and execute only actual parameter/env/startup expressions.
$tokens=$null;$errors=$null;$launcher=Join-Path $PSScriptRoot 'Start-BmxGame.ps1'
$ast=[Management.Automation.Language.Parser]::ParseFile($launcher,[ref]$tokens,[ref]$errors);Check (-not $errors) 'Launcher parse failed.'
$envStatement=@($ast.EndBlock.Statements|Where-Object{$_.Extent.Text.StartsWith('Set-BmxBoardHandoffEnvironment ')})
Check ($envStatement.Count -eq 1) 'Actual launcher must overwrite/clear both selectors once.'
$modeAssignment=@($ast.EndBlock.Statements|Where-Object{$_.Extent.Text.StartsWith("`$start.Environment['RESKATE_BMX_LAUNCH_MODE'] =")})
Check ($modeAssignment.Count -eq 1) 'Actual launch-mode assignment missing.'
$start=[Diagnostics.ProcessStartInfo]::new();$Ride=$true;$UnattendedTest=$false;$SwitchTest=$false
& ([scriptblock]::Create($modeAssignment[0].Extent.Text))
Check ($start.Environment['RESKATE_BMX_LAUNCH_MODE'] -ceq 'human-ride') 'Native-only must retain actual human-ride launch mode.'
foreach($mode in @('None','QuickSwitch','NativeOnly')){
    $opts=if($mode -ceq 'QuickSwitch'){$quick}elseif($mode -ceq 'NativeOnly'){$native}else{@{}}
    $bmxBoardSelection=Get-BmxBoardHandoffSelection $mode $repo $profile $opts
    $start.Environment['RESKATE_BMX_BOARD_HANDOFF_TRACE']='1';$start.Environment['RESKATE_BMX_BOARD_HANDOFF_MODE']='inherited';$start.Environment['RESKATE_BMX_NATIVE_BOARD_CONTROL']='1'
    & ([scriptblock]::Create($envStatement[0].Extent.Text))
    Check ($start.Environment['RESKATE_BMX_BOARD_HANDOFF_TRACE'] -ceq $bmxBoardSelection.trace -and $start.Environment['RESKATE_BMX_BOARD_HANDOFF_MODE'] -ceq $bmxBoardSelection.mode -and $start.Environment['RESKATE_BMX_NATIVE_BOARD_CONTROL'] -ceq $bmxBoardSelection.native_board_control) 'Actual setter statement mismatch.'
}
$startup=@($ast.EndBlock.Statements|Where-Object{$_.Extent.Text.StartsWith("`$start.Environment['RESKATE_STARTUP_COMMANDS'] =")})
Check ($startup.Count -eq 1) 'Actual startup command expression missing.'
foreach($mode in @('None','QuickSwitch','NativeOnly')){
    $QuickSwitchTest=$mode -ceq 'QuickSwitch';$Ride=-not $QuickSwitchTest
    $bmxBoardSelection=Get-BmxBoardHandoffSelection $mode $repo $profile $(if($QuickSwitchTest){$quick}else{$native})
    & ([scriptblock]::Create($startup[0].Extent.Text))
    $expected=if($mode -ceq 'NativeOnly'){Get-BmxBoardHandoffNativeControlCommands}elseif($QuickSwitchTest){'wait 15;bmx quicktest'}else{'wait 15;nobail 0;wait 1;nobail;bmx mount;wait 20;bmx status'}
    Check ($start.Environment['RESKATE_STARTUP_COMMANDS'] -ceq $expected) ('Actual startup schedule mismatch: '+$mode)
}
# Execute only actual parameter/selector AST; never supervisor side effects.
$supervisor=Join-Path $CandidateRoot 'build\bmx\run_native_chassis_ride.ps1'
$st=$null;$se=$null;$sa=[Management.Automation.Language.Parser]::ParseFile($supervisor,[ref]$st,[ref]$se)
Check (-not $se) 'Native-control supervisor parse failed.'
$firstGate=@($sa.EndBlock.Statements|Where-Object{$_.Extent.Text.StartsWith('if ($NativeBoardControl)')})
Check ($firstGate.Count -eq 1) 'Exactly one top-level exclusive active-control gate required.'
$gateScript=[scriptblock]::Create($sa.ParamBlock.Extent.Text+"`n"+$firstGate[0].Extent.Text)
$goodArgs=@{NativeBoardControl=$true;BoardHandoffMode='NativeOnly';CaptureGateDirectory='C:\Users\Jeffr\Desktop\reskate\build\fixture-gate';CaptureGateNonce='0123456789abcdef0123456789abcdef'}
& $gateScript @goodArgs;++$positive
foreach($fault in @('Switch','QuickSwitch','Tricks','Catalog','BindingObserver','CaptureGateDirectory','CaptureGateNonce','BoardHandoffMode')){
    $badArgs=$goodArgs.Clone();$badArgs[$fault]=if($fault -in @('CaptureGateDirectory','CaptureGateNonce')){''}elseif($fault -eq 'BoardHandoffMode'){'QuickSwitch'}else{$true}
    $rejected=$false;try{& $gateScript @badArgs}catch{$rejected=$true}
    Check $rejected ('Actual supervisor active gate admitted '+$fault);++$negative
}
$bmxBoardSelection=Get-BmxBoardHandoffSelection NativeOnly $repo $profile $active
$Ride=$true;$UnattendedTest=$false;$SwitchTest=$false;$QuickSwitchTest=$false
& ([scriptblock]::Create($startup[0].Extent.Text));& ([scriptblock]::Create($envStatement[0].Extent.Text))
Check ($start.Environment['RESKATE_STARTUP_COMMANDS'] -ceq (Get-BmxBoardHandoffNativeControlCommands $true) -and
    $start.Environment['RESKATE_BMX_NATIVE_BOARD_CONTROL'] -ceq '1') 'Actual launcher did not select the explicit active command and flag together.';++$positive
$pins=@(Get-BmxBoardHandoffSourcePins);Check ($pins.Count -eq @($pins|Sort-Object -Unique).Count) 'Duplicate helper source pins.'
$supervisors=@('build\bmx\run_native_chassis_ride.ps1','build\bmx\run_native_ramp_probe.ps1','build\bmx\run_native_getup_test.ps1','build\bmx\run_native_impact_test.ps1','build\bmx\run_native_fall_observation.ps1','build\bmx\run_chassis_drop.ps1','build\bmx\run_manual_nobail_bootstrap_test.ps1','contrib\bmx-lab\Run-BmxPostFallRemount.ps1')
foreach($relative in $supervisors){
    $path=Join-Path $CandidateRoot $relative;$t=Get-Content -LiteralPath $path -Raw
    $tokens=$null;$errors=$null;$superAst=[Management.Automation.Language.Parser]::ParseFile($path,[ref]$tokens,[ref]$errors);Check (-not $errors) ('Supervisor parse failed: '+$relative)
    Check ($t.Contains("BoardHandoffEvidence.ps1") -and $t.Contains('Get-BmxBoardHandoffSourcePins')) ('Source pin enrollment missing: '+$relative)
    # Execute only the actual data-only pin expression/append AST, no loop body.
    if($relative.EndsWith('run_chassis_drop.ps1')) {
        $loop=@($superAst.FindAll({param($n) $n -is [Management.Automation.Language.ForEachStatementAst] -and $n.Variable.VariablePath.UserPath -ceq 'bmxSourceRelative'},$true))
        Check ($loop.Count -eq 1) 'Expected one inline source archive enumeration.'
        $actual=@(& ([scriptblock]::Create($loop[0].Condition.Extent.Text)))
    } else {
        $append=@($superAst.FindAll({param($n) $n -is [Management.Automation.Language.AssignmentStatementAst] -and $n.Right.Extent.Text.Contains('Get-BmxBoardHandoffSourcePins')},$true))
        Check ($append.Count -eq 1) ('Expected one real pin append: '+$relative)
        $arrayName=$append[0].Left.Extent.Text
        $actual=@(& ([scriptblock]::Create($arrayName+'=@()'+"`n"+$append[0].Extent.Text+"`n"+$arrayName)))
    }
    foreach($required in $pins){Check ($actual -ccontains $required) ('Actual source enumeration omitted '+$required+' in '+$relative)}
    $repoVariable=if($relative.EndsWith('run_native_ramp_probe.ps1')){'rampRepo'}elseif($relative.EndsWith('run_manual_nobail_bootstrap_test.ps1')){'bmxRepo'}else{'bmxRepoRoot'}
    Check ($t.Contains('$'+$repoVariable+' = [IO.Path]::GetFullPath') -and $t.Contains(". (Join-Path `$"+$repoVariable+" 'contrib\bmx-lab\BoardHandoffEvidence.ps1')")) ('Pin helper root variable must be an actual initialized repo variable: '+$relative)
}
foreach($required in @('Extension\BMX\bmx_commands.cpp','Extension\BMX\bmx_board_handoff_trace.h','Engine\Game\BMX\bmx_board_handoff_policy.h','Engine\Game\BMX\Test\bmx_board_handoff_policy_tests.cpp')){Check ($pins -ccontains $required) ('Required source pin missing: '+$required)}
Write-Output "Board handoff selectors: $positive positives, $negative negatives; actual launcher env/mode AST, native-only active-control command boundary, capture gate and all8 source-pin enrollments passed. No launch, game or production build performed."
