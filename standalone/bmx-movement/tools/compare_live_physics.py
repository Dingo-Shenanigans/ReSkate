"""Describe matched live native/demo body responses with explicit clock limits."""
from __future__ import annotations
import argparse
import bisect
import csv
import importlib.util
import json
import math
from pathlib import Path
import statistics

spec=importlib.util.spec_from_file_location('live_physics_math',Path(__file__).with_name('analyze_suspension.py'))
a=importlib.util.module_from_spec(spec);spec.loader.exec_module(a);c=a.comparison


def pitch(front):return -math.degrees(math.atan2(front[1],math.hypot(front[0],front[2])))
def attitude(chassis,rider):
    front=a.rotate(chassis,[0,0,1]);rider_front=a.rotate(rider,[0,0,1]);right=a.rotate(chassis,[1,0,0])
    return dict(chassis_pitch_degrees=pitch(front),rider_pitch_degrees=pitch(rider_front),
        rider_relative_pitch_degrees=pitch(a.rotate(a.inverse(chassis),rider_front)),
        chassis_right_axis_elevation_degrees=math.degrees(math.atan2(right[1],math.hypot(right[0],right[2]))))


def native_rows(directory,driver):
    start=c.utc(driver['timeline_start_utc']);rows=[];invalid=0
    for frame in a.jsonlines(directory/'body-physics.jsonl'):
        if frame.get('sample_valid') is False:invalid+=1;continue
        bodies={body['role']:body for body in frame.get('bodies',[]) if body.get('available')}
        chassis=bodies.get('Chassis Body');rider=bodies.get('Spring Body')
        if not chassis or not rider:invalid+=1;continue
        q=chassis['rotation'];front=a.rotate(q,[0,0,1]);relative=a.local(rider['world_center_of_mass'],chassis['world_center_of_mass'],q)
        row=dict(t=c.utc(frame['utc'])-start,simulation=frame['simulation_seconds'],body_id=chassis['reference']['id'],
            y=chassis['world_center_of_mass'][1],relative_y=relative[1],vy=chassis['velocity'][1],up=a.rotate(q,[0,1,0])[1],
            speed=math.hypot(chassis['velocity'][0],chassis['velocity'][2]),yaw=-math.atan2(front[0],front[2]),
            grounded=frame.get('grounded',{}).get('Grounded'),
            front_grounded=frame.get('grounded',{}).get('_frontEndGrounded'),rear_grounded=frame.get('grounded',{}).get('_backEndGrounded'))
        mirror=lambda value:[value[0],-value[1],-value[2],value[3]]
        row.update(attitude(mirror(q),mirror(rider['rotation'])))
        inputs=frame.get('input') or {}
        row.update(manual=c.finite(inputs.get('MannyInput')),nose_manual=c.finite(inputs.get('NoseyInput')),
            preload=c.finite(inputs.get('PreloadInput')),pump=c.finite(inputs.get('PumpInput')))
        rows.append(row)
    return rows,invalid


def module_rows(directory,csv_path,driver):
    start=c.utc(driver['timeline_start_utc']);coupled=[];invalid=0;nonconverged=0
    for frame in a.jsonlines(directory/'coupled-physics.jsonl'):
        invalid+=frame.get('valid') is False;nonconverged+=frame.get('converged') is False
        if frame.get('valid') is False:continue
        coupled.append(dict(t=float(frame['utc_seconds'])-start,raw=frame))
    times=[row['t'] for row in coupled];rows=[];missed=0
    with csv_path.open(newline='',encoding='utf-8-sig') as stream:
        for raw in csv.DictReader(stream):
            if c.boolean(raw.get('valid')) is False:continue
            wall=float(raw['utc_seconds'])-start;index=bisect.bisect_left(times,wall)
            candidates=[i for i in (index-1,index) if 0<=i<len(times)]
            if not candidates:missed+=1;continue
            nearest=min(candidates,key=lambda i:abs(times[i]-wall))
            if abs(times[nearest]-wall)>.03:missed+=1;continue
            frame=coupled[nearest]['raw'];q=[float(raw['q'+key]) for key in ('x','y','z','w')];front=a.rotate(q,[0,0,1])
            row=dict(t=wall,simulation=float(raw['time']),body_id=raw.get('reset_counter'),
                y=frame['observed_chassis_com'][1],relative_y=frame['rider_relative_com'][1],vy=float(raw['vy']),
                up=a.rotate(q,[0,1,0])[1],speed=math.hypot(float(raw['vx']),float(raw['vz'])),yaw=math.atan2(front[0],front[2]),
                grounded=c.boolean(raw.get('grounded')),front_grounded=None,rear_grounded=None,
                manual=float(raw['manual']),nose_manual=float(raw['nose_manual']),preload=float(raw['preload']),pump=None)
            row.update(attitude(q,frame['rider_quaternion']))
            row['manual_pitch_setpoint_degrees']=c.finite(frame.get('manual_pitch_setpoint_degrees'))
            row['hop_kind']=frame.get('hop_kind')
            row['hop_request_accepted']=frame.get('hop_request_accepted')
            row['flick_preloading']=frame.get('flick_preloading')
            row['hop_impulse_y']=c.finite(raw.get('hop_impulse_y'))
            for name in ('manual_chassis_torque','manual_rider_torque'):
                torque=frame.get(name);row[name+'_magnitude_nm']=math.sqrt(sum(value*value for value in torque)) if torque else None
            contacts=frame.get('input_wheel_contacts') or []
            for index,name in enumerate(('front','rear')):
                contact=contacts[index] if index<len(contacts) else {}
                separation=c.finite(contact.get('separation'));status=contact.get('status')
                row[name+'_query_status']=status;row[name+'_query_separation']=separation
                row[name+'_strict_gap_query_candidate']=(status==2 and separation is not None and separation<=.005) if status in (1,2) else None
            rows.append(row)
    return rows,dict(invalid_coupled_samples=invalid,nonconverged_coupled_samples=nonconverged,unpaired_body_csv_samples=missed)


