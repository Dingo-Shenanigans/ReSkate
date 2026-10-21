"""Read-only V3 measured-drive fixture and headless CoupledWorld comparison.

No controller is opened. Per-step joint targets/inertia are observations, not
an inferred autonomous controller. Native and replay initial states differ.
"""
from __future__ import annotations
import argparse
import bisect
import csv
import importlib.util
import json
import math
from pathlib import Path
import statistics
import subprocess

spec=importlib.util.spec_from_file_location('bmx_suspension_analysis',Path(__file__).with_name('analyze_suspension.py'))
analysis=importlib.util.module_from_spec(spec);spec.loader.exec_module(analysis)
comparison=analysis.comparison
PREFIX=['index','wall_seconds','simulation_seconds','fixed_dt','buttons','lx','ly','rx','ry','lt','rt']
TARGET=['target_x','target_y','target_z','target_qx','target_qy','target_qz','target_qw']
DRIVES=[axis+'_'+field for axis in ('x','y','z','angular') for field in ('spring','damper','maximum_effort','acceleration')]
INERTIA=['linear_limit','inertia_x','inertia_y','inertia_z','inertia_qx','inertia_qy','inertia_qz','inertia_qw']
NATIVE=['native_relative_x','native_relative_y','native_relative_z','native_chassis_y','native_spring_y',
        'native_chassis_vy','native_spring_vy','native_grounded','report_index','report_age_seconds']
FIELDS=PREFIX+TARGET+DRIVES+INERTIA+NATIVE


def fixture(args):
    driver=comparison.read_driver(args.driver,'streets');start=comparison.utc(driver['timeline_start_utc'])
    if driver['scenario']!='suspension':raise ValueError('Expected suspension scenario')
    reports=driver['reports'];times=[comparison.utc(report['utc'])-start for report in reports]
    if times!=sorted(times):raise ValueError('Nonmonotonic driver reports')
    result=[];hop_trace=[];begin=None;last_force=None
    for frame in analysis.jsonlines(args.directory/'body-physics.jsonl'):
        if frame.get('sample_valid') is False:raise ValueError('Missing body sample; contiguous bench required')
        bodies={body['role']:body for body in frame['bodies'] if body.get('available')}
        chassis=bodies.get('Chassis Body');spring=bodies.get('Spring Body');joint=frame.get('drive_connection_joint')
        if not chassis or not spring or not joint:raise ValueError('Required native pair/joint unavailable')
        index=frame['fixed_index'];wall=comparison.utc(frame['utc'])-start
        if index!=len(result):raise ValueError('Noncontiguous source fixed indices')
        sim=frame['simulation_seconds']
        if result and abs(sim-result[-1]['simulation_seconds']-.01)>2e-4:raise ValueError('Native fixed clock gap')
        report_index=bisect.bisect_right(times,wall)-1
        if report_index<0:raise ValueError('Native sample precedes neutral bootstrap')
        cfg=joint['configuration'];target=cfg['target_position'];q=cfg['target_rotation']
        if any(abs(v)>1e-9 for key in ('target_velocity','target_angular_velocity') for v in cfg[key]):
            raise ValueError('Nonzero target velocity requires separately verified joint convention')
        if cfg['rotation_drive_mode']!='Slerp' or cfg['configured_in_world_space'] or cfg['swap_bodies']:
            raise ValueError('Unsupported observed joint basis')
        relative=analysis.local(spring['world_center_of_mass'],chassis['world_center_of_mass'],chassis['rotation'])
        iq=spring['inertia_rotation'];request=reports[report_index]['requested']
        row=dict(index=index,wall_seconds=wall,simulation_seconds=sim,fixed_dt=.01,
                 **{key:request[key] for key in ('buttons','lx','ly','rx','ry','lt','rt')},
                 target_x=target[0],target_y=-target[1],target_z=-target[2],
                 target_qx=-q[0],target_qy=q[1],target_qz=q[2],target_qw=q[3],
                 linear_limit=cfg['linear_limit']['limit'],
                 inertia_x=spring['inertia'][0],inertia_y=spring['inertia'][1],inertia_z=spring['inertia'][2],
                 inertia_qx=iq[0],inertia_qy=-iq[1],inertia_qz=-iq[2],inertia_qw=iq[3],
                 native_relative_x=-relative[0],native_relative_y=relative[1],native_relative_z=relative[2],
                 native_chassis_y=chassis['world_center_of_mass'][1],native_spring_y=spring['world_center_of_mass'][1],
                 native_chassis_vy=chassis['velocity'][1],native_spring_vy=spring['velocity'][1],
                 native_grounded=int(frame['grounded']['Grounded']),report_index=report_index,
                 report_age_seconds=wall-times[report_index])
        for axis,key in (('x','x_drive'),('y','y_drive'),('z','z_drive'),('angular','slerp_drive')):
            drive=cfg[key]
            row.update({axis+'_spring':drive['spring'],axis+'_damper':drive['damper'],
                        axis+'_maximum_effort':drive['maximum_force'],axis+'_acceleration':int(drive['use_acceleration'])})
        result.append(row)
        state=frame.get('vehicle_state',{});at_begin=state.get('_timeAtBeginJump',0)
        if begin is None and at_begin:begin=at_begin
        force=state.get('_lastJumpForce')
        hop_trace.append(dict(index=index,wall_seconds=wall,simulation_seconds=sim,
                              seconds_from_first_begin=sim-begin if begin else None,
                              source_target_x=target[0],source_target_y=target[1],source_target_z=target[2],
                              spring_relative_y=relative[1],world_com_y_gap=spring['world_center_of_mass'][1]-chassis['world_center_of_mass'][1],
                              native_chassis_vy=chassis['velocity'][1],native_spring_vy=spring['velocity'][1],
                              at_begin_jump=at_begin,at_last_jump=state.get('_timeAtLastJump'),
                              last_jump_force_y=force[1] if force else None))
    if len(result)<20:raise ValueError('Insufficient samples')
    with args.output.open('x',newline='') as target:
        writer=csv.DictWriter(target,fieldnames=FIELDS);writer.writeheader();writer.writerows(result)
    trace=[row for row in hop_trace if begin is not None and -.2<=row['simulation_seconds']-begin<=.5]
    trace_path=args.output.with_suffix('.first-hop.csv')
    with trace_path.open('x',newline='') as target:
        writer=csv.DictWriter(target,fieldnames=trace[0]);writer.writeheader();writer.writerows(trace)
    manifest=dict(schema=1,event='native_observed_suspension_fixture',actual_controller_recording=False,
        fixture_sha256=comparison.sha(args.output.read_bytes()),driver_sha256=comparison.sha(args.driver.read_bytes()),
        source_body_physics_sha256=comparison.sha((args.directory/'body-physics.jsonl').read_bytes()),
        scenario=driver['scenario'],timeline_sha256=driver['timeline_sha256'],samples=len(result),fixed_seconds=.01,
        first_begin_jump_engine_seconds=begin,first_hop_trace=str(trace_path),
        maximum_matching_report_age_during_timeline_seconds=max(row['report_age_seconds'] for row in result if row['wall_seconds']<=18),
        coordinate_mapping=dict(position='Native rider-minus-chassis COM in chassis axes: reflect X once.',
            target_position='Opposite joint convention: -mirrorX(native target).',
            target_rotation='inverse(mirrorX(native target quaternion)).',
            rider_inertia='Native principal values; mirrorX(inertia orientation).'),
        limits=['Measured-drive replay is headless and does not prove autonomous target generation or controller parity.',
            'Factory initial state and terrain remain independent; native world position is compared as height delta.',
            'Native Float.MaxValue drive limits are retained in the fixture; replay bounds drive effort at 1 MN and net force/torque at the host profile caps.',
            'Source radial hard constraint is represented by an explicit bounded dissipative radial soft stop.',
            'Native fixed callback phase versus integration can differ by one step.',
            'Driver reports map by UTC to each fixed sample; catch-up callbacks may share a raw request.'])
    args.output.with_suffix('.fixture.json').write_text(json.dumps(manifest,indent=2))
    print(json.dumps(manifest,indent=2))


