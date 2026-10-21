"""Source-only measured flight/neutral skeleton report for a captured Streets run.

No input, process inspection or game mutation. Body-local coordinates use the
recorded chassis quaternion; the combined vehicle COM is a second explicit origin.
The report preserves terrain contamination and does not auto-calibrate a module.
"""
from __future__ import annotations
import argparse
import bisect
import importlib.util
import json
import math
from pathlib import Path
import statistics

spec=importlib.util.spec_from_file_location('bmx_comparison_analysis',Path(__file__).with_name('comparison.py'))
comparison=importlib.util.module_from_spec(spec);spec.loader.exec_module(comparison)


def turn(q,v):
    x,y,z,w=q;u=(x,y,z)
    cross=lambda a,b:(a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0])
    a=cross(u,v);b=cross(u,a)
    return [v[i]+2*(w*a[i]+b[i]) for i in range(3)]


def summary(values):
    mid=statistics.median(values)
    return dict(median=mid,minimum=min(values),maximum=max(values),
                median_absolute_deviation=statistics.median(abs(v-mid) for v in values),samples=len(values))


def vector_summary(values):
    return {axis:summary([v[i] for v in values]) for i,axis in enumerate('xyz')}


def engine_speed_fit(rows,start,end):
    samples=[r for r in rows if start<=r['t']<=end and r['simulation_time'] is not None and r['speed'] is not None]
    if len(samples)<3:return None
    xs=[r['simulation_time']-samples[0]['simulation_time'] for r in samples];ys=[r['speed'] for r in samples]
    mx,my=statistics.mean(xs),statistics.mean(ys);variance=sum((x-mx)**2 for x in xs)
    if variance<1e-12:return None
    slope=sum((x-mx)*(y-my) for x,y in zip(xs,ys))/variance;intercept=my-slope*mx
    residual=sum((y-intercept-slope*x)**2 for x,y in zip(xs,ys));total=sum((y-my)**2 for y in ys)
    return dict(samples=len(samples),wall_interval=[samples[0]['t'],samples[-1]['t']],
                engine_interval=[samples[0]['simulation_time'],samples[-1]['simulation_time']],
                first_speed=ys[0],last_speed=ys[-1],minimum_speed=min(ys),maximum_speed=max(ys),
                ols_engine_acceleration_mps2=slope,residual_rmse_mps=math.sqrt(residual/len(samples)),
                r_squared=1-residual/total if total>1e-12 else None,
                chassis_height_span=max(r['y'] for r in samples)-min(r['y'] for r in samples),
                controls={key:sorted({r['raw'].get(key) for r in samples if r['raw'].get(key) is not None}) for key in ('pedal','brake')})


