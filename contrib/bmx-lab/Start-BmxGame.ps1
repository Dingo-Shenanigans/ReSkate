param([switch]$GripRequestHud, [switch]$GripProbe, [switch]$ApproachWitness, [switch]$ApproachNormal, [switch]$ContactCompliance, [switch]$LandingContactCompliance, [switch]$IntentActivation, [switch]$ImpactRecovery, [switch]$GetUpPulse, [switch]$PostFallRemountTest, [switch]$PhaseProbe, [switch]$FirstMountProducerProbe, [ValidateSet('Default','RenderOnly','None')][string]$RiderPoseMode = 'Default', [ValidateSet('Default','PreserveSuppliedBasis')][string]$MotionBasisMode = 'Default', [switch]$IntentObserver, [switch]$BindingObserver, [switch]$CatalogTest, [switch]$NativeFallTest, [ValidateSet('','inverted','side')][string]$ImpactTest = '', [switch]$ProfilePerformance, [switch]$Ride, [switch]$UnattendedTest, [switch]$SwitchTest, [switch]$QuickSwitchTest, [switch]$NativeInputHandoff, [switch]$HumanNativeSwitch, [ValidateSet('None','QuickSwitch','NativeOnly')][string]$BoardHandoffMode = 'None', [switch]$NativeBoardControl, [switch]$DeferNativeBoardArm, [switch]$NativeChassis, [switch]$TrickTest, [switch]$RampTest, [switch]$RampRideTest, [switch]$ChassisProbe, [switch]$ChassisPulse, [switch]$ChassisCalibration, [switch]$SmokeTest, [switch]$NativeTestSkater, [switch]$NativeStartupDelay, [switch]$NativePlayerStartup, [switch]$NativeObserveOnly, [switch]$NativeLocalProfile, [switch]$DisableNativeLocalProfile, [switch]$UseSteam, [switch]$UseLooseFiles, [ValidatePattern('^[A-Za-z0-9_-]+$')][string]$ProfileName = 'bmx-user-data', [ValidatePattern('^[A-Za-z0-9_.-]*$')][string]$Map = '', [string]$ArtifactDirectory = 'build\vs2022-x64\Release', [string]$CaptureGateDirectory = '', [ValidatePattern('^$|^[0-9a-f]{32}$')][string]$CaptureGateNonce = '', [ValidateRange(30,600)][int]$CaptureGateTimeoutSeconds = 180)
$ErrorActionPreference = 'Stop'
if ($HumanNativeSwitch -and (-not $Ride -or -not $NativeChassis -or $ProfileName -cnotin @('bmx-user-data-working-profile','bmx-user-data-human-native-test') -or
    $BoardHandoffMode -cne 'None' -or $NativeInputHandoff -or $NativeBoardControl -or $DeferNativeBoardArm -or
    $QuickSwitchTest -or $SwitchTest -or $UnattendedTest -or $RampTest -or $RampRideTest -or $ChassisProbe -or
    $ChassisPulse -or $ChassisCalibration -or $SmokeTest -or $TrickTest -or $NativeObserveOnly -or $NativeFallTest -or
    $IntentActivation -or $ImpactRecovery -or $GetUpPulse -or $PostFallRemountTest -or $PhaseProbe -or $FirstMountProducerProbe -or
    $GripProbe -or $GripRequestHud -or $ApproachWitness -or $ApproachNormal -or $ContactCompliance -or $LandingContactCompliance -or
    $IntentObserver -or $BindingObserver -or $CatalogTest -or $ImpactTest -or $NativeTestSkater -or $NativeStartupDelay -or
    $NativePlayerStartup -or $UseSteam -or $UseLooseFiles -or $DisableNativeLocalProfile -or $Map -or
    $RiderPoseMode -cne 'Default' -or $MotionBasisMode -cne 'Default')) {
    throw 'HumanNativeSwitch requires explicit Ride NativeChassis in the isolated working profile, with other experiment modes disabled.'
}

