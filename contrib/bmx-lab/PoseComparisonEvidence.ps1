# Data-only private comparison configuration and owned log evidence. No launch,
# input, process access, native calls or expected physics outcome.
function Assert-BmxPoseComparisonConfiguration {
    param([string]$Mode,[bool]$Phase,[bool]$Producer,[bool]$PostFall,[bool]$GetUp,
          [string]$Impact,[bool]$NativeChassis,[bool]$Unattended,[string]$Profile)
    if(@('Default','RenderOnly','None') -cnotcontains $Mode){throw 'Unknown or noncanonical private rider pose comparison mode.'}
    if($Mode -cne 'Default' -and (-not $Phase -or -not $Producer -or -not $PostFall -or -not $GetUp -or
       $Impact -cne 'side' -or -not $NativeChassis -or -not $Unattended -or $Profile -cne 'bmx-user-data-chassis-probe')){
        throw 'Nondefault rider pose comparison requires explicit FirstMountProducerProbe, PhaseProbe and the exact private side/GetUp PostFallRemount unattended native-chassis profile.'
    }
}
function Get-BmxPoseComparisonEvidence {
    param([string]$Text,[string]$Mode,[bool]$ExpectedScope,[int]$ProcessId,[DateTime]$CreatedUtc,
          [DateTime]$StartedUtc,[DateTime]$EndedUtc)
    if(@('Default','RenderOnly','None') -cnotcontains $Mode -or $ProcessId -le 0 -or
       $EndedUtc.ToUniversalTime() -lt $CreatedUtc.ToUniversalTime() -or
       $CreatedUtc.ToUniversalTime() -lt $StartedUtc.ToUniversalTime() -or
       ($Mode -cne 'Default' -and -not $ExpectedScope)){throw 'Comparison evidence request identity/scope is invalid.'}
    $rows=@();$previous=$null
    foreach($line in ($Text -split "`r?`n")){
        if($line -notmatch '^\[(?<at>[^\]]+)\] \[(?<process>[0-9]+):[0-9]+\] '){continue}
        if([uint64]$Matches.process -ne $ProcessId -or -not $line.Contains('private rider comparison=')){continue}
        if([regex]::Matches($line,'private rider comparison=').Count -ne 1){throw 'Ambiguous duplicate comparison mode status.'}
        $at=[DateTime]::MinValue
        if(-not [DateTime]::TryParse($Matches.at,[Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::AssumeLocal,[ref]$at) -or
           $at.ToUniversalTime() -lt $CreatedUtc.ToUniversalTime().AddMilliseconds(-1) -or
           $at.ToUniversalTime() -lt $StartedUtc.ToUniversalTime() -or
           $at.ToUniversalTime() -gt $EndedUtc.ToUniversalTime()){throw 'Comparison status is outside the owned run interval.'}
        if($line -cnotmatch 'rider pose updates=(?<poses>[0-9]+);.*rider render updates=(?<renders>[0-9]+),.*private rider comparison=(?<mode>Default|RenderOnly|None), comparison scope=(?<scope>true|false), comparison refused=(?<refused>true|false), suppressed animation applies=(?<animation>[0-9]+), suppressed render applies=(?<render>[0-9]+), animation apply dispatches=(?<animation_dispatches>[0-9]+), render apply dispatches=(?<render_dispatches>[0-9]+)(?:,|$)'){
            throw 'Owned comparison status lacks exact mode/counters.'
        }
        $row=[ordered]@{at_utc=$at.ToUniversalTime().ToString('o');mode=$Matches.mode;qualified_scope=($Matches.scope -ceq 'true');refused=($Matches.refused -ceq 'true')}
        foreach($field in @('poses','renders','animation','render','animation_dispatches','render_dispatches')){
            $number=[uint64]0
            if(-not [uint64]::TryParse($Matches[$field],[ref]$number)){throw 'Comparison counter overflow.'}
            $row[$field]=$number
        }
        if($row.mode -cne $Mode -or $row.qualified_scope -ne $ExpectedScope -or $row.refused){throw 'Observed comparison mode/scope/refusal differs from the owned request.'}
        if($Mode -ceq 'Default' -and ($row.animation -ne 0 -or $row.render -ne 0)){throw 'Default comparison suppressed a callback.'}
        if($Mode -ceq 'RenderOnly' -and ($row.render -ne 0 -or $row.animation_dispatches -ne 0)){throw 'RenderOnly observed a suppressed render or a dispatched animation apply.'}
        if($Mode -ceq 'None' -and ($row.poses -ne 0 -or $row.renders -ne 0 -or $row.animation_dispatches -ne 0 -or $row.render_dispatches -ne 0)){throw 'None observed a dispatched/completed custom rider application.'}
        if($previous){
            if([DateTime]::Parse($row.at_utc) -lt [DateTime]::Parse($previous.at_utc)){throw 'Comparison timestamps regressed.'}
            foreach($field in @('poses','renders','animation','render','animation_dispatches','render_dispatches')){if($row[$field] -lt $previous[$field]){throw 'Comparison counters regressed.'}}
        }
        $rows+=[pscustomobject]$row;$previous=$row
    }
    if($rows.Count -eq 0){throw 'No owned comparison mode status was observed.'}
    if($Mode -cne 'Default' -and $rows[-1].animation -eq 0){throw 'Requested animation callback suppression was not observed.'}
    if($Mode -ceq 'None' -and $rows[-1].render -eq 0){throw 'Requested render callback suppression was not observed.'}
    [pscustomobject]@{mode=$Mode;qualified_scope=$ExpectedScope;process_id=$ProcessId;owned_created_utc=$CreatedUtc.ToUniversalTime().ToString('o');rows=$rows
        custom_rider_application_counters_zero=($rows[-1].poses -eq 0 -and $rows[-1].renders -eq 0)
        boundary='Owned PID/status wall interval with separately supervisor-observed creation identity. Stage counters count callback routing, not planned or completed native writes. Individual counters are sampled separately, not an atomic cross-counter snapshot. No physics, native completion, visual quality or no-later-buffer-reuse proof.'}
}


function Assert-BmxMotionBasisConfiguration {
    param([string]$Mode,[string]$RiderMode,[bool]$Phase,[bool]$Producer,[bool]$PostFall,[bool]$GetUp,
          [string]$Impact,[bool]$NativeChassis,[bool]$Unattended,[string]$Profile)
    if(@('Default','PreserveSuppliedBasis') -cnotcontains $Mode){throw 'Unknown private motion-basis selector.'}
    if($Mode -cne 'Default' -and ($RiderMode -cne 'None' -or -not $Phase -or -not $Producer -or -not $PostFall -or -not $GetUp -or
       $Impact -cne 'side' -or -not $NativeChassis -or -not $Unattended -or $Profile -cne 'bmx-user-data-chassis-probe')){
        throw 'Motion basis contrast requires None plus explicit Phase/Producer and the exact private side/GetUp PostFall unattended native-chassis profile.'
    }
}
function Get-BmxMotionBasisEvidence {
    param([string]$Text,[string]$Mode,[int]$ProcessId,[DateTime]$CreatedUtc,[DateTime]$StartedUtc,[DateTime]$EndedUtc)
    if(@('Default','PreserveSuppliedBasis') -cnotcontains $Mode -or $ProcessId -le 0 -or
       $EndedUtc.ToUniversalTime() -lt $CreatedUtc.ToUniversalTime() -or $CreatedUtc.ToUniversalTime() -lt $StartedUtc.ToUniversalTime()){throw 'Basis evidence request identity/interval invalid.'}
    $rows=@();$previous=$null;$expectedScope=$Mode -cne 'Default'
    foreach($line in ($Text -split "`r?`n")){
        if($line -notmatch '^\[(?<at>[^\]]+)\] \[(?<process>[0-9]+):[0-9]+\] '){continue}
        if([uint64]$Matches.process -ne $ProcessId -or -not $line.Contains('motion basis=')){continue}
        if([regex]::Matches($line,'motion basis=').Count -ne 1){throw 'Ambiguous duplicate motion basis status.'}
        $at=[DateTime]::MinValue
        if(-not [DateTime]::TryParse($Matches.at,[Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::AssumeLocal,[ref]$at) -or $at.ToUniversalTime() -lt $CreatedUtc.ToUniversalTime().AddMilliseconds(-1) -or
            $at.ToUniversalTime() -lt $StartedUtc.ToUniversalTime() -or $at.ToUniversalTime() -gt $EndedUtc.ToUniversalTime()){throw 'Basis status is outside owned run.'}
        if($line -cnotmatch '; motion basis=(?<mode>Default|PreserveSuppliedBasis), basis scope=(?<scope>true|false), basis refused=(?<refused>true|false), selected full rotation targets=(?<selected>[0-9]+), preserved supplied bases=(?<preserved>[0-9]+)(?:,|$)'){throw 'Basis status is missing exact selector/counters.'}
        $row=[ordered]@{at_utc=$at.ToUniversalTime().ToString('o');mode=$Matches.mode;qualified_scope=($Matches.scope -ceq 'true');refused=($Matches.refused -ceq 'true')}
        foreach($field in @('selected','preserved')){$number=[uint64]0;if(-not [uint64]::TryParse($Matches[$field],[ref]$number)){throw 'Basis selection counter overflow.'};$row[$field]=$number}
        if($row.mode -cne $Mode -or $row.qualified_scope -ne $expectedScope -or $row.refused){throw 'Basis selector/scope/refusal differs from request.'}
        if($Mode -ceq 'Default' -and $row.preserved -ne 0){throw 'Default selected a preserved supplied basis.'}
        if($previous){
            if([DateTime]::Parse($row.at_utc) -lt [DateTime]::Parse($previous.at_utc)){throw 'Basis observation time regressed.'}
            foreach($field in @('selected','preserved')){if($row[$field] -lt $previous[$field]){throw 'Basis selection counter regressed.'}}
        };$rows+=[pscustomobject]$row;$previous=$row
    }
    if($rows.Count -eq 0){throw 'No owned basis-selection status observed.'}
    if($Mode -cne 'Default' -and ($rows[-1].preserved -eq 0 -or $rows[-1].selected -eq 0)){throw 'Requested preserved-basis selection was not observed.'}
    return [pscustomobject]@{mode=$Mode;qualified_scope=$expectedScope;process_id=$ProcessId;owned_created_utc=$CreatedUtc.ToUniversalTime().ToString('o');rows=$rows;
        boundary='Successful SDK target selections, sampled as independent counters. Original dispatch remains separately bracketed by the producer report. No atomic counter relation, native completion, physics or visual acceptance.'}
}
function Test-BmxMotionBasisMeasurement($Run,[string]$Mode) {
    if(@('Default','PreserveSuppliedBasis') -cnotcontains $Mode){return $false}
    foreach($field in @('source_unchanged_after_run','process_teardown_verified')){if($Run.$field -isnot [bool] -or -not $Run.$field){return $false}}
    if($Run.teardown_absence_observations -lt 2 -or -not $Run.motion_basis_observation -or $Run.motion_basis_observation_error -or
       $Run.motion_basis_observation.mode -cne $Mode -or $Run.motion_basis_observation.process_id -ne $Run.owned_pid -or
       $Run.motion_basis_observation.owned_created_utc -cne $Run.owned_created_utc){return $false}
    if($Mode -ceq 'Default'){return $true}
    return [bool]($Run.rider_pose_comparison_mode -ceq 'None' -and $Run.phase_observation -and -not $Run.phase_observation_error -and
        $Run.first_mount_producer_observation -and -not $Run.first_mount_producer_observation_error -and
        $Run.rider_pose_comparison_observation -and -not $Run.rider_pose_comparison_error -and
        $Run.rider_pose_comparison_observation.mode -ceq 'None' -and $Run.rider_pose_comparison_observation.qualified_scope -and
        $Run.rider_pose_comparison_observation.custom_rider_application_counters_zero)
}
