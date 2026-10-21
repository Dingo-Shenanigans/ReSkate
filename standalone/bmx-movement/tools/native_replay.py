"""Build native sampled-input fixtures and analyze a C++ headless replay.

This never sends inputs or connects a controller. Replay is explicitly a numeric
bench, not an actual controller recording or a substitute for matched video.
"""
from __future__ import annotations
import argparse
import bisect
import csv
import importlib.util
import json
import math
from pathlib import Path
import subprocess

spec=importlib.util.spec_from_file_location('bmx_replay_comparison',Path(__file__).with_name('comparison.py'))
comparison=importlib.util.module_from_spec(spec);spec.loader.exec_module(comparison)
spec=importlib.util.spec_from_file_location('bmx_replay_source_analysis',Path(__file__).with_name('analyze_reference_run.py'))
analysis=importlib.util.module_from_spec(spec);spec.loader.exec_module(analysis)

FIELDS=['index','wall_seconds','simulation_seconds','fixed_dt','buttons','lx','ly','rx','ry','lt','rt',
        'native_speed','native_chassis_y','native_vehicle_com_y','native_vy','native_grounded','report_index','report_age_seconds']


def fixture(args):
    driver=comparison.read_driver(args.driver,'streets')
    if driver['scenario']!='flat_short':raise ValueError('This calibration fixture requires flat_short')
    rows,meta=comparison.read_telemetry(args.telemetry,driver);start=comparison.utc(driver['timeline_start_utc'])
    reports=driver['reports'];times=[comparison.utc(report['utc'])-start for report in reports]
    if times!=sorted(times):raise ValueError('Driver reports out of order')
    out=[]
    for index,row in enumerate(rows):
        report_index=bisect.bisect_right(times,row['t'])-1
        if report_index<0:raise ValueError('No preceding driver report for native sample')
        report=reports[report_index];request=report['requested'];dt=float(row['raw']['fixed_dt'])
        if abs(dt-.01)>1e-6:raise ValueError('Native source is not100Hz')
        if index and abs(row['simulation_time']-rows[index-1]['simulation_time']-.01)>2e-4:
            raise ValueError('Native observer missed a fixed sample')
        if row['grounded'] is None or row['vehicle_com_y'] is None:raise ValueError('Required native observation unavailable')
        out.append(dict(index=index,wall_seconds=row['t'],simulation_seconds=row['simulation_time'],fixed_dt=.01,
                        **{key:request[key] for key in ('buttons','lx','ly','rx','ry','lt','rt')},
                        native_speed=row['speed'],native_chassis_y=row['y'],native_vehicle_com_y=row['vehicle_com_y'],
                        native_vy=row['vy'],native_grounded=int(row['grounded']),report_index=report_index,
                        report_age_seconds=row['t']-times[report_index]))
    with args.output.open('x',newline='') as target:
        writer=csv.DictWriter(target,fieldnames=FIELDS);writer.writeheader();writer.writerows(out)
    manifest=dict(schema=1,event='native_fixed_sample_replay_fixture',actual_controller_recording=False,
                  samples=len(out),fixed_seconds=.01,source=meta,driver_sha256=comparison.sha(args.driver.read_bytes()),
                  fixture_sha256=comparison.sha(args.output.read_bytes()),scenario=driver['scenario'],timeline_sha256=driver['timeline_sha256'],
                  input_mapping='Latest completed matching driver report UTC <= observed source fixed sample UTC; one requested state per native sample.',
                  maximum_report_age_during_timeline_seconds=max(row['report_age_seconds'] for row in out if row['wall_seconds']<=12),
                  limits=['Headless replay has no controller or rendered video.',
                          'Last verified neutral request is retained after driver cleanup.',
                          'Native FixedUpdate observation phase versus integration may differ by one fixed step.',
                          'Initial native velocity/terrain/multibody pose are not injected into the independent FlatWorld.'])
    args.output.with_suffix('.fixture.json').write_text(json.dumps(manifest,indent=2))
    print(json.dumps(manifest,indent=2))


def metrics(rows):
    phases={}
    for name,start,end in [('early_pedal',.7,2.3),('moving_brake',3.08,4.3),('brake_high_speed',3.08,3.4),
                           ('brake_middle_speed',3.4,3.9),('brake_low_speed',3.9,4.3)]:
        phases[name]=analysis.engine_speed_fit(rows,start,end)
    hop=[row for row in rows if 8<=row['t']<=11.8];base=comparison.interpolate(rows,'y',8)
    top=max(hop,key=lambda row:row['y']);rise=[row for row in hop if row['y']-base>=.05]
    phases['hop']=dict(baseline_y=base,maximum_rise_m=max(row['y'] for row in hop)-base,
                       maximum_upward_speed_mps=max(row['vy'] for row in hop),
                       peak_sample_wall_seconds=top['t'],peak_sample_engine_seconds=top['simulation_time'],
                       first_sample_rise_at_least_5cm_wall_seconds=rise[0]['t'] if rise else None,
                       first_sample_rise_at_least_5cm_engine_seconds=rise[0]['simulation_time'] if rise else None)
    return phases


