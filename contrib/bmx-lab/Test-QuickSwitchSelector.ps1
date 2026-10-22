$ErrorActionPreference='Stop'
# Extract ONLY the actual parameter block, new rejection guard and environment
# assignment expressions. No launcher body, filesystem or process start runs.
$tokens=$null;$errors=$null
$ast=[System.Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'Start-BmxGame.ps1'),[ref]$tokens,[ref]$errors)
if ($errors) { throw 'Launcher failed AST parsing.' }
$guard=@($ast.EndBlock.Statements | Where-Object { $_.Extent.Text.StartsWith('if ($QuickSwitchTest -and') })
if ($guard.Count -ne 1) { throw 'Expected one actual explicit quick-switch launch guard.' }
$selectors=@('RESKATE_BMX_QUICK_SWITCH_TEST','RESKATE_BMX_LAUNCH_MODE','RESKATE_BMX_AUTOMATED_SWITCH','RESKATE_BMX_AUTOMATED_RIDE','RESKATE_BMX_NATIVE_CHASSIS')
$expressions=@()
foreach ($selector in $selectors) {
    $prefix='$start.Environment['+"'"+$selector+"'] ="
    $expression=@($ast.EndBlock.Statements | Where-Object { $_.Extent.Text.StartsWith($prefix) })
    if ($expression.Count -ne 1) { throw "Missing or duplicate actual environment assignment: $selector" }
    $expressions+=$expression[0].Extent.Text
}
$body=$ast.ParamBlock.Extent.Text+"`n"+$guard[0].Extent.Text+"`n"+@'
$start=[Diagnostics.ProcessStartInfo]::new()
$start.Environment['RESKATE_BMX_QUICK_SWITCH_TEST']='1'
$start.Environment['RESKATE_BMX_LAUNCH_MODE']='unattended-test'
'@+"`n"+($expressions -join "`n")+"`n"+@'
[pscustomobject]@{
    quick=$start.Environment['RESKATE_BMX_QUICK_SWITCH_TEST'];mode=$start.Environment['RESKATE_BMX_LAUNCH_MODE']
    automated_switch=$start.Environment['RESKATE_BMX_AUTOMATED_SWITCH'];automated_ride=$start.Environment['RESKATE_BMX_AUTOMATED_RIDE']
    native=$start.Environment['RESKATE_BMX_NATIVE_CHASSIS']
}
'@
$select=[scriptblock]::Create($body)
$valid=@{QuickSwitchTest=$true;SwitchTest=$true;NativeChassis=$true;ProfileName='bmx-user-data-chassis-probe'}
$r=& $select @valid
if ($r.quick -cne '1' -or $r.mode -cne 'switch-test' -or $r.automated_switch -cne '1' -or $r.automated_ride -cne '0' -or $r.native -cne '1') { throw 'Explicit private quick-switch selector failed.' }
$negative=0
foreach ($missing in @('SwitchTest','NativeChassis')) {
    $bad=$valid.Clone();$bad.Remove($missing);$rejected=$false
    try { & $select @bad | Out-Null } catch { $rejected=$true }
    if (-not $rejected) { throw "Missing $missing was accepted." };++$negative
}
foreach ($profile in @('bmx-user-data','bmx-user-data-working-profile','bmx-user-data-chassis-probe-extra','BMX-USER-DATA-CHASSIS-PROBE')) {
    $bad=$valid.Clone();$bad.ProfileName=$profile;$rejected=$false
    try { & $select @bad | Out-Null } catch { $rejected=$true }
    if (-not $rejected) { throw "Invalid profile $profile was accepted." };++$negative
}
foreach ($conflict in @('Ride','UnattendedTest','GripProbe','IntentActivation','ImpactRecovery','GetUpPulse','PostFallRemountTest','PhaseProbe','FirstMountProducerProbe','IntentObserver','BindingObserver','CatalogTest','NativeFallTest','ProfilePerformance','TrickTest','RampTest','RampRideTest','ChassisProbe','ChassisPulse','ChassisCalibration','SmokeTest','NativeTestSkater','NativeStartupDelay','NativePlayerStartup','NativeObserveOnly','UseSteam','UseLooseFiles','DisableNativeLocalProfile')) {
    $bad=$valid.Clone();$bad[$conflict]=$true;$rejected=$false
    try { & $select @bad | Out-Null } catch { $rejected=$true }
    if (-not $rejected) { throw "Conflicting diagnostic $conflict was accepted." };++$negative
}
foreach ($pair in @(@('RiderPoseMode','RenderOnly'),@('MotionBasisMode','PreserveSuppliedBasis'),@('ImpactTest','side'))) {
    $bad=$valid.Clone();$bad[$pair[0]]=$pair[1];$rejected=$false
    try { & $select @bad | Out-Null } catch { $rejected=$true }
    if (-not $rejected) { throw "Conflicting variant $($pair[0]) was accepted." };++$negative
}
foreach ($mode in @('human','unattended','legacy_switch','ordinary')) {
    $args=@{NativeChassis=$true;ProfileName='bmx-user-data-working-profile'}
    $expected='other'
    if ($mode -eq 'human') { $args.Ride=$true;$expected='human-ride' }
    if ($mode -eq 'unattended') { $args.UnattendedTest=$true;$args.ProfileName='bmx-user-data-chassis-probe';$expected='unattended-test' }
    if ($mode -eq 'legacy_switch') { $args.SwitchTest=$true;$args.ProfileName='bmx-user-data-chassis-probe';$expected='switch-test' }
    $r=& $select @args
    if ($r.quick -cne '0' -or $r.mode -cne $expected) { throw "Inherited quick-switch selector leaked into $mode." }
}
Write-Output "Quick-switch selector: actual launcher AST guard/expressions passed valid private selection, $negative rejected configurations and four inherited-selector overwrite modes. No launch performed."
