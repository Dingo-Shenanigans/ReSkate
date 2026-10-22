$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'QuickSwitchEvidence.ps1')
function Assert-QuickEvidence([bool]$Good,[string]$Message) { if (-not $Good) { throw $Message } }
function ConvertTo-QuickFrameFixture($Observed) {
    [pscustomobject]@{
        owner=@($Observed.owner);available=$true;timing_available=$true;physics_available=$true
        physics=$Observed.physics;offboard=$Observed.offboard;standable=$Observed.standable;motion_supported=$Observed.motion_supported
        serial=$Observed.serial;native_time=$Observed.native_time;native_time_finite=$true
        active=$Observed.active;pending=$false;mounting=$false;parked=$Observed.parked;carried=$Observed.carried;crashed=$false
        pool_active=$Observed.pool_active;pool_parked=$Observed.pool_parked;pool_failed=$false;pool_suspended=$false
        capture=$Observed.capture;world=$Observed.world;allocations=$Observed.allocations;mounts=$Observed.mounts
        token=$Observed.token;applications=$Observed.applications;parts=$Observed.parts;strict_setdowns=$Observed.strict_setdowns
    }
}
function New-QuickSourceFixture($Frame,[string]$Stage,[string]$Phase,[int]$Wall) {
    [pscustomobject]@{
        stage=$Stage;phase=$Phase;wall_ms=$Wall;native_motion_enabled=$true;native_frame_read_attempted=$true
        native_frame_issue='';lifecycle_allowed=$true;physical_quiet=$true;focused=$true;native_ui_available=$true
        native_ui_menu=$false;overlay_ready=$true;overlay_menu=$false;capture_blocked=$false;menu_requested=$false
        pool_ready=$Frame.pool_active;pool_failed=$false;pool_blocked=$false;pool_quarantined=$false;pool_suspended=$false
        pool_deferred_park=$false;pool_detached=$false;pool_issue='idle';frame=$Frame
    }
}
function New-QuickEvidenceFixture {
    $names=@('idle','initial_mount','native_board','returned_bike','parked_near','nearby_bike','parked_carry','carried','final_bike')
    $serials=@(11,20,50,80,90,120,130,140,170);$mounts=@(0,1,1,2,2,3,3,3,4)
    $tokens=@(0,701,0,702,702,703,703,703,704);$applications=@(0,20,21,40,41,60,61,61,80)
    $observations=@()
    for ($i=0;$i -lt 9;++$i) {
        $bike=$i -in @(1,3,5,8);$parked=$i -in @(4,6,7);$board=$i -in @(0,2)
        $observations+=[pscustomobject]@{
            phase=$names[$i];serial=$serials[$i];native_time=($serials[$i]/60.0);token=$tokens[$i];applications=$applications[$i]
            world=$(if($i){100}else{0});allocations=$(if($i){1}else{0});mounts=$mounts[$i];parts=$(if($board){0}else{8})
            offboard=(-not $board);standable=(-not $board);motion_supported=(-not $board);active=$bike;parked=$parked;carried=($i -eq 7)
            pool_active=$bike;pool_parked=($i -ne 0 -and -not $bike);capture=$bike;physics=$(if($board){100}else{504})
            strict_setdowns=$(if($i -eq 8){1}else{0});owner=@(11,12,13,14,15)
        }
    }
    $pulses=@();$before=@(1,2,4,7);$routes=@(4,1,7,7)
    for ($i=0;$i -lt 4;++$i) {
        $first=$serials[$before[$i]]
        $pulses+=[pscustomobject]@{
            index=$i;samples=17;quick_events=1;ordinary_events=0;route=$routes[$i]
            first_serial=$first;edge_serial=($first+3);release_serial=($first+16);last_serial=($first+17)
            first_time=($first/60.0);edge_time=(($first+3)/60.0);release_time=(($first+16)/60.0);last_time=(($first+17)/60.0)
            north_capture_requested=$true;released=$true
        }
    }
    $firstFrame=ConvertTo-QuickFrameFixture $observations[0]
    $firstFrame.timing_available=$false;$firstFrame.serial=0;$firstFrame.native_time=0
    $firstClock=ConvertTo-QuickFrameFixture $observations[0]
    $firstClock.serial=10;$firstClock.native_time=10/60.0
    $qualifiedClock=ConvertTo-QuickFrameFixture $observations[0]
    $lastFrame=ConvertTo-QuickFrameFixture $observations[-1]
    [pscustomobject]@{
        schema_version=3;process_id=1234;completed=$true;passed=$true;issue='four_native_quick_switch_routes_completed'
        input_source='synthetic_virtual_device';virtual_device=1112365059;physical_controller_verified=$false
        xinput_first_packet_verified=$false;dpad_hardware_verified=$false;hold_native_seconds=.2;wall_limit_ms=190000
        guarded_setdown_mount_followup_requests=1;setup_requests=@(1,7,7,8);pulses=$pulses;observations=$observations
        board_transition_receipt=[pscustomobject]@{
            observed=$false;samples=0;grounded_proven=$true
            grounded_first=$( $v=ConvertTo-QuickFrameFixture $observations[2];$v.serial--;$v.native_time=$v.serial/60.0;$v )
            grounded_confirmed=(ConvertTo-QuickFrameFixture $observations[2])
            states=@(
                [pscustomobject]@{physics=200;samples=0;phase='native_board';first=$null;last=$null},
                [pscustomobject]@{physics=201;samples=0;phase='native_board';first=$null;last=$null}
            )
        }
        preflight=[pscustomobject]@{
            completed=$true;passed=$true;issue='qualified_two_native_clock_samples';samples=3
            wall_started_ms=1000;elapsed_ms=34;wall_limit_ms=2000;first_clock=$firstClock;qualified_clock=$qualifiedClock
        }
        first_native_observation=(New-QuickSourceFixture $firstFrame 'preflight' 'idle' 1000)
        last_native_observation=(New-QuickSourceFixture $lastFrame 'scenario' 'final_bike' 4000)
        first_rejected_native_observation=$null;first_rejection_issue='';first_rejection_observation_relation='' 
    }
}
function New-QuickBoardTransitionFixture {
    $r=New-QuickEvidenceFixture;$r.board_transition_receipt.observed=$true;$r.board_transition_receipt.samples=2
    for ($i=0;$i -lt 2;++$i) {
        $frame=ConvertTo-QuickFrameFixture $r.observations[2]
        $frame.physics=200+$i;$frame.serial=40+$i;$frame.native_time=$frame.serial/60.0
        $w=[pscustomobject]@{
            pending_for_owner=$true;request_clock_valid=$true;parked_pool_proven=$true;clock_unchanged=$true;physics_unchanged=$true
            accepted_requests=1;observed_wall_ms=(1400+17*$i);pending_deadline_ms=16000;request_wall_ms=1350
            request_serial=38;request_native_time=38/60.0;world=100;token=701;request_owner=@(11,12,13,14,15)
        }
        $frame | Add-Member -NotePropertyName board_transition -NotePropertyValue $w
        $r.board_transition_receipt.states[$i].samples=1
        $r.board_transition_receipt.states[$i].first=$frame;$r.board_transition_receipt.states[$i].last=$frame
    }
    return $r
}
$good=New-QuickEvidenceFixture
Assert-QuickEvidence (Test-BmxQuickSwitchEvidence $good 1234) 'Complete native receipt fixture should pass.'
Assert-QuickEvidence (-not (Test-BmxQuickSwitchEvidence $good 1235)) 'Wrong process identity must fail.'
$boarding=New-QuickBoardTransitionFixture
Assert-QuickEvidence (Test-BmxQuickSwitchEvidence $boarding 1234) 'Two proven native boarding states should wait neutrally before two grounded100 observations.'
$retry=New-QuickBoardTransitionFixture
$retry.board_transition_receipt.states[0].first.board_transition.accepted_requests=2
$retry.board_transition_receipt.states[1].first.board_transition.accepted_requests=2
Assert-QuickEvidence (Test-BmxQuickSwitchEvidence $retry 1234) 'Existing accepted native retries remain distinct from the one shortcut gesture.'