def replay_comparison(fixture_path,replay_path):
    with fixture_path.open(newline='') as source:fixture_rows=list(csv.DictReader(source))
    with replay_path.open(newline='') as source:replay_rows=list(csv.DictReader(source))
    if len(fixture_rows)!=len(replay_rows):raise ValueError('Replay sample count differs from fixture')
    native=[];replay=[];combined=[]
    for index,(source,actual) in enumerate(zip(fixture_rows,replay_rows)):
        if int(actual['source_index'])!=index or actual['valid']!='1':raise ValueError('Invalid replay sample')
        if abs(float(source['simulation_seconds'])-float(actual['source_simulation_seconds']))>1e-6:raise ValueError('Replay engine sample mismatch')
        common=dict(t=float(source['wall_seconds']),simulation_time=float(source['simulation_seconds']),raw={},grounded=source['native_grounded']=='1')
        native.append(dict(common,speed=float(source['native_speed']),y=float(source['native_chassis_y']),vy=float(source['native_vy'])))
        combined.append(dict(common,speed=float(source['native_speed']),y=float(source['native_vehicle_com_y']),vy=float(source['native_vy'])))
        replay.append(dict(common,speed=float(actual['speed']),y=float(actual['y']),vy=float(actual['vy']),grounded=actual['grounded']=='1'))
    errors={}
    for phase,start,end in [('pedal',.7,2.3),('brake',3.08,4.3),
                           ('brake_prefix',3.08,3.4),('brake_heldout',3.4,4.3)]:
        values=[b['speed']-a['speed'] for a,b in zip(native,replay) if start<=a['t']<=end]
        errors[phase]=dict(samples=len(values),speed_rmse_mps=math.sqrt(sum(v*v for v in values)/len(values)),
                            mean_signed_speed_error_mps=sum(values)/len(values))
    combined_hop=metrics(combined)['hop'];combined_hop.pop('maximum_upward_speed_mps')
    return dict(schema=1,event='headless_native_sample_replay_comparison',actual_controller_recording=False,
                fixture_sha256=comparison.sha(fixture_path.read_bytes()),replay_sha256=comparison.sha(replay_path.read_bytes()),
                native=metrics(native),replay=metrics(replay),native_combined_com_hop=combined_hop,
                phase_errors=errors,
                limits=['Numeric headless bench; actual controller/video validation remains separate.',
                        'Source sample schedule removes wall/engine pacing as a replay input difference.',
                        'Native chassis velocity and independent combined-body velocity have different composition.',
                        'Fixed sample phase may differ by one10ms integration step.',
                        'Native combinedCOM is used only for height; its velocity is not supplied by the observer.'])


def analyze(args):
    result=replay_comparison(args.fixture,args.replay)
    with args.output.open('x') as target:json.dump(result,target,indent=2)
    print(json.dumps(result,indent=2))


def sweep(args):
    count=round((args.max_hop-args.min_hop)/args.step)+1
    if not (.1<=args.min_hop<=args.max_hop<=15 and args.step>=.001 and 1<=count<=41):
        raise ValueError('Hop sweep requires bounded0.1..15m/s range and at most41 candidates')
    args.output.mkdir(parents=True,exist_ok=False);results=[]
    for index in range(count):
        speed=args.min_hop+index*args.step
        if speed>args.max_hop+1e-8:break
        csv_path=args.output/f'hop-{speed:.3f}.csv'
        command=[str(args.probe.resolve(strict=True)),str(args.fixture.resolve(strict=True)),str(csv_path.resolve()),'--hop-speed',str(speed)]
        if args.pedal_torque is not None:command.extend(['--pedal-torque',str(args.pedal_torque)])
        run=subprocess.run(command,check=True,capture_output=True,text=True,timeout=30)
        receipt=json.loads(run.stderr);report=replay_comparison(args.fixture,csv_path)
        if receipt['actual_controller_recording'] is not False or receipt['invalid']!=0:raise ValueError('Headless receipt invalid')
        report['probe_receipt']=receipt
        csv_path.with_suffix('.json').write_text(json.dumps(report,indent=2))
        rise=report['replay']['hop']['maximum_rise_m'];native=report['native']['hop']['maximum_rise_m'];combined=report['native_combined_com_hop']['maximum_rise_m']
        results.append(dict(hop_speed=speed,replay_rise_m=rise,native_chassis_rise_m=native,native_combined_com_rise_m=combined,
                            error_to_native_chassis_m=rise-native,error_to_native_combined_com_m=rise-combined,
                            early_engine_acceleration_mps2=report['replay']['early_pedal']['ols_engine_acceleration_mps2'],
                            phase_errors=report['phase_errors'],replay_sha256=report['replay_sha256']))
    summary=dict(schema=1,event='headless_native_sample_hop_sweep',actual_controller_recording=False,
                 probe_sha256=comparison.sha(args.probe.read_bytes()),fixture_sha256=comparison.sha(args.fixture.read_bytes()),
                 results=results,limits=['Empirical reduced-body candidate sweep, not recovered native hop source.',
                                        'Chassis and combined COM are different references; both errors are retained.',
                                        'Actual controller/video recording is required separately.'])
    (args.output/'sweep.json').write_text(json.dumps(summary,indent=2));print(json.dumps(summary,indent=2))


