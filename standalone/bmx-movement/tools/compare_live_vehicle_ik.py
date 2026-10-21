"""Verify actual logged vehicle/IK presentation against its pinned fit samples.

This audits runtime playback of generated data; it is not independent native
physics validation or a recovered grip/pedal contact interpretation.
"""
from __future__ import annotations
import argparse
import importlib.util
import json
import math
from pathlib import Path

spec=importlib.util.spec_from_file_location('live_vehicle_fit',Path(__file__).with_name('vehicle_ik_tracks.py'))
v=importlib.util.module_from_spec(spec);spec.loader.exec_module(v);p=v.poses;c=v.c
LIMBS=('left_hand','right_hand','left_foot','right_foot')


class Stats:
    def __init__(self):self.n=0;self.s=0.;self.m=0.
    def add(self,x):self.n+=1;self.s+=x*x;self.m=max(self.m,x)
    def export(self):return dict(samples=self.n,rmse=math.sqrt(self.s/self.n) if self.n else None,maximum=self.m if self.n else None)


def main(args):
    driver=c.read_driver(args.driver,'demo');provenance=json.loads(args.provenance.read_text());cache={};sources=[]
    for source in provenance['sources']:
        path=Path(source['path']);data=path.read_bytes()
        if c.sha(data)!=source['sha256']:raise ValueError('Pinned native fit source changed')
        bundle=json.loads(data);cache[str(path)]={kind:{(row['id'],row[key]):row for row in reversed(bundle[kind])}
            for kind,key in (('vehicle_tracks','node'),('ik_tracks','limb'))}
        sources.append(dict(path=str(path),sha256=source['sha256']))
    vehicle={(row['id'],row['node']):cache[str(Path(row['source']))]['vehicle_tracks'][(row['id'],row['node'])]
        for row in provenance['vehicle_tracks']}
    ik={(row['id'],row['limb']):cache[str(Path(row['source']))]['ik_tracks'][(row['id'],row['limb'])]
        for row in provenance['ik_tracks']}
    channels={};ids=set();active_frames=0
    with args.rider.open(encoding='utf-8-sig') as stream:
        for line in stream:
            frame=json.loads(line);id=frame.get('pose_track_id',-1);time=c.finite(frame.get('pose_sample_seconds'))
            if id<0 or time is None or frame.get('valid') is False:continue
            ids.add(id);active_frames+=1;q=p.qnorm(frame['body_quaternion']);origin=frame['body_position']
            for node,observed in frame.get('vehicle_nodes',{}).items():
                track=vehicle.get((id,node))
                if not track:continue
                reference=v.interp(track['keys'],time);out=channels.setdefault('vehicle:'+node,dict(position=Stats(),rotation=Stats()))
                if observed.get('position') is not None:
                    actual=p.rotate(p.qinv(q),[observed['position'][i]-origin[i] for i in range(3)])
                    out['position'].add(math.dist(actual,reference['position']))
                actual=p.qnorm(observed.get('rotation'))
                if actual is not None:out['rotation'].add(p.angle(p.qmul(p.qinv(q),actual),reference['rotation']))
            for observed in frame.get('ik_controls',[]):
                limb=LIMBS[observed['limb']];track=ik.get((id,limb))
                if not track:continue
                reference=v.interp(track['keys'],time,True)['command'];out=channels.setdefault('ik:'+limb,
                    dict(weights=Stats(),selector_compared=0,selector_mismatches=0,non_unknown_binding=0))
                for bit,name in enumerate(('position_weight','rotation_weight','reference_weight')):
                    if observed['available']&(1<<bit) and reference.get(name) is not None:
                        out['weights'].add(abs(observed[name]-reference[name]))
                if observed['available']&8 and reference.get('target_selector') is not None:
                    out['selector_compared']+=1;out['selector_mismatches']+=observed['target_selector']!=round(reference['target_selector'])
                out['non_unknown_binding']+=observed.get('binding',0)!=0
    results={name:{field:(value.export() if isinstance(value,Stats) else value) for field,value in values.items()}
        for name,values in sorted(channels.items())}
    result=dict(schema=1,event='actual_vehicle_ik_playback_vs_pinned_fit_samples',actual_module_recording=True,
        scenario=driver['scenario'],timeline_sha256=driver['timeline_sha256'],observed_module_ids=sorted(ids),active_render_frames=active_frames,
        source_rider=dict(path=str(args.rider),sha256=c.sha(args.rider.read_bytes())),
        source_provenance=dict(path=str(args.provenance),sha256=c.sha(args.provenance.read_bytes())),pinned_native_fit_sources=sources,channels=results,
        limits=['This is a runtime reconstruction check against fitted native data, not an independent holdout or physical-body parity score.',
            'Actual recorded transforms are converted back through the module aggregate-bike COM frame used by its renderer.',
            'Recorded sample clocks are used directly; no source body positions or velocities are injected into the physical model.',
            'IK weights remain literal controls and selectors hold the preceding observed key. Binding must remain unknown until independently proved.',
            'Visible independently built geometry is rebased through these transforms; native mesh, collider and joint-frame equality is not asserted.'])
    with args.output.open('x') as target:json.dump(result,target,indent=2)
    print(json.dumps(dict(output=str(args.output),observed_ids=len(ids),active_render_frames=active_frames,channels=len(results)),indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('rider','driver','provenance','output'):parser.add_argument('--'+name,type=Path,required=True)
    main(parser.parse_args())
