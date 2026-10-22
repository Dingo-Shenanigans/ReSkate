#requires -Version 7.0
# PowerShell 7 is required for System.Text.Json duplicate-property validation.
# Data-only evidence parsers. Never launch, stop, capture, or inject input.
# The private GetUp assessor below is copied unchanged from the existing lab;
# post-fall remount assesses a strict pre-interaction prefix using that contract.
function Test-BmxFiniteNumber($bmxValue,[double]$bmxMaximum=1000000) {
    if ($null -eq $bmxValue -or $bmxValue -is [bool] -or $bmxValue -is [string] -or $bmxValue -isnot [ValueType]) { return $false }
    $bmxNumber=[double]$bmxValue
    return -not [double]::IsNaN($bmxNumber) -and -not [double]::IsInfinity($bmxNumber) -and [math]::Abs($bmxNumber) -le $bmxMaximum
}
function Test-BmxUnsignedInteger($bmxValue,[switch]$AllowZero) {
    if($null -eq $bmxValue -or $bmxValue -is [bool] -or $bmxValue -is [string] -or $bmxValue -isnot [ValueType]){return $false}
    $bmxInteger=[uint64]0
    return [uint64]::TryParse([string]$bmxValue,[ref]$bmxInteger) -and ($AllowZero -or $bmxInteger -gt 0)
}
function Test-BmxImpactNativeRoute($bmxObservation) {
    if(-not (Test-BmxUnsignedInteger $bmxObservation.native_calls -AllowZero)){return $false}
    $bmxActivation=$bmxObservation.PSObject.Properties['activation_enabled']
    if($bmxActivation -and $bmxObservation.activation_enabled -isnot [bool]){return $false}
    if(-not $bmxActivation -or -not $bmxObservation.activation_enabled){
        # Preserve the original leaf route, including archived reports which
        # predate activation fields. A read-only intent observer is not a leaf.
        if($null -ne $bmxObservation.PSObject.Properties['intent_armed'] -and
           ($bmxObservation.intent_armed -isnot [bool] -or $bmxObservation.intent_armed)){return $false}
        return [uint64]$bmxObservation.native_calls -eq 1
    }
    foreach($bmxField in @('intent_armed','intent_prior_false','intent_true_returned','intent_true_readback',
        'intent_written','intent_reset','intent_reset_readback')){
        if($bmxObservation.$bmxField -isnot [bool] -or -not $bmxObservation.$bmxField){return $false}
    }
    foreach($bmxField in @('intent_reset_pending','intent_owner_expired')){
        if($bmxObservation.$bmxField -isnot [bool] -or $bmxObservation.$bmxField){return $false}
    }
    if($bmxObservation.intent_native_cleared -isnot [bool]){return $false}
    foreach($bmxField in @('intent_true_serial','intent_reset_serial','intent_true_calls','intent_native_calls')){
        if(-not (Test-BmxUnsignedInteger $bmxObservation.$bmxField)){return $false}
    }
    if(-not (Test-BmxUnsignedInteger $bmxObservation.intent_reset_calls -AllowZero)){return $false}
    $bmxTrueCalls=[uint64]$bmxObservation.intent_true_calls;$bmxResetCalls=[uint64]$bmxObservation.intent_reset_calls
    return [uint64]$bmxObservation.native_calls -eq 0 -and $bmxTrueCalls -eq 1 -and $bmxResetCalls -le 1 -and
        [uint64]$bmxObservation.intent_native_calls -eq (1+$bmxResetCalls) -and
        [uint64]$bmxObservation.intent_reset_serial -gt [uint64]$bmxObservation.intent_true_serial -and
        ($bmxResetCalls -eq 1 -or $bmxObservation.intent_native_cleared)
}
function Test-BmxImpactReport($bmxObservation,[int]$bmxProcessId,[int]$bmxVariantId) {
    foreach ($bmxField in @('process_id','completed','variant','bail','reason','region','body_token','serial','com','quaternion','velocity',
        'body_alignment','closing_speed','guard_rays','native_fall_consumed','native_ragdoll_observed','native_calls',
        'issue','chassis_active','elapsed_ms','passed')) {
        if ($null -eq $bmxObservation.PSObject.Properties[$bmxField]) { throw ('Incomplete impact report: '+$bmxField) }
    }
    if (-not (Test-BmxFiniteNumber $bmxObservation.process_id 4294967295) -or [double]$bmxObservation.process_id -ne $bmxProcessId) {
        throw 'Impact report process identity mismatch.'
    }
    if (-not (Test-BmxFiniteNumber $bmxObservation.variant) -or [double]$bmxObservation.variant -ne $bmxVariantId) {
        throw 'Impact report variant mismatch.'
    }
    foreach ($bmxField in @('completed','bail','native_fall_consumed','native_ragdoll_observed','passed')) {
        if ($bmxObservation.$bmxField -isnot [bool] -or -not $bmxObservation.$bmxField) { return $false }
    }
    if ($bmxObservation.chassis_active -isnot [bool] -or $bmxObservation.chassis_active) { return $false }
    foreach ($bmxField in @('native_calls','reason','region','guard_rays','body_alignment','closing_speed','elapsed_ms')) {
        if (-not (Test-BmxFiniteNumber $bmxObservation.$bmxField)) { return $false }
    }
    if (-not (Test-BmxImpactNativeRoute $bmxObservation) -or [double]$bmxObservation.reason -lt 1 -or
        [double]$bmxObservation.reason -gt 4 -or [double]$bmxObservation.region -lt 1 -or
        [double]$bmxObservation.region -gt 3 -or [double]$bmxObservation.guard_rays -lt 1 -or
        [double]$bmxObservation.guard_rays -gt 144 -or [math]::Abs([double]$bmxObservation.body_alignment) -gt 1.001 -or
        [math]::Abs([double]$bmxObservation.closing_speed) -gt 100 -or [double]$bmxObservation.elapsed_ms -lt 0 -or
        [double]$bmxObservation.elapsed_ms -gt 8500) { return $false }
    foreach ($bmxField in @('body_token','serial')) {
        if (-not (Test-BmxUnsignedInteger $bmxObservation.$bmxField)) { return $false }
    }
    foreach ($bmxField in @('com','quaternion','velocity')) {
        $bmxExpectedCount=if ($bmxField -eq 'quaternion') { 4 } else { 3 }
        if (@($bmxObservation.$bmxField).Count -ne $bmxExpectedCount) { return $false }
        foreach ($bmxValue in $bmxObservation.$bmxField) { if (-not (Test-BmxFiniteNumber $bmxValue)) { return $false } }
    }
    $bmxQuaternionNorm=0.0
    foreach ($bmxValue in $bmxObservation.quaternion) { $bmxQuaternionNorm+=[double]$bmxValue*[double]$bmxValue }
    return [math]::Abs($bmxQuaternionNorm-1) -le .001
}
function Test-BmxUniqueJsonElement([System.Text.Json.JsonElement]$bmxElement) {
    if($bmxElement.ValueKind -eq [System.Text.Json.JsonValueKind]::Object){
        $bmxNames=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach($bmxProperty in $bmxElement.EnumerateObject()){
            if(-not $bmxNames.Add($bmxProperty.Name) -or -not (Test-BmxUniqueJsonElement $bmxProperty.Value)){return $false}
        }
    } elseif($bmxElement.ValueKind -eq [System.Text.Json.JsonValueKind]::Array){
        foreach($bmxItem in $bmxElement.EnumerateArray()){if(-not (Test-BmxUniqueJsonElement $bmxItem)){return $false}}
    }
    return $true
}
function ConvertFrom-BmxStrictJson([string]$bmxJson) {
    $bmxDocument=$null
    try {
        $bmxDocument=[System.Text.Json.JsonDocument]::Parse($bmxJson)
        if(-not (Test-BmxUniqueJsonElement $bmxDocument.RootElement)){throw 'Duplicate or case-colliding JSON property.'}
        return ($bmxJson | ConvertFrom-Json -Depth 40)
    } finally {if($bmxDocument){$bmxDocument.Dispose()}}
}
function Test-BmxGuardIdentity($bmxIdentity) {
    foreach($bmxKey in @('client','entity','core','context','rig','world','native_state','body_token')){
        if(-not (Test-BmxUnsignedInteger $bmxIdentity.$bmxKey)){return $false}
    }
    return (Test-BmxUnsignedInteger $bmxIdentity.body_index -AllowZero) -and [uint64]$bmxIdentity.body_index -lt 1048576 -and
        (Test-BmxUnsignedInteger $bmxIdentity.body_generation -AllowZero)
}
function Test-BmxGuardIdentityEqual($bmxExpected,$bmxCurrent) {
    if(-not (Test-BmxGuardIdentity $bmxExpected) -or -not (Test-BmxGuardIdentity $bmxCurrent)){return $false}
    foreach($bmxKey in @('client','entity','core','context','rig','world','native_state','body_index','body_generation','body_token')){
        if([uint64]$bmxExpected.$bmxKey -ne [uint64]$bmxCurrent.$bmxKey){return $false}
    }
    return $true
}
function Get-BmxFreshFallReports([string]$bmxRoot,[int]$bmxProcessId,[DateTime]$bmxEarliestUtc) {
    if(-not (Test-Path -LiteralPath $bmxRoot)){return}
    foreach($bmxFile in (Get-ChildItem -LiteralPath $bmxRoot -Filter "fall-$bmxProcessId-*.json" -File)){
        if($bmxFile.Name -notmatch ('^fall-'+[regex]::Escape([string]$bmxProcessId)+'-\d+\.json$') -or
           $bmxFile.LastWriteTimeUtc -lt $bmxEarliestUtc -or $bmxFile.LastWriteTimeUtc -gt [DateTime]::UtcNow.AddSeconds(1)){continue}
        try {
            $bmxFall=ConvertFrom-BmxStrictJson (Get-Content -LiteralPath $bmxFile.FullName -Raw)
            if($bmxFall.event -eq 'bmx_passive_fall_lifecycle' -and (Test-BmxUnsignedInteger $bmxFall.process_id) -and $bmxFall.process_id -eq $bmxProcessId){
                [pscustomobject]@{file=$bmxFile;report=$bmxFall}
            }
        }catch{ } # A still-writing/invalid file never establishes evidence.
    }
}
function Assert-BmxGetupSample($sample,$baseline) {
    if(-not (Test-BmxUnsignedInteger $sample.serial) -or -not (Test-BmxFiniteNumber $sample.native_time) -or
       -not (Test-BmxFiniteNumber $sample.step_seconds) -or [math]::Abs($sample.step_seconds-1.0/60) -gt .000001 -or
       -not (Test-BmxUnsignedInteger $sample.snapshot_issue -AllowZero) -or $sample.snapshot_issue -ne 0 -or
       $null -eq $sample.body -or $null -eq $sample.rider -or $null -eq $sample.completed_counters){throw 'Native sample body, rider or clock unavailable.'}
    foreach($name in @('offboard','standable','native_protection')){if($sample.rider.$name -isnot [bool]){throw ('Native rider flag unavailable: '+$name)}}
    if($sample.rider.native_protection){throw 'Independent native protection became active.'}
    foreach($name in @('bmx','rider_motion','bike_drive','board_switch_pending')){
        if($sample.capture_leases.$name -isnot [bool] -or $sample.capture_leases.$name){throw 'BMX capture, drive, motion or board timer is active/unknown.'}
    }
    if($sample.capture_leases.interaction -isnot [bool] -or (-not $sample.rider.standable -and $sample.capture_leases.interaction)){throw 'Recovery interaction capture is active/unknown while down.'}
    foreach($name in @('velocity_updates','rider_pose_updates','rider_render_updates','force_applications')){
        if(-not (Test-BmxUnsignedInteger $sample.completed_counters.$name -AllowZero) -or $sample.completed_counters.$name -ne $baseline.$name){throw 'BMX actuator count changed after action preflight.'}
    }
    foreach($name in @('com','velocity','angular_velocity','quaternion')){
        $count=if($name -eq 'quaternion'){4}else{3}
        if(@($sample.body.$name).Count -ne $count){throw 'Native body lane missing.'}
        foreach($value in $sample.body.$name){if(-not (Test-BmxFiniteNumber $value)){throw 'Native body lane nonfinite.'}}
    }
    $norm=0.0;foreach($value in $sample.body.quaternion){$norm+=[double]$value*[double]$value};if([math]::Abs($norm-1) -gt .001){throw 'Native body quaternion invalid.'}
}
function Get-BmxGetupProgress([string]$text,[int]$processId,[int]$variantId,$impact,$action,$trace,[DateTime]$earliestAt) {
    # Pure evidence policy. Report flags alone never establish native recovery.
    $pulsePassed=$false;$stood=$false;$suffixStart=$null;$lastTime=$null;$postSamples=0
    try {
        if(-not (Test-BmxImpactReport $impact $processId $variantId)){throw 'Native fall report failed.'}
        foreach($name in @('accepted_request','request_matches','activation_enabled','intent_status_available')){
            if($impact.$name -isnot [bool] -or -not $impact.$name){throw 'Fall typed receipt unavailable.'}
        }
        foreach($name in @('request_id','expected_request_id','intent_generation','expected_generation')){if(-not (Test-BmxUnsignedInteger $impact.$name)){throw 'Fall receipt field invalid.'}}
        if($impact.request_id -ne $impact.expected_request_id -or $impact.intent_generation -ne $impact.expected_generation){throw 'Fall receipt mismatch.'}
        if($action.event -ne 'bmx_getup_action_test' -or -not (Test-BmxUnsignedInteger $action.process_id) -or $action.process_id -ne $processId){throw 'Action event or PID mismatch.'}
        foreach($name in @('completed','accepted_request','status_fresh','status_available','down_before_action','allowed_before_action')){
            if($action.$name -isnot [bool] -or -not $action.$name){throw ('Action qualification missing: '+$name)}
        }
        foreach($name in @('pulse_passed','standability_observed')){if($action.$name -isnot [bool]){throw 'Action result flag type invalid.'}}
        foreach($name in @('request_id','fall_request_id','fall_generation','entity_handle','requested_serial','allowed_serial')){if(-not (Test-BmxUnsignedInteger $action.$name)){throw ('Action identity or clock invalid: '+$name)}}
        if($action.fall_request_id -ne $impact.request_id -or $action.fall_generation -ne $impact.intent_generation -or
           -not (Test-BmxGuardIdentityEqual $action.identity $trace.identity) -or $action.identity.body_token -ne $impact.body_token){throw 'Action is not bound to the observed fall/body.'}
        $canonical=$trace.canonical_getup_observation
        if(-not (Test-BmxUnsignedInteger $canonical.entity) -or $canonical.entity -ne $action.identity.entity -or
           -not (Test-BmxUnsignedInteger $canonical.entity_handle) -or $canonical.entity_handle -ne $action.entity_handle -or
           $canonical.owner_changed -isnot [bool] -or $canonical.owner_changed){throw 'Full rider handle differs from the canonical lifecycle callback owner.'}
        foreach($name in @('parts','mounts','allocations','velocity_updates','rider_pose_updates','rider_render_updates','force_applications')){if(-not (Test-BmxUnsignedInteger $action.baseline.$name -AllowZero)){throw 'Action baseline incomplete.'}}
        if($action.baseline.parts -ne 8 -or $action.baseline.mounts -ne 1 -or $action.baseline.allocations -ne 1){throw 'Action baseline changed native bike pool.'}
        Assert-BmxGetupSample $action.preflight $action.baseline
        if($action.preflight.eligible -isnot [bool] -or -not $action.preflight.eligible -or
           -not (Test-BmxUnsignedInteger $action.preflight.issue -AllowZero) -or $action.preflight.issue -ne 0){throw 'Action preflight was not eligible.'}
        foreach($lane in @('velocity','angular_velocity')){
            $squared=0.0;foreach($value in $action.preflight.body.$lane){$squared+=[double]$value*[double]$value}
            $maximum=if($lane -eq 'velocity'){.5}else{1.0};if($squared -gt $maximum*$maximum){throw 'Native bike was still moving at action preflight.'}
        }
        if(@($action.preflight.rider.native_cached_velocity).Count -ne 3){throw 'Native cached rider velocity unavailable at action preflight.'}
        $cachedSquared=0.0;foreach($value in $action.preflight.rider.native_cached_velocity){if(-not (Test-BmxFiniteNumber $value)){throw 'Native cached rider velocity nonfinite.'};$cachedSquared+=[double]$value*[double]$value}
        if($cachedSquared -gt 1.0){throw 'Native rider was still moving at action preflight.'}
        if($action.preflight.rider.standable -or -not $action.preflight.rider.offboard -or
           $action.preflight.rider.known_ragdoll_substate -isnot [bool] -or -not $action.preflight.rider.known_ragdoll_substate -or
           $action.requested_serial -ne $action.preflight.serial -or $action.requested_serial -le $impact.serial -or
           -not (Test-BmxFiniteNumber $action.native_time) -or $action.native_time -ne $action.preflight.native_time){throw 'Fresh native down-state preflight required.'}
        $pulse=$action.pulse
        foreach($name in @('generation','true_serial','reset_serial','true_calls')){if(-not (Test-BmxUnsignedInteger $pulse.$name)){throw ('Pulse count/clock invalid: '+$name)}}
        if(-not (Test-BmxUnsignedInteger $pulse.reset_calls -AllowZero) -or $pulse.generation -ne $action.request_id -or
           $pulse.true_calls -ne 1 -or $pulse.reset_calls -gt 1 -or $pulse.true_serial -lt $action.requested_serial -or
           $pulse.reset_serial -le $pulse.true_serial -or $action.allowed_serial -ne $pulse.true_serial){throw 'Pulse generation, action count or later reset clock invalid.'}
        foreach($name in @('prior_false','true_returned','true_readback','intent_written','intent_reset','reset_readback')){
            if($pulse.$name -isnot [bool] -or -not $pulse.$name){throw ('Pulse delivery/reset not proven: '+$name)}
        }
        foreach($name in @('reset_pending','owner_changed','owner_expired')){if($pulse.$name -isnot [bool] -or $pulse.$name){throw 'Pulse cleanup or ownership unresolved.'}}
        if($pulse.native_cleared -isnot [bool] -or ($pulse.reset_calls -eq 0 -and -not $pulse.native_cleared)){throw 'Zero reset call needs verified native clearing.'}
        if(-not (Test-BmxFiniteNumber $action.allowed_native_time) -or $action.allowed_native_time -lt $action.native_time -or
           [math]::Abs(($action.allowed_native_time-$action.native_time)-([uint64]$pulse.true_serial-[uint64]$action.requested_serial)/60.0) -gt .00001){throw 'Allowed callback native epoch mismatch.'}
        if($trace.event -ne 'bmx_passive_fall_lifecycle' -or -not (Test-BmxUnsignedInteger $trace.process_id) -or $trace.process_id -ne $processId -or
           $trace.window_complete -isnot [bool] -or -not $trace.window_complete -or -not (Test-BmxUnsignedInteger $trace.reason_id) -or $trace.reason_id -ne 1 -or
           -not (Test-BmxFiniteNumber $trace.native_elapsed) -or $trace.native_elapsed -lt 29.99 -or $trace.native_elapsed -gt 30.25 -or
           $trace.counter_regression -isnot [bool] -or $trace.counter_regression -or
           -not (Test-BmxUnsignedInteger $trace.overwritten_samples -AllowZero) -or $trace.overwritten_samples -ne 0){throw 'Complete fresh 30-native-second lifecycle required.'}
        foreach($name in @('attempted','accepted','status_fresh','status_available','typed')){if($trace.request.$name -isnot [bool] -or -not $trace.request.$name){throw 'Lifecycle fall receipt unavailable.'}}
        foreach($name in @('id','generation','entity','binding')){if(-not (Test-BmxUnsignedInteger $trace.request.$name)){throw 'Lifecycle receipt field invalid.'}}
        if($trace.request.id -ne $impact.request_id -or $trace.request.generation -ne $impact.intent_generation -or
           $trace.request.entity -ne $action.identity.entity -or $trace.request.binding -ne 1){throw 'Lifecycle fall receipt mismatch.'}
        if(-not (Test-BmxUnsignedInteger $trace.initial.serial) -or -not (Test-BmxFiniteNumber $trace.initial.native_time) -or
           $trace.initial.serial -ne $impact.serial -or $action.requested_serial -le $trace.initial.serial -or
           [math]::Abs(($action.native_time-$trace.initial.native_time)-([uint64]$action.requested_serial-[uint64]$trace.initial.serial)/60.0) -gt .00001){throw 'Preflight is outside the lifecycle native epoch.'}
        Assert-BmxGetupSample $trace.initial $action.baseline
        $rows=@($trace.samples);if($rows.Count -lt 2 -or $rows.Count -gt 512){throw 'Lifecycle row count invalid.'}
        $priorSerial=[uint64]0;$priorTime=-1.0;$afterReset=0
        foreach($row in $rows){
            if(-not (Test-BmxUnsignedInteger $row.serial) -or -not (Test-BmxFiniteNumber $row.native_time) -or
               -not (Test-BmxFiniteNumber $row.step_seconds) -or [math]::Abs($row.step_seconds-1.0/60) -gt .000001 -or
               $row.serial -le $priorSerial -or $row.native_time -le $priorTime){throw 'Lifecycle clocks duplicated, reversed or unavailable.'}
            if($priorSerial -and [math]::Abs(($row.native_time-$priorTime)-([uint64]$row.serial-$priorSerial)/60.0) -gt .00001){throw 'Lifecycle serial/time epoch changed.'}
            if($row.serial -lt $trace.initial.serial -or $row.native_time -lt $trace.initial.native_time -or
               [math]::Abs(($row.native_time-$trace.initial.native_time)-([uint64]$row.serial-[uint64]$trace.initial.serial)/60.0) -gt .00001){throw 'Lifecycle row differs from its initial native epoch.'}
            $sampleGap=if($priorSerial){$row.native_time-$priorTime}else{0.0}
            $priorSerial=[uint64]$row.serial;$priorTime=[double]$row.native_time;$lastTime=$priorTime
            Assert-BmxGetupSample $row $action.baseline
            if($row.serial -lt $action.requested_serial){continue}
            ++$postSamples
            if($row.serial -le $pulse.reset_serial){continue}
            ++$afterReset
            if($sampleGap -gt .25){$suffixStart=$null}
            if($row.rider.standable -and $row.rider.offboard){if($null -eq $suffixStart){$suffixStart=$row.native_time}}
            else{$suffixStart=$null}
        }
        if($lastTime-$trace.initial.native_time -lt 29.9 -or $postSamples -lt 2 -or $afterReset -lt 2){throw 'Insufficient lifecycle observation after the action/reset.'}
        $pulsePassed=$true
        $stood=$null -ne $suffixStart -and $lastTime-$suffixStart -ge 2.0-0.000001
        if($action.pulse_passed -ne $pulsePassed -or $action.standability_observed -ne $stood){throw 'Runtime result disagrees with independently assessed pulse/standing evidence.'}
        $pidPattern='\['+[regex]::Escape([string]$processId)+':\d+\]';$requestLines=@();$statusRows=@();$lineNumber=0
        foreach($line in ($text -split '\r?\n')){
            ++$lineNumber;if($line -notmatch $pidPattern){continue};$at=[DateTime]::MinValue
            if($line.Length -lt 24 -or -not [DateTime]::TryParseExact($line.Substring(1,23),'yyyy-MM-dd HH:mm:ss.fff',[Globalization.CultureInfo]::InvariantCulture,[Globalization.DateTimeStyles]::None,[ref]$at) -or $at -lt $earliestAt){continue}
            if($line -match 'BMX GetUp action requested: request=(\d+); fall request=(\d+); fall generation=(\d+); serial=(\d+);'){
                if([uint64]$Matches[1] -ne $action.request_id -or [uint64]$Matches[2] -ne $action.fall_request_id -or [uint64]$Matches[3] -ne $action.fall_generation -or [uint64]$Matches[4] -ne $action.requested_serial){throw 'Explicit action log receipt mismatch.'}
                $requestLines+=$lineNumber
            }
            if($requestLines.Count -and $lineNumber -gt $requestLines[0]){
                if($line -match 'BMX removed\. Switching|native mount requested|Skateboard equipped:|Skateboard switch timed out|BMX natural mount begin:|BMX crash recovery verified:'){throw 'Another recovery, mount or equipment action followed the GetUp pulse.'}
                if($line -match 'native bike parts: (\d+)/8;.*chassis mounts=(\d+); allocations=(\d+); active=(true|false); parked=(true|false); force applications=(\d+); body world=(\d+);'){
                    if([uint64]$Matches[1] -ne 8 -or [uint64]$Matches[2] -ne 1 -or [uint64]$Matches[3] -ne 1 -or $Matches[4] -ne 'false' -or $Matches[5] -ne 'false' -or [uint64]$Matches[6] -ne $action.baseline.force_applications -or [uint64]$Matches[7] -ne $action.identity.world){throw 'Post-action status changed the retained pool/drive.'}
                    foreach($name in @('velocity_updates','rider_pose_updates','rider_render_updates','body_token')){
                        $pattern=switch($name){'velocity_updates'{'Native velocity updates: (\d+);'}'rider_pose_updates'{'rider pose updates=(\d+);'}'rider_render_updates'{'rider render updates=(\d+),'}'body_token'{'body token=(\d+)[.;]'}}
                        $found=[regex]::Matches($line,$pattern);$expected=if($name -eq 'body_token'){$action.identity.body_token}else{$action.baseline.$name}
                        if($found.Count -ne 1 -or [uint64]$found[0].Groups[1].Value -ne $expected){throw 'Post-action status counter or token unavailable/changed.'}
                    }
                    if($line -notmatch 'BMX capture: released;' -or $line -notmatch 'bike mode=crashed; rider mode=none;'){throw 'Post-action status did not retain the unmounted crashed bike.'}
                    $statusRows+=$at
                }
            }
        }
        if($requestLines.Count -ne 1 -or $statusRows.Count -lt 2 -or ($statusRows[-1]-$statusRows[0]).TotalSeconds -lt 2){throw 'One explicit action and two separated quiet statuses required.'}
        return [pscustomobject]@{complete=($pulsePassed -and $stood);pulse_passed=$pulsePassed;standability_observed=$stood;issue=if($stood){'observed'}else{'Verified typed pulse; native standability was not sustained through final trace.'}
            request_id=$action.request_id;generation=$pulse.generation;true_serial=$pulse.true_serial;reset_serial=$pulse.reset_serial;post_action_samples=$postSamples
            standing_suffix_seconds=if($null -ne $suffixStart){$lastTime-$suffixStart}else{0};trace_native_seconds=$trace.native_elapsed;post_status_samples=$statusRows.Count
            boundary='Private typed GetUp action and sampled native standing only; no production input mapping, full script guard equivalence, pickup, remount or visual acceptance.'}
    }catch{return [pscustomobject]@{complete=$false;pulse_passed=$pulsePassed;standability_observed=$stood;issue=$_.Exception.Message;post_action_samples=$postSamples}}
}
# Structured parsing accepts additive diagnostics while rejecting duplicate or
# malformed native fields. Historical rows have no health quartet; if any new
# health field is present, the complete quartet must be explicitly false.
function Get-BmxLifecycleStatusValues([string]$Line) {
$fields=[ordered]@{
            velocity='Native velocity updates: (\d+)(?=;)';parts='native bike parts: (\d+)/8(?=;)';placements='placements: (\d+)(?=;)'
            state='native physics state: (\d+|unavailable)(?=;)';capture='BMX capture: (held|released)(?=[.;])'
            mounts='chassis mounts=(\d+)(?=;)';allocations='allocations=(\d+)(?=;)';active='(?:^|; )active=(true|false)(?=;)'
            parked='(?:^|; )parked=(true|false)(?=;)';force='force applications=(\d+)(?=[.;])';world='body world=(\d+)(?=[.;])'
            poses='rider pose updates=(\d+)(?=[.;])';token='body token=(\d+)(?=[.;])'
            mode='bike mode=(mounted|mounting|crashed|stepping_off|parked|carrying|none)(?=[.;])';rider='rider mode=(riding|carrying|none)(?=[.;])'
            render='rider render updates=(\d+)(?=[,;])'
        }
    $row=[ordered]@{}
    foreach($key in $fields.Keys){
        $matchesFound=[regex]::Matches($Line,$fields[$key])
        if($matchesFound.Count -ne 1){throw ('Native status field missing/duplicated: '+$key)}
        $raw=$matchesFound[0].Groups[1].Value
        if($key -in @('state','capture','active','parked','mode','rider')){$row[$key]=$raw}
        else{$row[$key]=[uint64]::Parse($raw)}
    }
    $health=[ordered]@{};$present=0
    foreach($name in @('failed','blocked','observation suspended','deferred park')){
        $found=[regex]::Matches($Line,'(?:^|; )'+[regex]::Escape($name)+'=([^;]*)')
        if($found.Count -gt 1){throw ('Native health field duplicated: '+$name)}
        if($found.Count -eq 1){
            ++$present
            if($found[0].Groups[1].Value -cne 'false'){throw ('Native health flag must be explicitly false: '+$name)}
        }
        $health[$name]=($found.Count -eq 1)
    }
    if($present -ne 0 -and $present -ne 4){throw 'Native status health quartet is incomplete.'}
    $row['health_diagnostics_present']=($present -eq 4)
    [pscustomobject]$row
}
function ConvertTo-BmxPassiveStatusLine([string]$Line) {
    $r=Get-BmxLifecycleStatusValues $Line
    $prefix=$Line.Substring(0,$Line.IndexOf('Native velocity updates:'))
    # This is an in-memory adapter for the unchanged passive GetUp assessor.
    # All original values were uniquely parsed and health checked above; only
    # the field layout is canonicalized. Original log bytes stay archived.
    $prefix+'Native velocity updates: '+$r.velocity+'; native bike parts: '+$r.parts+'/8; placements: '+$r.placements+
        '; native physics state: '+$r.state+'; BMX capture: '+$r.capture+'; chassis mounts='+$r.mounts+
        '; allocations='+$r.allocations+'; active='+$r.active+'; parked='+$r.parked+
        '; force applications='+$r.force+'; body world='+$r.world+'; rider pose updates='+$r.poses+
        '; bike mode='+$r.mode+'; rider mode='+$r.rider+'; body token='+$r.token+
        '. pose listener=observed, rider render updates='+$r.render+','
}
# This assessor intentionally feeds only the passive pre-interaction prefix to
# the unchanged GetUp policy. It never weakens that lab's prohibition on remount.
function Get-BmxPostFallRemountProgress([string]$text,[int]$processId,[int]$variantId,$impact,$action,$trace,[DateTime]$earliestAt) {
    $pre=$null;$standing=$false;$pulse=$false
    try {
        if($variantId -ne 2){throw 'Post-fall remount requires the side-impact scenario.'}
        $pidPattern='\['+[regex]::Escape([string]$processId)+':\d+\]'
        $lines=@($text -split '\r?\n');$owned=@();$n=0
        foreach($line in $lines){
            $n++;if($line -notmatch $pidPattern){continue};$at=[DateTime]::MinValue
            if($line.Length -lt 24 -or -not [DateTime]::TryParseExact($line.Substring(1,23),'yyyy-MM-dd HH:mm:ss.fff',[Globalization.CultureInfo]::InvariantCulture,[Globalization.DateTimeStyles]::None,[ref]$at) -or $at -lt $earliestAt){continue}
            $owned+=[pscustomobject]@{line=$n;at=$at;text=$line}
        }
        $recoveries=@($owned|Where-Object{$_.text.Contains('BMX crash recovery verified:')})
        $cut=if($recoveries.Count){$recoveries[0].line-1}else{$lines.Count}
        $normalized=@{}
        foreach($entry in $owned){
            if($entry.line -le $cut -and $entry.text.Contains('Native velocity updates:')){
                $normalized[$entry.line]=ConvertTo-BmxPassiveStatusLine $entry.text
            }
        }
        $prefix=if($cut -gt 0){
            @(for($i=0;$i -lt $cut;++$i){if($normalized.ContainsKey($i+1)){$normalized[$i+1]}else{$lines[$i]}}) -join "`n"
        }else{''}
        $pre=Get-BmxGetupProgress $prefix $processId $variantId $impact $action $trace $earliestAt
        $standing=[bool]$pre.standability_observed;$pulse=[bool]$pre.pulse_passed
        if(-not $pre.complete){throw ('Passive GetUp evidence failed: '+$pre.issue)}
        if($recoveries.Count -ne 1){
            $refusal=@($owned|Where-Object{$_.text.Contains('BMX crash recovery waiting:')})
            throw $(if($refusal.Count){'Recovery was refused: '+$refusal[-1].text}else{'Exactly one post-fall recovery receipt required.'})
        }
        $recovery=$recoveries[0]
        $receipt=[regex]::Match($recovery.text,'BMX crash recovery verified: body token=(\d+); serial=(\d+); native elapsed=([0-9]+(?:\.[0-9]+)?)s; grounded placement accepted\.')
        if(-not $receipt.Success){throw 'Malformed recovery receipt.'}
        $token=[uint64]::Parse($receipt.Groups[1].Value);$serial=[uint64]::Parse($receipt.Groups[2].Value)
        $nativeElapsed=[double]::Parse($receipt.Groups[3].Value,[Globalization.CultureInfo]::InvariantCulture)
        if($token -ne $action.identity.body_token -or $serial -le [uint64]$trace.samples[-1].serial -or
           $nativeElapsed -lt 30 -or [Math]::Abs($nativeElapsed-($serial-[uint64]$impact.serial)/60.0) -gt .02){throw 'Recovery precedes the completed passive native window or changed its body/epoch.'}
        $labComplete=@($owned|Where-Object{$_.line -lt $recovery.line -and $_.text.Contains('BMX GetUp lab complete:')})
        if($labComplete.Count -ne 1 -or $labComplete[0].text -notmatch 'BMX GetUp lab complete: pulse=true;'){throw 'Completed private GetUp report must flush before explicit recovery.'}
        $quiet=@($owned|Where-Object{$_.line -gt $labComplete[0].line -and $_.at -ge $labComplete[0].at -and $_.line -lt $recovery.line -and $_.text.Contains('Native velocity updates:') -and $_.text -match 'BMX capture: released;.*bike mode=crashed; rider mode=none;'})
        if($quiet.Count -lt 2 -or ($quiet[-1].at-$quiet[0].at).TotalSeconds -lt 2){throw 'Two quiet crashed-bike statuses after passive report flush and before recovery are required.'}
        if($token -eq [uint64]::MaxValue){throw 'Cannot verify token increment at integer limit.'}
        $begins=@($owned|Where-Object{$_.line -gt $recovery.line -and $_.text.Contains('BMX natural mount begin:')})
        if($begins.Count -ne 1 -or $begins[0].text -notmatch 'BMX natural mount begin: token=(\d+);' -or
           [uint64]$Matches[1] -ne $token+1){throw 'One fresh native mount lease after recovery is required.'}
        if(@($owned|Where-Object{$_.line -gt $recovery.line -and $_.text -match 'BMX dismounted\.|BMX removed\. Switching|Skateboard equipped:|Skateboard switch timed out|BMX GetUp action requested:|BMX crash recovery waiting:|BMX private GetUp report unavailable|BMX crash recovery verified:'}).Count){throw 'A conflicting lifecycle action followed recovery.'}
        $rows=@()
        foreach($entry in $owned){
            if($entry.line -le $begins[0].line -or -not $entry.text.Contains('Native velocity updates:')){continue}
            $row=[ordered]@{line=$entry.line;at=$entry.at}
            $values=Get-BmxLifecycleStatusValues $entry.text
            foreach($property in $values.PSObject.Properties){$row[$property.Name]=$property.Value}
            if($row.parts -ne 8 -or $row.state -ne '504' -or $row.capture -ne 'held' -or $row.mounts -ne 2 -or
               $row.allocations -ne 1 -or $row.active -ne 'true' -or $row.parked -ne 'false' -or
               $row.world -ne $action.identity.world -or $row.token -ne $token+1 -or $row.mode -ne 'mounted' -or $row.rider -ne 'riding'){
                throw 'Post-recovery native status did not establish the same owned pool and a fresh mounted lease.'
            }
            foreach($pair in @(@('velocity','velocity_updates'),@('force','force_applications'),@('poses','rider_pose_updates'),@('render','rider_render_updates'))){
                if($row[$pair[0]] -le [uint64]$action.baseline.($pair[1])){throw ('Native counter did not advance after remount: '+$pair[0])}
            }
            $rows+=[pscustomobject]$row
        }
        if($rows.Count -lt 2){throw 'Two post-remount native statuses required.'}
        $first=$rows[0];$last=$rows[-1]
        if(($last.at-$first.at).TotalSeconds -lt 1){throw 'Post-remount observations were not separated by one wall second.'}
        for($i=1;$i -lt $rows.Count;++$i){
            if($rows[$i].at -le $rows[$i-1].at){throw 'Post-remount status clocks did not advance.'}
            foreach($name in @('velocity','force','poses','render','placements')){if($rows[$i].$name -lt $rows[$i-1].$name){throw 'Native counters regressed after remount.'}}
        }
        foreach($name in @('velocity','force','poses','render')){if($last.$name -le $first.$name){throw ('Native counter stalled after remount: '+$name)}}
        return [pscustomobject]@{complete=$true;passed=$true;remount_passed=$true;pulse_passed=$pulse;standability_observed=$standing
            issue='Accepted native fall, typed GetUp/reset, sustained native standing, completed passive report, recovery and fresh mounted lease with advancing native counters.'
            getup_proof=$pre;recovery_serial=$serial;recovery_native_elapsed=$nativeElapsed;old_body_token=$token;mounted_body_token=($token+1)
            mounted_world=$first.world;mounts=$last.mounts;allocations=$last.allocations;post_mount_status_count=$rows.Count;first_mounted=$first;last_mounted=$last
            boundary='Private console lifecycle and synthetic controls only. Physical controller delivery, full GetUp script guard equivalence and visual quality are not established.'}
    }catch{
        return [pscustomobject]@{complete=$false;passed=$false;remount_passed=$false;pulse_passed=$pulse;standability_observed=$standing
            issue=$_.Exception.Message;getup_proof=$pre}
    }
}
# Data-only supervisor/capture handoff. This receipt is deliberately provisional:
# final run acceptance still requires archived evidence and verified teardown.
function Get-BmxPostFallMonitorOutcome([string]$Outcome,[bool]$Complete,[DateTime]$Now,[DateTime]$Deadline) {
    $waiting=$Outcome -eq 'launching' -or $Outcome -eq 'awaiting_getup_action_and_full_lifecycle' -or
        $Outcome.StartsWith('awaiting_postfall_remount: ',[StringComparison]::Ordinal)
    if(-not $Complete -and $waiting -and $Now -ge $Deadline){return ('supervisor_wall_timeout: '+$Outcome)}
    return $Outcome
}
function New-BmxNativeTerminalReceipt {
    param([uint32]$OwnedPid,[Nullable[DateTime]]$CreatedUtc,[DateTime]$TerminalUtc,
          [bool]$Passed,[string]$Outcome,[string]$RuntimeSha256,[string]$Issue='')
    if(($OwnedPid -gt 0) -ne ($null -ne $CreatedUtc)){throw 'Terminal receipt requires the complete owned PID/creation pair or neither.'}
    if($Passed -and -not $OwnedPid){throw 'A passing native terminal receipt requires verified owned process identity.'}
    if($RuntimeSha256 -cnotmatch '^[0-9A-Fa-f]{64}$'){throw 'Terminal receipt requires the archived runtime SHA256.'}
    if(-not $Outcome){throw 'Terminal receipt requires the exact supervisor outcome.'}
    if($CreatedUtc -and ([DateTime]$CreatedUtc).ToUniversalTime() -gt $TerminalUtc.ToUniversalTime()){throw 'Terminal receipt predates its owned process creation.'}
    [pscustomobject][ordered]@{
        schema_version=1;event='bmx_native_test_terminal';terminal=$true;scenario='post_fall_remount'
        owned_pid=$OwnedPid;owned_created_utc=if($CreatedUtc){([DateTime]$CreatedUtc).ToUniversalTime().ToString('o')}else{$null}
        process_identity_verified=($OwnedPid -gt 0 -and $null -ne $CreatedUtc)
        terminal_utc=$TerminalUtc.ToUniversalTime().ToString('o');passed=$Passed
        phase=if($Passed){'native_evidence_complete'}else{'native_evidence_failed'}
        outcome=$Outcome;issue=$Issue;runtime_sha256=$RuntimeSha256.ToUpperInvariant()
        acceptance='Native evidence only; final archived evidence, source stability and process teardown remain pending.'
    }
}
function Write-BmxNativeTerminalReceipt {
    param([string]$RunDirectory,$Receipt)
    if(-not (Test-Path -LiteralPath $RunDirectory -PathType Container)){throw 'Terminal receipt requires an existing fresh run directory.'}
    $directory=[IO.Path]::GetFullPath($RunDirectory)
    if(((Get-Item -LiteralPath $directory).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0){throw 'Terminal receipt directory cannot be a reparse point.'}
    $target=Join-Path $directory 'native-terminal.json'
    if(Test-Path -LiteralPath $target){throw 'Terminal receipt already exists; preserved without replacement.'}
    $temporary=Join-Path $directory ('native-terminal.'+[guid]::NewGuid().ToString('N')+'.tmp')
    $stream=$null
    try {
        $bytes=[Text.UTF8Encoding]::new($false).GetBytes(($Receipt|ConvertTo-Json -Depth 8))
        $stream=[IO.FileStream]::new($temporary,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
        $stream.Write($bytes,0,$bytes.Length);$stream.Flush($true);$stream.Dispose();$stream=$null
        # Same-directory rename publishes one complete receipt. No partial JSON
        # is visible at the final path and an existing receipt is never replaced.
        [IO.File]::Move($temporary,$target)
        [pscustomobject]@{path=$target;sha256=(Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash}
    } finally {
        if($stream){$stream.Dispose()}
        if(Test-Path -LiteralPath $temporary -PathType Leaf){[IO.File]::Delete($temporary)}
    }
}
# Negative terminal proof for the completed private interaction sequence. Missing
# evidence, pending mounting or a moving/advancing lifecycle remains nonterminal.
function Get-BmxPostFallRefusalTerminal([string]$Text,[int]$ProcessId,[int]$VariantId,$Impact,$Action,$Trace,[DateTime]$EarliestAt) {
    try {
        if($VariantId -ne 2){throw 'Negative terminal policy requires the private side-impact trial.'}
        $pidPattern='\['+[regex]::Escape([string]$ProcessId)+':\d+\]';$owned=@();$all=@($Text -split '\r?\n');$n=0
        foreach($line in $all){
            ++$n;if($line -notmatch $pidPattern){continue};$at=[DateTime]::MinValue
            if($line.Length -lt 24 -or -not [DateTime]::TryParseExact($line.Substring(1,23),'yyyy-MM-dd HH:mm:ss.fff',[Globalization.CultureInfo]::InvariantCulture,[Globalization.DateTimeStyles]::None,[ref]$at) -or $at -lt $EarliestAt){continue}
            $owned+=[pscustomobject]@{line=$n;at=$at;text=$line}
        }
        $commands=@($owned|Where-Object{$_.text -match 'Startup command queued: bmx interact\s*$' -and -not $_.text.Contains('Repeated ')})
        $refusals=@($owned|Where-Object{$_.text.Contains('BMX crash recovery waiting:')})
        if($commands.Count -ne 1 -or $refusals.Count -ne 1){throw 'One fresh explicit interaction and one refusal are required for negative terminal proof.'}
        $command=$commands[0];$refusal=$refusals[0]
        if($command.line -lt 2){throw 'Explicit interaction lacks any prior passive evidence.'}
        if($refusal.line -le $command.line -or ($refusal.at-$command.at).TotalSeconds -lt 0 -or ($refusal.at-$command.at).TotalSeconds -gt 2){throw 'Refusal is not freshly correlated with the explicit interaction.'}
        if(@($owned|Where-Object{$_.line -gt $command.line -and $_.text -match 'BMX crash recovery verified:|BMX natural mount (begin|complete):|Skateboard equipped:|BMX removed\. Switching|BMX dismounted\.|Startup command queued: bmx (mount|toggle|carry|dismount|interact|selftest|ramp|getup-test)\b'}).Count){throw 'A later lifecycle action prevents negative terminal classification.'}
        $speedNumber='([0-9]+(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?)'
        $pattern='BMX crash recovery waiting: reason=(\d+); distance='+$speedNumber+'m; height='+$speedNumber+'m; speed='+$speedNumber+'m/s; native elapsed='+$speedNumber+'s; standable=(true|false); offboard=(true|false)\.\s*$'
        $receipt=[regex]::Match($refusal.text,$pattern)
        if(-not $receipt.Success -or [regex]::Matches($refusal.text,'BMX crash recovery waiting:').Count -ne 1){throw 'Malformed or duplicated native recovery refusal receipt.'}
        $reason=[int]::Parse($receipt.Groups[1].Value);$numbers=@()
        foreach($index in 2..5){
            $number=[double]::Parse($receipt.Groups[$index].Value,[Globalization.CultureInfo]::InvariantCulture)
            if(-not (Test-BmxFiniteNumber $number)){throw 'Nonfinite or unbounded refusal measurement.'};$numbers+=$number
        }
        $distance=$numbers[0];$height=$numbers[1];$speed=$numbers[2];$elapsed=$numbers[3]
        # Focus this early-stop policy on the stable released-pool refusals.
        # Unknown owners, incomplete GetUp, settling and down states stay pending.
        if($reason -notin @(10,11,12) -or $receipt.Groups[6].Value -cne 'true' -or $receipt.Groups[7].Value -cne 'true' -or
            $elapsed -lt 30 -or $elapsed -gt 600){throw 'Refusal is not a stable post-GetUp distance/speed refusal.'}
        if(($reason -eq 10 -and $speed -le 1) -or
           ($reason -eq 11 -and ($speed -gt 1 -or $distance -le 1.8)) -or
           ($reason -eq 12 -and ($speed -gt 1 -or $distance -gt 1.8 -or $height -le 1.5))){throw 'Refusal reason disagrees with native guard measurements.'}
        $later=@($owned|Where-Object{$_.line -gt $refusal.line -and $_.text.Contains('Native velocity updates:')})
        if($later.Count -lt 2 -or ($later[-1].at-$later[0].at).TotalSeconds -lt 1){throw 'Two later separated failed statuses have not completed.'}
        $post=@();$previous=$null
        foreach($entry in $later){
            if($entry.at -le $refusal.at){throw 'Post-refusal status timestamp does not follow the explicit refusal.'}
            $row=Get-BmxLifecycleStatusValues $entry.text
            if($row.parts -ne 8 -or $row.state -cne '504' -or $row.capture -cne 'released' -or $row.mounts -ne 1 -or $row.allocations -ne 1 -or
               $row.active -cne 'false' -or $row.parked -cne 'false' -or $row.mode -cne 'crashed' -or $row.rider -cne 'none' -or
               $row.world -ne $Action.identity.world -or $row.token -ne $Action.identity.body_token){throw 'Post-refusal lifecycle is not the same quiet released crashed bike.'}
            foreach($pair in @(@('velocity','velocity_updates'),@('force','force_applications'),@('poses','rider_pose_updates'),@('render','rider_render_updates'))){
                if($row.($pair[0]) -ne $Action.baseline.($pair[1])){throw 'Native drive/follow/pose counters advanced after refusal; not a quiet failed terminal.'}
            }
            if($previous -and ($entry.at -le $previous.at -or $row.placements -lt $previous.placements)){throw 'Post-refusal status clocks or placement counters regressed.'}
            $post+=[pscustomobject]@{line=$entry.line;at=$entry.at;placements=$row.placements;token=$row.token;world=$row.world;mounts=$row.mounts;allocations=$row.allocations;velocity=$row.velocity;force=$row.force;poses=$row.poses;render=$row.render}
            $previous=$post[-1]
        }
        $before=@($all[0..($command.line-2)]);$normalized=@{}
        foreach($entry in $owned){if($entry.line -lt $command.line -and $entry.text.Contains('Native velocity updates:')){$normalized[$entry.line]=ConvertTo-BmxPassiveStatusLine $entry.text}}
        $prefix=@(for($i=0;$i -lt $before.Count;++$i){if($normalized.ContainsKey($i+1)){$normalized[$i+1]}else{$before[$i]}}) -join "`n"
        $pre=Get-BmxGetupProgress $prefix $ProcessId $VariantId $Impact $Action $Trace $EarliestAt
        if(-not $pre.complete){throw ('Incomplete pre-interaction native GetUp/quiet proof: '+$pre.issue)}
        if($elapsed -lt ([double]$Trace.samples[-1].native_time-[double]$Trace.initial.native_time)){throw 'Refusal native elapsed precedes the completed passive observation window.'}
        $flush=@($owned|Where-Object{$_.line -lt $command.line -and $_.text -match 'BMX GetUp lab complete: pulse=true;'})
        if($flush.Count -ne 1){throw 'One completed passive lab flush before explicit interaction is required.'}
        $quiet=@($owned|Where-Object{$_.line -gt $flush[0].line -and $_.at -ge $flush[0].at -and $_.line -lt $command.line -and $_.text.Contains('Native velocity updates:')})
        if($quiet.Count -lt 2 -or ($quiet[-1].at-$quiet[0].at).TotalSeconds -lt 2){throw 'The post-flush quiet phase did not complete before interaction.'}
        if($command.at -lt $quiet[-1].at -or $quiet[-1].at -lt $quiet[0].at){throw 'Interaction timestamp precedes completion of the quiet phase.'}
        return [pscustomobject]@{terminal_failure=$true;passed=$false;phase='postfall_recovery_refused';issue='Completed explicit recovery was refused and two later statuses retain the same quiet crashed bike.'
            reason=$reason;horizontal_distance=$distance;vertical_distance=$height;speed=$speed;native_elapsed=$elapsed;interaction_at=$command.at;refusal_at=$refusal.at
            post_status_count=$post.Count;first_failed=$post[0];last_failed=$post[-1];getup_proof=$pre
            boundary='Negative private-trial terminal only; never positive recovery/mount acceptance and never a changed native guard.'}
    }catch{return [pscustomobject]@{terminal_failure=$false;passed=$false;issue=$_.Exception.Message}}
}
