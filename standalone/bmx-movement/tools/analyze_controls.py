"""Describe effective native controls without promoting requests to semantics."""
from __future__ import annotations
import argparse
import importlib.util
import json
import math
from pathlib import Path
import statistics

spec=importlib.util.spec_from_file_location('bmx_control_analysis',Path(__file__).with_name('analyze_suspension.py'))
a=importlib.util.module_from_spec(spec);spec.loader.exec_module(a);c=a.comparison


def main(args):
    driver=c.read_driver(args.driver,'streets');scenario=driver['scenario']
    if scenario not in ('controls_probe','air_rotation'):raise ValueError('Expected explicit controls_probe or air_rotation scenario')
    start=c.utc(driver['timeline_start_utc']);rows=[]
    for frame in a.jsonlines(args.directory/'body-physics.jsonl'):
        if frame.get('sample_valid') is False:continue
        bodies={body['role']:body for body in frame['bodies'] if body.get('available')};chassis=bodies.get('Chassis Body');spring=bodies.get('Spring Body')
        if not chassis or not spring:continue
        q=chassis['rotation'];up=a.rotate(q,[0,1,0]);front=a.rotate(q,[0,0,1]);relative=a.local(spring['world_center_of_mass'],chassis['world_center_of_mass'],q)
        rows.append(dict(wall=c.utc(frame['utc'])-start,simulation=frame['simulation_seconds'],input=frame.get('input') or {},
            grounded=frame.get('grounded') or {},speed=math.hypot(chassis['velocity'][0],chassis['velocity'][2]),
            chassis_up_y=up[1],chassis_y=chassis['world_center_of_mass'][1],spring_relative_y=relative[1],
            yaw=math.atan2(front[0],front[2]),angular_velocity=chassis['angular_velocity'],
            target=frame['drive_connection_joint']['configuration']['target_position'],body_id=chassis['reference']['id']))
    result=[];fields=('AccelerationInput','BrakeInput','FrontBrakeInput','RearBrakeInput','PreloadInput','MannyInput','NoseyInput','PumpInput')
    for index in range(4):
        onset=index*8+(2 if scenario=='controls_probe' else 1.74);end=index*8+(3.2 if scenario=='controls_probe' else 2.14)
        selected=[row for row in rows if onset<=row['wall']<=end];context=[row for row in rows if index*8+1.4<=row['wall']<=index*8+6]
        if not selected:continue
        inputs={name:a.stats([c.finite(row['input'].get(name)) for row in selected]) for name in fields}
        for field in ('MoveInput','MoveInputRaw','TrickStickInput','JumpInput'):
            values=[row['input'][field] for row in selected if isinstance(row['input'].get(field),list)]
            inputs[field]=[a.stats([value[i] for value in values]) for i in range(2)] if values else None
        angle=0
        for before,after in zip(context,context[1:]):angle+=math.remainder(after['yaw']-before['yaw'],2*math.pi)
        result.append(dict(attempt_index=index,request_interval_wall_seconds=[onset,end],
            request=(dict(ry=(-13107,13107,-32767,32767)[index]) if scenario=='controls_probe' else
                     dict(axis=('lx','lx','ly','ly')[index],value=(32767,-32767,32767,-32767)[index])),
            samples=len(selected),engine_interval_seconds=[selected[0]['simulation'],selected[-1]['simulation']],
            effective_native_inputs=inputs,horizontal_speed_mps=a.stats([row['speed'] for row in selected]),
            source_grounded_counts={name:sum(row['grounded'].get(name) is True for row in selected) for name in ('Grounded','_frontEndGrounded','_backEndGrounded')},
            context_chassis_up_y=a.stats([row['chassis_up_y'] for row in context]),context_spring_relative_y=a.stats([row['spring_relative_y'] for row in context]),
            context_net_yaw_degrees=angle*180/math.pi,context_angular_velocity=[a.stats([row['angular_velocity'][i] for row in context]) for i in range(3)],
            source_joint_target_xyz=[a.stats([row['target'][i] for row in context]) for i in range(3)],
            body_identity_count=len({row['body_id'] for row in context})))
    report=dict(schema=1,event='native_effective_control_observation',scenario=scenario,attempts=result,
        source_body_physics_sha256=c.sha((args.directory/'body-physics.jsonl').read_bytes()),source_driver_sha256=c.sha(args.driver.read_bytes()),
        limits=['Input requests, effective native input channels and resulting body motion are reported independently.',
            'A nonzero manual input does not establish a sustained manual or landed scoring.',
            'Body orientation excursions include quick-hop/landing dynamics and are not isolated control torque laws.',
            'Every reset starts a separate measurement context; absolute world origins are not compared.'])
    with args.output.open('x') as target:json.dump(report,target,indent=2)
    print(json.dumps(dict(output=str(args.output),scenario=scenario,attempts=len(result)),indent=2))


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('directory','driver','output'):p.add_argument('--'+name,type=Path,required=True)
    main(p.parse_args())
