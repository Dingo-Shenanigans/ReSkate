"""Compare measured native limb lengths to actual standalone rider landmarks.

This is an invariant segment-length comparison, not animation retargeting or a
claim that humanoid wrists/ankles equal grip/pedal contact anchors.
"""
from __future__ import annotations
import argparse
import bisect
import importlib.util
import json
import math
from pathlib import Path
import statistics

spec=importlib.util.spec_from_file_location('bmx_comparison_rider',Path(__file__).with_name('comparison.py'))
comparison=importlib.util.module_from_spec(spec);spec.loader.exec_module(comparison)


def compare(args):
    native=json.loads(args.streets_analysis.read_text(encoding='utf-8-sig'))
    driver=comparison.read_driver(args.demo_driver,'demo')
    rows,source=comparison.read_telemetry(args.demo_csv,driver)
    times=[row['t'] for row in rows];start=comparison.utc(driver['timeline_start_utc'])
    lengths={};selected=[]
    for line in args.demo_rider.read_text().splitlines():
        frame=json.loads(line);time=frame['utc_seconds']-start
        if not (0<=time<=.5 or 25<=time<=28.5):continue
        i=bisect.bisect_left(times,time);indices=[j for j in (i-1,i) if 0<=j<len(rows)]
        j=min(indices,key=lambda j:abs(times[j]-time));body=rows[j]
        if (abs(times[j]-time)>.04 or body['grounded'] is not True or body['speed'] is None
                or body['speed']>.1 or body['vy'] is None or abs(body['vy'])>.1):continue
        points={name:frame.get(name) for name in ('hips','head')}
        for group,one in [('shoulders','shoulder'),('elbows','elbow'),('hands','hand'),
                          ('hips_joint','hip_joint'),('knees','knee'),('feet','foot')]:
            for side,index in [('left',0),('right',1)]:
                points[side+'_'+one]=frame[group][index]
        pairs=[('hips_center_to_head','hips','head'),('hand_separation','left_hand','right_hand'),
               ('hip_joint_separation','left_hip_joint','right_hip_joint'),
               ('upper_arm_root_separation','left_shoulder','right_shoulder')]
        for side in ('left','right'):
            pairs.extend([(side+'_upper_arm',side+'_shoulder',side+'_elbow'),
                          (side+'_forearm',side+'_elbow',side+'_hand'),
                          (side+'_upper_leg',side+'_hip_joint',side+'_knee'),
                          (side+'_shin',side+'_knee',side+'_foot')])
        for name,a,b in pairs:lengths.setdefault(name,[]).append(math.dist(points[a],points[b]))
        selected.append(time)
    if not selected:raise ValueError('No supported neutral standalone rider samples')
    reference=native['neutral_skeleton']['bone_landmark_distances_m'];differences={}
    for name,values in lengths.items():
        if name not in reference:continue
        actual=statistics.median(values);expected=reference[name]['median'];delta=actual-expected
        differences[name]=dict(streets_m=expected,demo_m=actual,demo_minus_streets_m=delta,
                               relative_error_percent=delta/expected*100,
                               demo_minimum_m=min(values),demo_maximum_m=max(values))
    result=dict(schema=1,event='bmx_observed_rider_segment_comparison',differences=differences,
                demo_selected_samples=len(selected),demo_selected_timeline_seconds=selected,
                streets_selected_samples=native['neutral_skeleton']['selected_samples'],
                sources=dict(streets_analysis_sha256=comparison.sha(args.streets_analysis.read_bytes()),
                             streets_rider_sha256=native['rider_source_sha256'],demo_telemetry=source,
                             demo_rider_sha256=comparison.sha(args.demo_rider.read_bytes()),
                             demo_driver_sha256=comparison.sha(args.demo_driver.read_bytes())),
                limits=['Distances are invariant under world origin and orientation changes; coordinate retargeting is separate.',
                        'Neutral windows require low measured body speed and support, but do not enforce identical crank phase or animation phase.',
                        'Module landmark index0 is named left; native humanoid bone names define native sides.',
                        'Humanoid wrist/ankle origins are not proven grip/pedal anchors.',
                        'Matching segment lengths does not establish matching animation, rider forces, trick poses or full parity.'])
    with args.output.open('x') as target:json.dump(result,target,indent=2)
    print(json.dumps(result,indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('streets-analysis','demo-rider','demo-csv','demo-driver','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    compare(parser.parse_args())
