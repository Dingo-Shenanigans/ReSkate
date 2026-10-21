"""Measure V3 rider-holder/chassis response to the fixed suspension input bench.

Targets and joint drives retain their recorded coordinate/acceleration flags.
No spring equation, drive-force decomposition or control semantics are inferred.
"""
from __future__ import annotations
import argparse
import csv
import importlib.util
import json
import math
from pathlib import Path
import statistics

spec=importlib.util.spec_from_file_location('bmx_suspension_comparison',Path(__file__).with_name('comparison.py'))
comparison=importlib.util.module_from_spec(spec);spec.loader.exec_module(comparison)


def inverse(q):return [-q[0],-q[1],-q[2],q[3]]
def rotate(q,v):
    x,y,z,w=q;u=[x,y,z]
    cross=lambda a,b:[a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]]
    a=cross(u,v);b=cross(u,a)
    return [v[i]+2*(w*a[i]+b[i]) for i in range(3)]
def local(point,origin,q):return rotate(inverse(q),[point[i]-origin[i] for i in range(3)])
def number(value):return comparison.finite(value)
def stats(values):
    values=[value for value in values if value is not None and math.isfinite(value)]
    return (dict(samples=len(values),minimum=min(values),maximum=max(values),median=statistics.median(values),
                 first=values[0],last=values[-1],span=max(values)-min(values)) if values else None)
def jsonlines(path):
    with path.open(encoding='utf-8-sig') as source:
        for line in source:
            if line.strip():yield json.loads(line)


PHASES=[('neutral',0,.7),('RS_Y_negative_0p3',.7,1.2),('neutral_after_0p3',1.2,1.8),
        ('RS_Y_negative_0p6',1.8,2.2),('neutral_after_0p6',2.2,2.8),('RS_Y_negative_full',2.8,3.2),
        ('RS_Y_positive_flick',3.2,3.3),('neutral_after_flick',3.3,6),('R3_request_1',6,6.1),
        ('neutral_after_R3_1',6.1,9),('LS_Y_negative_half',9,9.4),('LS_Y_positive_half',9.4,9.8),
        ('neutral_after_LS',9.8,11),('R3_request_2',11,11.1),('final_neutral',11.1,18)]


