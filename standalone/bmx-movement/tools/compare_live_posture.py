"""Compare actual demo render-state poses against an independent native repeat.

The demo records the exact holder/world transforms supplied to its presentation.
This is channel comparison, not pose estimation from pixels or native mesh parity.
Only the independently observed native enter/hold/recover domains are covered.
"""
from __future__ import annotations
import argparse
import importlib.util
import json
import math
from pathlib import Path

spec=importlib.util.spec_from_file_location('live_posture_repeat',Path(__file__).with_name('compare_posture_references.py'))
r=importlib.util.module_from_spec(spec);spec.loader.exec_module(r);p=r.p;c=p.comparison


class Stats:
    def __init__(self):self.count=0;self.squares=0.;self.maximum=0.
    def add(self,value):self.count+=1;self.squares+=value*value;self.maximum=max(self.maximum,value)
    def export(self):return dict(samples=self.count,rmse=math.sqrt(self.squares/self.count) if self.count else None,
        maximum=self.maximum if self.count else None)


def playback_phase(frame,descriptor):
    state=frame['playback'];time=c.finite(state.get('pose_sample_seconds'))
    enter=c.finite(descriptor.get('enter_end_seconds'));end=c.finite(descriptor.get('exit_end_seconds'))
    exit_start=c.finite(descriptor.get('exit_start_seconds'))
    if time is None or enter is None or end is None or exit_start is None or time<0 or time>end:return None
    if state.get('pose_recovering') is True:
        return ('recover',(time-exit_start)/(end-exit_start),False) if end>exit_start else None
    if time<enter-1e-5:return ('enter',time/enter,False) if enter>0 else None
    if descriptor.get('hold_available') and abs(time-enter)<=1e-5:return 'hold',0.,True
    if time>=exit_start:return ('recover',(time-exit_start)/(end-exit_start),False) if end>exit_start else None
    return ('hold',(time-enter)/(exit_start-enter),False) if exit_start>enter else None


def main(args):
    driver=c.read_driver(args.driver,'demo')
    provenance=json.loads(args.provenance.read_text(encoding='utf-8-sig'))
    descriptors={row['module_id']:row for row in provenance['tracks']}
    native,sources=r.load(args.native)
    native_by_id={set_id*10+slot:track for (set_id,slot),track in native.items()}
    frames,rejected=p.read_frames(args.rider,driver,'demo')
    results={};aggregate_p=Stats();aggregate_q=Stats();uncovered={}
    for frame in frames:
        value=frame['playback'].get('pose_track_id');id=int(value) if c.finite(value) is not None else -1
        if id<0:continue
        track=native_by_id.get(id);descriptor=descriptors.get(id)
        if track is None or descriptor is None:
            uncovered[id]=uncovered.get(id,0)+1;continue
        phase=playback_phase(frame,descriptor)
        if phase is None:continue
        name,fraction,frozen=phase;ranges=r.phases(track)
        if name not in ranges or not 0<=fraction<=1:continue
        time=ranges[name][0]+fraction*(ranges[name][1]-ranges[name][0])
        out=results.setdefault(id,dict(id=id,set=id//10,slot=id%10,name=track['name'],
            render_frames=0,frozen_hold_endpoint_frames=0,channels={},phase_frames={}))
        out['render_frames']+=1;out['frozen_hold_endpoint_frames']+=frozen
        out['phase_frames'][name]=out['phase_frames'].get(name,0)+1
        for channel,value in frame['local'].items():
            if not channel.startswith('bone:'):continue
            neutral=track['neutral_local_pose'].get(channel) or {}
            position=p.sample(track,'delta_positions',channel,time);rotation=p.sample(track,'delta_rotations',channel,time)
            acc=out['channels'].setdefault(channel[5:],dict(position=Stats(),rotation=Stats()))
            if position is not None and neutral.get('position') is not None:
                actual=[position[i]+neutral['position'][i] for i in range(3)]
                error=math.dist(value['position'],actual);acc['position'].add(error);aggregate_p.add(error)
            if rotation is not None and neutral.get('rotation') is not None and value['rotation'] is not None:
                actual=p.qmul(rotation,neutral['rotation']);error=p.angle(value['rotation'],actual)
                acc['rotation'].add(error);aggregate_q.add(error)
    tracks=[]
    for id,out in sorted(results.items()):
        out['channels']={name:dict(absolute_holder_local_position_m=values['position'].export(),
            absolute_holder_local_rotation_degrees=values['rotation'].export()) for name,values in sorted(out['channels'].items())}
        tracks.append(out)
    result=dict(schema=1,event='actual_demo_render_state_vs_independent_native_posture',
        actual_demo_recording=True,scenario=driver['scenario'],timeline_sha256=driver['timeline_sha256'],
        source_demo_rider=dict(path=str(args.rider),sha256=c.sha(args.rider.read_bytes())),
        source_demo_driver=dict(path=str(args.driver),sha256=c.sha(args.driver.read_bytes())),
        source_playback_descriptor=dict(path=str(args.provenance),sha256=c.sha(args.provenance.read_bytes())),
        independent_native_sources=sources,observed_native_complete_tracks=len(native),compared_module_ids=len(tracks),
        aggregate_absolute_holder_local_position_m=aggregate_p.export(),aggregate_absolute_holder_local_rotation_degrees=aggregate_q.export(),
        rejected_module_frames=rejected,uncovered_native_ids=uncovered,tracks=tracks,
        limits=['Actual logged presentation transforms are compared; no pixel-based joint estimation or native mesh equality is asserted.',
            'The demo displays15 body landmarks/capsules and records52 available playback-bone transforms; these are distinct coverage claims.',
            'Holder rigid motion is removed independently. Native X reflection is already present in the source exports and is not repeated.',
            'Native comparison data is an independent repeat, not the first58 fit recordings. Context and neutral-geometry variation remain in the error.',
            'The recorded module sample clock maps through the production observed enter/recover boundaries onto mutually observed native phases.',
            'A frozen hold endpoint compares only the observed native hold start; this does not verify sustained loops.',
            'Missing source phases/bones/rotations and gaps greater than150ms remain uncovered. No body trajectory, contact or scoring parity follows from posture error.'])
    with args.output.open('x') as target:json.dump(result,target,indent=2)
    print(json.dumps({key:value for key,value in result.items() if key not in ('tracks','independent_native_sources')},indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('rider','driver','provenance','output'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--native',type=Path,action='append',required=True);main(parser.parse_args())