. (Join-Path $PSScriptRoot 'GripRequestHudEvidence.ps1')
Assert-BmxGripRequestHudSelection ([bool]$GripRequestHud) ([bool]$GripProbe) ([bool]$RampRideTest) ([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))) $ProfileName $BoardHandoffMode $RiderPoseMode
if ($LandingContactCompliance -and -not $ContactCompliance) { throw 'LandingContactCompliance requires explicit ContactCompliance, ApproachNormal and ApproachWitness in the exact private ramp profile.' }
if ($ContactCompliance -and -not $ApproachNormal) { throw 'ContactCompliance requires explicit private ApproachNormal and ApproachWitness; experimental tire compliance remains default off.' }
if ($NativeInputHandoff -and (-not $QuickSwitchTest -or -not $SwitchTest -or -not $NativeChassis -or $ProfileName -cne 'bmx-user-data-chassis-probe' -or $BoardHandoffMode -cne 'QuickSwitch' -or $NativeBoardControl -or $DeferNativeBoardArm)) { throw 'NativeInputHandoff requires the exact private NativeChassis QuickSwitchTest SwitchTest passive QuickSwitch observer profile.' }
if ($ApproachNormal -and -not $ApproachWitness) { throw 'ApproachNormal requires explicit ApproachWitness and the exact private ramp profile; activation additionally requires reviewed native geometry evidence.' }
if ($ApproachWitness -and (-not $RampRideTest -or $ProfileName -cne 'bmx-user-data-ramp-test' -or $BoardHandoffMode -cne 'None')) { throw 'ApproachWitness requires its exact private RampRideTest profile; it supplies geometry diagnostics only.' }
. (Join-Path $PSScriptRoot 'BoardHandoffEvidence.ps1')
$bmxBoardOptions=@{
    GripProbe=[bool]$GripProbe
    IntentActivation=[bool]$IntentActivation
    ImpactRecovery=[bool]$ImpactRecovery
    GetUpPulse=[bool]$GetUpPulse
    PostFallRemountTest=[bool]$PostFallRemountTest
    PhaseProbe=[bool]$PhaseProbe
    FirstMountProducerProbe=[bool]$FirstMountProducerProbe
    IntentObserver=[bool]$IntentObserver
    BindingObserver=[bool]$BindingObserver
    CatalogTest=[bool]$CatalogTest
    NativeFallTest=[bool]$NativeFallTest
    ProfilePerformance=[bool]$ProfilePerformance
    Ride=[bool]$Ride
    UnattendedTest=[bool]$UnattendedTest
    SwitchTest=[bool]$SwitchTest
    QuickSwitchTest=[bool]$QuickSwitchTest
    NativeBoardControl=[bool]$NativeBoardControl
    DeferNativeBoardArm=[bool]$DeferNativeBoardArm
    NativeChassis=[bool]$NativeChassis
    TrickTest=[bool]$TrickTest
    RampTest=[bool]$RampTest
    RampRideTest=[bool]$RampRideTest
    ChassisProbe=[bool]$ChassisProbe
    ChassisPulse=[bool]$ChassisPulse
    ChassisCalibration=[bool]$ChassisCalibration
    SmokeTest=[bool]$SmokeTest
    NativeTestSkater=[bool]$NativeTestSkater
    NativeStartupDelay=[bool]$NativeStartupDelay
    NativePlayerStartup=[bool]$NativePlayerStartup
    NativeObserveOnly=[bool]$NativeObserveOnly
    UseSteam=[bool]$UseSteam
    UseLooseFiles=[bool]$UseLooseFiles
    DisableNativeLocalProfile=[bool]$DisableNativeLocalProfile
    RiderPoseMode=$RiderPoseMode;MotionBasisMode=$MotionBasisMode;ImpactTest=$ImpactTest;Map=$Map
}
$bmxBoardSelection=Get-BmxBoardHandoffSelection $BoardHandoffMode ([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))) $ProfileName $bmxBoardOptions


# This is an explicit private synthetic source, never a controller emulator.
if ($QuickSwitchTest -and (-not $SwitchTest -or -not $NativeChassis -or $ProfileName -cne 'bmx-user-data-chassis-probe' -or
    $Ride -or $UnattendedTest -or $GripProbe -or $IntentActivation -or $ImpactRecovery -or $GetUpPulse -or
    $PostFallRemountTest -or $PhaseProbe -or $FirstMountProducerProbe -or $RiderPoseMode -cne 'Default' -or
    $MotionBasisMode -cne 'Default' -or $IntentObserver -or $BindingObserver -or $CatalogTest -or $NativeFallTest -or
    $ImpactTest -or $ProfilePerformance -or $TrickTest -or $RampTest -or $RampRideTest -or $ChassisProbe -or
    $ChassisPulse -or $ChassisCalibration -or $SmokeTest -or $NativeTestSkater -or $NativeStartupDelay -or
    $NativePlayerStartup -or $NativeObserveOnly -or $UseSteam -or $UseLooseFiles -or $DisableNativeLocalProfile)) {
    throw 'QuickSwitchTest requires its own exact private NativeChassis SwitchTest profile with all other diagnostic and human riding modes disabled.'
}

