param([Parameter(Mandatory=$true)][string]$EvidenceDirectory)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'BoardHandoffEvidence.ps1')
function Check([bool]$Value,[string]$Reason){if(-not $Value){throw $Reason}}
$created=[DateTime]::SpecifyKind([DateTime]'2026-10-20T21:00:00.1234567',[DateTimeKind]::Utc)
$exact=$created.ToFileTimeUtc().ToString([Globalization.CultureInfo]::InvariantCulture)
$nonce='0123456789abcdef0123456789abcdef';$gate='C:\Users\Jeffr\Desktop\reskate\build\fixture-gate'
$control=[pscustomobject]@{schema_version=1;event='bmx_recording_control';owned_pid=1234;owned_created_utc=$created.ToString('o');gate_state='released';gate_directory=$gate;gate_nonce=$nonce;process_creation_filetime=$exact}
Check ((Get-BmxBoardHandoffArchiveIdentity $control 1234 $created $gate $nonce) -ceq $exact) 'Exact native FILETIME was changed.'
# This fixture proves the preserved value is not reconstructed from reduced CIM precision.
$reduced=$created.AddTicks(-7)
$control.owned_created_utc=$reduced.ToString('o')
Check ((Get-BmxBoardHandoffArchiveIdentity $control 1234 $reduced $gate $nonce) -ceq $exact) 'Native precise FILETIME lost to CIM truncation.'
$control.owned_created_utc=$created.ToString('o')
# Exercise the actual supervisor JSON round-trip, including PowerShell7's
# automatic ISO-date-to-DateTime conversion, rather than string-only fixtures.
$roundTrip=$control|ConvertTo-Json -Depth 8|ConvertFrom-Json
Check ((Get-BmxBoardHandoffArchiveIdentity $roundTrip 1234 $created $gate $nonce) -ceq $exact) 'JSON-roundtrip creation lost fractional ticks.'
$reducedRoundTrip=$control.PSObject.Copy();$reducedRoundTrip.owned_created_utc=$reduced.ToString('o')
$reducedRoundTrip=$reducedRoundTrip|ConvertTo-Json -Depth 8|ConvertFrom-Json
Check ((Get-BmxBoardHandoffArchiveIdentity $reducedRoundTrip 1234 $reduced $gate $nonce) -ceq $exact) 'JSON-roundtrip reduced-CIM association changed raw native FILETIME.'
$offsetControl=$control.PSObject.Copy();$offsetControl.owned_created_utc=([DateTimeOffset]::new($created)).ToOffset([TimeSpan]::FromHours(-4))
Check ((Get-BmxBoardHandoffArchiveIdentity $offsetControl 1234 $created $gate $nonce) -ceq $exact) 'DateTimeOffset creation lost fractional UTC ticks.'
$negative=0
foreach($ticks in @(-1,1)) {
    $bad=$offsetControl.PSObject.Copy();$bad.owned_created_utc=$offsetControl.owned_created_utc.AddTicks($ticks)
    $rejected=$false;try{Get-BmxBoardHandoffArchiveIdentity $bad 1234 $created $gate $nonce|Out-Null}catch{$rejected=$true}
    Check $rejected 'DateTimeOffset one-tick owner mismatch must remain rejected.';++$negative
}
foreach($ticks in @(-1,1)) {
    foreach($typed in @($false,$true)) {
        $bad=$roundTrip.PSObject.Copy();$bad.owned_created_utc=if($typed){$created.AddTicks($ticks)}else{$created.AddTicks($ticks).ToString('o')}
        $rejected=$false;try{Get-BmxBoardHandoffArchiveIdentity $bad 1234 $created $gate $nonce|Out-Null}catch{$rejected=$true}
        Check $rejected 'String/DateTime one-tick owner mismatch must remain rejected.';++$negative
    }
}
foreach($field in @('schema_version','event','owned_pid','owned_created_utc','gate_state','gate_directory','gate_nonce','process_creation_filetime')){
    $bad=$control.PSObject.Copy();$bad.$field=if($field -ceq 'schema_version'){2}elseif($field -ceq 'owned_pid'){5678}elseif($field -ceq 'owned_created_utc'){$created.AddSeconds(1).ToString('o')}else{'wrong'}
    $rejected=$false;try{Get-BmxBoardHandoffArchiveIdentity $bad 1234 $created $gate $nonce|Out-Null}catch{$rejected=$true}
    Check $rejected ('Bad control identity accepted: '+$field);++$negative
}
function Bytes($Value){return ,([Text.UTF8Encoding]::new($false).GetBytes(($Value|ConvertTo-Json -Depth 8 -Compress)))}
$report=[pscustomobject]@{schema=1;mode=1;process_id=1234;creation_filetime=$exact;private_profile='bmx-user-data-chassis-probe';private_root='C:\Users\Jeffr\Desktop\reskate\build\bmx-user-data-chassis-probe';capacity=512;window_limit_ms=8000;count=0;rows=@();stop='client_owner_clock_or_read_unavailable'}
$nativeHeader=$report.PSObject.Copy();$nativeHeader.mode=2
Check ((Test-BmxBoardHandoffArchiveBytes (Bytes $nativeHeader) 1234 $exact 2).mode -eq 2) 'Explicit NativeOnly mode2 archive rejected.'
$modeRejected=$false;try{Test-BmxBoardHandoffArchiveBytes (Bytes $nativeHeader) 1234 $exact|Out-Null}catch{$modeRejected=$true}
Check $modeRejected 'Mode2 must not be silently accepted by the legacy default-mode1 archive.'
$bytes=Bytes $report
Check ((Test-BmxBoardHandoffArchiveBytes $bytes 1234 $exact).count -eq 0) 'Early-stop trace must archive as identity evidence, not pose success.'
foreach($field in @('schema','mode','process_id','creation_filetime','private_profile','private_root','capacity','window_limit_ms','count','stop')){
    $bad=$report.PSObject.Copy();$bad.$field=if($field -ceq 'count'){513}elseif($field -in @('schema','mode','process_id','capacity','window_limit_ms')){99}elseif($field -ceq 'stop'){''}else{'wrong'}
    $rejected=$false;try{Test-BmxBoardHandoffArchiveBytes (Bytes $bad) 1234 $exact|Out-Null}catch{$rejected=$true}
    Check $rejected ('Bad final header accepted: '+$field);++$negative
}
foreach($invalid in @([byte[]]@(),[byte[]](0xff,0xfe,0x7b),[byte[]]::new(2MB+1),[Text.Encoding]::UTF8.GetBytes('{broken'))){
    $rejected=$false;try{Test-BmxBoardHandoffArchiveBytes $invalid 1234 $exact|Out-Null}catch{$rejected=$true}
    Check $rejected 'Invalid/oversize final bytes accepted.';++$negative
}
$fixture=Join-Path ([IO.Path]::GetFullPath($EvidenceDirectory)) ('archive-fixture-'+[Guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $fixture)
$archived=Write-BmxBoardHandoffArchiveBytes $bytes $fixture
Check ([Convert]::ToBase64String([IO.File]::ReadAllBytes($archived.path)) -ceq [Convert]::ToBase64String($bytes)) 'Archive changed exact bytes.'
Check ($archived.sha256 -ceq (Get-FileHash -LiteralPath $archived.path -Algorithm SHA256).Hash) 'Archive hash mismatch.'
$rejected=$false;try{Write-BmxBoardHandoffArchiveBytes $bytes $fixture|Out-Null}catch{$rejected=$true}
Check $rejected 'Existing final archive was overwritten.';++$negative
Check ([Convert]::ToBase64String([IO.File]::ReadAllBytes($archived.path)) -ceq [Convert]::ToBase64String($bytes)) 'Rejected overwrite changed prior archive.'
# Execute the actual supervisor diagnostic-only block against a data spy. This
# proves parameter propagation/default-off and preserves native verdict fields.
$candidateRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$supervisor=Join-Path $candidateRoot 'build\bmx\run_native_chassis_ride.ps1'
$tokens=$null;$errors=$null;$ast=[Management.Automation.Language.Parser]::ParseFile($supervisor,[ref]$tokens,[ref]$errors)
Check (-not $errors) 'Archive supervisor parse failed.'
$block=@($ast.FindAll({param($node) $node -is [Management.Automation.Language.IfStatementAst] -and $node.Extent.Text.StartsWith('if ($bmxRunInfo.board_handoff_requested)')},$true))
Check ($block.Count -eq 1) 'Expected exactly one real supervisor archive block.'
$bmxRunDirectory=$fixture;$bmxProfile='C:\Users\Jeffr\Desktop\reskate\build\bmx-user-data-chassis-probe'
$bmxOwnedPid=1234;$bmxOwnedCreation=$created;$CaptureGateDirectory=$gate;$CaptureGateNonce=$nonce
$control|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $fixture 'control.json')
$script:archiveSpyCalls=0;$script:archiveSpyResult=$null
function Save-BmxBoardHandoffArchive {
    param($ProfileDirectory,$RunDirectory,$Control,$ProcessId,$CreatedUtc,$GateDirectory,$GateNonce,$ExpectedMode=1)
    ++$script:archiveSpyCalls
    Check ($ProfileDirectory -ceq $bmxProfile -and $RunDirectory -ceq $fixture -and $ProcessId -eq 1234 -and
        $CreatedUtc.Ticks -eq $created.Ticks -and $GateDirectory -ceq $gate -and $GateNonce -ceq $nonce -and
        $Control.process_creation_filetime -ceq $exact) 'Actual supervisor archive arguments differ.'
    return $script:archiveSpyResult
}
foreach($requested in @($false,$true)){
    $bmxRunInfo=[ordered]@{board_handoff_requested=$requested;outcome='sentinel_native_verdict';verified_quick_switch_pass=$true;quick_switch_evidence_passed=$true;native_terminal_passed=$true;process_teardown_verified=$true}
    $script:archiveSpyResult=[pscustomobject]@{present=$false;identity_verified=$false;archived=$false;archive_file=$null;sha256=$null;bytes=0;source_file=$null;creation_filetime=$exact;stop=$null;issue='missing_final_trace';coverage='not_assessed'}
    & ([scriptblock]::Create($block[0].Extent.Text))
    Check ($bmxRunInfo.outcome -ceq 'sentinel_native_verdict' -and $bmxRunInfo.verified_quick_switch_pass -and
        $bmxRunInfo.quick_switch_evidence_passed -and $bmxRunInfo.native_terminal_passed -and $bmxRunInfo.process_teardown_verified) 'Diagnostic archive changed native verdict/teardown.'
    if($requested){Check (-not $bmxRunInfo.board_handoff_archived -and $bmxRunInfo.board_handoff_issue -ceq 'missing_final_trace') 'Missing trace became archive success.'}
}
Check ($script:archiveSpyCalls -eq 1) 'Ordinary/default launch invoked archive.'
# Exercise active-baseline acceptance independently from archival identity.
$owner=@(1,2,3,4,5,6);$rows=@()
for($i=0;$i -le 52;$i++){
    $physics=if($i -le 1){504}elseif($i -eq 2){200}else{100}
    foreach($kind in 0,1,2){
        $rows+=[pscustomobject]@{kind=$kind;serial=10+$i*3;native_time=1+$i*.05;wall_ms=1000+$i*50;physics=$physics;owner=$owner;same_owner_clock_after=$true
            offboard=($physics -eq 504);standable=($physics -eq 504);motion_supported=($physics -eq 504)
            client_motion_updates=0;client_pose_updates=0;client_render_updates=0;client_board_hide_updates=0;chassis_allocations=0;chassis_mounts=0;parts=0
            capture=$false;lease_active=$false;bmx_active=$false;board_pending=$false;stepping_off=$false;native_control_input_allowed=$true
            publication_pose_updates=0;publication_render_updates=0;custom_board_hide_updates=0;sdk_pose_publication_active=$false;sdk_pose_publication_entity=0
            published_pointer_matches=$true;holder=100+$kind;buffer=200+$kind;render_data=300+$kind;bone_count=$(if($kind -eq 1){395}else{16})}
    }
}
$activeReport=[pscustomobject]@{mode=2;stop='observed_board100_dwell_complete_not_a_pose_pass';contention_drops=0;count=$rows.Count;rows=$rows
    active_control=[pscustomobject]@{enabled=$true;kind='existing_native_board_mount_request';ordinary_Y_or_hardware_test=$false;attempts=1;accepted=$true;failed=$false;baseline_retained=$true
        baseline_owner=$owner;baseline_serial=10;baseline_native_time=1;baseline_wall_ms=1000;request_serial=13;request_native_time=1.05;request_wall_ms=1050}}
