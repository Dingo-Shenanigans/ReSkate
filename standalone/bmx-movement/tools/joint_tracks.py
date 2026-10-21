"""Export observed actuator commands, distinct from body/animation trajectories.

Each native joint command uses the documented inverse/reflected convention.
No measured body position or velocity is supplied to runtime physics.
"""
from __future__ import annotations
import argparse
import importlib.util
import json
import math
from pathlib import Path
import statistics

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,Path(__file__).with_name(path))
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
poses=module('bmx_joint_pose_math','pose_tracks.py');c=poses.comparison
driver_tools=module('bmx_joint_fixed_driver','drive_target.py')


def load(path,driver):
    start=c.utc(driver['timeline_start_utc']);rows=[];rejected=0
    with path.open(encoding='utf-8-sig') as source:
        for line in source:
            if not line.strip():continue
            frame=json.loads(line)
            if frame.get('sample_valid') is False:rejected+=1;continue
            bodies={body['role']:body for body in frame['bodies'] if body.get('available')}
            rider=bodies.get('Spring Body');chassis=bodies.get('Chassis Body');joint=frame.get('drive_connection_joint')
            if not rider or not chassis or not joint:rejected+=1;continue
            cfg=joint.get('configuration') or {};position=cfg.get('target_position');q=cfg.get('target_rotation')
            if not position or poses.qnorm(q) is None:rejected+=1;continue
            if abs(sum(value*value for value in q)-1)>1e-5:raise ValueError('Observed target quaternion is not unit length')
            if (cfg.get('configured_in_world_space') or cfg.get('swap_bodies') or cfg.get('rotation_drive_mode')!='Slerp'
                    or any(abs(value)>1e-8 for key in ('target_velocity','target_angular_velocity') for value in cfg[key])):
                raise ValueError('Unsupported native target convention or nonzero target velocity')
            iq=rider['inertia_rotation']
            if poses.qnorm(iq) is None or abs(sum(value*value for value in iq)-1)>1e-5:raise ValueError('Invalid native principal inertia orientation')
            rows.append(dict(wall=c.utc(frame['utc'])-start,simulation=frame['simulation_seconds'],fixed_index=frame['fixed_index'],
                identity=[chassis['reference']['id'],rider['reference']['id'],joint['reference']['id']],
                command=dict(target_position=[position[0],-position[1],-position[2]],
                    target_rotation=[-q[0],q[1],q[2],q[3]],
                    rider_inertia=rider['inertia'],rider_inertia_orientation=[iq[0],-iq[1],-iq[2],iq[3]],
                    linear_spring=[cfg[axis+'_drive']['spring'] for axis in ('x','y','z')],
                    linear_damper=[cfg[axis+'_drive']['damper'] for axis in ('x','y','z')],
                    linear_acceleration=[cfg[axis+'_drive']['use_acceleration'] for axis in ('x','y','z')],
                    source_linear_maximum_effort=[cfg[axis+'_drive']['maximum_force'] for axis in ('x','y','z')],
                    slerp_spring=cfg['slerp_drive']['spring'],slerp_damper=cfg['slerp_drive']['damper'],
                    slerp_acceleration=cfg['slerp_drive']['use_acceleration'],
                    source_slerp_maximum_effort=cfg['slerp_drive']['maximum_force'],radial_limit=cfg['linear_limit']['limit']),
                source_input=frame.get('input'),source_vehicle_state=frame.get('vehicle_state'),grounded=frame.get('grounded'),bike_articulation=frame.get('bike_control')))
    return rows,rejected


def neutral_command(rows):
    result={}
    for name in rows[0]['command']:
        values=[row['command'][name] for row in rows]
        if name in ('target_rotation','rider_inertia_orientation'):result[name]=poses.medoid(values)
        elif isinstance(values[0],list):
            result[name]=([statistics.median(value[index] for value in values) for index in range(len(values[0]))]
                          if not isinstance(values[0][0],bool) else values[-1])
        elif isinstance(values[0],bool):result[name]=values[-1]
        else:result[name]=statistics.median(values)
    return result