def analyze(args):
    driver=comparison.read_driver(args.driver,'streets')
    rows,source=comparison.read_telemetry(args.directory/'telemetry.csv',driver)
    metrics=comparison.run_metrics(rows,driver['scenario'])
    neutral=[];all_frames=[];times=[r['t'] for r in rows];start=comparison.utc(driver['timeline_start_utc'])
    rider_path=args.directory/'rider.jsonl'
    for line in rider_path.read_text().splitlines():
        frame=json.loads(line);t=comparison.utc(frame['utc'])-start
        i=bisect.bisect_left(times,t);choices=[j for j in (i-1,i) if 0<=j<len(rows)];j=min(choices,key=lambda j:abs(times[j]-t));body=rows[j]
        all_frames.append(frame)
        if not (0<=t<=.5 or 25<=t<=28.5):continue
        if (abs(times[j]-t)>.04 or body['grounded'] is not True or body['speed'] is None or body['speed']>.1
                or body['vy'] is None or abs(body['vy'])>.1):continue
        q=[body[k] for k in ('qx','qy','qz','qw')]
        if any(v is None for v in q):continue
        n=math.sqrt(sum(v*v for v in q));q=[-q[0]/n,-q[1]/n,-q[2]/n,q[3]/n]
        origin=[body[k] for k in ('x','y','z')];combined=[body[k] for k in ('vehicle_com_x','vehicle_com_y','vehicle_com_z')]
        measured=dict(t=t,maximum_telemetry_time_error_seconds=abs(times[j]-t),chassis_local={},combined_com_local={},world={},bone_targets=frame.get('bones',{}))
        targets={name:frame.get(name) for name in ('hips','head','left_hand','right_hand','left_foot','right_foot','left_knee','right_knee','left_elbow','right_elbow')}
        targets.update({'bone:'+name:value for name,value in frame.get('bones',{}).items()})
        for name,target in targets.items():
            if not target or len(target.get('position',[]))!=3:continue
            point=target['position'];measured['world'][name]=point
            measured['chassis_local'][name]=turn(q,[point[k]-origin[k] for k in range(3)])
            if all(v is not None for v in combined):measured['combined_com_local'][name]=turn(q,[point[k]-combined[k] for k in range(3)])
        neutral.append(measured)
    if not neutral:raise ValueError('No supported neutral, low-speed skeleton samples found')
    local={}
    for origin in ('chassis_local','combined_com_local'):
        names={name for frame in neutral for name in frame[origin]}
        local[origin]={name:vector_summary([f[origin][name] for f in neutral if name in f[origin]]) for name in sorted(names)}
    pairs=[('left_forearm','left_elbow','left_hand'),('right_forearm','right_elbow','right_hand'),
           ('left_shin','left_knee','left_foot'),('right_shin','right_knee','right_foot'),
           ('hips_center_to_head','hips','head'),('hand_separation','left_hand','right_hand'),
           ('hips_center_to_left_knee_proxy','hips','left_knee'),('hips_center_to_right_knee_proxy','hips','right_knee')]
    distances={}
    for label,a,b in pairs:
        values=[math.dist(f['world'][a],f['world'][b]) for f in neutral if a in f['world'] and b in f['world']]
        if values:distances[label]=summary(values)
    exact_pairs=[('left_upper_arm','LeftUpperArm','LeftLowerArm'),('right_upper_arm','RightUpperArm','RightLowerArm'),
                 ('left_upper_leg','LeftUpperLeg','LeftLowerLeg'),('right_upper_leg','RightUpperLeg','RightLowerLeg'),
                 ('left_clavicle','LeftShoulder','LeftUpperArm'),('right_clavicle','RightShoulder','RightUpperArm'),
                 ('hip_joint_separation','LeftUpperLeg','RightUpperLeg'),('upper_arm_root_separation','LeftUpperArm','RightUpperArm')]
    for label,a,b in exact_pairs:
        values=[math.dist(f['world']['bone:'+a],f['world']['bone:'+b]) for f in neutral if 'bone:'+a in f['world'] and 'bone:'+b in f['world']]
        if values:distances[label]=summary(values)
    edges={}
    for frame in neutral:
        bones=frame['bone_targets'];ids={v['id']:name for name,v in bones.items() if v}
        for name,bone in bones.items():
            if bone and bone.get('parent_id') in ids:
                parent=ids[bone['parent_id']];key=parent+' -> '+name
                edges.setdefault(key,[]).append(math.dist(bone['position'],bones[parent]['position']))
    phase_quality={}
    phase_intervals={'full':[('pedal',.5,4.5),('coast',4.5,5.2),('brake',5.2,8.5)],
                     'flat_short':[('pedal',.5,2.5),('coast',2.5,3.),('brake',3.,6.)],
                     'accel_brake':[('pedal',.5,4.5),('coast',4.5,6.),('brake',6.,9.)]}
    for name,a,b in phase_intervals.get(driver['scenario'],[]):
        values=[r for r in rows if a<=r['t']<=b]
        if values:
            phase_quality[name]=dict(samples=len(values),chassis_height_span_m=max(r['y'] for r in values)-min(r['y'] for r in values),
                net_chassis_height_change_m=values[-1]['y']-values[0]['y'],maximum_absolute_vertical_speed_mps=max(abs(r['vy']) for r in values),
                observed_clear_samples=sum(r['grounded'] is False for r in values),
                horizontal_distance_m=sum(math.hypot(q['x']-p['x'],q['z']-p['z']) for p,q in zip(values,values[1:])),
                note='Near-level body trajectory is a calibration candidate, not proof of surface geometry/normal.')
    result=dict(schema=1,event='bmx_streets_source_only_measurements',source=source,
                driver_sha256=comparison.sha(args.driver.read_bytes()),rider_source_sha256=comparison.sha(rider_path.read_bytes()),
                acceleration_calibration_eligible=not args.terrain_contaminated,
                terrain_contaminated=args.terrain_contaminated,
                exclusion_reason='Nearby ledge/collision contaminates acceleration/braking; these phases must not set tuning.' if args.terrain_contaminated else None,
                raw_observed_metrics=metrics,
                phase_trajectory_quality=phase_quality,
                neutral_skeleton=dict(selected_samples=len(neutral),windows_seconds=[[0,.5],[25,28.5]],
                    low_speed_gate_mps=.1,maximum_telemetry_time_error_seconds=max(f['maximum_telemetry_time_error_seconds'] for f in neutral),
                    observed_sample_times=[f['t'] for f in neutral],bone_landmark_distances_m=distances,body_local_joint_positions_m=local,
                    captured_bone_count=max(len(f.get('bones',{})) for f in all_frames),
                    captured_parent_child_world_lengths_m={name:summary(values) for name,values in sorted(edges.items())},
                    local_frame='Inverse recorded chassis Hamilton XYZW rotation; origins explicitly chassis world COM or recorded combined vehicle COM.',
                    native_left_hand_x_less_than_right_hand_x=(local['chassis_local']['left_hand']['x']['median']<local['chassis_local']['right_hand']['x']['median']),
                    anchors_available_frames=sum(f.get('anchors') is not None for f in all_frames),
                    pose_is_complete_rig_alignment=False),
                limits=['All results are observed estimates, not original movement source or full parity proof.',
                        'Chassis velocity is a component of a multi-body vehicle; it is not the combined body velocity.',
                        'Hop velocity intercept at input-request time is an extrapolation and is not a measured launch impulse.',
                        'Early hop can include staged preload/launch forces; the interior flight fit omits transition edges.',
                        'Hip-center-to-knee lengths are proxies; exact upper-leg origins are separately measured when the expanded bones dictionary is present.',
                        'Wrist/foot humanoid bone positions are not proven grip/pedal contact targets.',
                        'Root/bone handedness and target skeleton mapping need explicit retargeting; body-local output is not full rig alignment.',
                        'Ground/obstacle effects remain possible even in an interior flight fit; repeat on a clear surface before calibration.'])
    if driver['scenario'] in ('full','flat_short'):
        result['longitudinal_engine_clock_fits']={'early_pedal':engine_speed_fit(rows,.7,2.3)}
    if driver['scenario']=='flat_short':
        intervals={'moving_brake':(3.08,4.3),'brake_high_speed':(3.08,3.4),
                   'brake_middle_speed':(3.4,3.9),'brake_low_speed':(3.9,4.3)}
        result['longitudinal_engine_clock_fits'].update({name:engine_speed_fit(rows,*window) for name,window in intervals.items()})
        result['brake_fit_note']='Fixed observed subwindows; moving-only fit omits standing time. Interpret separate slopes as empirical speed-dependent behavior, not a recovered brake equation.'
        trace=[]
        for step in range(31):
            target=3+step*.05;r=min(rows,key=lambda r:abs(r['t']-target))
            if abs(r['t']-target)<=.05:
                trace.append({key:r[key] for key in ('t','simulation_time','speed','y','vehicle_com_y')})
        result['observed_brake_speed_trace']=trace
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with args.output.open('x') as target:json.dump(result,target,indent=2)
    print(json.dumps(dict(output=str(args.output),source=source,hop_phases=metrics.get('hop_request_phases'),
                         neutral_skeleton_samples=len(neutral),bone_distances=distances),indent=2))


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('directory',type=Path);p.add_argument('--driver',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True);p.add_argument('--terrain-contaminated',action='store_true')
    analyze(p.parse_args())