if ($GripProbe -and (-not $RampRideTest -or $ProfileName -cne 'bmx-user-data-ramp-test' -or $RiderPoseMode -cne 'Default')) { throw 'GripProbe requires an explicit isolated RampRideTest and Default rider pose.' }
. (Join-Path $PSScriptRoot 'PoseComparisonEvidence.ps1')
Assert-BmxPoseComparisonConfiguration $RiderPoseMode ([bool]$PhaseProbe) ([bool]$FirstMountProducerProbe) ([bool]$PostFallRemountTest) ([bool]$GetUpPulse) $ImpactTest ([bool]$NativeChassis) ([bool]$UnattendedTest) $ProfileName
Assert-BmxMotionBasisConfiguration $MotionBasisMode $RiderPoseMode ([bool]$PhaseProbe) ([bool]$FirstMountProducerProbe) ([bool]$PostFallRemountTest) ([bool]$GetUpPulse) $ImpactTest ([bool]$NativeChassis) ([bool]$UnattendedTest) $ProfileName
if ($FirstMountProducerProbe -and (-not $PhaseProbe -or -not $PostFallRemountTest)) { throw 'FirstMountProducerProbe requires explicit PhaseProbe and private PostFallRemountTest.' }
if ($PhaseProbe -and -not $PostFallRemountTest) { throw 'PhaseProbe requires the explicit private PostFallRemountTest profile.' }
if ($PostFallRemountTest -and (-not $GetUpPulse -or $ImpactTest -cne 'side' -or -not $NativeChassis -or -not $UnattendedTest -or $ProfileName -cne 'bmx-user-data-chassis-probe')) { throw 'PostFallRemountTest requires the distinct side ImpactTest, GetUpPulse and exact isolated unattended native chassis profile.' }
if ($GetUpPulse -and (-not $ImpactTest -or -not $NativeChassis -or -not $UnattendedTest -or $ProfileName -cne 'bmx-user-data-chassis-probe' -or $ImpactRecovery -or $IntentObserver -or $IntentActivation -or $BindingObserver -or $CatalogTest -or $NativeFallTest -or $Ride -or $SwitchTest -or $TrickTest -or $RampTest -or $RampRideTest -or $ChassisProbe -or $ChassisPulse -or $ChassisCalibration -or $ProfilePerformance)) { throw 'GetUpPulse requires its own exact isolated unattended ImpactTest NativeChassis probe profile without other diagnostic modes.' }
if ($IntentActivation -and (-not $IntentObserver -or -not $ImpactTest)) { throw 'IntentActivation requires the explicit isolated IntentObserver ImpactTest profile.' }
if ($IntentObserver -and (-not $NativeChassis -or -not $UnattendedTest -or $Ride -or $SwitchTest -or $ProfileName -cne 'bmx-user-data-chassis-probe')) { throw 'IntentObserver requires the isolated unattended native chassis profile.' }
if ($BindingObserver -and (-not $NativeChassis -or -not $UnattendedTest -or $Ride -or $SwitchTest -or $ProfileName -ne 'bmx-user-data-chassis-probe')) { throw 'BindingObserver requires the isolated automated native chassis profile.' }
if ($CatalogTest -and (-not $NativeChassis -or -not $UnattendedTest -or -not $TrickTest -or $SwitchTest -or $NativeFallTest -or $ImpactTest -or $Ride -or $ProfileName -ne 'bmx-user-data-chassis-probe')) { throw 'CatalogTest requires its isolated NativeChassis UnattendedTest TrickTest probe profile.' }
if ($NativeFallTest -and (-not $NativeChassis -or -not $SwitchTest -or $UnattendedTest -or $Ride -or $TrickTest -or $ImpactTest -or $ProfileName -ne 'bmx-user-data-chassis-probe')) { throw 'NativeFallTest requires its isolated NativeChassis SwitchTest probe profile.' }
if ($ImpactRecovery -and -not $ImpactTest) { throw 'ImpactRecovery requires the explicit isolated ImpactTest profile.' }
if ($ImpactTest -and (-not $NativeChassis -or -not $UnattendedTest -or $TrickTest -or $SwitchTest -or $ProfileName -ne 'bmx-user-data-chassis-probe')) { throw 'ImpactTest requires the isolated NativeChassis UnattendedTest probe profile.' }
if ($RampRideTest) {
    if ($RampTest -or $NativeChassis) { throw 'RampRideTest selects its own physical backend and ramp diagnostic gates.' }
    $RampTest = $true; $NativeChassis = $true
}
if ($RampTest -and ($Ride -or $UnattendedTest -or $SwitchTest -or ($NativeChassis -and -not $RampRideTest) -or $TrickTest -or $ChassisProbe -or $SmokeTest -or $NativeTestSkater -or $NativeStartupDelay -or $NativePlayerStartup -or $NativeObserveOnly -or $UseSteam -or $DisableNativeLocalProfile -or $UseLooseFiles -or $ProfileName -ne 'bmx-user-data-ramp-test')) {
    throw 'RampTest requires its own packaged offline bmx-user-data-ramp-test profile and no other rehearsal mode.'
}
# The user-facing isolated ride uses the independently simulated bike. Other
# diagnostic modes retain explicit selection for legacy comparison runs.
if ($Ride) { $NativeChassis = $true }
if ($TrickTest -and (-not $NativeChassis -or -not $UnattendedTest -or $SwitchTest)) {
    throw 'TrickTest requires NativeChassis and UnattendedTest as its own isolated riding rehearsal.'
}
if ($NativeChassis -and (-not ($Ride -or $UnattendedTest -or $SwitchTest -or $RampRideTest) -or $ChassisProbe -or $DisableNativeLocalProfile -or $UseLooseFiles)) {
    throw 'NativeChassis requires an isolated Ride, UnattendedTest or SwitchTest with the local profile and packaged files.'
}
if ($ChassisPulse -and -not $ChassisProbe) { throw 'ChassisPulse requires the dedicated ChassisProbe run.' }
if ($ChassisCalibration -and (-not $ChassisProbe -or -not $ChassisPulse)) { throw 'ChassisCalibration requires ChassisProbe and ChassisPulse.' }
if ($ChassisProbe -and ($Ride -or $UnattendedTest -or $SwitchTest -or $SmokeTest -or $NativeTestSkater -or $NativeStartupDelay -or $NativePlayerStartup -or $NativeObserveOnly -or $UseSteam -or $DisableNativeLocalProfile -or $UseLooseFiles)) {
    throw 'ChassisProbe requires its own isolated offline run; omit ride, rehearsal and other diagnostic modes.'
}
if ($ChassisProbe -and $ProfileName -ne 'bmx-user-data-chassis-probe') {
    throw 'ChassisProbe requires -ProfileName bmx-user-data-chassis-probe to preserve the working riding profile.'
}
if (($Ride -or $UnattendedTest -or $SwitchTest) -and ($SmokeTest -or $NativeTestSkater -or $NativeStartupDelay -or $NativePlayerStartup -or $NativeObserveOnly -or $UseSteam)) {
    throw 'Ride, UnattendedTest and SwitchTest require the ordinary isolated offline startup; omit other diagnostic modes and UseSteam.'
}
if ($Ride -and ($UnattendedTest -or $SwitchTest)) { throw 'Choose Ride or an automated rehearsal.' }
if ($NativeChassis -and $SwitchTest -and ($UnattendedTest -or $ProfileName -cne 'bmx-user-data-chassis-probe')) {
    throw 'Native chassis SwitchTest requires its own exact bmx-user-data-chassis-probe profile without UnattendedTest.'
}
# An ordinary resort load was verified to close the startup Hub and create a
# grounded native rider. It avoids a manual menu step without forcing UI flags.
if (($Ride -or $UnattendedTest -or $SwitchTest -or $ChassisProbe -or $RampTest) -and -not $Map) { $Map = 'MPR' }
if ($NativeLocalProfile -and $UseSteam) { throw 'NativeLocalProfile requires the isolated offline configuration.' }
if ($NativeLocalProfile -and $DisableNativeLocalProfile) { throw 'Choose NativeLocalProfile or DisableNativeLocalProfile.' }
if ($NativeObserveOnly -and ($SmokeTest -or $NativeTestSkater -or $NativeStartupDelay -or $NativePlayerStartup -or $Map)) {
    throw 'NativeObserveOnly collects ordinary startup snapshots; omit other native experiments and Map.'
}
if ($NativeStartupDelay -and ($NativeTestSkater -or $Map)) {
    throw 'NativeStartupDelay observes the default startup route; omit NativeTestSkater and Map for this experiment.'
}
if ($NativePlayerStartup -and ($NativeTestSkater -or $NativeStartupDelay -or $Map)) {
    throw 'NativePlayerStartup observes one initial-player request in the default city; omit other startup experiments and Map.'
}
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
. (Join-Path $PSScriptRoot 'RecordingGate.ps1')
Assert-BmxCaptureGateConfiguration $repo $CaptureGateDirectory $CaptureGateNonce $CaptureGateTimeoutSeconds
if ($CaptureGateDirectory) {
    $coupledEnvironment=@{}
    foreach($key in @('RESKATE_BMX_NATIVE_COUPLED','RESKATE_BMX_COUPLED_RIDE','RESKATE_BMX_COUPLED_CONTROLLER_TRIAL','RESKATE_BMX_COUPLED_FORCE_PROBE','RESKATE_BMX_COUPLED_CALIBRATION')) {
        $coupledEnvironment[$key]=[string][Environment]::GetEnvironmentVariable($key)
    }
    $coupledOptions=$bmxBoardOptions.Clone();$coupledOptions.NativeChassis=[bool]$NativeChassis
    $coupledOptions.BoardHandoffMode=$BoardHandoffMode;$coupledOptions.GripRequestHud=[bool]$GripRequestHud
    $coupledAllowed=Test-BmxCoupledCaptureGateAllowed $repo $ProfileName $coupledOptions $coupledEnvironment
    $coupledRequested=$coupledEnvironment['RESKATE_BMX_NATIVE_COUPLED'] -ceq '1' -and
        $coupledEnvironment['RESKATE_BMX_COUPLED_RIDE'] -ceq '1' -and $coupledEnvironment['RESKATE_BMX_COUPLED_CONTROLLER_TRIAL'] -ceq '1'
    $legacyAllowed=Test-BmxBoardHandoffCaptureGateAllowed $bmxBoardSelection ([bool]$Ride) ([bool]$UnattendedTest) ([bool]$SwitchTest) ([bool]$RampTest) $ProfileName
    if(($coupledRequested -and -not $coupledAllowed) -or (-not $coupledRequested -and -not $legacyAllowed)) {
        throw 'Capture gate requires an isolated automated/native-only profile or the exact coupled three-flag no-Ride profile with all legacy modes disabled.'
    }
}
if ($CaptureGateDirectory) { $CaptureGateDirectory=[IO.Path]::GetFullPath($CaptureGateDirectory) }
$game = Join-Path $repo 'build\bmx-game'
$data = Join-Path (Join-Path $repo 'build') $ProfileName
$artifacts = [IO.Path]::GetFullPath((Join-Path $repo $ArtifactDirectory))
foreach ($path in @($game, $data, $artifacts)) {
    if (-not [IO.Path]::GetFullPath($path).StartsWith($repo + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'BMX test path escaped the repository workspace.'
    }
}
$exe = Join-Path $game 'Skate.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw 'The separate build\bmx-game copy of the supported game is missing.' }
if ((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne 'FBCE74D5E28EF525DBBA2CB4ADBEBC13405BDBD88F31BC940BCA45E4AE88B8F9') {
    throw 'BMX test Skate.exe is not the verified supported build.'
}
if (Get-Process -Name Skate -ErrorAction SilentlyContinue) { throw 'Close the running Skate game before starting the isolated BMX test.' }
foreach ($name in @('ReSkate.dll', 'ReSkateLauncher.exe')) {
    $source = Join-Path $artifacts $name
    if (-not (Test-Path -LiteralPath $source)) { throw "Build artifact missing: $source" }
    Copy-Item -LiteralPath $source -Destination (Join-Path $game $name) -Force
}
if ($ProfilePerformance) {
    $bmxSymbols=Join-Path $artifacts 'ReSkate.pdb'
    if (-not (Test-Path -LiteralPath $bmxSymbols -PathType Leaf)) { throw 'Performance profiling requires the matching runtime PDB.' }
    Copy-Item -LiteralPath $bmxSymbols -Destination (Join-Path $game 'ReSkate.pdb') -Force
}
New-Item -ItemType Directory -Force -Path $data | Out-Null
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = Join-Path $game 'ReSkateLauncher.exe'
$start.WorkingDirectory = $game
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.Arguments = '--no-gui --no-update --no-discord --windowed --width=1280 --height=720 --dev-data-root="' + $data + '"'
if (-not $UseSteam) { $start.Arguments += ' --offline' }
if (-not $UseLooseFiles) { $start.Arguments += ' --no-loose-files' }
if ($Map) { $start.Arguments += ' -map=' + $Map }
if ($UnattendedTest -or $SwitchTest -or $ChassisProbe -or $RampTest -or $SmokeTest -or $NativeTestSkater -or $NativeStartupDelay -or $NativePlayerStartup -or $NativeObserveOnly) { $start.Arguments += ' --log-level=debug' }
$switchCommands = 'bmx mount;wait 8;bmx status;bmx toggle;wait 8;bmx status;wait 8;bmx status;bmx toggle;wait 16;bmx status;bmx dismount;wait 2;bmx status;wait 2;bmx status;bmx carry;wait 2;bmx status;wait 2;bmx status;bmx carry;wait 2;bmx status;wait 2;bmx status;bmx interact;wait 3;bmx status'
if ($NativeChassis -and $SwitchTest) {
    # Keep two actual quiet-board observations >=1s apart without letting the
    # native board coast across the plaza for sixteen seconds before remount.
    # Native state, force counters and fresh wheel-support checks still decide success.
    $switchCommands = 'bmx mount;wait 8;bmx chassis pause-test;wait 2;bmx status;bmx toggle;wait 2;bmx status;wait 2;bmx status;bmx toggle;wait 16;bmx status;bmx dismount;wait 2;bmx status;wait 2;bmx status;bmx carry;wait 2;bmx status;wait 2;bmx status;bmx carry;wait 2;bmx status;wait 2;bmx status;bmx interact;wait 3;bmx status'
}
$start.Environment['RESKATE_STARTUP_COMMANDS'] = if ($bmxBoardSelection.native_control) {
    Get-BmxBoardHandoffNativeControlCommands ($bmxBoardSelection.native_board_control -ceq '1') ([bool]$bmxBoardSelection.defer_native_board_arm)
} elseif ($QuickSwitchTest) {
    'wait 15;bmx quicktest'
} elseif ($NativeFallTest) {
    'wait 15;bmx mount;wait 8;bmx toggle;wait 8;nobail 0;wait 1;nobail;bmx status;bmx native-fall observe;wait 8;bmx status;bmx diagnose'
} elseif ($PostFallRemountTest) {
    # This distinct opt-in lifecycle waits 65 WALL seconds after the private
    # pulse. The supervisor still requires 30 NATIVE seconds of completed
    # passive evidence before recovery; wall delay never proves that condition.
    if ($PhaseProbe) {
    'wait 15;nobail 0;wait 1;nobail;bmx selftest unattended;wait 12;bmx status;bmx getup-test;wait 65;bmx status;wait 2;bmx status;bmx phase-dump;wait 2;bmx interact;wait 8;bmx status;wait 3;bmx status'
    } else {
    'wait 15;nobail 0;wait 1;nobail;bmx selftest unattended;wait 12;bmx status;bmx getup-test;wait 65;bmx status;wait 2;bmx status;bmx interact;wait 8;bmx status;wait 3;bmx status'
    }
} elseif ($GetUpPulse) {
    # One explicitly requested typed action after a native fall. No pickup,
    # mount, equipment switch or additional input follows the action.
    'wait 15;nobail 0;wait 1;nobail;bmx selftest unattended;wait 12;bmx status;bmx getup-test;wait 4;bmx status;wait 2;bmx status'
} elseif ($ImpactRecovery) {
    'wait 15;nobail 0;wait 1;bmx selftest unattended;wait 12;bmx status;bmx interact;wait 4;bmx status;bmx toggle;wait 3;bmx status;wait 2;bmx status'
} elseif ($ImpactTest) {
    # Only the explicitly validated private impact profile. The native bail
    # adapter retains manual/flight protection priority; release the saved
    # manual toggle through its normal command and let its lease expire first.
    'wait 15;nobail 0;wait 1;nobail;bmx diagnose;bmx status;ui dump 0x2c5645a1;bmx selftest unattended;wait 65;bmx diagnose;bmx status'
} elseif ($RampRideTest) {
    if ($ProfilePerformance) { 'wait 8;perf hud on;wait 2;perf sample 5 client 250;wait 6;perf report;perf status;bmx ramp ride;wait 3;perf sample 3 client 250;wait 4;perf report;perf status;wait 15;perf status;wait 15;perf status;wait 15;perf status;wait 35;bmx diagnose;bmx status' }
    else { 'wait 15;bmx ramp ride;wait 95;bmx diagnose;bmx status' }
} elseif ($RampTest) {
    'wait 15;bmx ramp probe;wait 95;bmx diagnose;bmx status'
} elseif ($ChassisProbe) {
    'wait 15;bmx diagnose;bmx chassis probe;wait 65;bmx diagnose'
} elseif ($NativeObserveOnly) {
    'wait 2;bmx diagnose;wait 13;bmx diagnose;wait 25;bmx diagnose;wait 25;bmx diagnose'
} elseif ($NativePlayerStartup) {
    'wait 15;bmx diagnose;bmx recover player;wait 15;bmx diagnose;bmx status;bmx selftest;wait 25;bmx diagnose;bmx status'
} elseif ($NativeStartupDelay) {
    # Observe before and after the process-only 45-second queue delay.
    'wait 15;bmx diagnose;bmx status;wait 40;bmx diagnose;bmx status;wait 20;bmx diagnose;bmx status'
} elseif ($NativeTestSkater) {
    # One process-only experiment through the existing native setting API.
    # reset restores the captured original value; exiting also discards the override.
    'wait 15;bmx diagnose;DingoTest.SpawnLocalSkater 1;load MPR;wait 15;bmx diagnose;reset DingoTest.SpawnLocalSkater;bmx selftest;wait 25;bmx diagnose;bmx status'
} elseif ($UnattendedTest) {
    $rideCommands = 'wait 15;bmx diagnose;bmx status;ui dump 0x2c5645a1;bmx selftest unattended;wait 65;bmx diagnose;bmx status'
    if ($SwitchTest) { $rideCommands + ';' + $switchCommands } else { $rideCommands }
} elseif ($SwitchTest) {
    'wait 15;' + $switchCommands
} elseif ($Ride) {
    # The isolated controller test needs the same fall setting as impact tests.
    # Use the normal command after saved settings restore; the adapter continues
    # to respect independent protection enabled later by the player.
    'wait 15;nobail 0;wait 1;nobail;bmx mount;wait 20;bmx status'
} elseif ($SmokeTest) { 'wait 15;bmx diagnose;bmx status;ui dump 0x2c5645a1;bmx selftest;wait 25;bmx diagnose;bmx status' } else { '' }
# Overwrite both selectors on every launch, including ordinary user profiles.
$start.Environment['RESKATE_BMX_QUICK_SWITCH_TEST'] = if ($QuickSwitchTest) { '1' } else { '0' }
$start.Environment['RESKATE_BMX_NATIVE_INPUT_HANDOFF'] = if ($NativeInputHandoff) { '1' } else { '0' }
$start.Environment['RESKATE_BMX_HUMAN_NATIVE_SWITCH'] = if ($HumanNativeSwitch) { '1' } else { '0' }
if ($HumanNativeSwitch) { $start.Environment['RESKATE_BMX_NATIVE_BOARD_INPUT_BRIDGE'] = '0' }
if ($NativeInputHandoff) { $start.Environment['RESKATE_BMX_NATIVE_BOARD_INPUT_BRIDGE'] = '0' }
$start.Environment['RESKATE_BMX_LAUNCH_MODE'] = if ($Ride) { 'human-ride' } elseif ($UnattendedTest) { 'unattended-test' } elseif ($SwitchTest) { 'switch-test' } else { 'other' }
# Always overwrite both inherited board-handoff selectors, including ordinary launches.
Set-BmxBoardHandoffEnvironment $start.Environment $bmxBoardSelection
$start.Environment['RESKATE_BMX_HANDOFF_PHASE_PROBE'] = if ($PhaseProbe) { '1' } else { '0' }
$start.Environment['RESKATE_BMX_FIRST_MOUNT_PRODUCER_PROBE'] = if ($FirstMountProducerProbe) { '1' } else { '0' }
# Always overwrite inherited selectors, including ordinary Default launches.
$start.Environment['RESKATE_BMX_POSE_COMPARISON'] = $RiderPoseMode
$start.Environment['RESKATE_BMX_MOTION_BASIS_COMPARISON'] = $MotionBasisMode
$start.Environment['RESKATE_BMX_POSTFALL_REMOUNT_TEST'] = if ($PostFallRemountTest) { '1' } else { '0' }
$start.Environment['RESKATE_BMX_BINDING_OBSERVER'] = if ($BindingObserver) { '1' } else { '0' }
$start.Environment['RESKATE_BMX_INTENT_OBSERVER'] = if ($IntentObserver) { '1' } else { '0' }
$start.Environment['RESKATE_BMX_AUTOMATED_RIDE'] = if ($UnattendedTest -and $ProfileName -eq 'bmx-user-data-chassis-probe') { '1' } else { '0' }
$start.Environment['RESKATE_BMX_AUTOMATED_SWITCH'] = if ($NativeChassis -and $SwitchTest -and -not $NativeFallTest -and $ProfileName -ceq 'bmx-user-data-chassis-probe') { '1' } else { '0' }
$start.Environment['RESKATE_CRASH_REPORTING'] = '0'
$start.Environment['RESKATE_BMX_FLOW_DIAGNOSTICS'] = if ($UnattendedTest -or $SwitchTest -or $ChassisProbe -or $RampTest -or $SmokeTest -or $NativeTestSkater -or $NativeStartupDelay -or $NativePlayerStartup -or $NativeObserveOnly) { '1' } else { '0' }
$start.Environment['RESKATE_BMX_RAMP_TEST'] = if ($RampTest) { '1' } else { '0' }
$start.Environment['RESKATE_BMX_RAMP_RIDE'] = if ($RampRideTest) { '1' } else { '0' }
$start.Environment['RESKATE_BMX_GRIP_PROBE'] = if ($GripProbe) { '1' } else { '0' }
# Clear inherited diagnostic state even on ordinary/default launches.
$start.Environment['RESKATE_BMX_GRIP_REQUEST_HUD'] = if ($GripRequestHud) { '1' } else { '0' }
$start.Environment['RESKATE_BMX_CHASSIS_PROBE'] = if ($ChassisProbe) { '1' } else { '0' }
$start.Environment['RESKATE_BMX_NATIVE_CHASSIS'] = if ($NativeChassis) { '1' } else { '0' }
$start.Environment['RESKATE_BMX_APPROACH_WITNESS'] = if ($ApproachWitness) { '1' } else { '0' }
$start.Environment['RESKATE_BMX_APPROACH_NORMAL'] = if ($ApproachNormal) { '1' } else { '0' }
$start.Environment['RESKATE_BMX_CONTACT_COMPLIANCE'] = if ($ContactCompliance) { '1' } else { '0' }
$start.Environment['RESKATE_BMX_LANDING_CONTACT_COMPLIANCE'] = if ($LandingContactCompliance) { '1' } else { '0' }
$start.Environment['RESKATE_BMX_INTENT_ACTIVATION'] = if ($IntentActivation) { '1' } else { '0' }
$start.Environment['RESKATE_BMX_RIDING_FALLS'] = if ($NativeChassis -and -not $IntentObserver -and -not $NativeFallTest) { '1' } else { '0' }
$start.Environment['RESKATE_BMX_RIDE_TRACE'] = if ($NativeChassis) { '1' } else { '0' }
$start.Environment['RESKATE_BMX_IMPACT_TEST'] = $ImpactTest
$start.Environment['RESKATE_BMX_IMPACT_RECOVERY'] = if ($ImpactRecovery) { '1' } else { '0' }
$start.Environment['RESKATE_BMX_GETUP_PULSE'] = if ($GetUpPulse) { '1' } else { '0' }
$start.Environment['RESKATE_BMX_NATIVE_FALL_OBSERVER'] = if ($NativeFallTest) { '1' } else { '0' }
$start.Environment['RESKATE_BMX_TRICK_REHEARSAL'] = if ($TrickTest) { '1' } else { '0' }
$start.Environment['RESKATE_BMX_CATALOG_TEST'] = if ($CatalogTest) { 'superman' } else { '' }
$start.Environment['RESKATE_BMX_CHASSIS_PROBE_PULSE'] = if ($ChassisProbe -and $ChassisPulse) { '1' } else { '0' }
$start.Environment['RESKATE_BMX_CHASSIS_PROBE_VARIANT'] = if ($ChassisProbe -and $ChassisPulse -and $ChassisCalibration) { 'mass-inertia-terrain' } else { '' }
$start.Environment['RESKATE_BMX_NATIVE_STARTUP_DELAY'] = if ($NativeStartupDelay) { '1' } else { '0' }
$start.Environment['RESKATE_BMX_PLAYER_STARTUP_REQUEST'] = if ($NativePlayerStartup) { '1' } else { '0' }
# This script always supplies an isolated development root. Keep the proven
# local Guest settings fix on for its offline launches; comparisons can opt out.
$start.Environment['RESKATE_BMX_LOCAL_PROFILE'] = if (-not $UseSteam -and -not $DisableNativeLocalProfile) { '1' } else { '0' }
$start.Environment['RESKATE_BMX_CAPTURE_GATE_DIRECTORY'] = $CaptureGateDirectory
$start.Environment['RESKATE_BMX_CAPTURE_GATE_NONCE'] = $CaptureGateNonce
$start.Environment['RESKATE_BMX_CAPTURE_GATE_TIMEOUT_SECONDS'] = if ($CaptureGateDirectory) { [string]$CaptureGateTimeoutSeconds } else { '' }
$process = [Diagnostics.Process]::Start($start)
Write-Output "BMX launcher PID $($process.Id). Isolated data: $data"
if ($HumanNativeSwitch) {
    Write-Output 'OPT-IN human XInput native bike-to-board prototype. Stop with B/Circle, use LB/L1 + fresh Y/Triangle, then release both. One actual original pad required; no native input retry/software fallback on this leg. F6 and reverse switch retain existing behavior. Physical acceptance and visuals require testing.'
} elseif ($bmxBoardSelection.native_board_control -ceq '1') {
    Write-Output 'ACTIVE private NativeOnly software native-board request: one attempt after an armed untouched standing baseline and later fresh clock; no BMX mount, motion/body/pose/teleport or OS/controller input. Existing native selector checks remain authoritative; retained100 CPU dwell is not a visual pass.'
} elseif ($bmxBoardSelection.defer_native_board_arm) {
    Write-Output 'Private deferred passive NativeOnly observer: startup queues only no-bail setup/status. After world load, coordinate the physical input, run bmx handoff-trace arm, and promptly CLOSE the console; readiness has a separate bounded 30-second allowance. Queued does not prove armed. With the game focused, ordinary physical Y/Triangle without LB must occur during the unchanged 8-second observation window. Inspect logs/report externally afterward to establish actual arming and input timing. No board action or synthetic input is injected; input consumption and animation remain unproved.'
} elseif ($bmxBoardSelection.native_control) {
    Write-Output 'Private native-only board control: observer arming is queued after no-bail setup; no BMX mount or native board input is injected. After the delayed startup status proves armed=1 sealed=0 while game focus remains intact, ordinary Y/Triangle without LB is a separate ACTIVE manual control action. Retain at least2.5 native and wall seconds after100; stop this exact owned process after evidence.'
} elseif ($QuickSwitchTest) {
    Write-Output 'Private synthetic-source LB+North test: actual focused native clock, canonical owner, UI guards, guarded support and existing contextual routing. Four bounded pulses; stop on interference/fall/unknown state. This does not test hardware packets, XInput or physical D-pad mappings.'
} elseif ($PostFallRemountTest) {
    Write-Output 'Private post-fall remount lab: one typed GetUp, passive evidence window, explicit interaction and later native mount/counter observations. Controller mapping and visual quality are not tested.'
} elseif ($GetUpPulse) {
    Write-Output 'Private typed GetUp action lab: one explicit pulse with later-clock reset, then passive native recovery observations. No bike pickup, remount, equipment switch or normal input mapping is tested.'
} elseif ($NativeFallTest) {
    Write-Output 'Private native skateboard fall observation: switch to the board, request its normal fall, and record native transition state.'
} elseif ($RampRideTest) {
    Write-Output 'Private physical ramp rehearsal: verifies the existing fixture and corridor, then rides the measured uphill direction once.'
} elseif ($RampTest) {
    Write-Output 'Private ramp collision probe: creates one native BuildKit fixture and measures its shape; no controller input is injected.'
} elseif ($ChassisProbe) {
    Write-Output 'One-body physics experiment: no BMX mount or controller input. Collect the chassis report, then stop this isolated Skate process to release all experiment allocations.'
} elseif ($UnattendedTest) {
    Write-Output 'Unattended rehearsal: native movement, steering, braking and hop measurements use synthetic input in this test profile only.'
    if ($SwitchTest) { Write-Output 'Then verify the native bike/skateboard round trip and leave BMX mounted for controller testing.' }
} elseif ($SwitchTest) {
    Write-Output 'Vehicle switch rehearsal: BMX, native skateboard, parked bike, carry, setdown and remount. Native-chassis probe mode uses neutral synthetic BMX input with a 200-second console lifecycle lease; menus and capture owners still block. Stop the isolated process after evidence collection.'
} elseif ($Ride) {
    Write-Output 'BMX ride: the resort loads and mount is queued. Leave menus closed, focus the game and center both sticks to begin.'
} elseif ($SmokeTest -or $NativeTestSkater -or $NativeStartupDelay -or $NativePlayerStartup -or $NativeObserveOnly) {
    Write-Output 'Automated diagnostic run: leave the test window open while it collects logs.'
} elseif ($ArtifactDirectory -ne 'build\vs2022-x64\Release') {
    Write-Output 'Comparison build launched without BMX startup commands. Inspect its isolated log for native player creation.'
} else {
    Write-Output 'In game: open the console with `, run bmx mount, close the console and center both sticks.'
}
