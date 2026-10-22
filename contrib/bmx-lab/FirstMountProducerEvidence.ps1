#requires -Version 7.0
# Data-only bounded file/JSON/type/identity checks. No process, input or native calls.
# Load PostFallRemountEvidence.ps1 first for duplicate-property-safe JSON parsing.
function Test-BmxProducerUInt($Value,[uint64]$Maximum=[uint64]::MaxValue) {
    if($null -eq $Value -or $Value.GetType().FullName -notin @('System.Byte','System.UInt16','System.UInt32','System.UInt64','System.SByte','System.Int16','System.Int32','System.Int64','System.Numerics.BigInteger')){return $false}
    $parsed=[uint64]0
    return [uint64]::TryParse([string]$Value,[ref]$parsed) -and $parsed -le $Maximum
}
function Assert-BmxProducerFields($Object,[string[]]$Fields) {
    if($null -eq $Object -or $Object -is [array] -or $Object -is [string] -or $Object -is [ValueType]){throw 'Producer object missing or wrong type.'}
    foreach($field in $Fields){$property=$Object.PSObject.Properties[$field];if(-not $property -or $property.Name -cne $field){throw ('Producer field missing or wrong case: '+$field)}}
}
function Assert-BmxProducerBool($Object,[string]$Field,[switch]$FalseOnly) {
    Assert-BmxProducerFields $Object @($Field)
    if($Object.$Field -isnot [bool] -or ($FalseOnly -and $Object.$Field)){throw ('Producer Boolean/proof invalid: '+$Field)}
}
function Assert-BmxProducerWords($Words,[int]$Count,[switch]$Nullable) {
    if($null -eq $Words -and $Nullable){return}
    if($Words -isnot [array] -or $Words.Count -ne $Count){throw 'Producer raw word count invalid.'}
    foreach($word in $Words){if(-not (Test-BmxProducerUInt $word 4294967295)){throw 'Producer raw word type/range invalid.'}}
}
function Test-BmxProducerFiniteWord($Word) {
    # IEEE754 exponent bits: preserves unknown raw lanes; no value interpretation.
    return (([uint32]$Word -band [uint32]2139095040) -ne [uint32]2139095040)
}
function Assert-BmxProducerRaw($Raw,[int]$Count) {
    $finiteFields=if($Count -eq 4){@('xyz_finite','all_finite')}else{@('translation_xyz_finite','all_finite')}
    Assert-BmxProducerFields $Raw (@('raw_words')+$finiteFields)
    Assert-BmxProducerWords $Raw.raw_words $Count -Nullable
    foreach($field in $finiteFields){Assert-BmxProducerBool $Raw $field}
    $all=$null -ne $Raw.raw_words;$xyz=$all
    if($all){for($i=0;$i -lt $Count;$i++){if(-not (Test-BmxProducerFiniteWord $Raw.raw_words[$i])){$all=$false}}
        $first=if($Count -eq 4){0}else{12};for($i=$first;$i -lt $first+3;$i++){if(-not (Test-BmxProducerFiniteWord $Raw.raw_words[$i])){$xyz=$false}}}
    if($Raw.all_finite -ne $all -or $Raw.($finiteFields[0]) -ne $xyz){throw 'Producer raw finite claims disagree with raw bits/availability.'}
}
function Assert-BmxProducerOwner($Owner) {
    Assert-BmxProducerFields $Owner @('base','entity','core','context','rig')
    foreach($field in @('base','entity','core','context','rig')){if(-not (Test-BmxProducerUInt $Owner.$field)){throw 'Producer owner type invalid.'}}
}
function Test-BmxProducerSameOwner($A,$B) {
    foreach($field in @('base','entity','core','context','rig')){if($A.$field -eq 0 -or $A.$field -ne $B.$field){return $false}};return $true
}
function Assert-BmxProducerClock($Clock) {
    Assert-BmxProducerFields $Clock @('available','pre_original_listener_serial','native_time');Assert-BmxProducerBool $Clock 'available'
    if(-not (Test-BmxProducerUInt $Clock.pre_original_listener_serial)){throw 'Producer listener serial type invalid.'}
    if($Clock.available){if(-not (Test-BmxFiniteNumber $Clock.native_time ([double]::MaxValue)) -or $Clock.native_time -lt 0){throw 'Producer native time invalid.'}}
    elseif($null -ne $Clock.native_time){throw 'Unavailable producer clock cannot have a native time.'}
}
function Test-BmxProducerSameClock($A,$B) {
    return $A.available -and $B.available -and $A.pre_original_listener_serial -eq $B.pre_original_listener_serial -and $A.native_time -eq $B.native_time
}
function Assert-BmxProducerSnapshot($Snapshot,[string]$Kind) {
    Assert-BmxProducerFields $Snapshot @('qpc_begin','qpc_end','owner_before','owner_after','clock_before','clock_after','root_words','context_13cc',
        'world_publication_before','world_publication_after','rig_4630_dispatch_target_evidence','rig_4638_raw_pointer_lane_words','physical_rig','parts','physical_flags',
        'raw_5860','raw_5870','raw_context_17ec')
    foreach($field in @('owner_available','final_owner_available','same_owner_and_listener_clock','component_proved','physical_typed','physical_record_owner','physical_chain_stable')){Assert-BmxProducerBool $Snapshot $field}
    foreach($field in @('atomic_native_snapshot','full_canonical_world_coherence_proved','copy_branch_execution_proved')){Assert-BmxProducerBool $Snapshot $field -FalseOnly}
    foreach($field in @('qpc_begin','qpc_end','physical_rig','parts')){if(-not (Test-BmxProducerUInt $Snapshot.$field)){throw ('Producer snapshot integer invalid: '+$field)}}
    if($Snapshot.qpc_end -lt $Snapshot.qpc_begin){throw 'Producer QPC interval reversed.'}
    Assert-BmxProducerOwner $Snapshot.owner_before;Assert-BmxProducerOwner $Snapshot.owner_after
    Assert-BmxProducerClock $Snapshot.clock_before;Assert-BmxProducerClock $Snapshot.clock_after
    if($Snapshot.component_proved -and (-not $Snapshot.owner_available -or -not $Snapshot.final_owner_available -or
        -not (Test-BmxProducerSameOwner $Snapshot.owner_before $Snapshot.owner_after))){throw 'Component proof lacks two fresh matching present owners.'}
    if($Snapshot.same_owner_and_listener_clock -and (-not $Snapshot.owner_available -or -not $Snapshot.final_owner_available -or -not $Snapshot.component_proved -or
        -not (Test-BmxProducerSameOwner $Snapshot.owner_before $Snapshot.owner_after) -or -not (Test-BmxProducerSameClock $Snapshot.clock_before $Snapshot.clock_after))){throw 'Producer qualified snapshot lacks matching owner/clock/component.'}
    Assert-BmxProducerWords $Snapshot.root_words 3 -Nullable
    foreach($field in @('context_13cc','physical_flags')){if($null -ne $Snapshot.$field -and -not (Test-BmxProducerUInt $Snapshot.$field 4294967295)){throw 'Producer flag word invalid.'}}
    Assert-BmxProducerFields $Snapshot @('context_13cc_bit8000')
    if($null -eq $Snapshot.context_13cc){if($null -ne $Snapshot.context_13cc_bit8000){throw 'Unavailable context flags cannot claim bit8000.'}}
    else{Assert-BmxProducerBool $Snapshot 'context_13cc_bit8000';if($Snapshot.context_13cc_bit8000 -ne (([uint32]$Snapshot.context_13cc -band 32768) -ne 0)){throw 'Context bit8000 disagrees with raw flags.'}}
    foreach($field in @('context_830','context_7c0','rig_57f0','raw_5860','raw_5870')){Assert-BmxProducerRaw $Snapshot.$field 4}
    foreach($field in @('rig_4720','rig_4760','cached_35c0_record23_raw_basis_unknown','physical_record23_matrix')){Assert-BmxProducerRaw $Snapshot.$field 16}
    Assert-BmxProducerWords $Snapshot.rig_4638_raw_pointer_lane_words 2 -Nullable
    $raw=$Snapshot.raw_context_17ec;Assert-BmxProducerFields $raw @('available','raw_word','finite');Assert-BmxProducerBool $raw 'available';Assert-BmxProducerBool $raw 'finite'
    if($raw.available){if(-not (Test-BmxProducerUInt $raw.raw_word 4294967295) -or $raw.finite -ne (Test-BmxProducerFiniteWord $raw.raw_word)){throw 'Producer denominator raw type/finite claim invalid.'}}
    elseif($null -ne $raw.raw_word -or $raw.finite){throw 'Unavailable denominator cannot carry value/finite claim.'}
    foreach($field in @('world_publication_before','world_publication_after')){
        $world=$Snapshot.$field
        foreach($key in @('sdk_available','published_owner_matches','fresh_entity_context_available')){Assert-BmxProducerBool $world $key}
        Assert-BmxProducerBool $world 'canonical_native_world_proved' -FalseOnly
        foreach($key in @('client','client_context','world','native_state','body_token')){Assert-BmxProducerFields $world @($key);if(-not (Test-BmxProducerUInt $world.$key)){throw 'Producer SDK-world publication scalar invalid.'}}
    }
    $tail=$Snapshot.rig_4630_dispatch_target_evidence
    foreach($key in @('object_pointer_bounds','vtable_in_verified_readable_data_section','target_in_verified_readable_executable_section','target_first_byte_readable')){Assert-BmxProducerBool $tail $key}
    foreach($key in @('call_performed','subobject_atomic_snapshot_proved','completion_contract_proved')){Assert-BmxProducerBool $tail $key -FalseOnly}
    foreach($key in @('object_raw','vtable_raw','slot_d8_target_raw')){Assert-BmxProducerFields $tail @($key);if($null -ne $tail.$key -and -not (Test-BmxProducerUInt $tail.$key)){throw 'Producer raw pointer integer invalid.'}}
}
function Test-BmxFirstMountProducerEnvelope($Report,[int]$ProcessId,[DateTime]$CreatedUtc,[DateTime]$StartedUtc,[DateTime]$ModifiedUtc) {
    Assert-BmxProducerFields $Report @('schema_version','event','process_id','process_creation_filetime','source_sha256','qpc_frequency','completion_boundary','clock_boundary',
        'record_capacity','snapshot_capacity','epoch_window','per_kind_per_epoch','stored_snapshots','static_storage_bytes','anchor_listener_serial','rows')
    if(-not (Test-BmxProducerUInt $Report.schema_version 1) -or $Report.schema_version -ne 1 -or $Report.event -cne 'bmx_first_mount_producer_probe' -or
        -not (Test-BmxProducerUInt $Report.process_id 4294967295) -or $Report.process_id -ne $ProcessId -or
        $Report.process_creation_filetime -isnot [string] -or $Report.process_creation_filetime -cnotmatch '^[1-9][0-9]{0,18}$' -or
        $Report.source_sha256 -cne 'fbce74d5e28ef525dbba2cb4adbebc13405bdbd88f31bc940bca45e4ae88b8f9'){throw 'Producer envelope identity/source invalid.'}
    foreach($field in @('read_only_native_state','explicit_producer_opt_in','armed','sealed')){Assert-BmxProducerBool $Report $field}
    Assert-BmxProducerBool $Report 'mount_token_continuity_proved' -FalseOnly
    if(-not $Report.read_only_native_state -or -not $Report.explicit_producer_opt_in){throw 'Producer report lacks explicit read-only opt-in.'}
    $created=[DateTime]::FromFileTimeUtc([Int64]::Parse($Report.process_creation_filetime,[Globalization.CultureInfo]::InvariantCulture))
    if([Math]::Abs(($created-$CreatedUtc.ToUniversalTime()).TotalMilliseconds) -gt 2 -or $ModifiedUtc.ToUniversalTime() -lt $StartedUtc.ToUniversalTime() -or $ModifiedUtc.ToUniversalTime() -gt [DateTime]::UtcNow.AddSeconds(1)){throw 'Producer creation/mtime stale, future or foreign.'}
    if($Report.completion_boundary -cne 'existing_call_return_only_not_native_physics_or_animation_completion' -or
        $Report.clock_boundary -cne 'SDK_listener_advances_before_original_and_is_not_a_completion_fence' -or
        -not (Test-BmxProducerUInt $Report.qpc_frequency) -or $Report.qpc_frequency -eq 0){throw 'Producer measurement boundary invalid.'}
    foreach($pair in @(@('record_capacity',48),@('snapshot_capacity',80),@('epoch_window',8),@('per_kind_per_epoch',2))){if(-not (Test-BmxProducerUInt $Report.($pair[0])) -or $Report.($pair[0]) -ne $pair[1]){throw 'Producer fixed capacity changed.'}}
    foreach($field in @('contention','unavailable','dropped','bad_cookies','owner_changes')){Assert-BmxProducerFields $Report @($field);if(-not (Test-BmxProducerUInt $Report.$field)){throw 'Producer diagnostic counter invalid.'}}
    if(-not (Test-BmxProducerUInt $Report.static_storage_bytes 131072) -or -not (Test-BmxProducerUInt $Report.stored_snapshots 80) -or
        $Report.rows -isnot [array] -or $Report.rows.Count -gt 48 -or ($null -ne $Report.anchor_listener_serial -and -not (Test-BmxProducerUInt $Report.anchor_listener_serial))){throw 'Producer storage/window types invalid.'}
    if($Report.rows.Count -gt 0 -and -not $Report.armed){throw 'Nonempty producer report must be armed.'}
    $records=[Collections.Generic.HashSet[uint64]]::new();$invocations=[Collections.Generic.HashSet[uint64]]::new();$slots=[Collections.Generic.HashSet[string]]::new();$epochs=@{};$snapshotCount=0
    foreach($row in $Report.rows){
        Assert-BmxProducerFields $row @('record_index','invocation','thread_id','kind','slot_in_kind','listener_serial_at_reservation','component','rendering','frame_entity','mount_token','frame_revision','flags','selected_target_words','before','post_observed','original_returned','after','pair_same_owner_and_listener_clock')
        if(-not (Test-BmxProducerUInt $row.record_index 47) -or -not $records.Add([uint64]$row.record_index) -or
            -not (Test-BmxProducerUInt $row.invocation) -or $row.invocation -eq 0 -or -not $invocations.Add([uint64]$row.invocation) -or -not (Test-BmxProducerUInt $row.thread_id 4294967295) -or $row.thread_id -eq 0 -or
            $row.kind -cnotin @('motion_original','local_animation_original','before_bmx_pose_write') -or -not (Test-BmxProducerUInt $row.slot_in_kind 15) -or
            -not $slots.Add(('{0}:{1}' -f $row.kind,$row.slot_in_kind))){throw 'Producer row identity/kind/slot invalid.'}
        $kindIndex=if($row.kind -ceq 'motion_original'){0}elseif($row.kind -ceq 'local_animation_original'){1}else{2}
        if($row.record_index -ne $kindIndex*16+$row.slot_in_kind){throw 'Producer record index differs from fixed kind storage slot.'}
        foreach($field in @('component','frame_entity','mount_token','frame_revision')){if(-not (Test-BmxProducerUInt $row.$field)){throw 'Producer row frame scalar invalid.'}}
        if($row.kind -ceq 'motion_original'){
            if($row.component -ne 0 -or -not (Test-BmxProducerUInt $row.flags 255)){throw 'Producer motion discriminator invalid.'};Assert-BmxProducerWords $row.selected_target_words 16
        }elseif($null -ne $row.flags -or $null -ne $row.selected_target_words -or $row.component -eq 0){throw 'Non-motion row requires a component and cannot claim motion fields.'}
        if($row.kind -ceq 'before_bmx_pose_write'){if($row.rendering -isnot [bool]){throw 'Writer rendering discriminator invalid.'}}
        elseif($null -ne $row.rendering -or $row.frame_entity -ne 0 -or $row.mount_token -ne 0 -or $row.frame_revision -ne 0){throw 'Non-writer row cannot claim writer fields.'}
        $epoch=$row.listener_serial_at_reservation
        if($null -ne $epoch){
            if(-not (Test-BmxProducerUInt $epoch) -or $null -eq $Report.anchor_listener_serial -or [uint64]$epoch -lt [uint64]$Report.anchor_listener_serial -or
                [uint64]$epoch-[uint64]$Report.anchor_listener_serial -ge 8){throw 'Producer reservation outside original8-epoch arithmetic window.'}
        }
        $key='{0}:{1}' -f $row.kind,$epoch;$epochs[$key]=1+$epochs[$key];if($epochs[$key] -gt 2){throw 'Producer kind/epoch reservation capacity exceeded.'}
        Assert-BmxProducerBool $row 'post_observed';Assert-BmxProducerBool $row 'pair_same_owner_and_listener_clock';Assert-BmxProducerSnapshot $row.before $row.kind;$snapshotCount++
        if($row.kind -ceq 'before_bmx_pose_write' -and ($row.post_observed -or $row.pair_same_owner_and_listener_clock)){throw 'Writer-only observation cannot claim post/original return or pair.'}
        if($row.post_observed){
            if($row.original_returned -isnot [bool] -or -not $row.original_returned){throw 'Observed producer post lacks existing-call return.'};Assert-BmxProducerSnapshot $row.after $row.kind;$snapshotCount++
            if($row.pair_same_owner_and_listener_clock -and (-not $row.before.same_owner_and_listener_clock -or -not $row.after.same_owner_and_listener_clock -or
                -not (Test-BmxProducerSameOwner $row.before.owner_before $row.after.owner_before) -or -not (Test-BmxProducerSameClock $row.before.clock_before $row.after.clock_after))){throw 'Producer pair proof lacks associated owner/listener clock.'}
        }elseif($null -ne $row.original_returned -or $null -ne $row.after -or $row.pair_same_owner_and_listener_clock){throw 'Unobserved producer post cannot claim return, after snapshot or pair.'}
    }
    if($snapshotCount -ne $Report.stored_snapshots -or $snapshotCount -gt 80){throw 'Producer stored snapshot count differs from published immutable halves.'}
    return [pscustomobject]@{valid=$true;process_id=$ProcessId;process_creation_filetime=$Report.process_creation_filetime;rows=$Report.rows.Count;stored_snapshots=$snapshotCount
        boundary='Data-only owned passive samples; mount tokens are raw evidence without continuity proof. No minimum coverage, expected physics result, target execution, native completion, lifetime or whole-rider fence is established.'}
}
function Read-BmxFirstMountProducerReport([string]$Path) {
    $stream=$null
    try{
        $stream=[IO.FileStream]::new($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        $buffer=[byte[]]::new(2097153);$count=0
        while($count -lt $buffer.Length){$read=$stream.Read($buffer,$count,$buffer.Length-$count);if($read -eq 0){break};$count+=$read}
        if($count -gt 2097152){throw 'Producer JSON exceeds2MiB bounded file limit.'}
        $bytes=[byte[]]::new($count);[Array]::Copy($buffer,$bytes,$count)
        $encoding=[Text.UTF8Encoding]::new($false,$true);$text=$encoding.GetString($bytes).TrimStart([char]0xfeff)
        $hash=[Security.Cryptography.SHA256]::Create();try{$digest=[Convert]::ToHexString($hash.ComputeHash($bytes))}finally{$hash.Dispose()}
        return [pscustomobject]@{text=$text;sha256=$digest;bytes=$count}
    }finally{if($stream){$stream.Dispose()}}
}
