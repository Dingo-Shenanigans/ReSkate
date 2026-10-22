# Offline synthetic fixtures. No real CIM query, screen capture or game action.
$ErrorActionPreference='Stop'
$testHelper=Join-Path $PSScriptRoot 'Confirm-BmxRecording.ps1'
$testTokens=$null;$testErrors=$null
$testAst=[Management.Automation.Language.Parser]::ParseFile($testHelper,[ref]$testTokens,[ref]$testErrors)
if($testErrors.Count){throw 'Acknowledgement helper did not parse.'}
foreach($testFunction in $testAst.FindAll({param($node)$node -is [Management.Automation.Language.FunctionDefinitionAst]},$false)){
    . ([scriptblock]::Create($testFunction.Extent.Text))
}
$testRepository=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$testRoot=Join-Path $testRepository ('build\bmx\acknowledgement-fixtures-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot|Out-Null
$script:testProcess=$null;$script:testCimCalls=0;$script:testMutation=$null
function Get-CimInstance {
    param($ClassName,$Filter,$ErrorAction)
    if($ClassName -cne 'Win32_Process' -or $Filter -cne 'ProcessId=12345'){throw 'Unexpected mocked CIM query.'}
    $script:testCimCalls++
    if($script:testMutation){&$script:testMutation $script:testCimCalls}
    return $script:testProcess
}
function Save-AckFixtureJson($Value,[string]$File){[IO.File]::WriteAllText($File,($Value|ConvertTo-Json -Depth 12),[Text.UTF8Encoding]::new($false))}
function New-AckFixture([string]$Name){
    $workspace=Join-Path $testRoot $Name
    $gate=Join-Path $workspace 'build\gate';$capture=Join-Path $workspace 'build\capture';$game=Join-Path $workspace 'build\bmx-game'
    New-Item -ItemType Directory -Path $gate,$capture,$game|Out-Null
    $created=[DateTime]::UtcNow.AddSeconds(-1);$now=[DateTime]::UtcNow
    $nonce='1234567890abcdef1234567890abcdef';$exe=Join-Path $game 'Skate.exe';$dll=Join-Path $game 'ReSkate.dll'
    [IO.File]::WriteAllText($exe,'SYNTHETIC EXE FIXTURE');[IO.File]::WriteAllText($dll,'SYNTHETIC RUNTIME FIXTURE')
    $frame=Join-Path $capture 'frame-000000.png';[IO.File]::WriteAllBytes($frame,[byte[]](1,2,3,4))
    $status=[ordered]@{schema_version=1;event='bmx_recording_startup_gate';state='waiting';nonce=$nonce;process_id=12345;process_creation_filetime=$created.ToFileTimeUtc().ToString();observed_utc=$now.ToString('o');started_tick_ms=100;elapsed_ms=0}
    $statusPath=Join-Path $gate 'recording-gate.status.json';Save-AckFixtureJson $status $statusPath
    $control=[ordered]@{schema_version=1;event='bmx_recording_control';owned_pid=12345;owned_created_utc=$created.ToString('o');gate_directory=$gate;gate_nonce=$nonce;gate_status_file=$statusPath;gate_ready_file=(Join-Path $gate 'recording-gate.ready');gate_state='waiting';process_creation_filetime=$status.process_creation_filetime}
    $controlPath=Join-Path $workspace 'build\control.json';Save-AckFixtureJson $control $controlPath
    $window=[ordered]@{app=('process:'+$exe);id=234}
    $manifest=[ordered]@{schema=1;event='bmx_sampled_live_window_capture';sampled_live=$true;completed=$false;failure=$null;capture_ended_utc=$null;termination_reason=$null;capture_started_utc=$now.ToString('o');source_window=$window
        owned_process=[ordered]@{process_id=12345;process_created_utc=$created.ToString('o');runtime_sha256=(Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash;game_exe=$exe}
        samples=@([ordered]@{index=0;file='frame-000000.png';sha256=(Get-FileHash -LiteralPath $frame -Algorithm SHA256).Hash;bytes=4;window_id=234;app=$window.app;capture_started_utc=$now.ToString('o');capture_ended_utc=$now.ToString('o')});audit=[ordered]@{sample_count=1}}
    $manifestPath=Join-Path $capture 'capture-manifest.json';Save-AckFixtureJson $manifest $manifestPath
    $script:testProcess=[pscustomobject]@{ExecutablePath=$exe;CreationDate=$created};$script:testCimCalls=0;$script:testMutation=$null
    return [pscustomobject]@{workspace=$workspace;control=$control;controlPath=$controlPath;status=$status;statusPath=$statusPath;manifest=$manifest;manifestPath=$manifestPath;frame=$frame;dll=$dll}
}
function Reject-AckFixture($Fixture,[string]$Reason){
    $rejected=$false
    try{Confirm-BmxRecording $Fixture.controlPath $Fixture.manifestPath $Fixture.workspace|Out-Null}catch{$rejected=$true}
    if(-not $rejected -or (Test-Path -LiteralPath $Fixture.control.gate_ready_file)){throw ('Invalid acknowledgement was admitted: '+$Reason)}
}
$testValid=New-AckFixture 'valid'
$testResult=Confirm-BmxRecording $testValid.controlPath $testValid.manifestPath $testValid.workspace|ConvertFrom-Json
if($testResult.event -cne 'bmx_recording_acknowledged' -or $script:testCimCalls -ne 2){throw 'Complete fixture must verify live identity twice.'}
$testExpected="RESKATE_BMX_CAPTURE_READY_V1`n$($testValid.control.gate_nonce)`n12345`n$($testValid.status.process_creation_filetime)`n"
if([IO.File]::ReadAllText($testValid.control.gate_ready_file) -cne $testExpected){throw 'Receipt must preserve exact native FILETIME and framing.'}
$testRepeatRejected=$false;try{Confirm-BmxRecording $testValid.controlPath $testValid.manifestPath $testValid.workspace|Out-Null}catch{$testRepeatRejected=$true}
if(-not $testRepeatRejected){throw 'Existing acknowledgement must not be overwritten.'}
$testCase=New-AckFixture 'wrong-source';$testCase.manifest.source_window.app='process:C:\wrong\Skate.exe';Save-AckFixtureJson $testCase.manifest $testCase.manifestPath;Reject-AckFixture $testCase 'different capture executable'
$testCase=New-AckFixture 'wrong-nonce';$testCase.status.nonce='0234567890abcdef1234567890abcdef';Save-AckFixtureJson $testCase.status $testCase.statusPath;Reject-AckFixture $testCase 'gate nonce mismatch'
$testCase=New-AckFixture 'wrong-pid';$testCase.manifest.owned_process.process_id=12346;Save-AckFixtureJson $testCase.manifest $testCase.manifestPath;Reject-AckFixture $testCase 'capture PID mismatch'
$testCase=New-AckFixture 'wrong-filetime';$testCase.status.process_creation_filetime='123456789';Save-AckFixtureJson $testCase.status $testCase.statusPath;Reject-AckFixture $testCase 'native FILETIME mismatch'
$testCase=New-AckFixture 'changed-bytes';[IO.File]::WriteAllBytes($testCase.frame,[byte[]](4,3,2,1));Reject-AckFixture $testCase 'frame hash mismatch'
$testCase=New-AckFixture 'stale-frame';$testCase.manifest.capture_started_utc=[DateTime]::UtcNow.AddSeconds(-30).ToString('o');$testCase.manifest.samples[0].capture_started_utc=$testCase.manifest.capture_started_utc;$testCase.manifest.samples[0].capture_ended_utc=$testCase.manifest.capture_started_utc;Save-AckFixtureJson $testCase.manifest $testCase.manifestPath;Reject-AckFixture $testCase 'stale frame'
$testCase=New-AckFixture 'path-escape';$testCase.manifest.samples[0].file='..\outside.png';Save-AckFixtureJson $testCase.manifest $testCase.manifestPath;Reject-AckFixture $testCase 'frame escaped capture directory'
$testCase=New-AckFixture 'failed-capture';$testCase.manifest.failure='capture failed';Save-AckFixtureJson $testCase.manifest $testCase.manifestPath;Reject-AckFixture $testCase 'failed capture'
$testCase=New-AckFixture 'string-bool';$testCase.manifest.sampled_live='true';Save-AckFixtureJson $testCase.manifest $testCase.manifestPath;Reject-AckFixture $testCase 'string boolean'
$testCase=New-AckFixture 'cim-reused';$script:testProcess.CreationDate=$script:testProcess.CreationDate.AddMilliseconds(10);Reject-AckFixture $testCase 'CIM creation identity changed'
$testCase=New-AckFixture 'before-write-reuse';$script:testMutation={param($call)if($call -eq 2){$script:testProcess.CreationDate=$script:testProcess.CreationDate.AddMilliseconds(10)}};Reject-AckFixture $testCase 'PID reused before atomic publication'
$testCase=New-AckFixture 'before-write-release';$script:testMutation={param($call)if($call -eq 1){$testCase.status.state='released';Save-AckFixtureJson $testCase.status $testCase.statusPath}};Reject-AckFixture $testCase 'gate no longer waiting before atomic publication'
$testCase=New-AckFixture 'before-write-runtime';$script:testMutation={param($call)if($call -eq 2){[IO.File]::WriteAllText($testCase.dll,'CHANGED SYNTHETIC RUNTIME')}};Reject-AckFixture $testCase 'runtime changed before atomic publication'
$testCase=New-AckFixture 'forward-slash-exe';$testCase.manifest.owned_process.game_exe=$testCase.manifest.owned_process.game_exe.Replace('\','/');Save-AckFixtureJson $testCase.manifest $testCase.manifestPath
Confirm-BmxRecording $testCase.controlPath $testCase.manifestPath $testCase.workspace|Out-Null
if(-not(Test-Path -LiteralPath $testCase.control.gate_ready_file)){throw 'Normalized captured executable path must be accepted.'}
Write-Output ('Recording acknowledgement offline fixtures PASS: valid exact receipt, forward-slash executable, repeated acknowledgement and13 rejection cases. Evidence: '+$testRoot)