def phases(scenario):
    if scenario=='suspension':return [(name,start,end) for name,start,end in a.PHASES]+[
        ('RS_flick_total_response',3.2,5.8),('R3_1_total_response',6,8.9),('R3_2_total_response',11,17.8)]
    if scenario=='controls_probe':return [(name,index*8+2,index*8+3.2) for index,name in enumerate(('RS_negative_partial','RS_positive_partial','RS_negative_full','RS_positive_full'))]
    if scenario=='air_rotation':return [(name,index*8+1.4,index*8+6) for index,name in enumerate(('LS_X_positive_air','LS_X_negative_air','LS_Y_positive_air','LS_Y_negative_air'))]
    raise ValueError('Expected suspension, controls_probe or air_rotation')


def describe(rows):
    if not rows:return dict(samples=0)
    result=dict(samples=len(rows),wall_interval=[rows[0]['t'],rows[-1]['t']],engine_interval=[rows[0]['simulation'],rows[-1]['simulation']],
        engine_to_wall_ratio=(rows[-1]['simulation']-rows[0]['simulation'])/(rows[-1]['t']-rows[0]['t']) if rows[-1]['t']>rows[0]['t'] else None,
        body_identity_count=len({row['body_id'] for row in rows}),
        body_state={name:a.stats([row.get(name) for row in rows]) for name in ('y','relative_y','vy','up','speed','manual','nose_manual','preload','pump',
            'chassis_pitch_degrees','rider_pitch_degrees','rider_relative_pitch_degrees','chassis_right_axis_elevation_degrees',
            'manual_pitch_setpoint_degrees','manual_chassis_torque_magnitude_nm','manual_rider_torque_magnitude_nm',
            'front_query_separation','rear_query_separation')},
        wheel_grounded_observations={name:dict(true=sum(row.get(name) is True for row in rows),false=sum(row.get(name) is False for row in rows),
            unavailable=sum(row.get(name) is None for row in rows)) for name in ('grounded','front_grounded','rear_grounded',
                'front_strict_gap_query_candidate','rear_strict_gap_query_candidate')})
    angle=sum(math.remainder(after['yaw']-before['yaw'],2*math.pi) for before,after in zip(rows,rows[1:]) if before['body_id']==after['body_id'])
    result['net_yaw_in_module_basis_degrees']=angle*180/math.pi
    result['input_first_nonzero_observation']={}
    for name in ('manual','nose_manual','preload','pump'):
        active=next((row for row in rows if row.get(name) is not None and abs(row[name])>1e-6),None)
        result['input_first_nonzero_observation'][name]=(dict(wall_seconds=active['t'],engine_seconds=active['simulation'],value=active[name]) if active else None)
    result['chassis_height_range_m']=max(row['y'] for row in rows)-min(row['y'] for row in rows)
    result['chassis_rise_from_phase_first_sample_m']=max(row['y'] for row in rows)-rows[0]['y']
    takeoffs=[];landings=[]
    for before,after in zip(rows,rows[1:]):
        if before['body_id']!=after['body_id'] or after['t']-before['t']>.1:continue
        stamp=dict(wall_seconds=after['t'],engine_seconds=after['simulation'])
        if before['grounded'] is True and after['grounded'] is False:takeoffs.append(stamp)
        if before['grounded'] is False and after['grounded'] is True:landings.append(stamp)
    result['observed_takeoffs']=takeoffs;result['observed_landings']=landings
    result['single_flight_engine_seconds']=(landings[0]['engine_seconds']-takeoffs[0]['engine_seconds']
        if len(takeoffs)==len(landings)==1 and landings[0]['engine_seconds']>takeoffs[0]['engine_seconds'] else None)
    result['hop_diagnostics']=dict(
        acceptance_samples_available=sum(row.get('hop_request_accepted') is not None for row in rows),
        accepted_requests=[dict(wall_seconds=row['t'],engine_seconds=row['simulation'],kind=row.get('hop_kind'))
            for row in rows if row.get('hop_request_accepted') is True],
        flick_preload_samples_available=sum(row.get('flick_preloading') is not None for row in rows),
        flick_preload_true_samples=sum(row.get('flick_preloading') is True for row in rows),
        pulse_samples_available=sum(row.get('hop_impulse_y') is not None for row in rows),
        nonzero_chassis_pulse_samples=sum(abs(row.get('hop_impulse_y') or 0)>1e-8 for row in rows))
    return result


