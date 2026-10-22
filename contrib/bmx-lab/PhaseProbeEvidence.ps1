# Data-only identity and ordering policy. No process launch, input or native action.
function Test-BmxPhaseEnvelope($Report,[int]$ProcessId,[DateTime]$CreatedUtc,[DateTime]$StartedUtc,[DateTime]$ModifiedUtc) {
    if($Report.schema_version -isnot [long] -and $Report.schema_version -isnot [int]){throw 'Phase schema type invalid.'}
    if($Report.schema_version -ne 1 -or $Report.event -cne 'bmx_skeleton_response_phase_probe' -or
       -not (Test-BmxUnsignedInteger $Report.process_id) -or $Report.process_id -ne $ProcessId -or
       $Report.process_creation_filetime -isnot [string] -or $Report.process_creation_filetime -cnotmatch '^[1-9][0-9]{0,18}$' -or
       $Report.read_only_native_state -isnot [bool] -or -not $Report.read_only_native_state -or
       $Report.source_sha256 -cne 'fbce74d5e28ef525dbba2cb4adbebc13405bdbd88f31bc940bca45e4ae88b8f9') {throw 'Phase report envelope lacks exact owned identity/source.'}
    $created=[DateTime]::FromFileTimeUtc([Int64]::Parse($Report.process_creation_filetime,[Globalization.CultureInfo]::InvariantCulture))
    if([Math]::Abs(($created-$CreatedUtc.ToUniversalTime()).TotalMilliseconds) -gt 2 -or $ModifiedUtc.ToUniversalTime() -lt $StartedUtc.ToUniversalTime()) {throw 'Phase report creation/mtime is stale or differs from owned process.'}
    if($Report.completion_boundary -cne 'original_return_only_not_native_physics_or_animation_completion' -or
       $Report.clock_boundary -cne 'SDK_listener_advances_before_original_and_is_not_a_completion_fence' -or
       -not (Test-BmxUnsignedInteger $Report.qpc_frequency) -or $Report.rows -isnot [array] -or $Report.rows.Count -gt 512) {throw 'Phase report measurement boundary or capacity invalid.'}
    $ids=[Collections.Generic.HashSet[uint64]]::new()
    foreach($row in $Report.rows) {
        if(-not (Test-BmxUnsignedInteger $row.invocation) -or -not $ids.Add([uint64]$row.invocation) -or
           -not (Test-BmxUnsignedInteger $row.thread_id) -or -not (Test-BmxUnsignedInteger $row.window -AllowZero) -or $row.window -gt 2 -or
           $row.post_observed -isnot [bool] -or $row.incoming_third -isnot [bool] -or $row.effective_third -isnot [bool]){throw 'Phase row identity/type invalid.'}
        if($row.post_observed){if($row.original_returned -isnot [bool] -or -not $row.original_returned -or $null -eq $row.after_original){throw 'Observed post lacks a returned row.'}}
        elseif($null -ne $row.original_returned -or $null -ne $row.after_original){throw 'Unobserved post cannot claim native return or post fields.'}
    }
    # No expected guard value, displacement, copy result, post coverage or physics outcome is required.
    return [pscustomobject]@{valid=$true;process_id=$ProcessId;process_creation_filetime=$Report.process_creation_filetime;rows=$Report.rows.Count
        boundary='Owned passive measurements only; native completion, copy branch and expected physics result are not established.'}
}
function Get-BmxPhaseCheckpoint([string]$Text,[int]$ProcessId,[int]$Variant,$Impact,$Action,$Trace,[DateTime]$EarliestAt,[DateTime]$ModifiedUtc) {
    $lines=@($Text -split '\r?\n');$pidPattern='\['+[regex]::Escape([string]$ProcessId)+':\d+\]';$dumps=@();$interactions=@();$n=0
    foreach($line in $lines){
        $n++;if($line -notmatch $pidPattern -or $line.Length -lt 24){continue};$at=[DateTime]::MinValue
        if(-not [DateTime]::TryParseExact($line.Substring(1,23),'yyyy-MM-dd HH:mm:ss.fff',[Globalization.CultureInfo]::InvariantCulture,[Globalization.DateTimeStyles]::None,[ref]$at) -or $at -lt $EarliestAt){continue}
        if($line -match 'Startup command queued: bmx phase-dump\s*$'){$dumps+=@([pscustomobject]@{line=$n;at=$at})}
        if($line -match 'Startup command queued: bmx interact\s*$'){$interactions+=@([pscustomobject]@{line=$n;at=$at})}
    }
    $ordered=$dumps.Count -eq 1 -and $interactions.Count -eq 1 -and $dumps[0].line -lt $interactions[0].line
    $pre=$null;$writtenBetween=$false
    if($dumps.Count -eq 1){
        # Use the same strict layout adapter as the full post-fall assessor.
        # Only owned, fresh status rows before this dump are normalized; all
        # native values and health checks are retained, and raw logs stay intact.
        $prefix=@(for($i=0;$i -lt $dumps[0].line;++$i){
            $prefixLine=$lines[$i];$prefixAt=[DateTime]::MinValue
            $ownedFresh=$prefixLine -match $pidPattern -and $prefixLine.Length -ge 24 -and
                [DateTime]::TryParseExact($prefixLine.Substring(1,23),'yyyy-MM-dd HH:mm:ss.fff',[Globalization.CultureInfo]::InvariantCulture,[Globalization.DateTimeStyles]::None,[ref]$prefixAt) -and
                $prefixAt -ge $EarliestAt
            if($ownedFresh -and $prefixLine.Contains('Native velocity updates:')){ConvertTo-BmxPassiveStatusLine $prefixLine}
            else{$prefixLine}
        }) -join "`n"
        $pre=Get-BmxGetupProgress $prefix $ProcessId $Variant $Impact $Action $Trace $EarliestAt
        if($ordered){$writtenBetween=$ModifiedUtc.ToUniversalTime() -ge $dumps[0].at.ToUniversalTime() -and $ModifiedUtc.ToUniversalTime() -le $interactions[0].at.ToUniversalTime()}
    }
    return [pscustomobject]@{dump_commands=$dumps.Count;interaction_commands=$interactions.Count;dump_before_interaction=$ordered
        file_written_before_interaction=$writtenBetween;native_getup_and_standing_before_dump=[bool]($pre -and $pre.complete)
        issue=if($pre){$pre.issue}else{'No unambiguous owned dump command.'};boundary='Independent ordering/standing measurement; a wall wait does not prove native evidence.'}
}