def export(args):
    if args.poses is None:raise ValueError('--poses is required for a trick command export')
    driver=c.read_driver(args.driver,'streets');bundle=json.loads(args.poses.read_text(encoding='utf-8-sig'))
    if bundle.get('driver_sha256')!=c.sha(args.driver.read_bytes()):raise ValueError('Pose bundle/driver mismatch')
    rows,rejected=load(args.directory/'body-physics.jsonl',driver);attempts=driver_tools.matrix_attempts(driver['scenario'])
    if not attempts:raise ValueError('Expected fixed matrix or retry scenario')
    tracks=[];excluded=[]
    for track in bundle['tracks']:
        pair=track['set'],track['slot'];fire=track['fire_observation'];clock=track['fire_engine_seconds']
        if (track.get('observation_window') or {}).get('partial'):
            excluded.append(dict(set=pair[0],slot=pair[1],reason='Partial native posture/vehicle window'));continue
        attempt=next((attempt for attempt in attempts if (attempt['set'],attempt['slot'])==pair and
                      attempt['selection_start_seconds']-.1<=fire['t']<attempt['end_seconds']),None)
        if not attempt:raise ValueError('No fixed request window for observed fire')
        start=(attempt['reset_release_seconds']+.3 if attempt.get('reset_release_seconds') is not None else attempt['start_seconds'])
        neutral=[row for row in rows if start<=row['wall']<attempt['hop_request_seconds']-.05]
        idle=next((phase for phase in track.get('phase_observations',[]) if phase.get('from_phase')==5 and phase.get('to_phase')==0),None)
        last_clock=idle['simulation_seconds']+.15 if idle and idle.get('simulation_seconds') is not None else None
        selected=[row for row in rows if fire['t']-.15<=row['wall']<attempt['end_seconds'] and
                  (last_clock is None or row['simulation']<=last_clock)]
        identities={tuple(row['identity']) for row in neutral+selected}
        if len(neutral)<2 or len(selected)<3 or len(identities)!=1:
            excluded.append(dict(set=pair[0],slot=pair[1],reason='Insufficient samples or physical joint identity transition'));continue
        reference=neutral_command(neutral)
        tracks.append(dict(id=pair[0]*10+pair[1],set=pair[0],slot=pair[1],name=track['name'],phase='timeline',
            time_origin='Verified native _timeAtLastFire engine time',origin_engine_seconds=clock,
            request_amplitude=1,source_identity=selected[0]['identity'],reference=reference,
            neutral_samples=len(neutral),phase_observations=track.get('phase_observations'),
            keys=[dict(seconds=row['simulation']-clock,command=row['command'],source_fixed_index=row['fixed_index']) for row in selected]))
    result=dict(schema=1,event='bmx_observed_joint_actuation_tracks',target='streets',actual_controller_recording=False,
        source_body_physics_sha256=c.sha((args.directory/'body-physics.jsonl').read_bytes()),
        source_pose_bundle_sha256=c.sha(args.poses.read_bytes()),driver_sha256=c.sha(args.driver.read_bytes()),
        timeline_sha256=driver['timeline_sha256'],rejected_physics_samples=rejected,tracks=tracks,excluded_tracks=excluded,
        coordinate_mapping=dict(target_position='-mirrorX(native joint target)',target_rotation='inverse(mirrorX(native joint target quaternion))',
            inertia='Native principal moments with once-reflected inertia orientation'),
        limits=['Actuator observations are not measured positions/velocities and do not prescribe a body trajectory.',
            'Commands include the captured stationary quick-hop context and coupled base animation; they are not isolated universal clips.',
            'Reference command is the settled per-attempt neutral median/quaternion medoid.',
            'Original acceleration flags and effectively uncapped source effort values are retained; runtime solver limits are independent.',
            'Per-trick full-window sampling requires the original confirmed native fire and unchanged physical joint identity.'])
    with args.output.open('x') as target:json.dump(result,target,indent=2)
    print(json.dumps(dict(output=str(args.output),tracks=len(tracks),excluded=excluded,rejected=rejected),indent=2))


def bench(args):
    driver=c.read_driver(args.driver,'streets')
    if driver['scenario']!='suspension':raise ValueError('Expected suspension input bench')
    rows,rejected=load(args.directory/'body-physics.jsonl',driver);start=c.utc(driver['timeline_start_utc'])
    reports=[dict(wall=c.utc(report['utc'])-start,request=report['requested']) for report in driver['reports']]
    hops=[];previous=0
    for report in reports:
        pressed=report['request']['buttons']&128
        if pressed and not previous:hops.append(report['wall'])
        previous=pressed
    negative=next(report['wall'] for report in reports if report['request']['ly']<0)
    positive=next(report['wall'] for report in reports if report['request']['ly']>0)
    released=next(report['wall'] for report in reports if report['wall']>positive and report['request']['ly']==0)
    at=lambda wall:next(row['simulation'] for row in rows if row['wall']>=wall)
    tracks=[]
    for identity,phase,name,wall,end in [(80,'timeline','first observed R3 quick hop',hops[0],None),
        (81,'enter','negative LS pump request',negative,positive),
        (82,'transition','negative-to-positive LS request',positive,released),
        (82,'recover','post-positive neutral recovery',released,hops[1]-.1)]:
        clock=at(wall);end_clock=clock+1.8 if end is None else at(end)
        neutral_origin=hops[0] if identity==80 else negative
        neutral=[row for row in rows if neutral_origin-.5<=row['wall']<neutral_origin-.05]
        selected=[row for row in rows if clock-.03<=row['simulation']<=end_clock]
        source_amplitudes=[abs(row['source_input'].get('PumpInput') or 0) for row in selected if row.get('source_input')]
        amplitude=max(source_amplitudes) if identity!=80 else 1
        tracks.append(dict(id=identity,name=name,phase=phase,origin_engine_seconds=clock,
            time_origin='First observed fixed callback at or after matching raw driver request UTC',
            raw_request_wall_seconds=wall,request_amplitude=amplitude,reference=neutral_command(neutral),
            source_identity=selected[0]['identity'],neutral_samples=len(neutral),
            keys=[dict(seconds=row['simulation']-clock,command=row['command'],source_fixed_index=row['fixed_index']) for row in selected]))
    result=dict(schema=1,event='bmx_observed_joint_actuation_tracks',target='streets',actual_controller_recording=False,
        driver_sha256=c.sha(args.driver.read_bytes()),source_body_physics_sha256=c.sha((args.directory/'body-physics.jsonl').read_bytes()),
        timeline_sha256=driver['timeline_sha256'],rejected_physics_samples=rejected,tracks=tracks,excluded_tracks=[],
        held_out_hop_raw_request_wall_seconds=hops[1],
        limits=['Only the first R3 trajectory is used for command fitting; the second R3 remains held out.',
            'Pump-negative is followed immediately by pump-positive; an isolated negative-to-neutral recovery is unavailable.',
            'The positive transition and its subsequent neutral recovery retain their exact observed context.',
            'Raw request UTC is mapped to the first actual native fixed callback, not to an inferred native coroutine start.',
            'These are joint commands and inertia, never body positions or velocities.'])
    with args.output.open('x') as target:json.dump(result,target,indent=2)
    print(json.dumps(dict(output=str(args.output),tracks=len(tracks),rejected=rejected),indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('directory','driver','output'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--poses',type=Path);parser.add_argument('--bench',action='store_true')
    args=parser.parse_args();(bench if args.bench else export)(args)