$mutations=@(
    {param($r) $r.passed='true'},
    {param($r) $r.completed=1},
    {param($r) $r.input_source='physical_adapter'},
    {param($r) $r.physical_controller_verified=$true},
    {param($r) $r.xinput_first_packet_verified=$true},
    {param($r) $r.dpad_hardware_verified=$true},
    {param($r) $r.virtual_device='1112365059'},
    {param($r) $r.wall_limit_ms=190001},
    {param($r) $r.setup_requests=@(1,4,7,8)},
    {param($r) $r.guarded_setdown_mount_followup_requests=0},
    {param($r) $r.observations[5].world=101},
    {param($r) $r.observations[4].owner[2]=99},
    {param($r) $r.observations[8].token=703},
    {param($r) $r.observations[8].mounts=3},
    {param($r) $r.observations[7].allocations=2},
    {param($r) $r.observations[2].physics=504},
    {param($r) $r.observations[2].capture=$true},
    {param($r) $r.observations[8].strict_setdowns=0},
    {param($r) $r.observations[7].motion_supported=$false},
    {param($r) $r.observations[3].applications=$r.observations[1].applications},
    {param($r) $r.observations[5].serial=$r.observations[4].serial},
    {param($r) $r.observations[7].native_time=[double]::NaN},
    {param($r) $r.pulses[0].ordinary_events=1},
    {param($r) $r.pulses[1].quick_events=2},
    {param($r) $r.pulses[2].route=4},
    {param($r) $r.pulses[3].released=$false},
    {param($r) $r.pulses[0].north_capture_requested=$false},
    {param($r) $r.pulses[0].release_time=$r.pulses[0].edge_time+.1},
    {param($r) $r.pulses[1].release_time=$r.pulses[1].edge_time+.5},
    {param($r) $r.pulses[2].last_serial=$r.observations[5].serial},
    {param($r) $r.observations=@($r.observations[0..7])},
    {param($r) $r.pulses=@($r.pulses)+@($r.pulses[0])},
    {param($r) $r.schema_version=1},
    {param($r) $r.preflight.passed=$false},
    {param($r) $r.preflight.completed=$false},
    {param($r) $r.preflight.samples=1},
    {param($r) $r.preflight.elapsed_ms=2000},
    {param($r) $r.preflight.wall_limit_ms=2001},
    {param($r) $r.preflight.first_clock.serial=$r.preflight.qualified_clock.serial},
    {param($r) $r.preflight.first_clock.native_time=$r.preflight.qualified_clock.native_time},
    {param($r) $r.preflight.first_clock.timing_available=$false},
    {param($r) $r.preflight.qualified_clock.timing_available=$false},
    {param($r) $r.preflight.first_clock.owner[2]=99},
    {param($r) $r.preflight.qualified_clock.serial++},
    {param($r) $r.preflight.qualified_clock.native_time=[double]::NaN},
    {param($r) $r.preflight.qualified_clock.physics=300},
    {param($r) $r.first_native_observation.native_motion_enabled=$false},
    {param($r) $r.first_native_observation.native_frame_read_attempted=$false},
    {param($r) $r.first_native_observation.native_frame_issue='busy'},
    {param($r) $r.first_native_observation.lifecycle_allowed=$false},
    {param($r) $r.first_native_observation.focused=$false},
    {param($r) $r.first_native_observation.physical_quiet=$false},
    {param($r) $r.first_native_observation.pool_failed=$true},
    {param($r) $r.first_native_observation.pool_blocked=$true},
    {param($r) $r.first_native_observation.pool_quarantined=$true},
    {param($r) $r.first_native_observation.pool_suspended=$true},
    {param($r) $r.first_native_observation.pool_deferred_park=$true},
    {param($r) $r.first_native_observation.pool_detached=$true},
    {param($r) $r.first_native_observation.frame.available=$false},
    {param($r) $r.first_native_observation.frame.physics_available=$false},
    {param($r) $r.first_native_observation.frame.owner[0]=99},
    {param($r) $r.first_native_observation.frame.allocations=1},
    {param($r) $r.first_native_observation.wall_ms=999},
    {param($r) $r.last_native_observation.wall_ms=191000},
    {param($r) $r.last_native_observation.frame.serial++},
    {param($r) $r.last_native_observation.stage='preflight'},
    {param($r) $r.first_rejected_native_observation=$r.first_native_observation},
    {param($r) $r.first_rejection_issue='preflight_failed'},
    {param($r) $r.first_rejection_observation_relation='prior_native_attempt_no_current_read'},
    {param($r) $r.schema_version=2},
    {param($r) $r.board_transition_receipt.grounded_proven=$false},
    {param($r) $r.board_transition_receipt.grounded_first.serial=$r.board_transition_receipt.grounded_confirmed.serial},
    {param($r) $r.board_transition_receipt.grounded_first.native_time=$r.board_transition_receipt.grounded_confirmed.native_time},
    {param($r) $r.board_transition_receipt.grounded_first.physics=201},
    {param($r) $r.board_transition_receipt.grounded_confirmed.capture=$true},
    {param($r) $r.board_transition_receipt.grounded_confirmed.serial++},
    {param($r) $r.board_transition_receipt.observed=$true}

)
$index=0
foreach ($mutate in $mutations) {
    $broken=New-QuickEvidenceFixture;& $mutate $broken
    Assert-QuickEvidence (-not (Test-BmxQuickSwitchEvidence $broken 1234)) "Malformed evidence mutation $index was accepted."
    ++$index
}
$transitionMutations=@(
    {param($r) $r.board_transition_receipt.states[0].first.board_transition.pending_for_owner=$false},
    {param($r) $r.board_transition_receipt.states[0].first.board_transition.request_clock_valid=$false},
    {param($r) $r.board_transition_receipt.states[0].first.board_transition.parked_pool_proven=$false},
    {param($r) $r.board_transition_receipt.states[0].first.board_transition.clock_unchanged=$false},
    {param($r) $r.board_transition_receipt.states[0].first.board_transition.physics_unchanged=$false},
    {param($r) $r.board_transition_receipt.states[0].first.board_transition.accepted_requests=0},
    {param($r) $r.board_transition_receipt.states[0].first.board_transition.accepted_requests=65},
    {param($r) $r.board_transition_receipt.states[0].first.board_transition.request_owner[2]=99},
    {param($r) $r.board_transition_receipt.states[0].first.board_transition.world=101},
    {param($r) $r.board_transition_receipt.states[0].first.board_transition.token=702},
    {param($r) $r.board_transition_receipt.states[0].first.board_transition.pending_deadline_ms=1400},
    {param($r) $r.board_transition_receipt.states[0].first.board_transition.pending_deadline_ms=16401},
    {param($r) $r.board_transition_receipt.states[0].first.board_transition.request_wall_ms=1401},
    {param($r) $r.board_transition_receipt.states[0].first.board_transition.request_serial=22},
    {param($r) $r.board_transition_receipt.states[0].first.board_transition.request_native_time=[double]::NaN},
    {param($r) $r.board_transition_receipt.states[0].first.active=$true},
    {param($r) $r.board_transition_receipt.states[0].first.pending=$true},
    {param($r) $r.board_transition_receipt.states[0].first.mounting=$true},
    {param($r) $r.board_transition_receipt.states[0].first.parked=$true},
    {param($r) $r.board_transition_receipt.states[0].first.carried=$true},
    {param($r) $r.board_transition_receipt.states[0].first.capture=$true},
    {param($r) $r.board_transition_receipt.states[0].first.parts=8},
    {param($r) $r.board_transition_receipt.states[0].first.token=701},
    {param($r) $r.board_transition_receipt.states[0].first.strict_setdowns=1},
    {param($r) $r.board_transition_receipt.states[0].first.mounts=2},
    {param($r) $r.board_transition_receipt.states[0].first.allocations=2},
    {param($r) $r.board_transition_receipt.states[0].first.pool_active=$true},
    {param($r) $r.board_transition_receipt.states[0].first.pool_parked=$false},
    {param($r) $r.board_transition_receipt.states[0].first.offboard=$true},
    {param($r) $r.board_transition_receipt.states[0].first.standable=$true},
    {param($r) $r.board_transition_receipt.states[0].first.motion_supported=$true},
    {param($r) $r.board_transition_receipt.states[0].first.physics=101},
    {param($r) $r.board_transition_receipt.states[0].first.physics=300},
    {param($r) $r.board_transition_receipt.states[0].first.serial=$r.pulses[0].last_serial},
    {param($r) $r.board_transition_receipt.states[0].first.native_time=$r.pulses[0].last_time},
    {param($r) $r.board_transition_receipt.states[0].first.serial=$r.board_transition_receipt.grounded_first.serial},
    {param($r) $r.board_transition_receipt.states[0].first.native_time=$r.board_transition_receipt.grounded_first.native_time},
    {param($r) $r.board_transition_receipt.states[0].phase='pulse'},
    {param($r) $r.board_transition_receipt.states[0].samples=2},
    {param($r) $r.board_transition_receipt.states[0].samples=0},
    {param($r) $r.board_transition_receipt.observed=$false},
    {param($r) $r.board_transition_receipt.samples=3},
    {param($r) $r.board_transition_receipt.states[1].physics=200},
    {param($r) $r.board_transition_receipt.states[1].first.board_transition.request_serial++},
    {param($r) $r.board_transition_receipt.states[0].first.PSObject.Properties.Remove('board_transition')}
)
$transitionIndex=0
foreach ($mutate in $transitionMutations) {
    $broken=New-QuickBoardTransitionFixture;& $mutate $broken
    Assert-QuickEvidence (-not (Test-BmxQuickSwitchEvidence $broken 1234)) "Malformed transition mutation $transitionIndex was accepted."
    ++$transitionIndex
}
foreach ($field in @('observations','pulses','setup_requests','completed','input_source','preflight','first_native_observation','last_native_observation','first_rejected_native_observation','first_rejection_issue','first_rejection_observation_relation','board_transition_receipt')) {
    $broken=New-QuickEvidenceFixture;$broken.PSObject.Properties.Remove($field)
    Assert-QuickEvidence (-not (Test-BmxQuickSwitchEvidence $broken 1234)) "Missing field $field was accepted."
}
Write-Output "Quick-switch evidence: three valid fast/boarding/retry fixtures and $($mutations.Count+$transitionMutations.Count+13) negative identity, schema, native-state, ownership, hold/release, counter, two-clock preflight, board transition context/stability, granular diagnostics and hardware-claim cases passed. Synthetic fixtures only."