def brake_sweep(args):
    gains=[float(value) for value in args.gains.split(',')]
    if not 1<=len(gains)<=15 or any(not math.isfinite(gain) or not 0<=gain<=4 for gain in gains):
        raise ValueError('Provide1..15 finite brake gains in0..4')
    args.output.mkdir(parents=True,exist_ok=False);results=[]
    settings=[('legacy',1.,1.,0.)]+[(f'gain-{gain:.3f}',args.min_scale,args.max_scale,gain) for gain in gains]
    for name,minimum,maximum,gain in settings:
        csv_path=args.output/(name+'.csv')
        command=[str(args.probe.resolve(strict=True)),str(args.fixture.resolve(strict=True)),str(csv_path.resolve()),
                 '--brake-min-scale',str(minimum),'--brake-max-scale',str(maximum),'--brake-speed-gain',str(gain)]
        run=subprocess.run(command,check=True,capture_output=True,text=True,timeout=30)
        receipt=json.loads(run.stderr);report=replay_comparison(args.fixture,csv_path)
        if receipt['actual_controller_recording'] is not False or receipt['invalid']!=0:raise ValueError('Headless receipt invalid')
        report['probe_receipt']=receipt;csv_path.with_suffix('.json').write_text(json.dumps(report,indent=2))
        results.append(dict(name=name,brake_min_scale=minimum,brake_max_scale=maximum,brake_speed_gain=gain,
                            phase_errors=report['phase_errors'],replay_sha256=report['replay_sha256']))
    candidates=results[1:];selected=min(candidates,key=lambda row:row['phase_errors']['brake_prefix']['speed_rmse_mps'])
    summary=dict(schema=1,event='headless_native_sample_brake_sweep',actual_controller_recording=False,
                 selection_metric='Smallest speed RMSE in fixed prefix wall3.08..3.4; later3.4..4.3 excluded from gain selection.',
                 selected_by_prefix=selected,results=results,
                 probe_sha256=comparison.sha(args.probe.read_bytes()),fixture_sha256=comparison.sha(args.fixture.read_bytes()),
                 limits=['Headless numeric calibration; actual controller/video validation remains separate.',
                         'Initial candidate bounds/minimum were informed by this same native capture, so later interval is a within-run holdout, not an independent session.',
                         'Coefficients scale the independent tire-brake torques; they are not recovered native brake source.'])
    (args.output/'brake-sweep.json').write_text(json.dumps(summary,indent=2));print(json.dumps(summary,indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);commands=parser.add_subparsers(dest='command',required=True)
    build=commands.add_parser('fixture')
    for name in ('telemetry','driver','output'):build.add_argument('--'+name,type=Path,required=True)
    build.set_defaults(function=fixture)
    compare=commands.add_parser('analyze')
    for name in ('fixture','replay','output'):compare.add_argument('--'+name,type=Path,required=True)
    compare.set_defaults(function=analyze)
    candidates=commands.add_parser('sweep')
    for name in ('fixture','probe','output'):candidates.add_argument('--'+name,type=Path,required=True)
    candidates.add_argument('--min-hop',type=float,default=7.6);candidates.add_argument('--max-hop',type=float,default=8.)
    candidates.add_argument('--step',type=float,default=.05);candidates.add_argument('--pedal-torque',type=float)
    candidates.set_defaults(function=sweep)
    brake=commands.add_parser('brake-sweep')
    for name in ('fixture','probe','output'):brake.add_argument('--'+name,type=Path,required=True)
    brake.add_argument('--gains',default='.34,.39,.44,.49,.54')
    brake.add_argument('--min-scale',type=float,default=.69);brake.add_argument('--max-scale',type=float,default=1.9)
    brake.set_defaults(function=brake_sweep)
    args=parser.parse_args();args.function(args)
