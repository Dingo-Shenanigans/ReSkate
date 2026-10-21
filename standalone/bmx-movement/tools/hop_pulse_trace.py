"""Report source multibody momentum and hop support, without fitting a fudge."""
from __future__ import annotations
import argparse
import csv
import importlib.util
import json
from pathlib import Path

spec=importlib.util.spec_from_file_location('hop_trace_math',Path(__file__).with_name('pose_tracks.py'))
poses=importlib.util.module_from_spec(spec);spec.loader.exec_module(poses);c=poses.comparison


def main(args):
    with args.fixture.open(newline='') as source:fixture=list(csv.DictReader(source))
    rows=[]
    with args.physics.open(encoding='utf-8-sig') as source:
        for line in source:
            frame=json.loads(line);index=frame['fixed_index'];bodies=[body for body in frame['bodies'] if body.get('available')]
            if len(bodies)!=8:raise ValueError('Eight selected native physical bodies required')
            by_role={body['role']:body for body in bodies};chassis=by_role['Chassis Body'];spring=by_role['Spring Body']
            mass=sum(body['mass'] for body in bodies);q=poses.qnorm(chassis['rotation']);ground=frame.get('grounded') or {}
            rows.append(dict(index=index,simulation=frame['simulation_seconds'],wall=float(fixture[index]['wall_seconds']),
                mass=mass,com_vy=sum(body['mass']*body['velocity'][1] for body in bodies)/mass,
                com_y=sum(body['mass']*body['world_center_of_mass'][1] for body in bodies)/mass,
                chassis_vy=chassis['velocity'][1],spring_vy=spring['velocity'][1],
                chassis_up_y=poses.rotate(q,[0,1,0])[1],grounded=ground.get('Grounded'),front_grounded=ground.get('_frontEndGrounded'),rear_grounded=ground.get('_backEndGrounded'),
                spring_relative_q=poses.qmul(poses.qinv(q),poses.qnorm(spring['rotation'])),
                source_target_q=frame['drive_connection_joint']['configuration']['target_rotation'],
                at_begin_jump=frame['vehicle_state'].get('_timeAtBeginJump'),at_last_jump=frame['vehicle_state'].get('_timeAtLastJump'),
                per_body_vy={body['role']:body['velocity'][1] for body in bodies}))
    requests=[];previous=False
    for row in fixture:
        current=bool(int(row['buttons'])&128)
        if current and not previous:requests.append(int(row['index']))
        previous=current
    attempts=[]
    for request in requests:
        segment=rows[request:min(len(rows),request+230)];pulse=[]
        for row in segment:
            if row['index']==0:continue
            before=rows[row['index']-1];change=row['com_vy']-before['com_vy']
            if row['index']<request+35 and change>.6:
                pulse.append(dict(row,change_com_vy=change,gravity_corrected_impulse_ns=(change+12.5*.01)*row['mass'],
                    per_body_change_vy={name:row['per_body_vy'][name]-before['per_body_vy'][name] for name in row['per_body_vy']}))
        first=pulse[0]['index'] if pulse else request
        airborne=False;landing=None
        for row in segment:
            if row['grounded'] is False:airborne=True
            elif airborne and row['grounded'] is True:landing=row;break
        attempts.append(dict(raw_request_sample=rows[request],before_first_pulse=rows[max(0,first-1)],observed_pulses=pulse,
            first_recontact=landing,maximum_chassis_vy=max(row['chassis_vy'] for row in segment),
            maximum_com_vy=max(row['com_vy'] for row in segment),
            observed_com_rise=max(row['com_y'] for row in segment)-segment[0]['com_y']))
    result=dict(schema=1,event='native_hop_momentum_and_support_trace',source_physics_sha256=c.sha(args.physics.read_bytes()),
        source_fixture_sha256=c.sha(args.fixture.read_bytes()),attempts=attempts,
        limits=['Source-selected eight-body mass and momentum exclude avatar/ragdoll bodies outside this explicit group.',
            'Pulse candidates are observed COM velocity increments >0.6 m/s within35 fixed samples of raw request.',
            'Gravity correction uses observed12.5 m/s² and10ms; contact/external forces are not isolated by that arithmetic.',
            'Observer callback timing versus physics integration can offset a pulse by one sample.'])
    with args.output.open('x') as target:json.dump(result,target,indent=2)
    print(json.dumps(dict(output=str(args.output),attempts=len(attempts),pulses=[len(attempt['observed_pulses']) for attempt in attempts]),indent=2))


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('physics','fixture','output'):p.add_argument('--'+name,type=Path,required=True)
    main(p.parse_args())