def rms(values):return math.sqrt(statistics.mean(value*value for value in values)) if values else None


def analyze(args):
    with args.fixture.open(newline='') as source:source_rows=list(csv.DictReader(source))
    with args.replay.open(newline='') as source:replay_rows=list(csv.DictReader(source))
    if len(source_rows)!=len(replay_rows):raise ValueError('Replay length differs')
    rows=[];invalid=[];nonconverged=[]
    for index,(source,replay) in enumerate(zip(source_rows,replay_rows)):
        if int(replay['source_index'])!=index:raise ValueError('Reordered replay')
        if replay['valid']!='1':
            invalid.append(dict(index=index,simulation_seconds=float(source['simulation_seconds']),
                wall_seconds=float(source['wall_seconds']),suspension_issue=replay.get('suspension_issue'),bike_issue=replay.get('bike_issue'),
                coupling_residual=comparison.finite(replay.get('coupling_residual'))))
            if not args.allow_invalid:raise ValueError('Invalid replay; --allow-invalid is diagnostic and excludes invalid samples explicitly')
            continue
        if replay.get('converged')!='1':
            nonconverged.append(dict(index=index,simulation_seconds=float(source['simulation_seconds']),wall_seconds=float(source['wall_seconds']),coupling_residual=comparison.finite(replay.get('coupling_residual'))))
            if not args.allow_invalid:raise ValueError('Nonconverged replay; diagnostic mode is required')
            continue
        row={key:float(value) for key,value in source.items()}
        row.update({'replay_'+key:float(value) for key,value in replay.items() if key!='source_index'})
        rows.append(row)
    phases={}
    for name,start,end in [('neutral',.2,.65),('RS_request',.7,3.3),('hop_1',6,8.9),('LS_pump_request',9,10.8),('hop_2',11,17.8)]:
        selected=[row for row in rows if start<=row['wall_seconds']<=end]
        if not selected:continue
        errors=[row['replay_relative_y']-row['native_relative_y'] for row in selected]
        native_base=selected[0]['native_chassis_y'];replay_base=selected[0]['replay_chassis_y']
        phases[name]=dict(samples=len(selected),engine_seconds=selected[-1]['simulation_seconds']-selected[0]['simulation_seconds'],
            invalid_samples_excluded=sum(start<=row['wall_seconds']<=end for row in invalid),
            nonconverged_samples_excluded=sum(start<=row['wall_seconds']<=end for row in nonconverged),
            relative_y_rmse_m=rms(errors),relative_y_mean_signed_error_m=statistics.mean(errors),
            native_relative_y_range_m=[min(row['native_relative_y'] for row in selected),max(row['native_relative_y'] for row in selected)],
            replay_relative_y_range_m=[min(row['replay_relative_y'] for row in selected),max(row['replay_relative_y'] for row in selected)],
            native_chassis_rise_m=max(row['native_chassis_y'] for row in selected)-native_base,
            replay_chassis_rise_m=max(row['replay_chassis_y'] for row in selected)-replay_base,
            native_peak_chassis_vy_mps=max(row['native_chassis_vy'] for row in selected),
            replay_peak_chassis_vy_mps=max(row['replay_chassis_vy'] for row in selected))
    result=dict(schema=1,event='headless_measured_suspension_replay_comparison',actual_controller_recording=False,
        diagnostic_only=bool(invalid or nonconverged),invalid_samples=invalid,nonconverged_samples=nonconverged,valid_converged_samples=len(rows),
        fixture_sha256=comparison.sha(args.fixture.read_bytes()),replay_sha256=comparison.sha(args.replay.read_bytes()),
        phases=phases,limits=['Per-source-fixed-sample schedule; not actual live controller pacing.',
            'No native initial multibody state injection, joint projection or exact PhysX hard constraint.',
            'Measured target replay is identification evidence, not autonomous control recovery.',
            'Cross-session hop measurements differ; use this source run for this numerical bench.'])
    with args.output.open('x') as target:json.dump(result,target,indent=2)
    print(json.dumps(result,indent=2))


