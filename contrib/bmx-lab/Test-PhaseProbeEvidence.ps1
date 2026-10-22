#requires -Version 7.0
# Synthetic, pure envelope/order fixtures; does not execute a supervisor or game.
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
# Candidate tests may set this explicit root because this file is still staged.
if($env:BMX_PHASE_TEST_REPO){$repo=$env:BMX_PHASE_TEST_REPO}
. (Join-Path $repo 'contrib\bmx-lab\PostFallRemountEvidence.ps1')
$helper=Join-Path $PSScriptRoot 'PhaseProbeEvidence.ps1'
. $helper
function Require($value,[string]$message){if(-not $value){throw $message}}
$created=[DateTime]::UtcNow.AddMinutes(-1);$started=$created.AddSeconds(1);$modified=$created.AddSeconds(20)
function New-Envelope {
    [pscustomobject]@{schema_version=1;event='bmx_skeleton_response_phase_probe';process_id=123
        process_creation_filetime=([string]$created.ToFileTimeUtc());read_only_native_state=$true
        source_sha256='fbce74d5e28ef525dbba2cb4adbebc13405bdbd88f31bc940bca45e4ae88b8f9';qpc_frequency=10000000
        completion_boundary='original_return_only_not_native_physics_or_animation_completion'
        clock_boundary='SDK_listener_advances_before_original_and_is_not_a_completion_fence';rows=@()}
}
function Reject([scriptblock]$mutate,[string]$name){$r=New-Envelope;& $mutate $r;$rejected=$false;try{[void](Test-BmxPhaseEnvelope $r 123 $created $started $modified)}catch{$rejected=$true};Require $rejected $name}
Require (Test-BmxPhaseEnvelope (New-Envelope) 123 $created $started $modified).valid 'valid empty diagnostic must not require a physics result'
Reject {param($r)$r.process_id=124} 'different PID rejected'
Reject {param($r)$r.process_id='123'} 'string PID rejected'
Reject {param($r)$r.process_creation_filetime=[string]$created.AddSeconds(2).ToFileTimeUtc()} 'different creation rejected'
Reject {param($r)$r.process_creation_filetime='01'} 'noncanonical creation string rejected'
Reject {param($r)$r.source_sha256='wrong'} 'different source rejected'
Reject {param($r)$r.read_only_native_state='true'} 'typed read-only flag required'
Reject {param($r)$r.completion_boundary='completed_physics'} 'false completion boundary rejected'
Reject {param($r)$r.rows=@(1..513)} 'fixed capacity enforced'
Reject {param($r)$r.rows=@([pscustomobject]@{invocation=1;thread_id=1;window=0;incoming_third=$false;effective_third=$false;post_observed=$false;original_returned=$true;after_original=$null})} 'missing post cannot claim native return'
$r=New-Envelope;$r.rows=@([pscustomobject]@{invocation=1;thread_id=2;window=2;incoming_third=$true;effective_third=$false;post_observed=$false;original_returned=$null;after_original=$null})
Require (Test-BmxPhaseEnvelope $r 123 $created $started $modified).valid 'unknown post remains valid unavailable coverage'
Reject {param($r)$row=[pscustomobject]@{invocation=1;thread_id=1;window=0;incoming_third=$false;effective_third=$false;post_observed=$false;original_returned=$null;after_original=$null};$r.rows=@($row,$row)} 'duplicate invocation rejected'
$rejected=$false;try{[void](Test-BmxPhaseEnvelope (New-Envelope) 123 $created $started $created)}catch{$rejected=$true};Require $rejected 'stale mtime rejected'
# Ordering helper receives independently supplied GetUp proof; no expected success imposed.
function Get-BmxGetupProgress {param($text,$processId,$variant,$impact,$action,$trace,$earliestAt);[pscustomobject]@{complete=$script:standing;issue='synthetic independent standing proof'}}
$script:standing=$true
$log="[2026-10-19 12:00:00.000] [123:1] BMX GetUp lab complete: pulse=true;`n[2026-10-19 12:00:02.000] [123:1] Startup command queued: bmx phase-dump`n[2026-10-19 12:00:04.000] [123:1] Startup command queued: bmx interact"
$early=[DateTime]::Parse('2026-10-19T11:00:00');$written=[DateTime]::Parse('2026-10-19T12:00:03')
$order=Get-BmxPhaseCheckpoint $log 123 2 $null $null $null $early $written
Require ($order.dump_before_interaction -and $order.file_written_before_interaction -and $order.native_getup_and_standing_before_dump) 'owned dump sits between standing evidence and interaction'
$script:standing=$false;$order=Get-BmxPhaseCheckpoint $log 123 2 $null $null $null $early $written
Require (-not $order.native_getup_and_standing_before_dump -and $order.dump_before_interaction) 'failed standing is recorded independently, not promoted or rejected as a different physics outcome'
$order=Get-BmxPhaseCheckpoint $log 124 2 $null $null $null $early $written
Require ($order.dump_commands -eq 0 -and -not $order.dump_before_interaction) 'foreign process log does not establish order'
Write-Output 'Phase probe pure identity/order fixtures passed (16 cases).'
