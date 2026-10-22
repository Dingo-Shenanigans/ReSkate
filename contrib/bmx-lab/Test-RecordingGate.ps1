# Offline fixtures only: no process launch, capture, input or termination.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'RecordingGate.ps1')
$testRepository=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$testDirectory=Join-Path $testRepository ('build\bmx\recording-gate-fixtures-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testDirectory | Out-Null
$testNonce='1234567890abcdef1234567890abcdef'
$testControl=Join-Path $testDirectory 'control.json'
function Require-Gate([bool]$Condition,[string]$Reason) { if(-not $Condition){throw $Reason} }
function Require-GateReject([scriptblock]$Action,[string]$Reason) { $testRejected=$false;try{&$Action}catch{$testRejected=$true};Require-Gate $testRejected $Reason }
Assert-BmxCaptureGateConfiguration $testRepository $testDirectory $testNonce 30
Require-GateReject { Assert-BmxCaptureGateConfiguration $testRepository $testRepository $testNonce 30 } 'Workspace root cannot be a capture gate.'
Require-GateReject { Assert-BmxCaptureGateConfiguration $testRepository $testDirectory 'ABC' 30 } 'Malformed nonce must fail.'
Require-GateReject { Assert-BmxCaptureGateConfiguration $testRepository $testDirectory $testNonce 601 } 'Unbounded timeout must fail.'
$testCreated=[DateTime]::UtcNow
Require-Gate ((Update-BmxCaptureGateControl $testDirectory $testNonce 123 $testCreated $testControl) -eq 'awaiting_native_world') 'No native status may be treated as released.'
$testStatus=[ordered]@{schema_version=1;event='bmx_recording_startup_gate';state='waiting';nonce=$testNonce;process_id=123;process_creation_filetime=$testCreated.ToFileTimeUtc().ToString();observed_utc=$testCreated.ToString('o');started_tick_ms=100;elapsed_ms=0}
$testStatusPath=Join-Path $testDirectory 'recording-gate.status.json'
function Save-TestGateStatus { [IO.File]::WriteAllText($testStatusPath,($testStatus|ConvertTo-Json),[Text.UTF8Encoding]::new($false)) }
Save-TestGateStatus
Require-Gate ((Update-BmxCaptureGateControl $testDirectory $testNonce 123 $testCreated $testControl) -eq 'waiting') 'Owned waiting status is visible before the UI recorder starts.'
Require-GateReject { Update-BmxCaptureGateControl $testDirectory $testNonce 124 $testCreated $testControl } 'A different owned PID must fail.'
Require-GateReject { Update-BmxCaptureGateControl $testDirectory $testNonce 123 $testCreated.AddMilliseconds(10) $testControl } 'A different process creation must fail.'
$testStatus.nonce='0234567890abcdef1234567890abcdef';Save-TestGateStatus
Require-GateReject { Update-BmxCaptureGateControl $testDirectory $testNonce 123 $testCreated $testControl } 'Another run nonce must fail.'
$testStatus.nonce=$testNonce;$testStatus.state='released';Save-TestGateStatus
Require-Gate ((Update-BmxCaptureGateControl $testDirectory $testNonce 123 $testCreated $testControl) -eq 'released') 'Release is visible only with complete owned identity.'
$testWrittenControl=Get-Content -LiteralPath $testControl -Raw|ConvertFrom-Json
Require-Gate ($testWrittenControl.process_creation_filetime -ceq $testStatus.process_creation_filetime) 'Native FILETIME must be preserved exactly for the receipt.'
Require-GateReject { Assert-BmxCaptureGateConfiguration $testRepository $testDirectory $testNonce 30 } 'Existing status cannot silently start a new gate.'
Write-Output ('Recording gate path, identity and control fixtures passed. Evidence: '+$testDirectory)