def sweep(args):
    delays=[float(value) for value in args.delays.split(',')]
    if not 1<=len(delays)<=8 or any(not math.isfinite(value) or not 0<=value<=.3 for value in delays):
        raise ValueError('Expected 1..8 finite latency candidates in 0..0.3 seconds')
    args.output.mkdir(parents=True,exist_ok=False);results=[]
    for delay in delays:
        path=args.output/f'delay-{delay:.3f}.csv'
        command=[str(args.probe.resolve(strict=True)),str(args.fixture.resolve(strict=True)),str(path.resolve()),'--hop-delay',str(delay)]
        run=subprocess.run(command,capture_output=True,text=True,timeout=30)
        if run.returncode not in (0,2):raise RuntimeError(run.stderr)
        receipt=json.loads(run.stderr.strip().splitlines()[-1])
        if receipt.get('actual_controller_recording') is not False:raise ValueError('Headless receipt required')
        report_args=argparse.Namespace(fixture=args.fixture,replay=path,output=path.with_suffix('.json'),allow_invalid=True)
        analyze(report_args);report=json.loads(report_args.output.read_text())
        results.append(dict(hop_delay_seconds=delay,receipt=receipt,phases=report['phases'],
                            diagnostic_only=report['diagnostic_only'],report=str(report_args.output)))
    summary=dict(schema=1,event='headless_measured_target_hop_latency_sweep',actual_controller_recording=False,
        fixture_sha256=comparison.sha(args.fixture.read_bytes()),probe_sha256=comparison.sha(args.probe.read_bytes()),results=results,
        limits=['Pipeline latency candidates are empirical; they are not the recovered coroutine wait in isolation.',
                'Any invalid sample makes its candidate diagnostic only; covered-sample metrics do not certify a complete trajectory.',
                'Measured targets/inertia do not establish autonomous target-generation parity.'])
    (args.output/'sweep.json').write_text(json.dumps(summary,indent=2));print(json.dumps(summary,indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);sub=parser.add_subparsers(dest='command',required=True)
    p=sub.add_parser('fixture');p.add_argument('--directory',type=Path,required=True);p.add_argument('--driver',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.set_defaults(run=fixture)
    p=sub.add_parser('analyze');p.add_argument('--fixture',type=Path,required=True);p.add_argument('--replay',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--allow-invalid',action='store_true');p.set_defaults(run=analyze)
    p=sub.add_parser('sweep');p.add_argument('--fixture',type=Path,required=True);p.add_argument('--probe',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--delays',default='.10,.11,.12,.13');p.set_defaults(run=sweep)
    args=parser.parse_args();args.run(args)