def interpolate(rows,name,time):
    times=[row['t'] for row in rows];index=bisect.bisect_right(times,time)-1
    if not 0<=index<len(rows)-1:return None
    before,after=rows[index:index+2];a,b=before.get(name),after.get(name)
    if a is None or b is None or before['body_id']!=after['body_id'] or after['t']-before['t']>.1:return None
    return a+(b-a)*(time-before['t'])/(after['t']-before['t'])


def main(args):
    sd=c.read_driver(args.streets_driver,'streets');dd=c.read_driver(args.demo_driver,'demo')
    if sd['scenario']!=dd['scenario'] or sd['timeline_sha256']!=dd['timeline_sha256']:raise ValueError('Matched request timeline required')
    native,invalid=native_rows(args.streets_directory,sd);demo,quality=module_rows(args.demo_directory,args.demo_csv,dd)
    reports=[]
    for name,start,end in phases(sd['scenario']):
        nr=[row for row in native if start<=row['t']<end];dr=[row for row in demo if start<=row['t']<end]
        errors=[]
        for index in range(max(1,math.floor((end-start)*50))):
            time=start+index/50;na=interpolate(nr,'relative_y',time);da=interpolate(dr,'relative_y',time)
            if na is not None and da is not None:errors.append(da-na)
        report=dict(name=name,requested_wall_interval=[start,end],native=describe(nr),demo=describe(dr),
            wall_aligned_relative_y_comparison=dict(covered_50hz_samples=len(errors),
                rmse_m=math.sqrt(statistics.mean(error*error for error in errors)) if errors else None,
                mean_signed_error_m=statistics.mean(errors) if errors else None))
        if sd['scenario']=='controls_probe':
            for key,data in (('native',native),('demo',demo)):
                baseline=describe([row for row in data if start-.2<=row['t']<start])
                report[key]['pre_request_baseline_200ms']=baseline
                report[key]['median_change_from_pre_request_degrees']={field:
                    report[key]['body_state'][field]['median']-baseline['body_state'][field]['median']
                    if baseline.get('body_state',{}).get(field) and report[key]['body_state'].get(field) else None
                    for field in ('chassis_pitch_degrees','rider_pitch_degrees','rider_relative_pitch_degrees','chassis_right_axis_elevation_degrees')}
        reports.append(report)
    result=dict(schema=1,event='matched_actual_controller_coupled_body_comparison',actual_live_controller_recordings=True,
        scenario=sd['scenario'],timeline_sha256=sd['timeline_sha256'],native_invalid_samples=invalid,module_quality=quality,phases=reports,
        sources=[dict(path=str(path),sha256=c.sha(path.read_bytes())) for path in
            (args.streets_directory/'body-physics.jsonl',args.demo_directory/'coupled-physics.jsonl',args.demo_csv,args.streets_driver,args.demo_driver)],
        limits=['Every request/report matches, but native and module engine-clock pacing and effective input staging can differ.',
            'Native Chassis 10 kg and Spring 80 kg are compared with an independent 30 kg aggregate bike and 80 kg rider reduction.',
            'Observed chassis-origin COM correspondence is used for module height; CSV velocity is the aggregate bike body, not a separately integrated 10 kg chassis.',
            'Source world-height offsets are removed for phase rise; source terrain and flat-host contacts remain different.',
            'Wall-aligned relative-Y RMSE includes engine-phase differences; it is not a fitted suspension-law or physical parity score.',
            'No interpolation crosses 100 ms gaps or body/reset identity changes.',
            'Module wheel queries are the actual pre-integration inputs (status 0 unavailable, 1 clear, 2 hit); a hit with gap <= 5 mm is reported only as a strict-gap candidate, not a postintegration grounded flag or full eligibility predicate.',
            'Native actual grounded flags and module supplied-query candidates have distinct solver/callback semantics. Older module recordings lack these wheel queries.',
            'Native Grounded can be overridden false before the hop impulse; reported ground-flag flight intervals are not identical strict-support airborne durations.',
            'Module HopKind values are 0 none, 1 R3, 2 Ollie and 3 Nollie; acceptance is recorded directly and is not inferred from a raw request. Native authoritative hop acceptance is unavailable here.',
            'Chassis and Spring/rider pitch are measured separately; 200 ms pre-request baseline changes retain ongoing pedaling/banking context rather than claiming an isolated manual torque fit.',
            'Native yaw reflects X once into module coordinates; controller requests and orientation responses remain separate observations.'])
    with args.output.open('x') as target:json.dump(result,target,indent=2)
    print(json.dumps(dict(output=str(args.output),scenario=result['scenario'],phases=len(reports),module_quality=quality),indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('streets-directory','streets-driver','demo-directory','demo-driver','demo-csv','output'):parser.add_argument('--'+name,type=Path,required=True)
    main(parser.parse_args())
