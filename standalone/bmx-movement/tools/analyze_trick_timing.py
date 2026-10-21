"""Keep current catalog durations separate from measured V3 playback timing."""
from __future__ import annotations
import argparse
import importlib.util
import json
import math
from pathlib import Path
import statistics

spec=importlib.util.spec_from_file_location('bmx_timing_matrix',Path(__file__).with_name('trick_matrix.py'))
matrix=importlib.util.module_from_spec(spec);spec.loader.exec_module(matrix)
c=matrix.comparison
PHASES={'Idle':0,'Launch':1,'Tweak':2,'Loop':3,'Hold':4,'Exit':5}
PHASE_CLIP={1:'enter',2:'tweak',3:'loop',5:'exit'}


def phase(value):return c.finite(value) if c.finite(value) is not None else PHASES.get(str(value))
def fit_rate(points):
    if len(points)<3:return None
    x=[row[0]-points[0][0] for row in points];y=[row[1] for row in points];mx=statistics.mean(x);my=statistics.mean(y)
    variance=sum((v-mx)**2 for v in x)
    if variance<1e-10:return None
    slope=sum((a-mx)*(b-my) for a,b in zip(x,y))/variance;intercept=my-slope*mx
    residual=math.sqrt(sum((b-intercept-slope*a)**2 for a,b in zip(x,y))/len(x))
    return dict(clip_time_per_engine_second=slope,residual_seconds=residual,engine_interval=[points[0][0],points[-1][0]],samples=len(points))


def analyze(args):
    catalog_path=args.directory/'trick-catalog.json';catalog=json.loads(catalog_path.read_text(encoding='utf-8-sig'))
    active=next(table for table in catalog['tables'] if table['role']==catalog['canonical_role']);rows=[]
    for item in active['slots']:
        data=item['data']
        if not data['available']:continue
        rows.append(dict(set=item['set_id'],slot=item['slot_id'],name=data['trick_name'],source_reference=data['reference'],
            raw_speed_getters=data['speed'],allow_landing_holding=data['allow_landing_holding'],only_fire_if_in_air=data['only_fire_if_in_air'],
            clips={name:({key:value[key] for key in ('reference','length_seconds','frame_rate','is_looping') if key in value} if value else None)
                   for name,value in data['clips'].items()}))
    observed=[]
    if args.driver:
        driver=c.read_driver(args.driver,'streets');start=c.utc(driver['timeline_start_utc']);frames=[]
        for line in (args.directory/'trick-states.jsonl').read_text(encoding='utf-8-sig').splitlines():
            frame=json.loads(line)
            if frame.get('selection_current') is False:continue
            anim=frame['animation'];control=anim.get('controller_state') or {}
            frames.append(dict(t=c.utc(frame['utc'])-start,simulation=frame['simulation_seconds'],phase=phase(control.get('_phase')),
                               animation=anim,control=control))
        requests=matrix.driver_tools.matrix_attempts(driver['scenario'])
        for request_index,request in enumerate(requests):
            selected=[frame for frame in frames if request['selection_start_seconds']<=frame['t']<request['end_seconds']]
            next_selection=requests[request_index+1]['selection_start_seconds'] if request_index+1<len(requests) else None
            following=[frame for frame in frames if next_selection is not None and abs(frame['t']-next_selection)<=.1]
            if not selected:continue
            phases=[];last=None
            for frame in selected:
                if frame['phase']!=last:
                    phases.append(dict(phase=frame['phase'],first_wall_seconds=frame['t'],first_engine_seconds=frame['simulation']))
                    last=frame['phase']
            playback={}
            for number,name in PHASE_CLIP.items():
                segment=[frame for frame in selected if frame['phase']==number]
                for who in ('player','vehicle'):
                    groups={}
                    for frame in segment:
                        state=(frame['animation'].get(name) or {}).get(who) or {};clip=state.get('clip') or {};values=state.get('playback') or {}
                        clip_id=(clip.get('reference') or {}).get('id');clock=c.finite(values.get('Time'))
                        if clock is None or clip_id is None:continue
                        groups.setdefault(str(clip_id),[]).append((frame['simulation'],clock,values))
                    for clip_id,points in groups.items():
                        fit=fit_rate(points)
                        playback[name+'.'+who+'.'+clip_id]=dict(fit=fit,
                            observed_effective_speeds=sorted({c.finite(point[2].get('EffectiveSpeed')) for point in points if c.finite(point[2].get('EffectiveSpeed')) is not None}),
                            observed_weights_range=([min(c.finite(point[2].get('EffectiveWeight')) for point in points if c.finite(point[2].get('EffectiveWeight')) is not None),
                                                     max(c.finite(point[2].get('EffectiveWeight')) for point in points if c.finite(point[2].get('EffectiveWeight')) is not None)]
                                if any(c.finite(point[2].get('EffectiveWeight')) is not None for point in points) else None))
            at_next=min(following,key=lambda frame:abs(frame['t']-next_selection)) if following else None
            observed.append(dict(set=request['set'],slot=request['slot'],phase_transitions=phases,playback=playback,
                phase_at_following_selection=at_next['phase'] if at_next else None,
                requested_following_selection_wall_seconds=next_selection,
                busy_at_following_selection=c.boolean(at_next['control'].get('IsBusy')) if at_next else None,
                actual_following_sample_wall_seconds=at_next['t'] if at_next else None))
    result=dict(schema=1,event='bmx_current_catalog_and_observed_timing',catalog_sha256=c.sha(catalog_path.read_bytes()),
        canonical_role=catalog['canonical_role'],catalog_slots=rows,observed_attempt_timing=observed,
        controller_settings=catalog['controller_settings'],animator_settings=catalog['animator_settings'],
        limits=['Raw clip duration and speed getters do not establish their native composition or effective duration.',
                'Measured clip Time slope against native simulation clock is reported independently of EffectiveSpeed getter.',
                'A shared Scooter-named clip in the active BMX provider is retained as an actual binding.',
                'Busy/phase observations guide request spacing; they do not establish successful landed scoring.'])
    with args.output.open('x',encoding='utf-8') as target:json.dump(result,target,indent=2)
    print(json.dumps(dict(output=str(args.output),catalog_nonempty=len(rows),observed_attempts=len(observed),
                         busy_next=sum(row['busy_at_following_selection'] is True for row in observed)),indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('directory',type=Path)
    parser.add_argument('--driver',type=Path);parser.add_argument('--output',type=Path,required=True)
    analyze(parser.parse_args())
