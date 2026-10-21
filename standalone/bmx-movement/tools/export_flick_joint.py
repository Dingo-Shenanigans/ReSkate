"""Extract native full-preload commands and distinct flick/neutral recoveries."""
from __future__ import annotations
import argparse
import importlib.util
import json
from pathlib import Path

spec=importlib.util.spec_from_file_location('flick_joint_source',Path(__file__).with_name('joint_tracks.py'))
j=importlib.util.module_from_spec(spec);spec.loader.exec_module(j)


def main(args):
    sd=j.c.read_driver(args.suspension_driver,'streets')
    cd=j.c.read_driver(args.controls_driver,'streets')
    if sd['scenario']!='suspension' or cd['scenario']!='controls_probe':raise ValueError('Expected the two native fixed benches')
    rows,rejected=j.load(args.suspension_directory/'body-physics.jsonl',sd)
    controls,control_rejected=j.load(args.controls_directory/'body-physics.jsonl',cd)
    negative=next(row for row in rows if 2.75<=row['wall']<3.4 and row['source_input']['PreloadInput']<=-.9)
    positive=next(row for row in rows if row['simulation']>negative['simulation'] and row['source_input']['PreloadInput']>=.9)
    cross=float(positive['source_vehicle_state']['_timeAtBeginJump'])
    if not negative['simulation']<cross<=positive['simulation'] or positive['simulation']-cross>.021:
        raise ValueError('No directly observed in-capture flick BeginJump boundary')
    neutral=[row for row in rows if 2.35<=row['wall']<2.75 and abs(row['source_input']['PreloadInput'])<.01]
    if len(neutral)<3:raise ValueError('Insufficient neutral before full preload')
    reference=j.neutral_command(neutral)
    enter=[row for row in rows if negative['simulation']-.03<=row['simulation']<=cross]
    transition=[row for row in rows if cross-.03<=row['simulation'] and row['wall']<5.8]
    held=next(row for row in controls if 18<=row['wall']<19.8 and row['source_input']['PreloadInput']<=-.9)
    release=next(row for row in controls if row['simulation']>held['simulation'] and abs(row['source_input']['PreloadInput'])<.01)
    control_neutral=[row for row in controls if 17.8<=row['wall']<17.98 and abs(row['source_input']['PreloadInput'])<.01]
    if len(control_neutral)<3:raise ValueError('Insufficient independent neutral before full-negative control')
    recovery=[row for row in controls if release['simulation']-.03<=row['simulation']<=release['simulation']+1.4]
    if any(row['grounded']['Grounded'] is not True for row in recovery):raise ValueError('Neutral recovery contains a hop or lost ground')
    tracks=[]
    for phase,name,origin,selected,baseline,neutral_count in [
        ('enter','full negative RS preload',negative['simulation'],enter,reference,len(neutral)),
        ('transition','observed full RS flick hop',cross,transition,reference,len(neutral)),
        ('recover','full negative RS to neutral without hop',release['simulation'],recovery,j.neutral_command(control_neutral),len(control_neutral))]:
        if len(selected)<3 or len({tuple(row['identity'])for row in selected})!=1:raise ValueError('Incomplete or changed source joint identity')
        tracks.append(dict(id=83,name=name,phase=phase,origin_engine_seconds=origin,request_amplitude=1,
            time_origin=('Observed native _timeAtBeginJump' if phase=='transition' else 'First actual effective native input sample'),
            phase_source=str(args.controls_directory if phase=='recover' else args.suspension_directory),
            source_identity=selected[0]['identity'],neutral_samples=neutral_count,reference=baseline,
            keys=[dict(seconds=row['simulation']-origin,command=row['command'],source_fixed_index=row['fixed_index'])for row in selected]))
    result=dict(schema=1,event='bmx_observed_joint_actuation_tracks',target='streets',actual_controller_recording=False,
        driver_sha256=j.c.sha(args.suspension_driver.read_bytes()),
        source_body_physics_sha256=j.c.sha((args.suspension_directory/'body-physics.jsonl').read_bytes()),
        timeline_sha256=sd['timeline_sha256'],rejected_physics_samples=rejected+control_rejected,
        independent_recovery_source=dict(directory=str(args.controls_directory),driver_sha256=j.c.sha(args.controls_driver.read_bytes()),
            body_physics_sha256=j.c.sha((args.controls_directory/'body-physics.jsonl').read_bytes())),
        boundaries=dict(full_negative_engine=negative['simulation'],flick_begin_engine=cross,
            first_effective_positive_engine=positive['simulation'],neutral_recovery_engine=release['simulation']),
        tracks=tracks,excluded_tracks=[],limits=[
            'Only actuator commands, inertia, gains and limits are exported; no body position or velocity enters playback.',
            'Enter holds its measured endpoint for arbitrary user duration; transition and neutral recovery have independent event clocks.',
            'Flick enter/transition share one measured neutral reference. Neutral recovery comes from a separate moving control attempt with its own neutral reference; endpoint deltas can differ.',
            'The observed flick BeginJump-to-pulse latency includes native coroutine/render scheduling; it is not a gesture dwell threshold.',
            'The source force magnitude is approximately 4.5; explicit R3 remains 6.4. Playback data does not set either impulse magnitude.'])
    with args.output.open('x',encoding='utf-8')as stream:json.dump(result,stream,indent=2)
    print(json.dumps(dict(output=str(args.output),tracks=len(tracks),boundaries=result['boundaries']),indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('suspension-directory','suspension-driver','controls-directory','controls-driver','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    main(parser.parse_args())
