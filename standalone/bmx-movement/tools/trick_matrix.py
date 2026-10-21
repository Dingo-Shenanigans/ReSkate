"""Describe fixed all-slot inputs and audit per-request observed trick coverage.

This tool never controls a device. Historical names are separate from a fresh
runtime catalog and live fire/completion/scoring evidence. Unknown is not failure.
"""
from __future__ import annotations
import argparse
import csv
import hashlib
import importlib.util
import json
from pathlib import Path
import re

ROOT=Path(__file__).resolve().parents[1]
def module(name,file):
    spec=importlib.util.spec_from_file_location(name,Path(__file__).with_name(file));result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result);return result
driver_tools=module('matrix_driver_tools','drive_target.py')
comparison=module('matrix_comparison_tools','comparison.py')


def historical_catalog():
    path=ROOT/'include/bmx/detail/source/bmx_trick_catalog.h';source=path.read_text(encoding='utf-8')
    ids={name:int(value) for name,value in re.findall(r'^\s+(\w+)=(\d+),',source,re.M)}
    slots={(ids[key]//10,ids[key]%10):dict(name=name,module_id=ids[key])
           for key,name in re.findall(r'\{TrickId::(\w+),\s*"([^"]+)"',source)}
    if len(slots)!=58:raise ValueError('Expected explicit58-entry historical catalog; inspect changes')
    return slots,dict(path=str(path),sha256=comparison.sha(path.read_bytes()),status='historical_names_pending_fresh_runtime_verification')


def runtime_catalog(path):
    if path is None:return {},None
    source=json.loads(path.read_text(encoding='utf-8-sig'))
    # Deliberately accept only explicit tuples, never split an integer ID into
    # native set/slot fields or infer empty slots from missing entries.
    entries=source.get('slots')
    if not isinstance(entries,list):raise ValueError('Runtime catalog requires slots:[{set,slot,available,name}]')
    slots={}
    for entry in entries:
        pair=entry.get('set'),entry.get('slot')
        if any(type(index) is not int or not 0<=index<8 for index in pair):raise ValueError('Runtime tuple outside8x8 catalog')
        if pair in slots or type(entry.get('available')) is not bool:raise ValueError('Duplicate tuple or unknown availability')
        slots[pair]=entry
    return slots,dict(path=str(path),sha256=comparison.sha(path.read_bytes()),explicit_slots=len(slots),
                      all64_positions_explicit=len(slots)==64,source_metadata=source.get('source'),
                      canonical_role=source.get('canonical_role'),
                      source_table_counts={table['role']:table.get('set_count_observed') for table in source.get('tables',[])})


def plan(args):
    historical,hsource=historical_catalog();current,csource=runtime_catalog(args.runtime_catalog);batches=[]
    for name in tuple('trick_matrix_'+suffix+f'{index:02}' for suffix in ('','reset_') for index in range(1,5)):
        timeline=driver_tools.scenarios()[name];attempts=[]
        for request in driver_tools.matrix_attempts(name):
            pair=request['set'],request['slot'];old=historical.get(pair);now=current.get(pair)
            attempts.append(request|dict(historical=old,current_runtime=now,
                current_catalog_verified=now is not None,
                historical_empty=old is None))
        batches.append(dict(scenario=name,duration_seconds=timeline[-1][0],
            timeline_sha256=comparison.sha(json.dumps(timeline,separators=(',',':'),sort_keys=True).encode()),attempts=attempts))
    name_key=lambda name:re.sub(r'[^a-z0-9]','',name.lower()) if name else ''
    disagreements=[]
    for pair,now in current.items():
        old=historical.get(pair)
        if now['available']!=(old is not None) or (old and now['available'] and name_key(old['name'])!=name_key(now.get('name'))):
            disagreements.append(dict(set=pair[0],slot=pair[1],historical=old,current=now))
    result=dict(schema=1,event='bmx_all_slot_matrix_plan',historical_source=hsource,current_runtime_source=csource,
                historical_runtime_disagreements=disagreements,
                total_requests_per_family=64,historical_nonempty=58,historical_empty_controls=6,
                batches=[batch for batch in batches if '_reset_' not in batch['scenario']],
                reset_batches=[batch for batch in batches if '_reset_' in batch['scenario']],
                limits=['Plan identity certifies requested inputs only, not effective native selection or success.',
                        'Fresh runtime catalog availability/name is separate from live execution.',
                        'Stationary flat quick hops may not permit every trick; missing fire, bail or failure must remain visible.'])
    with args.output.open('x',encoding='utf-8') as target:json.dump(result,target,indent=2)
    print(json.dumps(dict(output=str(args.output),batches=[{k:v for k,v in b.items() if k!='attempts'} for b in batches],
                         current_runtime_source=csource,historical_runtime_disagreements=disagreements),indent=2))


def source_rows(path,driver):
    start=comparison.utc(driver['timeline_start_utc']);out=[]
    with path.open(newline='',encoding='utf-8-sig') as source:
        for raw in csv.DictReader(source):
            if comparison.boolean(raw.get('valid','1')) is False:continue
            t=float(raw['utc_seconds'])-start if raw.get('utc_seconds') else comparison.utc(raw['utc'])-start
            if out and t<out[-1]['t']:raise ValueError('Source clock moved backwards')
            out.append(dict(t=t,raw=raw,grounded=comparison.boolean(raw.get('grounded')),
                            body_id=raw.get('body_id')))
    if not out:raise ValueError('Empty observed source')
    return out


def rider_source_rows(path,driver):
    start=comparison.utc(driver['timeline_start_utc']);out=[]
    with path.open(encoding='utf-8-sig') as source:
        for line in source:
            if not line.strip():continue
            frame=json.loads(line)
            if frame.get('selection_current') is False:continue
            brain=(frame.get('animation') or {}).get('brain_state') or {}
            wall=comparison.utc(frame['utc'])-start
            out.append(dict(t=wall,grounded=None,body_id=str(frame.get('chassis_body_id')),
                raw=dict(trick_v2_time_since_fire=brain.get('TimeSinceLastFire'),
                    trick_v2_set=brain.get('_lastFiredSetId'),trick_v2_slot=brain.get('_lastFiredSlotId'),
                    simulation_seconds=frame.get('simulation_seconds'))))
    if not out:raise ValueError('No current rider animation observations')
    return out


def true_field(rows,names):
    available=[name for name in names if any(name in row['raw'] for row in rows)]
    return (any(comparison.boolean(row['raw'].get(name)) is True for row in rows for name in available)
            if available else None),available


def json_frames(path):
    with path.open(encoding='utf-8-sig') as source:
        for line in source:
            if line.strip():yield json.loads(line)


def ledger(args):
    driver=comparison.read_driver(args.driver,args.target);requests=driver_tools.matrix_attempts(driver['scenario'])
    if not requests:raise ValueError('Expected one fixed trick_matrix scenario')
    canonical=json.dumps(driver_tools.scenarios()[driver['scenario']],separators=(',',':'),sort_keys=True).encode()
    if comparison.sha(canonical)!=driver['timeline_sha256']:raise ValueError('Receipt timeline differs from fixed matrix')
    rider_only=args.rider_observations is not None
    rows=(rider_source_rows(args.rider_observations,driver) if rider_only else source_rows(args.telemetry,driver))
    historical,hsource=historical_catalog();current,csource=runtime_catalog(args.runtime_catalog)
    fires=[];previous=None;previous_body=None;animation_rows=[]
    states_path=args.rider_observations if rider_only else args.telemetry.parent/'trick-states.jsonl'
    if args.target=='streets' and states_path.exists():
        previous_fire=None;previous_brain=None;start=comparison.utc(driver['timeline_start_utc'])
        for frame in json_frames(states_path):
            anim=frame.get('animation') or {};brain=anim.get('brain') or {}
            if frame.get('selection_current') is False:continue
            identity=brain.get('native_pointer') or brain.get('id');state=anim.get('brain_state') or {};control=anim.get('controller_state') or {}
            at=comparison.finite(state.get('_timeAtLastFire'));wall=comparison.utc(frame['utc'])-start
            phase=control.get('_phase');phase_number=comparison.finite(phase)
            if phase_number is None:phase_number={'Idle':0,'Launch':1,'Tweak':2,'Loop':3,'Hold':4,'Exit':5}.get(str(phase))
            animation_rows.append(dict(t=wall,simulation_seconds=frame.get('simulation_seconds'),brain=identity,animation=anim,
                                       phase=phase_number))
            if at is not None and previous_fire is not None and identity==previous_brain and at>previous_fire+1e-5:
                fires.append(dict(t=wall,simulation_seconds=at,set=comparison.finite(state.get('_lastFiredSetId')),
                    slot=comparison.finite(state.get('_lastFiredSlotId')),fixed_index=frame.get('fixed_index'),
                    evidence='Observed same-brain _timeAtLastFire advance and _lastFired tuple'+(' in actual rider-frame channel' if rider_only else ' in fixed-step channel')))
            previous_fire=at;previous_brain=identity
    for row_index,row in enumerate(rows):
        raw=row['raw'];elapsed=comparison.finite(raw.get('trick_v2_time_since_fire'))
        if args.target=='streets' and not animation_rows:
            if (elapsed is not None and previous is not None and elapsed<previous-.05
                    and row['body_id']==previous_body):
                fires.append(dict(t=row['t'],set=comparison.finite(raw.get('trick_v2_set')),
                                  slot=comparison.finite(raw.get('trick_v2_slot')),
                                  simulation_seconds=(comparison.finite(raw.get('simulation_seconds'))-elapsed
                                      if comparison.finite(raw.get('simulation_seconds')) is not None else None),
                                  evidence='time_since_fire reset; fire engine time estimated from elapsed timer'))
            previous=elapsed;previous_body=row['body_id']
        elif args.target=='demo':
            selected=comparison.finite(raw.get('selected_action_trick'))
            if selected is not None and selected!=255:
                actual=comparison.finite(raw.get('movement_id',raw.get('trick_id')))
                legacy_channel=('bar' if int(selected) in (34,44) else 'tail' if int(selected) in (52,56) else None)
                legacy_active=(comparison.boolean(raw.get(legacy_channel+'_rotating')) is True or
                    abs(comparison.finite(raw.get(legacy_channel+'_angle')) or 0)>1e-6) if legacy_channel else False
                if legacy_channel and not legacy_active and row_index+1<len(rows):
                    # A newly adopted legacy channel begins at angle zero. Its
                    # first integration is the next fixed tick; require the
                    # same reset/body and exactly one short engine-time step.
                    following=rows[row_index+1];next_raw=following['raw']
                    at=comparison.finite(raw.get('simulation_seconds',raw.get('time')))
                    after=comparison.finite(next_raw.get('simulation_seconds',next_raw.get('time')))
                    legacy_active=(at is not None and after is not None and 0<after-at<=.011 and
                        row['body_id']==following['body_id'] and raw.get('reset_counter')==next_raw.get('reset_counter') and
                        abs(comparison.finite(next_raw.get(legacy_channel+'_angle')) or 0)>1e-6)
                if actual==selected or legacy_active:
                    fires.append(dict(t=row['t'],module_id=int(selected),simulation_seconds=comparison.finite(raw.get('simulation_seconds',raw.get('time'))),
                        evidence='selection pulse plus matching adopted state' if not legacy_channel else 'selection pulse plus legacy rotation channel active now or within one fixed tick'))
    results=[]
    for request in requests:
        pair=request['set'],request['slot'];old=historical.get(pair);active=current.get(pair)
        subset=[row for row in rows if request['start_seconds']<=row['t']<request['end_seconds']]
        accepted=[fire for fire in fires if request['selection_start_seconds']-.1<=fire['t']<request['end_seconds']]
        correct=[fire for fire in accepted if ((fire.get('set'),fire.get('slot'))==pair if args.target=='streets'
            else old is not None and fire.get('module_id')==old['module_id'])]
        identities={row['body_id'] for row in subset if row['body_id'] and row['t']>=request['hop_request_seconds']-.1}
        takeoffs=[];recontacts=[]
        for before,after in zip(subset,subset[1:]):
            if (after['t']<request['hop_request_seconds']-.05 or before['body_id']!=after['body_id']
                    or after['t']-before['t']>.1):continue
            if before['grounded'] is True and after['grounded'] is False:takeoffs.append(after['t'])
            if before['grounded'] is False and after['grounded'] is True:recontacts.append(after['t'])
        outcome=[row for row in subset if correct and row['t']>=correct[0]['t']]
        legacy_completion=('bar_caught',) if pair in ((3,4),(4,4)) else ('tail_caught',) if pair in ((5,2),(5,6)) else ('event_movement_caught','movement_caught')
        completed,complete_fields=true_field(outcome,legacy_completion) if args.target=='demo' else true_field(outcome,('trick_completed','trick_completion_event'))
        landed,land_fields=true_field(outcome,('combo_landed',)) if args.target=='demo' else true_field(outcome,('trick_landed','trick_landed_event'))
        failed,fail_fields=true_field(outcome,('combo_failed','combo_canceled')) if args.target=='demo' else true_field(outcome,('trick_failed','trick_bailed'))
        selection_observable=(any('selected_action_trick' in row['raw'] for row in subset) if args.target=='demo'
                              else any(comparison.finite(row['raw'].get('trick_v2_time_since_fire')) is not None for row in subset))
        phases=[frame for frame in animation_rows if correct and correct[0]['t']<=frame['t']<request['end_seconds']]
        transitions=[dict(t=b['t'],simulation_seconds=b['simulation_seconds'],from_phase=a['phase'],to_phase=b['phase']) for a,b in zip(phases,phases[1:]) if a['phase']!=b['phase']]
        exit_idle=any(t['from_phase']==5 and t['to_phase']==0 for t in transitions) if phases else None
        results.append(request|dict(historical=old,current_runtime=active,observed_samples=len(subset),
            observed_source_wall_interval=[subset[0]['t'],subset[-1]['t']] if subset else None,
            source_complete_through_attempt=bool(subset) and subset[-1]['t']>=request['end_seconds']-.1,
            input_readback_verified=True,body_identity_change=len(identities)>1,
            selection_observable=selection_observable,correct_fire_observed=bool(correct) if selection_observable else None,
            fire_observations=accepted,observed_takeoffs=takeoffs,physical_recontacts=recontacts,
            completion_signal=completed,completion_fields=complete_fields,landed_scoring_signal=landed,landed_fields=land_fields,
            native_animation_phase_transitions=transitions,native_animation_exit_to_idle_observed=exit_idle,
            failure_signal=failed,failure_fields=fail_fields,
            success_established=bool(correct) and completed is True and landed is True and len(identities)<=1))
    result=dict(schema=1,event='bmx_all_slot_runtime_coverage',target=args.target,scenario=driver['scenario'],
        timeline_sha256=driver['timeline_sha256'],driver_sha256=comparison.sha(args.driver.read_bytes()),
        telemetry_sha256=comparison.sha(args.telemetry.read_bytes()),historical_source=hsource,current_runtime_source=csource,
        observation_source=dict(path=str(states_path if rider_only else args.telemetry),
            sha256=comparison.sha((states_path if rider_only else args.telemetry).read_bytes()),
            channel='actual rider-frame animation only' if rider_only else 'actual fixed-step telemetry/animation',
            fixed_physics_available=not rider_only,physical_recontact_observable=not rider_only),
        counts=dict(requested=len(results),historical_nonempty_requested=sum(r['historical'] is not None for r in results),
                    correct_fire_observed=sum(r['correct_fire_observed'] is True for r in results),
                    completed_signal=sum(r['completion_signal'] is True for r in results),
                    landed_signal=sum(r['landed_scoring_signal'] is True for r in results),
                    success_established=sum(r['success_established'] for r in results)),attempts=results,
        limits=['Physical recontact is not landed trick scoring.',
                'Persistent set/slot or module ID alone is not a new fire.',
                'Missing completion/score signals remain null, not success or failure.',
                'Unsupported historical slots are requested controls; fresh runtime availability may differ.',
                'Body replacement within an attempt prevents established success.'])
    with args.output.open('x',encoding='utf-8') as target:json.dump(result,target,indent=2)
    print(json.dumps(dict(output=str(args.output),counts=result['counts']),indent=2))


def aggregate(args):
    current,csource=runtime_catalog(args.runtime_catalog)
    if len(current)!=64:raise ValueError('Fresh explicit 64-slot catalog required')
    candidates={pair:[] for pair in current};sources=[]
    for path in args.ledger:
        source=json.loads(path.read_text(encoding='utf-8-sig'))
        if source.get('target')!='streets' or source.get('event')!='bmx_all_slot_runtime_coverage':
            raise ValueError('Aggregate requires native coverage ledgers')
        pose_path=path.parent/'pose-tracks-native.json';poses=[]
        if pose_path.exists():
            pose=json.loads(pose_path.read_text(encoding='utf-8-sig'))
            if pose.get('coverage_sha256')!=comparison.sha(path.read_bytes()):raise ValueError('Posture bundle/coverage hash mismatch')
            poses=pose['tracks']
        sources.append(dict(ledger=str(path),ledger_sha256=comparison.sha(path.read_bytes()),
            posture=str(pose_path) if pose_path.exists() else None,
            posture_sha256=comparison.sha(pose_path.read_bytes()) if pose_path.exists() else None,
            scenario=source['scenario'],timeline_sha256=source['timeline_sha256']))
        for attempt in source['attempts']:
            pair=attempt['set'],attempt['slot'];tracks=[track for track in poses if (track['set'],track['slot'])==pair
                and attempt['selection_start_seconds']-.1<=track['fire_observation']['t']<attempt['end_seconds']]
            idle=[phase for phase in attempt.get('native_animation_phase_transitions',[]) if phase['from_phase']==5 and phase['to_phase']==0]
            full_posture=False
            for track in tracks:
                idle_end=comparison.finite(idle[0].get('simulation_seconds')) if idle else None
                key_end=max(key['t'] for key in track['keys'])+track['fire_engine_seconds']
                if idle_end is not None and key_end>=idle_end+.14:full_posture=True
            candidates[pair].append(dict(ledger=str(path),scenario=source['scenario'],attempt_index=attempt['attempt_index'],
                fire_observed=attempt['correct_fire_observed'],native_exit_to_idle_observed=attempt['native_animation_exit_to_idle_observed'],
                full_posture_window_observed=full_posture,posture_track_count=len(tracks),
                source_complete_through_attempt=attempt['source_complete_through_attempt'],body_identity_change=attempt['body_identity_change'],
                native_landed_scoring_signal=attempt['landed_scoring_signal'],
                unexpected_fires=([fire for fire in attempt['fire_observations'] if (fire.get('set'),fire.get('slot'))!=pair]
                                  if current[pair]['available'] else attempt['fire_observations']),
                fire_observations=attempt['fire_observations']))
    rows=[];retry=[]
    for pair,slot in sorted(current.items()):
        options=candidates[pair]
        best=max(options,key=lambda item:(item['full_posture_window_observed'],item['native_exit_to_idle_observed'] is True,
            item['fire_observed'] is True,item['source_complete_through_attempt'],not item['body_identity_change'])) if options else None
        row=dict(set=pair[0],slot=pair[1],name=slot['name'],available=slot['available'],attempts=options,best_attempt=best)
        row['empty_control_verified_no_fire']=bool(options) and not slot['available'] and any(
            item['source_complete_through_attempt'] and not item['fire_observations'] for item in options)
        if slot['available'] and (best is None or not best['full_posture_window_observed']):
            retry.append(dict(set=pair[0],slot=pair[1],name=slot['name'],reason=(
                'No attempt observed' if best is None else 'No correct fire' if best['fire_observed'] is not True
                else 'Missing native Exit-to-Idle or its complete posture tail')))
        rows.append(row)
    available=[row for row in rows if row['available']]
    result=dict(schema=1,event='bmx_current_all_slot_coverage_aggregate',current_runtime_source=csource,sources=sources,slots=rows,
        counts=dict(catalog_available=len(available),catalog_empty=64-len(available),
            available_correct_fire_observed=sum(any(item['fire_observed'] is True for item in row['attempts']) for row in available),
            available_native_exit_to_idle_observed=sum(any(item['native_exit_to_idle_observed'] is True for item in row['attempts']) for row in available),
            available_full_posture_window_observed=sum(any(item['full_posture_window_observed'] for item in row['attempts']) for row in available),
            native_landed_scoring_observed=sum(any(item['native_landed_scoring_signal'] is True for item in row['attempts']) for row in available),
            empty_controls_verified_no_fire=sum(row['empty_control_verified_no_fire'] for row in rows)),
        retry_requests=retry,limits=['Correct fire, animation Exit-to-Idle, posture coverage, physical recontact and landed scoring are separate observations.',
            'A complete posture window requires the actual Exit-to-Idle transition plus at least 0.14 observed engine seconds of tail.',
            'Native landed scoring remains unestablished when the source does not expose a scoring signal.',
            'Repeated attempts are retained; the best observed window does not erase unsuccessful or censored attempts.'])
    with args.output.open('x',encoding='utf-8') as target:json.dump(result,target,indent=2)
    print(json.dumps(dict(output=str(args.output),counts=result['counts'],retry_requests=retry),indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);commands=parser.add_subparsers(dest='command',required=True)
    planning=commands.add_parser('plan');planning.add_argument('--runtime-catalog',type=Path);planning.add_argument('--output',type=Path,required=True);planning.set_defaults(function=plan)
    coverage=commands.add_parser('ledger')
    for name in ('driver','telemetry','output'):coverage.add_argument('--'+name,type=Path,required=True)
    coverage.add_argument('--target',choices=['streets','demo'],required=True);coverage.add_argument('--runtime-catalog',type=Path)
    coverage.add_argument('--rider-observations',type=Path,help='Explicit native rider-animation-only channel; no fixed physics or grounding claim')
    coverage.set_defaults(function=ledger)
    aggregate_parser=commands.add_parser('aggregate');aggregate_parser.add_argument('--ledger',type=Path,nargs='+',required=True)
    aggregate_parser.add_argument('--runtime-catalog',type=Path,required=True);aggregate_parser.add_argument('--output',type=Path,required=True)
    aggregate_parser.set_defaults(function=aggregate);args=parser.parse_args();args.function(args)
