# Data-only native receipt checks. These never poll or write game memory.
function Test-BmxQuickUnsigned($Value,[decimal]$Minimum=0,[decimal]$Maximum=[uint64]::MaxValue) {
    if ($null -eq $Value -or $Value -is [bool] -or $Value -is [string] -or $Value -isnot [ValueType]) { return $false }
    try { $number=[decimal]$Value } catch { return $false }
    return $number -ge $Minimum -and $number -le $Maximum -and $number % 1 -eq 0
}
function Test-BmxQuickFinite($Value) {
    if ($null -eq $Value -or $Value -is [bool] -or $Value -is [string] -or $Value -isnot [ValueType]) { return $false }
    $number=[double]$Value
    return -not [double]::IsNaN($number) -and -not [double]::IsInfinity($number) -and $number -ge 0
}
function Test-BmxQuickFrameShape($Frame,[bool]$AllowBoarding=$false) {
    if (-not $Frame) { return $false }
    foreach ($field in @('owner','available','timing_available','physics_available','physics','offboard','standable','motion_supported',
        'serial','native_time','native_time_finite','active','pending','mounting','parked','carried','crashed','pool_active','pool_parked',
        'pool_failed','pool_suspended','capture','world','allocations','mounts','token','applications','parts','strict_setdowns')) {
        if ($null -eq $Frame.PSObject.Properties[$field]) { return $false }
    }
    foreach ($field in @('available','timing_available','physics_available','offboard','standable','motion_supported','native_time_finite',
        'active','pending','mounting','parked','carried','crashed','pool_active','pool_parked','pool_failed','pool_suspended','capture')) {
        if ($Frame.$field -isnot [bool]) { return $false }
    }
    foreach ($field in @('physics','serial','world','allocations','mounts','token','applications','parts','strict_setdowns')) {
        if (-not (Test-BmxQuickUnsigned $Frame.$field)) { return $false }
    }
    if (-not $Frame.available -or -not $Frame.physics_available -or -not $Frame.native_time_finite -or
        -not (Test-BmxQuickFinite $Frame.native_time) -or $Frame.pool_failed -or $Frame.pool_suspended -or $Frame.crashed) { return $false }
    $owner=@($Frame.owner);if ($owner.Count -ne 5) { return $false }
    foreach ($lane in $owner) { if (-not (Test-BmxQuickUnsigned $lane 1)) { return $false } }
    if ($Frame.offboard) {
        if ($Frame.physics -ne 504 -or -not $Frame.motion_supported) { return $false }
    } elseif ($Frame.physics -ne 100) {
        if (-not $AllowBoarding -or $Frame.physics -notin @(200,201) -or $Frame.standable -or $Frame.motion_supported) { return $false }
    }
    return $true
}
function Test-BmxQuickIdleFrame($Frame,[bool]$RequireClock) {
    if (-not (Test-BmxQuickFrameShape $Frame)) { return $false }
    if ($RequireClock -and (-not $Frame.timing_available -or -not (Test-BmxQuickUnsigned $Frame.serial 1))) { return $false }
    foreach ($field in @('active','pending','mounting','parked','carried','pool_active','pool_parked','capture')) { if ($Frame.$field) { return $false } }
    foreach ($field in @('world','allocations','mounts','token','applications','parts','strict_setdowns')) { if ($Frame.$field -ne 0) { return $false } }
    return $true
}
function Test-BmxQuickSourceObservation($Sample,[string]$Stage) {
    if (-not $Sample) { return $false }
    foreach ($field in @('stage','phase','wall_ms','native_motion_enabled','native_frame_read_attempted','native_frame_issue',
        'lifecycle_allowed','physical_quiet','focused','native_ui_available','native_ui_menu','overlay_ready','overlay_menu',
        'capture_blocked','menu_requested','pool_ready','pool_failed','pool_blocked','pool_quarantined','pool_suspended',
        'pool_deferred_park','pool_detached','pool_issue','frame')) {
        if ($null -eq $Sample.PSObject.Properties[$field]) { return $false }
    }
    if ($Sample.stage -cne $Stage -or $Sample.phase -isnot [string] -or -not (Test-BmxQuickUnsigned $Sample.wall_ms) -or
        $Sample.native_frame_issue -isnot [string] -or $Sample.native_frame_issue -cne '' -or $Sample.pool_issue -isnot [string] -or
        $Sample.pool_ready -isnot [bool]) { return $false }
    foreach ($field in @('native_motion_enabled','native_frame_read_attempted','lifecycle_allowed','physical_quiet','focused','native_ui_available','overlay_ready')) {
        if ($Sample.$field -isnot [bool] -or -not $Sample.$field) { return $false }
    }
    foreach ($field in @('native_ui_menu','overlay_menu','capture_blocked','menu_requested','pool_failed','pool_blocked','pool_quarantined',
        'pool_suspended','pool_deferred_park','pool_detached')) {
        if ($Sample.$field -isnot [bool] -or $Sample.$field) { return $false }
    }
    return Test-BmxQuickFrameShape $Sample.frame
}
function Test-BmxQuickPreflightEvidence($Report) {
    $p=$Report.preflight
    if (-not $p -or -not (Test-BmxQuickSourceObservation $Report.first_native_observation 'preflight') -or
        -not (Test-BmxQuickSourceObservation $Report.last_native_observation 'scenario') -or
        $null -ne $Report.first_rejected_native_observation -or $Report.first_rejection_issue -isnot [string] -or
        $Report.first_rejection_issue -cne '' -or $Report.first_rejection_observation_relation -isnot [string] -or
        $Report.first_rejection_observation_relation -cne '') { return $false }
    foreach ($field in @('completed','passed','issue','samples','wall_started_ms','elapsed_ms','wall_limit_ms','first_clock','qualified_clock')) {
        if ($null -eq $p.PSObject.Properties[$field]) { return $false }
    }
    if ($p.completed -isnot [bool] -or -not $p.completed -or $p.passed -isnot [bool] -or -not $p.passed -or
        $p.issue -cne 'qualified_two_native_clock_samples' -or -not (Test-BmxQuickUnsigned $p.samples 2 ([uint32]::MaxValue)) -or
        -not (Test-BmxQuickUnsigned $p.wall_started_ms) -or -not (Test-BmxQuickUnsigned $p.wall_limit_ms 2000 2000) -or
        -not (Test-BmxQuickUnsigned $p.elapsed_ms 0 1999) -or
        -not (Test-BmxQuickIdleFrame $p.first_clock $true) -or -not (Test-BmxQuickIdleFrame $p.qualified_clock $true) -or
        -not (Test-BmxQuickIdleFrame $Report.first_native_observation.frame $false)) { return $false }
    if ([decimal]$p.qualified_clock.serial -le [decimal]$p.first_clock.serial -or
        [double]$p.qualified_clock.native_time -le [double]$p.first_clock.native_time) { return $false }
    $initial=$Report.observations[0];$final=$Report.observations[-1]
    $owner=@($initial.owner)
    foreach ($frame in @($Report.first_native_observation.frame,$p.first_clock,$p.qualified_clock,$Report.last_native_observation.frame)) {
        for ($lane=0;$lane -lt 5;++$lane) { if ([decimal]$frame.owner[$lane] -ne [decimal]$owner[$lane]) { return $false } }
    }
    foreach ($frame in @($Report.first_native_observation.frame,$p.first_clock)) {
        if ($frame.physics -ne $p.qualified_clock.physics -or $frame.offboard -ne $p.qualified_clock.offboard) { return $false }
    }
    if ($p.qualified_clock.serial -ne $initial.serial -or $p.qualified_clock.native_time -ne $initial.native_time -or
        $p.qualified_clock.physics -ne $initial.physics -or $p.qualified_clock.offboard -ne $initial.offboard -or
        $Report.first_native_observation.phase -cne 'idle' -or $Report.last_native_observation.phase -cne 'final_bike') { return $false }
    foreach ($field in @('serial','native_time','token','applications','world','allocations','mounts','parts','offboard','standable','active',
        'parked','carried','pool_active','pool_parked','capture','physics','motion_supported','strict_setdowns')) {
        if ($Report.last_native_observation.frame.$field -ne $final.$field) { return $false }
    }
    if ([decimal]$Report.first_native_observation.wall_ms -lt [decimal]$p.wall_started_ms -or
        [decimal]$Report.first_native_observation.wall_ms -gt ([decimal]$p.wall_started_ms+[decimal]$p.elapsed_ms) -or
        [decimal]$Report.last_native_observation.wall_ms -lt ([decimal]$p.wall_started_ms+[decimal]$p.elapsed_ms) -or
        ([decimal]$Report.last_native_observation.wall_ms-[decimal]$p.wall_started_ms) -ge 190000) { return $false }
    return $true
}
function Test-BmxQuickGroundedBoardFrame($Frame,$Report) {
    if (-not (Test-BmxQuickFrameShape $Frame) -or -not $Frame.timing_available -or
        -not (Test-BmxQuickUnsigned $Frame.serial 1) -or $Frame.physics -ne 100 -or $Frame.offboard -or
        $Frame.active -or $Frame.pending -or $Frame.mounting -or $Frame.parked -or $Frame.carried -or $Frame.capture -or
        $Frame.parts -ne 0 -or $Frame.token -ne 0 -or $Frame.mounts -ne 1 -or $Frame.allocations -ne 1 -or
        $Frame.pool_active -or -not $Frame.pool_parked -or $Frame.strict_setdowns -ne 0 -or
        $Frame.world -ne $Report.observations[1].world) { return $false }
    for ($lane=0;$lane -lt 5;++$lane) { if ($Frame.owner[$lane] -ne $Report.observations[1].owner[$lane]) { return $false } }
    return $Frame.serial -gt $Report.pulses[0].last_serial -and $Frame.native_time -gt $Report.pulses[0].last_time
}
function Test-BmxQuickBoardingFrame($Frame,$Report) {
    # The200/201 exception is callable only from the dedicated transition receipt.
    if (-not (Test-BmxQuickFrameShape $Frame $true) -or $Frame.physics -notin @(200,201) -or
        -not $Frame.timing_available -or -not (Test-BmxQuickUnsigned $Frame.serial 1) -or $Frame.offboard -or $Frame.standable -or $Frame.motion_supported -or
        $Frame.active -or $Frame.pending -or $Frame.mounting -or $Frame.parked -or $Frame.carried -or $Frame.capture -or
        $Frame.parts -ne 0 -or $Frame.token -ne 0 -or $Frame.mounts -ne 1 -or $Frame.allocations -ne 1 -or
        $Frame.pool_active -or -not $Frame.pool_parked -or $Frame.strict_setdowns -ne 0 -or
        $Frame.world -ne $Report.observations[1].world -or $null -eq $Frame.PSObject.Properties['board_transition']) { return $false }
    $w=$Frame.board_transition
    foreach ($field in @('pending_for_owner','request_clock_valid','parked_pool_proven','clock_unchanged','physics_unchanged','accepted_requests',
        'observed_wall_ms','pending_deadline_ms','request_wall_ms','request_serial','request_native_time','world','token','request_owner')) {
        if ($null -eq $w.PSObject.Properties[$field]) { return $false }
    }
    foreach ($field in @('pending_for_owner','request_clock_valid','parked_pool_proven','clock_unchanged','physics_unchanged')) {
        if ($w.$field -isnot [bool] -or -not $w.$field) { return $false }
    }
    foreach ($field in @('observed_wall_ms','pending_deadline_ms','request_wall_ms','request_serial','world','token')) {
        if (-not (Test-BmxQuickUnsigned $w.$field)) { return $false }
    }
    if (-not (Test-BmxQuickUnsigned $w.accepted_requests 1 64) -or -not (Test-BmxQuickFinite $w.request_native_time) -or
        $w.world -ne $Frame.world -or $w.token -ne $Report.observations[1].token -or @($w.request_owner).Count -ne 5) { return $false }
    for ($lane=0;$lane -lt 5;++$lane) {
        if (-not (Test-BmxQuickUnsigned $w.request_owner[$lane] 1) -or $w.request_owner[$lane] -ne $Frame.owner[$lane] -or
            $Frame.owner[$lane] -ne $Report.observations[1].owner[$lane]) { return $false }
    }
    $now=[decimal]$w.observed_wall_ms;$deadline=[decimal]$w.pending_deadline_ms;$requested=[decimal]$w.request_wall_ms
    if ($deadline -lt 15000 -or $now -lt $deadline-15000 -or $now -ge $deadline -or
        $requested -lt $deadline-15000 -or $requested -gt $now -or $now -lt [decimal]$Report.preflight.wall_started_ms -or
        $now-[decimal]$Report.preflight.wall_started_ms -ge 190000 -or
        [decimal]$w.request_serial -lt [decimal]$Report.pulses[0].edge_serial -or [decimal]$w.request_serial -gt [decimal]$Frame.serial -or
        $w.request_native_time -lt $Report.pulses[0].edge_time -or $w.request_native_time -gt $Frame.native_time -or
        $Frame.serial -le $Report.pulses[0].last_serial -or $Frame.native_time -le $Report.pulses[0].last_time -or
        $Frame.serial -ge $Report.board_transition_receipt.grounded_first.serial -or
        $Frame.native_time -ge $Report.board_transition_receipt.grounded_first.native_time) { return $false }
    return $true
}
function Test-BmxQuickBoardTransitionEvidence($Report) {
    $r=$Report.board_transition_receipt
    if (-not $r) { return $false }
    foreach ($field in @('observed','samples','states','grounded_proven','grounded_first','grounded_confirmed')) {
        if ($null -eq $r.PSObject.Properties[$field]) { return $false }
    }
    if ($r.observed -isnot [bool] -or $r.grounded_proven -isnot [bool] -or -not $r.grounded_proven -or
        -not (Test-BmxQuickUnsigned $r.samples 0 1800) -or @($r.states).Count -ne 2 -or
        -not (Test-BmxQuickGroundedBoardFrame $r.grounded_first $Report) -or
        -not (Test-BmxQuickGroundedBoardFrame $r.grounded_confirmed $Report) -or
        $r.grounded_confirmed.serial -le $r.grounded_first.serial -or $r.grounded_confirmed.native_time -le $r.grounded_first.native_time) { return $false }
    foreach ($field in @('serial','native_time','token','applications','world','allocations','mounts','parts','offboard','standable','active',
        'parked','carried','pool_active','pool_parked','capture','physics','motion_supported','strict_setdowns')) {
        if ($r.grounded_confirmed.$field -ne $Report.observations[2].$field) { return $false }
    }
    $total=0;$requestIdentity=$null
    for ($i=0;$i -lt 2;++$i) {
        $state=$r.states[$i];$expected=200+$i
        foreach ($field in @('physics','samples','phase','first','last')) { if ($null -eq $state.PSObject.Properties[$field]) { return $false } }
        if (-not (Test-BmxQuickUnsigned $state.physics $expected $expected) -or -not (Test-BmxQuickUnsigned $state.samples 0 1800) -or
            $state.phase -cne 'native_board') { return $false }
        $total+=$state.samples
        if (-not $state.samples) { if ($null -ne $state.first -or $null -ne $state.last) { return $false };continue }
        foreach ($frame in @($state.first,$state.last)) {
            if (-not (Test-BmxQuickBoardingFrame $frame $Report) -or $frame.physics -ne $expected) { return $false }
            $w=$frame.board_transition
            if ($requestIdentity) {
                foreach ($field in @('request_wall_ms','request_serial','request_native_time','pending_deadline_ms','world','token')) {
                    if ($w.$field -ne $requestIdentity.$field) { return $false }
                }
            } else { $requestIdentity=$w }
        }
        if ($state.samples -eq 1) {
            if ($state.first.serial -ne $state.last.serial -or $state.first.native_time -ne $state.last.native_time) { return $false }
        } elseif ($state.last.serial -le $state.first.serial -or $state.last.native_time -le $state.first.native_time -or
            [decimal]$state.samples -gt ([decimal]$state.last.serial-[decimal]$state.first.serial+1)) { return $false }
    }
    return $total -eq $r.samples -and $r.observed -eq ($total -gt 0)
}
function Test-BmxQuickSwitchEvidence($Report,[int]$ProcessId) {
    if (-not $Report) { return $false }
    foreach ($field in @('schema_version','process_id','completed','passed','issue','input_source','virtual_device',
        'physical_controller_verified','xinput_first_packet_verified','dpad_hardware_verified','hold_native_seconds',
        'wall_limit_ms','guarded_setdown_mount_followup_requests','setup_requests','pulses','observations',
        'board_transition_receipt','preflight','first_native_observation','last_native_observation','first_rejected_native_observation','first_rejection_issue','first_rejection_observation_relation')) {
        if ($null -eq $Report.PSObject.Properties[$field]) { return $false }
    }
    foreach ($field in @('completed','passed')) { if ($Report.$field -isnot [bool] -or -not $Report.$field) { return $false } }
    foreach ($field in @('physical_controller_verified','xinput_first_packet_verified','dpad_hardware_verified')) {
        if ($Report.$field -isnot [bool] -or $Report.$field) { return $false }
    }
    if (-not (Test-BmxQuickUnsigned $Report.schema_version 3 3) -or
        -not (Test-BmxQuickUnsigned $Report.process_id $ProcessId $ProcessId) -or
        -not (Test-BmxQuickUnsigned $Report.virtual_device 1112365059 1112365059) -or
        -not (Test-BmxQuickUnsigned $Report.wall_limit_ms 190000 190000) -or
        -not (Test-BmxQuickUnsigned $Report.guarded_setdown_mount_followup_requests 1 1) -or
        $Report.input_source -cne 'synthetic_virtual_device' -or
        $Report.issue -cne 'four_native_quick_switch_routes_completed' -or
        -not (Test-BmxQuickFinite $Report.hold_native_seconds) -or $Report.hold_native_seconds -ne .2) { return $false }
    $setup=@($Report.setup_requests);$expectedSetup=@(1,7,7,8)
    if ($setup.Count -ne 4) { return $false }
    for ($i=0;$i -lt 4;++$i) { if (-not (Test-BmxQuickUnsigned $setup[$i] $expectedSetup[$i] $expectedSetup[$i])) { return $false } }
    $observations=@($Report.observations)
    $names=@('idle','initial_mount','native_board','returned_bike','parked_near','nearby_bike','parked_carry','carried','final_bike')
    if ($observations.Count -ne $names.Count) { return $false }
    $owner=$null;$world=[decimal]0;$initialToken=[decimal]0;$lastApplications=[decimal]0;$lastMountedApplications=[decimal]0
    for ($i=0;$i -lt $observations.Count;++$i) {
        $v=$observations[$i]
        foreach ($field in @('phase','serial','native_time','token','applications','world','allocations','mounts','parts',
            'offboard','standable','motion_supported','active','parked','carried','pool_active','pool_parked','capture','physics','strict_setdowns','owner')) {
            if ($null -eq $v.PSObject.Properties[$field]) { return $false }
        }
        if ($v.phase -cne $names[$i] -or -not (Test-BmxQuickUnsigned $v.serial 1) -or -not (Test-BmxQuickFinite $v.native_time)) { return $false }
        foreach ($field in @('token','applications','world','allocations','mounts','parts','physics','strict_setdowns')) {
            if (-not (Test-BmxQuickUnsigned $v.$field)) { return $false }
        }
        foreach ($field in @('offboard','standable','motion_supported','active','parked','carried','pool_active','pool_parked','capture')) {
            if ($v.$field -isnot [bool]) { return $false }
        }
        if ($v.offboard -and -not $v.motion_supported) { return $false }
        $currentOwner=@($v.owner)
        if ($currentOwner.Count -ne 5) { return $false }
        for ($lane=0;$lane -lt 5;++$lane) {
            if (-not (Test-BmxQuickUnsigned $currentOwner[$lane] 1)) { return $false }
            if ($owner -and [decimal]$currentOwner[$lane] -ne [decimal]$owner[$lane]) { return $false }
        }
        if (-not $owner) { $owner=$currentOwner }
        if ($i -and ([decimal]$v.serial -le [decimal]$observations[$i-1].serial -or
            [double]$v.native_time -le [double]$observations[$i-1].native_time)) { return $false }
        if ([decimal]$v.applications -lt $lastApplications) { return $false };$lastApplications=[decimal]$v.applications
        if ($i -eq 0) { continue }
        if ($i -eq 1) { $world=[decimal]$v.world;$initialToken=[decimal]$v.token }
        if (-not $world -or $v.allocations -ne 1 -or [decimal]$v.world -ne $world -or
            -not $initialToken -or $initialToken -gt ([decimal][uint64]::MaxValue-3)) { return $false }
        $mounted=$i -in @(1,3,5,8)
        if ($mounted) {
            $mounts=switch ($i) { 1 {1} 3 {2} 5 {3} 8 {4} }
            if (-not $v.active -or $v.parked -or $v.carried -or -not $v.pool_active -or $v.pool_parked -or
                -not $v.capture -or -not $v.offboard -or $v.physics -ne 504 -or $v.parts -ne 8 -or
                $v.mounts -ne $mounts -or [decimal]$v.token -ne $initialToken+$mounts-1 -or
                [decimal]$v.applications -le $lastMountedApplications) { return $false }
            $lastMountedApplications=[decimal]$v.applications
        } elseif ($i -eq 2) {
            if ($v.active -or $v.parked -or $v.carried -or $v.pool_active -or -not $v.pool_parked -or
                $v.capture -or $v.offboard -or $v.physics -ne 100 -or $v.parts -ne 0 -or $v.mounts -ne 1 -or $v.token -ne 0) { return $false }
        } else {
            $mounts=if($i -eq 4){2}else{3}
            if ($v.active -or -not $v.parked -or $v.carried -ne ($i -eq 7) -or $v.pool_active -or
                -not $v.pool_parked -or $v.capture -or -not $v.offboard -or -not $v.standable -or
                $v.physics -ne 504 -or $v.parts -ne 8 -or $v.mounts -ne $mounts -or
                [decimal]$v.token -ne $initialToken+$mounts-1) { return $false }
        }
        if ($v.strict_setdowns -ne $(if($i -eq 8){1}else{0})) { return $false }
    }
    $pulses=@($Report.pulses);if ($pulses.Count -ne 4) { return $false }
    $routes=@(4,1,7,7);$before=@(1,2,4,7);$after=@(2,3,5,8)
    for ($i=0;$i -lt 4;++$i) {
        $v=$pulses[$i]
        foreach ($field in @('index','samples','quick_events','ordinary_events','route','first_serial','edge_serial',
            'release_serial','last_serial','first_time','edge_time','release_time','last_time','north_capture_requested','released')) {
            if ($null -eq $v.PSObject.Properties[$field]) { return $false }
        }
        foreach ($field in @('index','samples','quick_events','ordinary_events','route','first_serial','edge_serial','release_serial','last_serial')) {
            if (-not (Test-BmxQuickUnsigned $v.$field)) { return $false }
        }
        foreach ($field in @('first_time','edge_time','release_time','last_time')) { if (-not (Test-BmxQuickFinite $v.$field)) { return $false } }
        foreach ($field in @('north_capture_requested','released')) { if ($v.$field -isnot [bool] -or -not $v.$field) { return $false } }
        if ($v.index -ne $i -or $v.samples -lt 5 -or $v.quick_events -ne 1 -or $v.ordinary_events -ne 0 -or
            $v.route -ne $routes[$i] -or [decimal]$v.first_serial -ne [decimal]$observations[$before[$i]].serial -or
            [decimal]$v.edge_serial -le [decimal]$v.first_serial -or [decimal]$v.release_serial -le [decimal]$v.edge_serial -or
            [decimal]$v.last_serial -le [decimal]$v.release_serial -or
            [decimal]$v.last_serial -ge [decimal]$observations[$after[$i]].serial -or
            $v.first_time -ne $observations[$before[$i]].native_time -or $v.edge_time -le $v.first_time -or
            $v.release_time -le $v.edge_time -or $v.last_time -le $v.release_time -or
            $v.last_time -ge $observations[$after[$i]].native_time -or
            ($v.release_time-$v.edge_time) -lt .2 -or ($v.release_time-$v.edge_time) -gt .35) { return $false }
    }
    return (Test-BmxQuickPreflightEvidence $Report) -and (Test-BmxQuickBoardTransitionEvidence $Report)
}
