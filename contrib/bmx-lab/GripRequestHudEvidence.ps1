# Data-only private launch/archive helpers. Dot-sourcing performs no launch or native action.
function Assert-BmxGripRequestHudSelection {
    param([bool]$GripRequestHud,[bool]$GripProbe,[bool]$RampRide,[string]$RepoRoot,[string]$ProfileName,
          [string]$BoardHandoffMode='None',[string]$RiderPoseMode='Default')
    if(-not $GripRequestHud){return}
    if(-not $GripProbe -or -not $RampRide -or $ProfileName -cne 'bmx-user-data-ramp-test' -or
       -not [IO.Path]::GetFullPath($RepoRoot).Equals('C:\Users\Jeffr\Desktop\reskate',[StringComparison]::OrdinalIgnoreCase) -or
       $BoardHandoffMode -cne 'None' -or $RiderPoseMode -cne 'Default') {
        throw 'GripRequestHud requires explicit GripProbe and native ramp Ride/RampRideTest in the exact private ramp workspace/profile with Default pose and no board-handoff mode.'
    }
}
function Get-BmxGripRequestHudSourcePins {
    @('Extension\BMX\bmx_grip_projection_trace.h','Extension\BMX\bmx_grip_probe.h',
      'Extension\Skater\client_debug.cpp','Extension\UI\Overlay\overlay_render.cpp')
}
function Assert-BmxHudInteger {
    param($Value,[long]$Minimum=0,[long]$Maximum=[long]::MaxValue)
    if(($Value -isnot [int] -and $Value -isnot [long] -and $Value -isnot [uint32] -and $Value -isnot [uint64]) -or
       [decimal]$Value -lt $Minimum -or [decimal]$Value -gt $Maximum){throw 'HUD integer type/range invalid.'}
}
function Assert-BmxHudNumber {
    param($Value,[double]$Minimum=-1000000,[double]$Maximum=1000000)
    if(($Value -isnot [int] -and $Value -isnot [long] -and $Value -isnot [uint32] -and $Value -isnot [uint64] -and
        $Value -isnot [double] -and $Value -isnot [single] -and $Value -isnot [decimal]) -or
       [double]::IsNaN([double]$Value) -or [double]::IsInfinity([double]$Value) -or
       [double]$Value -lt $Minimum -or [double]$Value -gt $Maximum){throw 'HUD number type/range invalid.'}
}
function Assert-BmxHudArray {
    param($Value,[int]$Count)
    if($Value -isnot [array] -or $Value.Count -ne $Count){throw 'HUD array shape invalid.'}
}
function Assert-BmxHudRequired {
    param($Object,[string[]]$Names)
    foreach($name in $Names){if($null -eq $Object.PSObject.Properties[$name]){throw ('Missing HUD field: '+$name)}}
}
function Test-BmxGripRequestHudReport {
    param($Report,[int]$OwnedPid,[DateTime]$OwnedCreation,$GripIndex)
    Assert-BmxHudRequired $Report @('schema','process_id','process_creation_filetime','capacity','count','rows','point_order',
        'contention_drops','rendered_identity_proven','camera_gpu_join_proven','projection_qualified_by_pixels','boundary')
    Assert-BmxHudInteger $Report.schema 1 1;Assert-BmxHudInteger $Report.process_id $OwnedPid $OwnedPid
    Assert-BmxHudInteger $Report.capacity 512 512;Assert-BmxHudInteger $Report.count 0 512
    Assert-BmxHudInteger $Report.contention_drops
    Assert-BmxHudArray $Report.rows ([int]$Report.count)
    if($Report.process_creation_filetime -isnot [string] -or $Report.process_creation_filetime -cnotmatch '^[1-9][0-9]{0,18}$' -or
       $Report.process_creation_filetime -cne $GripIndex.process_creation_filetime){throw 'HUD creation FILETIME differs from qualified grip index.'}
    $created=[DateTime]::FromFileTimeUtc([long]::Parse($Report.process_creation_filetime,[Globalization.CultureInfo]::InvariantCulture))
    if([Math]::Abs(($created-$OwnedCreation.ToUniversalTime()).TotalMilliseconds) -gt 2){throw 'HUD creation differs from owned process.'}
    if($OwnedPid -le 0 -or [int]$GripIndex.process_id -ne $OwnedPid -or [int]$GripIndex.schema -ne 2 -or
       $GripIndex.rows -isnot [array] -or $GripIndex.rows.Count -ne [int]$GripIndex.count -or [int]$GripIndex.count -gt 128){throw 'HUD lacks qualified matching grip index.'}
    foreach($name in @('rendered_identity_proven','camera_gpu_join_proven','projection_qualified_by_pixels')){
        if($Report.$name -isnot [bool] -or $Report.$name){throw ('HUD unsupported identity/qualification claim: '+$name)}
    }
    if($Report.boundary -isnot [string] -or -not $Report.boundary -or $Report.boundary.Length -gt 1024){throw 'HUD boundary text invalid.'}
    $order=@('requested_left_grip','requested_right_grip','requested_rear_axle','requested_front_axle')
    Assert-BmxHudArray $Report.point_order 4
    for($i=0;$i -lt 4;$i++){if($Report.point_order[$i] -cne $order[$i]){throw 'HUD point order changed.'}}
    $spatial=0;$camera=0;$lastTick=0L
    for($i=0;$i -lt $Report.rows.Count;$i++){
        $row=$Report.rows[$i]
        Assert-BmxHudRequired $row @('sequence','tick_ms','grip_record','source_base','source_owner','listener_serial','listener_time',
            'sdk_frame_placement_revision','source_tick_ms','cpu_requested_world_points','camera_tick_ms','camera_address',
            'camera_associated_grip_record','camera_world','camera_vertical_fov','viewport','spatial_available','projected_points')
        Assert-BmxHudInteger $row.sequence ($i+1) ($i+1);Assert-BmxHudInteger $row.tick_ms 1
        Assert-BmxHudInteger $row.grip_record 0 ($GripIndex.rows.Count-1)
        Assert-BmxHudInteger $row.source_base 65536 140737488355327
        Assert-BmxHudInteger $row.listener_serial 1;Assert-BmxHudNumber $row.listener_time 0 1000000000
        Assert-BmxHudInteger $row.sdk_frame_placement_revision 1;Assert-BmxHudInteger $row.source_tick_ms 1
        if($row.tick_ms -lt $lastTick -or $row.tick_ms -lt $row.source_tick_ms -or $row.tick_ms-$row.source_tick_ms -gt 100){throw 'HUD source/draw clock is stale or rewound.'}
        $lastTick=[long]$row.tick_ms;$grip=$GripIndex.rows[[int]$row.grip_record]
        Assert-BmxHudInteger $grip.record 0 127;Assert-BmxHudInteger $grip.sdk_frame_revision 1
        Assert-BmxHudInteger $grip.sdk_placement_receipt_revision 1;Assert-BmxHudInteger $grip.after_readback.listener_serial 1
        Assert-BmxHudInteger $grip.after_readback.tick_ms 1;Assert-BmxHudInteger $grip.after_readback.actual_base 65536 140737488355327
        Assert-BmxHudNumber $grip.after_readback.listener_time 0 1000000000
        if([int]$grip.record -ne $row.grip_record -or $grip.readback_available -isnot [bool] -or -not $grip.readback_available -or
           $grip.write_complete -isnot [bool] -or -not $grip.write_complete -or $grip.solver_ready -isnot [bool] -or -not $grip.solver_ready -or
           $grip.after_readback.listener_clock_available -isnot [bool] -or -not $grip.after_readback.listener_clock_available -or
           $row.listener_serial -ne $grip.after_readback.listener_serial -or [double]$row.listener_time -ne [double]$grip.after_readback.listener_time -or
           $row.source_tick_ms -ne $grip.after_readback.tick_ms -or $row.source_base -ne $grip.after_readback.actual_base -or
           $row.sdk_frame_placement_revision -ne $grip.sdk_frame_revision -or $row.sdk_frame_placement_revision -ne $grip.sdk_placement_receipt_revision){throw 'HUD row does not join its qualified retained grip record.'}
        Assert-BmxHudArray $row.source_owner 5;Assert-BmxHudArray $grip.after_readback.actual_owner 5
        for($j=0;$j -lt 5;$j++){Assert-BmxHudInteger $row.source_owner[$j] 1 140737488355327
            Assert-BmxHudInteger $grip.after_readback.actual_owner[$j] 1 140737488355327
            if($row.source_owner[$j] -ne $grip.after_readback.actual_owner[$j]){throw 'HUD owner differs from retained record.'}}
        Assert-BmxHudArray $row.cpu_requested_world_points 4
        foreach($point in $row.cpu_requested_world_points){Assert-BmxHudArray $point 3;foreach($value in $point){Assert-BmxHudNumber $value}}
        Assert-BmxHudArray $row.viewport 2;foreach($value in $row.viewport){Assert-BmxHudNumber $value 1 16384}
        if($row.spatial_available -isnot [bool]){throw 'HUD spatial availability is not boolean.'}
        if($null -eq $row.camera_tick_ms){
            if($null -ne $row.camera_address -or $null -ne $row.camera_associated_grip_record -or $null -ne $row.camera_world -or
               $null -ne $row.camera_vertical_fov -or $row.spatial_available){throw 'Unavailable HUD camera has non-null payload or spatial claim.'}
        }else{
            ++$camera;Assert-BmxHudInteger $row.camera_tick_ms 1
            # draw_packet stamps time before its try-lock; a camera published
            # during that interval can be later, and must remain unavailable.
            if([decimal]$row.camera_tick_ms-[decimal]$row.tick_ms -gt 100){throw 'HUD camera is outside the bounded source/camera interval.'}
            Assert-BmxHudInteger $row.camera_address 65536 140737488355151
            Assert-BmxHudInteger $row.camera_associated_grip_record 0 ($GripIndex.rows.Count-1)
            Assert-BmxHudArray $row.camera_world 16;foreach($value in $row.camera_world){Assert-BmxHudNumber $value}
            Assert-BmxHudNumber $row.camera_vertical_fov 1 175
            if($row.camera_vertical_fov -le 1 -or $row.camera_vertical_fov -ge 175){throw 'HUD camera FOV is outside supported bounds.'}
            $cameraGrip=$GripIndex.rows[[int]$row.camera_associated_grip_record]
            if($cameraGrip.after_readback.actual_base -ne $row.source_base -or
               ($cameraGrip.after_readback.actual_owner -join ',') -cne ($row.source_owner -join ',')){throw 'HUD camera association crosses owner.'}
        }
        if($row.spatial_available){
            ++$spatial;if($null -eq $row.camera_tick_ms -or $row.camera_tick_ms -gt $row.tick_ms -or $row.tick_ms-$row.camera_tick_ms -gt 100){throw 'HUD spatial claim uses stale/future/unavailable camera.'}
            Assert-BmxHudArray $row.projected_points 4
            foreach($point in $row.projected_points){Assert-BmxHudArray $point 3;Assert-BmxHudNumber $point[0] (-8*$row.viewport[0]) (8*$row.viewport[0]);
                Assert-BmxHudNumber $point[1] (-8*$row.viewport[1]) (8*$row.viewport[1]);Assert-BmxHudNumber $point[2] 0.08 10000000}
        }elseif($null -ne $row.projected_points){throw 'Unavailable HUD spatial projection must be null.'}
    }
    return [ordered]@{count=[int]$Report.count;camera_rows=$camera;spatial_rows=$spatial;
        diagnostic_outcome=$(if($spatial){'cpu_projection_available_unqualified'}elseif($Report.count){'camera_unavailable'}else{'no_draw_packets'});
        native_quality_evaluated=$false;rendered_identity_proven=$false;projection_anchor_qualified=$false}
}
function Copy-BmxGripRequestHudArchive {
    param([string]$RepoRoot,[string]$ProfileRoot,[string]$ArchiveRoot,[int]$OwnedPid,[DateTime]$OwnedCreation,[DateTime]$RunStart,
          $GripIndex,[string]$GripIndexSha256,[string]$GripBinarySha256)
    if($OwnedPid -le 0 -or -not $OwnedCreation -or -not $RunStart){throw 'HUD archive lacks owned PID/creation/start.'}
    foreach($hash in @($GripIndexSha256,$GripBinarySha256)){if($hash -cnotmatch '^[A-Fa-f0-9]{64}$'){throw 'HUD archive lacks qualified grip artifact hashes.'}}
    $repo=[IO.Path]::GetFullPath($RepoRoot);$hudProfilePath=[IO.Path]::GetFullPath($ProfileRoot);$hudArchivePath=[IO.Path]::GetFullPath($ArchiveRoot)
    foreach($path in @($hudProfilePath,$hudArchivePath)){if(-not $path.StartsWith($repo+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'HUD archive path escaped workspace.'}}
    if($hudProfilePath.Equals($hudArchivePath,[StringComparison]::OrdinalIgnoreCase)){throw 'HUD archive must be separate from its source profile.'}
    $name='bmx-grip-probe-'+$OwnedPid+'-projection.json';$hudInputPath=Join-Path $hudProfilePath $name;$hudOutputPath=Join-Path $hudArchivePath $name
    if(-not (Test-Path -LiteralPath $hudInputPath -PathType Leaf)){return [ordered]@{available=$false;issue='No completed private CPU-request HUD export before existing bounded deadline.'}}
    $info=Get-Item -LiteralPath $hudInputPath
    if($info.Length -le 0 -or $info.Length -gt 2097152){throw 'HUD export must be nonempty and at most2MiB.'}
    $freshAfter=$RunStart.ToUniversalTime();if($OwnedCreation.ToUniversalTime() -gt $freshAfter){$freshAfter=$OwnedCreation.ToUniversalTime()}
    if($info.LastWriteTimeUtc -lt $freshAfter -or $info.LastWriteTimeUtc -gt [DateTime]::UtcNow.AddSeconds(1)){throw 'HUD export file is stale or future relative to owned run.'}
    $hash=(Get-FileHash -LiteralPath $hudInputPath -Algorithm SHA256).Hash;$bytes=[IO.File]::ReadAllBytes($hudInputPath)
    if($bytes.Length -ne $info.Length -or $bytes.Length -gt 2097152){throw 'HUD export changed size during read.'}
    $report=([Text.UTF8Encoding]::new($false,$true)).GetString($bytes) | ConvertFrom-Json
    if((Get-FileHash -LiteralPath $hudInputPath -Algorithm SHA256).Hash -ne $hash){throw 'HUD export changed across parse.'}
    $result=Test-BmxGripRequestHudReport $report $OwnedPid $OwnedCreation $GripIndex
    if(Test-Path -LiteralPath $hudOutputPath){if((Get-FileHash -LiteralPath $hudOutputPath -Algorithm SHA256).Hash -ne $hash){throw 'Preserve existing different HUD archive.'}}
    else{
        $file=[IO.File]::Open($hudOutputPath,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
        try{$file.Write($bytes,0,$bytes.Length);$file.Flush($true)}finally{$file.Dispose()}
    }
    $after=Get-Item -LiteralPath $hudInputPath
    if((Get-FileHash -LiteralPath $hudOutputPath -Algorithm SHA256).Hash -ne $hash -or (Get-FileHash -LiteralPath $hudInputPath -Algorithm SHA256).Hash -ne $hash -or
       $after.Length -ne $info.Length -or $after.LastWriteTimeUtc -ne $info.LastWriteTimeUtc){throw 'HUD export changed or archive differs.'}
    $result.available=$true;$result.process_creation_filetime=$report.process_creation_filetime
    $result.grip_index_sha256=$GripIndexSha256;$result.grip_binary_sha256=$GripBinarySha256
    $result.artifact=[ordered]@{name=$name;sha256=$hash;bytes=$after.Length;modified_utc=$after.LastWriteTimeUtc.ToString('o')}
    return $result
}
