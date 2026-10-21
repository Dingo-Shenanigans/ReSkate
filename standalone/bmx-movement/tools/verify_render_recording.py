"""Audit recorded demo presentation channels against its15 visible landmarks."""
from __future__ import annotations
import argparse
import collections
import hashlib
import json
import math
from pathlib import Path


def main(args):
    ids=collections.Counter();bones=collections.Counter();vehicles=collections.Counter();ik=collections.Counter()
    maximum=0.;paired=0;inactive_with_vehicle=0;invalid=0
    singles={'Hips':'hips','Chest':'chest','Head':'head'}
    pairs={'UpperArm':'shoulders','LowerArm':'elbows','Hand':'hands','UpperLeg':'hips_joint','LowerLeg':'knees','Foot':'feet'}
    with args.rider.open(encoding='utf-8-sig') as stream:
        for line in stream:
            frame=json.loads(line);id=frame.get('pose_track_id',-1);invalid+=frame.get('valid') is False
            if id<0:
                inactive_with_vehicle+=bool(frame.get('vehicle_nodes'));continue
            ids[id]+=1;bones[len(frame.get('bones',{}))]+=1;vehicles[len(frame.get('vehicle_nodes',{}))]+=1
            ik[sum(bool(value.get('available')) for value in frame.get('ik_controls',[]))]+=1
            for bone,key in singles.items():
                observed=frame['bones'].get(bone)
                if observed and observed.get('position') is not None:
                    maximum=max(maximum,math.dist(observed['position'],frame[key]));paired+=1
            for suffix,key in pairs.items():
                for index,side in enumerate(('Left','Right')):
                    observed=frame['bones'].get(side+suffix)
                    if observed and observed.get('position') is not None:
                        maximum=max(maximum,math.dist(observed['position'],frame[key][index]));paired+=1
    result=dict(schema=1,event='actual_render_landmark_consistency',
        source=dict(path=str(args.rider),sha256=hashlib.sha256(args.rider.read_bytes()).hexdigest()),
        observed_pose_ids=sorted(ids),per_id_pose_samples=dict(ids),available_bones_per_active_frame=dict(bones),
        vehicle_nodes_per_active_frame=dict(vehicles),available_ik_controls_per_active_frame=dict(ik),
        paired_visible_landmark_samples=paired,maximum_recorded_landmark_vs_bone_distance_m=maximum,
        inactive_pose_frames_with_stale_vehicle_nodes=inactive_with_vehicle,invalid_render_frames=invalid,
        limits=['Checks recorded15 visible body landmarks against corresponding playback bones; no native mesh equality is asserted.',
            'Vehicle/IK availability audits do not prove their native target-contact semantics.'])
    with args.output.open('x') as target:json.dump(result,target,indent=2)
    print(json.dumps({key:value for key,value in result.items() if key!='per_id_pose_samples'},indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rider',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    main(parser.parse_args())