Check (Test-BmxNativeBoardControlEvidence $activeReport) 'Complete active software native-board CPU baseline rejected.'
$activeNegative=0
foreach($field in @('enabled','ordinary_Y_or_hardware_test','attempts','accepted','failed','baseline_retained','baseline_serial','request_serial','request_native_time','request_wall_ms')){
    $bad=$activeReport|ConvertTo-Json -Depth 20|ConvertFrom-Json
    if($field -in @('enabled','accepted','baseline_retained')){$bad.active_control.$field=$false}
    elseif($field -in @('ordinary_Y_or_hardware_test','failed')){$bad.active_control.$field=$true}
    elseif($field -eq 'attempts'){$bad.active_control.$field=2}else{$bad.active_control.$field=0}
    Check (-not(Test-BmxNativeBoardControlEvidence $bad)) ('Invalid active control accepted: '+$field);++$activeNegative
}
foreach($field in @('client_motion_updates','client_pose_updates','client_render_updates','client_board_hide_updates','chassis_allocations','chassis_mounts','parts','capture','lease_active','bmx_active','board_pending','stepping_off','native_control_input_allowed','same_owner_clock_after','physics')){
    $bad=$activeReport|ConvertTo-Json -Depth 20|ConvertFrom-Json
    $bad.rows[30].$field=if($field -in @('capture','lease_active','bmx_active','board_pending','stepping_off')){$true}elseif($field -in @('native_control_input_allowed','same_owner_clock_after')){$false}elseif($field -eq 'physics'){999}else{1}
    Check (-not(Test-BmxNativeBoardControlEvidence $bad)) ('Dirty/interfered row accepted: '+$field);++$activeNegative
}
foreach($fault in @('duplicate_clock','owner_change','pointer_change','missing_publication','short_wall','short_native','ragdoll504','missing_active_metadata')){
    $bad=$activeReport|ConvertTo-Json -Depth 20|ConvertFrom-Json
    switch($fault){
        duplicate_clock {$bad.rows[30].serial=$bad.rows[27].serial;$bad.rows[30].native_time=$bad.rows[27].native_time}
        owner_change {$bad.rows[30].owner[5]=99}
        pointer_change {$bad.rows[31].buffer=999}
        missing_publication {$bad.rows=@($bad.rows|Where-Object{$_.kind -ne 2});$bad.count=$bad.rows.Count}
        short_wall {foreach($r in $bad.rows){$r.wall_ms=1000+[int](($r.wall_ms-1000)*.5)}}
        short_native {foreach($r in $bad.rows){$r.native_time=1+($r.native_time-1)*.5}}
        ragdoll504 {$bad.rows[30].physics=504;$bad.rows[30].offboard=$true;$bad.rows[30].standable=$false;$bad.rows[30].motion_supported=$false}
        missing_active_metadata {$bad.active_control=$null}
    }
    Check (-not(Test-BmxNativeBoardControlEvidence $bad)) ('Invalid active baseline accepted: '+$fault);++$activeNegative
}
Write-Output "Active native-board software baseline: strict acceptance and $activeNegative adversarial failures passed; no hardware/ordinary-Y or visual claims."
Write-Output "Board handoff archive: exact native FILETIME/reduced-CIM fixture, early-stop identity, $negative negatives, exact-byte atomic archive/hash/no-overwrite, actual supervisor argument/default-off/native-verdict isolation passed. Fixture=$fixture. No game, launch or production build."