def analyze(args):
    driver=comparison.read_driver(args.driver,'streets')
    if driver['scenario']!='suspension':raise ValueError('Expected fixed suspension driver receipt')
    start=comparison.utc(driver['timeline_start_utc']);rows=[];fixed=[];joint_configs={}
    for frame in jsonlines(args.directory/'body-physics.jsonl'):
        fixed.append(frame['fixed_index']);bodies={body['role']:body for body in frame['bodies'] if body.get('available')}
        chassis=bodies.get('Chassis Body');spring=bodies.get('Spring Body')
        if not chassis or not spring:continue
        delta=local(spring['world_center_of_mass'],chassis['world_center_of_mass'],chassis['rotation'])
        relative_v=rotate(inverse(chassis['rotation']),[spring['velocity'][i]-chassis['velocity'][i] for i in range(3)])
        row=dict(fixed_index=frame['fixed_index'],wall_seconds=comparison.utc(frame['utc'])-start,
                 simulation_seconds=frame['simulation_seconds'],spring_relative_x=delta[0],spring_relative_y=delta[1],spring_relative_z=delta[2],
                 spring_minus_chassis_vy_in_chassis_axes=relative_v[1],chassis_world_y=chassis['world_center_of_mass'][1],
                 spring_world_y=spring['world_center_of_mass'][1],preload_input=number(frame.get('input',{}).get('PreloadInput')),
                 manny_input=number(frame.get('input',{}).get('MannyInput')),pump_input=number(frame.get('input',{}).get('PumpInput')),
                 driver_target_y=None,drive_spring=None,drive_damper=None,drive_maximum_force=None,drive_use_acceleration=None)
        targets=[]
        for body in (spring,chassis):
            for joint in body.get('joints',[]):
                cfg=joint.get('configuration');ref=joint.get('reference',{})
                if not cfg:continue
                joint_configs[str(ref.get('id'))]=dict(body_role=body['role'],reference=ref,configuration=cfg,
                    mass_scale=joint.get('mass_scale'),connected_mass_scale=joint.get('connected_mass_scale'))
                if body['role']=='Spring Body':targets.append(cfg)
        # Keep this as an explicitly selected raw Spring Body joint field. Joint
        # axes and anchor offsets are retained in joint_configs for interpretation.
        if len(targets)==1:
            cfg=targets[0];drive=cfg.get('y_drive') or {};position=cfg.get('target_position')
            row.update(driver_target_y=position[1] if position else None,drive_spring=number(drive.get('spring')),
                       drive_damper=number(drive.get('damper')),drive_maximum_force=number(drive.get('maximum_force')),
                       drive_use_acceleration=drive.get('use_acceleration'))
        rows.append(row)
    if not rows:raise ValueError('No observed Spring/Chassis body pairs')
    bones=[];references={}
    for frame in jsonlines(args.directory/'rider.jsonl'):
        t=comparison.utc(frame['utc'])-start;ref=frame.get('pose_reference');row=dict(wall_seconds=t,simulation_seconds=frame['simulation_seconds'])
        if ref:references[str(ref['id'])]={k:ref.get(k) for k in ('id','name','kind','skeleton_bound','binding_evidence','position_role')}
        for name in ('hips','head'):
            bone=frame.get(name);row[name+'_holder_local_y']=None
            if bone and ref and ref.get('skeleton_bound') is True:
                row[name+'_holder_local_y']=local(bone['position'],ref['position'],ref['rotation'])[1]
        nearest=min(rows,key=lambda q:abs(q['wall_seconds']-t))
        row['physics_time_error_seconds']=abs(nearest['wall_seconds']-t)
        for name in ('hips','head'):
            bone=frame.get(name);row[name+'_world_minus_chassis_y']=(bone['position'][1]-nearest['chassis_world_y']
                if bone and row['physics_time_error_seconds']<=.03 else None)
        bones.append(row)
    phases=[]
    for name,a,b in PHASES:
        selected=[row for row in rows if a<=row['wall_seconds']<b];rider=[row for row in bones if a<=row['wall_seconds']<b]
        phases.append(dict(name=name,requested_wall_interval=[a,b],samples=len(selected),
            measurements={key:stats([row[key] for row in selected]) for key in ('spring_relative_y','spring_minus_chassis_vy_in_chassis_axes','driver_target_y','drive_spring','drive_damper','preload_input','manny_input','pump_input')},
            rider_measurements={key:stats([row[key] for row in rider]) for key in ('hips_holder_local_y','head_holder_local_y','hips_world_minus_chassis_y','head_world_minus_chassis_y')}))
    result=dict(schema=1,event='bmx_observed_suspension_response',driver_sha256=comparison.sha(args.driver.read_bytes()),
                physics_sha256=comparison.sha((args.directory/'body-physics.jsonl').read_bytes()),
                rider_sha256=comparison.sha((args.directory/'rider.jsonl').read_bytes()),
                fixed_samples=len(fixed),missing_fixed_indices=sum(max(0,b-a-1) for a,b in zip(fixed,fixed[1:])),
                observed_pose_references=references,phases=phases,recorded_joint_configurations=joint_configs,
                limits=['Named phases are raw input requests; preload/manual/pump semantics are not presumed.',
                        'Spring-relative position uses recorded COMs in chassis axes; target_position is a separate raw joint coordinate.',
                        'Spring-minus-chassis velocity omits rotation-of-offset correction and is labeled as such.',
                        'Drive use_acceleration, force limits, axes and anchors must be considered before interpreting stiffness/damping.',
                        'Bone holder-local response requires verified skeleton binding; missing binding remains unavailable.',
                        'World-minus-chassis bone height includes holder motion and posture and must not be applied twice.',
                        'Callback ordering and native solver state can affect response; this tool does not identify a native spring equation.'])
    args.output.mkdir(parents=True,exist_ok=False)
    (args.output/'response.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    for filename,data in [('spring-trace.csv',rows),('rider-response.csv',bones)]:
        if data:
            with (args.output/filename).open('x',newline='') as target:
                writer=csv.DictWriter(target,fieldnames=list(data[0]));writer.writeheader();writer.writerows(data)
    print(json.dumps(dict(output=str(args.output),fixed_samples=len(fixed),missing_fixed_indices=result['missing_fixed_indices'],pose_references=references),indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('directory',type=Path)
    parser.add_argument('--driver',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    analyze(parser.parse_args())
