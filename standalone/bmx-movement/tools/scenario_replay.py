"""Replay a shared fixed request timeline numerically; never access a controller."""
from __future__ import annotations
import argparse
import bisect
import csv
import hashlib
import importlib.util
import json
from pathlib import Path
import statistics
import subprocess

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('shared_request_timelines',Path(__file__).with_name('drive_target.py'))
driver=importlib.util.module_from_spec(spec);spec.loader.exec_module(driver)


def artifact(path):return dict(path=str(path.resolve()),sha256=hashlib.sha256(path.read_bytes()).hexdigest())


def describe(rows):
    if not rows:return dict(samples=0)
    takeoffs=[after['seconds']for before,after in zip(rows,rows[1:])if before['grounded']==1 and after['grounded']==0 and not after['reset']]
    landings=[after['seconds']for before,after in zip(rows,rows[1:])if before['grounded']==0 and after['grounded']==1 and not after['reset']]
    return dict(samples=len(rows),invalid=sum(not row['valid']for row in rows),nonconverged=sum(not row['converged']for row in rows),
        hop_requests=sum(row['hop_pressed']for row in rows),accepted_hop_requests=sum(row['hop_accepted']for row in rows),
        hop_pulse_samples=sum(abs(row['hop_impulse_y'])>1e-8 for row in rows),
        hop_kind_values=sorted({int(row['hop_kind'])for row in rows if row['hop_pressed']}),
        takeoffs=takeoffs,landings=landings,
        single_flight_seconds=landings[0]-takeoffs[0]if len(takeoffs)==len(landings)==1 and landings[0]>takeoffs[0]else None,
        chassis_rise_from_first_m=max(row['chassis_y']for row in rows)-rows[0]['chassis_y'],
        grounded_samples=sum(row['grounded']for row in rows),
        median_actions={name:statistics.median(row[name]for row in rows)for name in ('preload','manual','nose_manual')})


def main(args):
    if args.output.exists():raise ValueError('Fresh output directory required')
    args.output.mkdir()
    timeline=driver.scenarios()[args.scenario];duration=timeline[-1][0];starts=[entry[0]for entry in timeline]
    fixture=args.output/'requests.csv'
    with fixture.open('w',newline='')as stream:
        writer=csv.writer(stream);writer.writerow(['seconds','buttons','lx','ly','rx','ry','lt','rt'])
        for index in range(round(duration*100)+1):
            seconds=index/100;state=timeline[bisect.bisect_right(starts,seconds)-1][1]
            writer.writerow([seconds]+[state[key]for key in ('buttons','lx','ly','rx','ry','lt','rt')])
    output=args.output/'samples.csv'
    completed=subprocess.run([str(args.probe.resolve(strict=True)),str(fixture.resolve()),str(output.resolve())],capture_output=True,text=True,timeout=90)
    if completed.returncode not in (0,2):raise RuntimeError(completed.stderr or completed.stdout)
    receipt=json.loads(completed.stdout.strip().splitlines()[-1])
    with output.open(newline='')as stream:rows=[{name:float(value)for name,value in row.items()}for row in csv.DictReader(stream)]
    if args.scenario=='controls_probe':
        phases=[(name,i*8+2,i*8+3.2)for i,name in enumerate(('negative_partial','positive_partial','negative_full','positive_full'))]
    elif args.scenario=='suspension':phases=[('RS_flick',3.2,5.8),('R3_1',6,8.9),('R3_2',11,17.8)]
    else:phases=[(name,i*8+1.4,i*8+6)for i,name in enumerate(('LS_X_positive','LS_X_negative','LS_Y_positive','LS_Y_negative'))]
    result=dict(schema=1,event='headless_shared_scenario_benchmark',actual_controller_recording=False,scenario=args.scenario,
        duration_seconds=duration,fixed_seconds=.01,receipt=receipt,whole=describe(rows),
        phases={name:describe([row for row in rows if start<=row['seconds']<end])for name,start,end in phases},
        sources=[artifact(path)for path in (fixture,output,args.probe,args.library,Path(__file__),ROOT/'tools/replay_requests.cpp',ROOT/'tools/drive_target.py')],
        limits=['Deterministic 100 Hz request playback is separate from actual controller/video recordings.',
            'DpadDown resets the independent flat world; native respawn, engine pacing and input callback consumption can differ.',
            'Only normal production settings are used; no body trajectory, initial native state or measured target is injected.'])
    if args.scenario=='controls_probe':result['no_unrequested_hop']=not result['whole']['hop_requests']and not result['whole']['hop_pulse_samples']
    (args.output/'summary.json').write_text(json.dumps(result,indent=2))
    print(json.dumps({key:result[key]for key in ('scenario','receipt','phases')},indent=2))
    if receipt['invalid']or receipt['nonconverged']or result.get('no_unrequested_hop')is False:raise SystemExit(2)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scenario',choices=['controls_probe','suspension','air_rotation'],required=True)
    for name in ('probe','library','output'):parser.add_argument('--'+name,type=Path,required=True)
    main(parser.parse_args())
