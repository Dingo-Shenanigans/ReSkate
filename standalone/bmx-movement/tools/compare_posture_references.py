"""Independent native repeat comparison, normalized only within observed phases."""
from __future__ import annotations
import argparse
import importlib.util
import json
import math
from pathlib import Path

spec=importlib.util.spec_from_file_location('pose_repeat_math',Path(__file__).with_name('pose_tracks.py'))
p=importlib.util.module_from_spec(spec);spec.loader.exec_module(p);c=p.comparison


def phases(track):
    result={};origin=track['fire_engine_seconds'];observed=track.get('phase_observations') or []
    enter=next((phase['simulation_seconds']-origin for phase in observed if phase['from_phase']==1),None)
    exit_start=next((phase['simulation_seconds']-origin for phase in observed if phase['to_phase']==5),None)
    exit_end=next((phase['simulation_seconds']-origin for phase in observed if phase['from_phase']==5 and phase['to_phase']==0),None)
    if enter is not None and enter>0:result['enter']=[0,enter]
    if enter is not None and exit_start is not None and exit_start-enter>.005:result['hold']=[enter,exit_start]
    if exit_start is not None and exit_end is not None and exit_end>exit_start:result['recover']=[exit_start,exit_end]
    return result


def load(paths):
    tracks={};sources=[]
    for path in paths:
        data=json.loads(path.read_text(encoding='utf-8-sig'))
        if data.get('target')!='streets' or data.get('neutral_and_deltas_share_export_basis') is not True:raise ValueError('Expected reflected native posture export')
        sources.append(dict(path=str(path),sha256=c.sha(path.read_bytes())))
        for track in data['tracks']:
            ranges=phases(track);key=track['set'],track['slot']
            # Completeness concerns the observed animation domain, not later
            # stationary rest that a faster reset batch may have censored.
            if key in tracks or 'recover' not in ranges or track['keys'][-1]['t']<ranges['recover'][1]+.14:continue
            tracks[key]=track
    return tracks,sources


def stats(values):return dict(samples=len(values),rmse=math.sqrt(sum(v*v for v in values)/len(values)) if values else None,maximum=max(values) if values else None)


def main(args):
    reference,rs=load(args.reference);holdout,hs=load(args.holdout);reports=[];all_p=[];all_q=[]
    for pair in sorted(reference.keys()&holdout.keys()):
        a,b=reference[pair],holdout[pair];ar,br=phases(a),phases(b);channels=[];track_p=[];track_q=[]
        names=sorted({name for track in (a,b) for key in track['keys'] for name in key['delta_positions'] if name.startswith('bone:')})
        for name in names:
            positions=[];rotations=[];phase_reports=[]
            for phase in sorted(ar.keys()&br.keys()):
                pp=[];qq=[]
                for index in range(21):
                    t=index/20;at=ar[phase][0]+t*(ar[phase][1]-ar[phase][0]);bt=br[phase][0]+t*(br[phase][1]-br[phase][0])
                    av=p.sample(a,'delta_positions',name,at);bv=p.sample(b,'delta_positions',name,bt)
                    if av is not None and bv is not None:pp.append(math.dist(av,bv))
                    aq=p.sample(a,'delta_rotations',name,at);bq=p.sample(b,'delta_rotations',name,bt)
                    if aq is not None and bq is not None:qq.append(p.angle(aq,bq))
                positions+=pp;rotations+=qq;phase_reports.append(dict(phase=phase,position_m=stats(pp),rotation_degrees=stats(qq),requested_samples=21))
            an=a['neutral_local_pose'].get(name) or {};bn=b['neutral_local_pose'].get(name) or {}
            channels.append(dict(bone=name[5:],delta_position_m=stats(positions),delta_rotation_degrees=stats(rotations),
                neutral_position_difference_m=math.dist(an['position'],bn['position']) if an.get('position') is not None and bn.get('position') is not None else None,
                neutral_rotation_difference_degrees=p.angle(an['rotation'],bn['rotation']) if an.get('rotation') is not None and bn.get('rotation') is not None else None,
                phases=phase_reports))
            track_p+=positions;track_q+=rotations
        reports.append(dict(set=pair[0],slot=pair[1],name=a['name'],reference_phases=ar,holdout_phases=br,
            delta_position_m=stats(track_p),delta_rotation_degrees=stats(track_q),channels=channels))
        all_p+=track_p;all_q+=track_q
    result=dict(schema=1,event='native_independent_posture_repeat_comparison',reference_sources=rs,holdout_sources=hs,
        reference_complete_tracks=len(reference),holdout_complete_tracks=len(holdout),compared_tracks=len(reports),
        aggregate_delta_position_m=stats(all_p),aggregate_delta_rotation_degrees=stats(all_q),tracks=reports,
        limits=['Independent native recordings measure context/repeatability; these are not module-vs-native live comparison errors.',
            '21 samples normalize each mutually observed enter/hold/recover phase independently; original phase timings are retained.',
            'Offsets compare neutral-subtracted holder-local posture; absolute neutral differences are reported separately.',
            'No interpolation crosses source bone gaps greater than150ms; missing bones/rotations remain uncovered.',
            'A source without fixed physics still supports actual rider-frame animation and bone observations at its recorded cadence.'])
    with args.output.open('x') as target:json.dump(result,target,indent=2)
    print(json.dumps({key:value for key,value in result.items() if key not in ('tracks','reference_sources','holdout_sources')},indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference',type=Path,action='append',required=True);parser.add_argument('--holdout',type=Path,action='append',required=True)
    parser.add_argument('--output',type=Path,required=True);main(parser.parse_args())
