"""Export observed holder-local posture keys and compare normalized trajectories.

Native pose references must be explicitly skeleton-bound. Posture offsets are
separate from holder/body motion, which is analysis-only. Missing bones, rotations
and IK channels remain missing; this tool does not invent an animation rig.
"""
from __future__ import annotations
import argparse
import bisect
import importlib.util
import json
import math
from pathlib import Path
import statistics

spec=importlib.util.spec_from_file_location('bmx_pose_comparison',Path(__file__).with_name('comparison.py'))
comparison=importlib.util.module_from_spec(spec);spec.loader.exec_module(comparison)


def qnorm(q):
    if not isinstance(q,list) or len(q)!=4 or any(comparison.finite(v) is None for v in q):return None
    n=math.sqrt(sum(v*v for v in q));return [v/n for v in q] if n>1e-10 else None
def qinv(q):return [-q[0],-q[1],-q[2],q[3]]
def qmul(a,b):
    x,y,z,w=a;X,Y,Z,W=b
    return [w*X+x*W+y*Z-z*Y,w*Y-x*Z+y*W+z*X,w*Z+x*Y-y*X+z*W,w*W-x*X-y*Y-z*Z]
def rotate(q,v):return qmul(qmul(q,list(v)+[0]),qinv(q))[:3]
def vector(value):return value if isinstance(value,list) and len(value)==3 and all(comparison.finite(v) is not None for v in value) else None
def point(value):return vector(value.get('position')) if isinstance(value,dict) else vector(value)
def angle(a,b):return 2*math.acos(min(1,max(0,abs(sum(x*y for x,y in zip(a,b))))))*180/math.pi
def reflect_p(p,enabled):return [-p[0],p[1],p[2]] if enabled else p
def reflect_q(q,enabled):return [q[0],-q[1],-q[2],q[3]] if enabled else q
def medoid(values):return min(values,key=lambda a:sum(angle(a,b) for b in values))
def slerp(a,b,t):
    dot=sum(x*y for x,y in zip(a,b))
    if dot<0:b=[-v for v in b];dot=-dot
    if dot>.9995:return qnorm([x+(y-x)*t for x,y in zip(a,b)])
    theta=math.acos(min(1,max(-1,dot)));den=math.sin(theta)
    return [(math.sin((1-t)*theta)*x+math.sin(t*theta)*y)/den for x,y in zip(a,b)]


ALIASES={'Hips':'hips','Head':'head','Chest':'chest','LeftUpperArm':'left_shoulder','RightUpperArm':'right_shoulder',
         'LeftLowerArm':'left_elbow','RightLowerArm':'right_elbow','LeftHand':'left_hand','RightHand':'right_hand',
         'LeftUpperLeg':'left_hip_joint','RightUpperLeg':'right_hip_joint','LeftLowerLeg':'left_knee','RightLowerLeg':'right_knee',
         'LeftFoot':'left_foot','RightFoot':'right_foot'}


def read_frames(path,driver,target,allow_demo_reference=False):
    start=comparison.utc(driver['timeline_start_utc']);frames=[];rejected={}
    for source in json_frames(path):
        ref=source.get('pose_reference')
        if source.get('selection_current') is False:
            rejected['stale_vehicle_selection']=rejected.get('stale_vehicle_selection',0)+1;continue
        if not ref and target=='demo' and allow_demo_reference:
            ref=dict(id='independent_combined_body',position=source.get('body_position'),rotation=source.get('body_quaternion'),
                skeleton_bound=True,kind='explicit_tool_option_independent_combined_body_COM')
        if not ref or ref.get('skeleton_bound') is not True:
            rejected['no_verified_pose_reference']=rejected.get('no_verified_pose_reference',0)+1;continue
        origin=vector(ref.get('position'));rotation=qnorm(ref.get('rotation'))
        if origin is None or rotation is None:
            rejected['invalid_pose_reference']=rejected.get('invalid_pose_reference',0)+1;continue
        wall=float(source['utc_seconds'])-start if 'utc_seconds' in source else comparison.utc(source['utc'])-start
        simulation=comparison.finite(source.get('simulation_seconds'))
        if simulation is None:continue
        targets={'bone:'+name:value for name,value in source.get('bones',{}).items() if value}
        for bone,name in ALIASES.items():
            value=source.get(name) or source.get('bones',{}).get(bone)
            if value:targets['landmark:'+name]=value
        for group,label in [('shoulders','shoulder'),('elbows','elbow'),('hands','hand'),('hips_joint','hip_joint'),('knees','knee'),('feet','foot')]:
            if group in source:
                for side,index in [('left',0),('right',1)]:targets['landmark:'+side+'_'+label]=source[group][index]
        for name,value in (source.get('anchors') or source.get('contact_anchors') or {}).items():
            if point(value) is not None:targets['anchor:'+name]=value
        local={}
        for name,value in targets.items():
            p=point(value)
            if p is None:continue
            q=qnorm(value.get('rotation')) if isinstance(value,dict) else None
            local[name]=dict(position=rotate(qinv(rotation),[p[i]-origin[i] for i in range(3)]),
                             rotation=qmul(qinv(rotation),q) if q else None)
        helpers={name:value.get('local_position') for name,value in (source.get('helper_transforms') or {}).items()
                 if value and vector(value.get('local_position')) is not None}
        frames.append(dict(wall=wall,simulation=simulation,reference=ref,reference_position=origin,reference_rotation=rotation,
                           local=local,helper_channels=helpers,motion_fields=source.get('motion_fields') or {},
                           playback={key:source.get(key) for key in ('pose_track_id','pose_elapsed','pose_sample_seconds','pose_recovering')}))
    return frames,rejected


def json_frames(path):
    with path.open(encoding='utf-8-sig') as source:
        for line in source:
            if line.strip():yield json.loads(line)


def neutral_pose(frames):
    names={name for frame in frames for name in frame['local']};result={}
    for name in names:
        values=[frame['local'][name] for frame in frames if name in frame['local']]
        rotations=[value['rotation'] for value in values if value['rotation'] is not None]
        result[name]=dict(position=[statistics.median(value['position'][i] for value in values) for i in range(3)],
                          rotation=medoid(rotations) if rotations else None,samples=len(values))
    return result


def export(args):
    driver=comparison.read_driver(args.driver,args.target);coverage=json.loads(args.coverage.read_text(encoding='utf-8-sig'))
    if coverage['timeline_sha256']!=driver['timeline_sha256'] or coverage['target']!=args.target:raise ValueError('Coverage/driver identity mismatch')
    frames,rejected=read_frames(args.rider,driver,args.target,args.allow_demo_body_reference)
    if not frames:raise ValueError('No verified pose-reference frames; posture keys cannot use an unrelated Spring/chassis')
    reflect=args.target=='streets';tracks=[];unexported=[]
    for attempt in coverage['attempts']:
        if attempt['correct_fire_observed'] is not True:
            unexported.append(dict(set=attempt['set'],slot=attempt['slot'],reason='No observed correct fire'));continue
        if attempt.get('source_complete_through_attempt') is False and not args.allow_partial:
            unexported.append(dict(set=attempt['set'],slot=attempt['slot'],reason='Source capture censored before attempt end'));continue
        expected=(attempt['set'],attempt['slot']);old=attempt.get('historical')
        fires=[fire for fire in attempt['fire_observations'] if ((fire.get('set'),fire.get('slot'))==expected
            if args.target=='streets' else old and fire.get('module_id')==old['module_id'])]
        if not fires:continue
        fire=fires[0];near=min(frames,key=lambda frame:abs(frame['wall']-fire['t']))
        fire_sim=comparison.finite(fire.get('simulation_seconds'))
        if fire_sim is None:
            # The module fire pulse is recorded at the actual fixed-step UTC;
            # nearest rider clock remains explicitly bounded by sample spacing.
            fire_sim=near['simulation'];fire_clock='nearest observed rider frame to module adopted-state pulse'
        else:fire_clock='recorded native last-fire engine clock' if args.target=='streets' else 'module adopted-state fixed-step clock'
        neutral_start=(attempt['reset_release_seconds']+.3 if attempt.get('reset_release_seconds') is not None else attempt['start_seconds'])
        identity=near['reference']['id']
        before=[frame for frame in frames if neutral_start<=frame['wall']<attempt['hop_request_seconds']-.05
                and frame['reference']['id']==identity]
        selected=[];identity_truncated=False
        for frame in frames:
            if not fire['t']-.15<=frame['wall']<attempt['end_seconds']:continue
            if frame['reference']['id']!=identity:
                if frame['wall']>=fire['t']:identity_truncated=True;break
                continue
            selected.append(frame)
        identities={frame['reference']['id'] for frame in before+selected}
        partial=attempt.get('source_complete_through_attempt') is False or identity_truncated
        if partial and not args.allow_partial:
            unexported.append(dict(set=attempt['set'],slot=attempt['slot'],reason='Holder identity or source availability truncates attempt'));continue
        if len(before)<2 or len(selected)<3 or len(identities)!=1:
            unexported.append(dict(set=attempt['set'],slot=attempt['slot'],reason='Insufficient neutral/key samples or holder identity change'));continue
        neutral=neutral_pose(before);ref0=before[-1];keys=[]
        neutral_export={name:dict(position=reflect_p(value['position'],reflect),
            rotation=reflect_q(value['rotation'],reflect) if value['rotation'] is not None else None,samples=value['samples'])
            for name,value in neutral.items()}
        for frame in selected:
            positions={};rotations={}
            for name,value in frame['local'].items():
                if name not in neutral:continue
                positions[name]=reflect_p([value['position'][i]-neutral[name]['position'][i] for i in range(3)],reflect)
                if value['rotation'] is not None and neutral[name]['rotation'] is not None:
                    rotations[name]=reflect_q(qmul(value['rotation'],qinv(neutral[name]['rotation'])),reflect)
            ref_position=rotate(qinv(ref0['reference_rotation']),[frame['reference_position'][i]-ref0['reference_position'][i] for i in range(3)])
            ref_rotation=qmul(qinv(ref0['reference_rotation']),frame['reference_rotation'])
            keys.append(dict(t=frame['simulation']-fire_sim,source_wall_seconds=frame['wall'],delta_positions=positions,
                             delta_rotations=rotations,reference_motion_for_analysis_only=dict(position=reflect_p(ref_position,reflect),
                                 rotation=reflect_q(ref_rotation,reflect)),helper_channels=frame['helper_channels'],motion_fields=frame['motion_fields']))
        landings=[value for value in attempt['physical_recontacts'] if value>fire['t']]
        landing=min(frames,key=lambda frame:abs(frame['wall']-landings[0])) if landings else None
        tracks.append(dict(set=attempt['set'],slot=attempt['slot'],name=(attempt.get('current_runtime') or old or {}).get('name'),
            fire_observation=fire,fire_engine_seconds=fire_sim,fire_clock=fire_clock,
            fire_to_rider_nearest_wall_error_seconds=abs(near['wall']-fire['t']),
            pose_reference={k:ref0['reference'].get(k) for k in ('id','name','kind','skeleton_bound','binding_evidence','position_role')},
            observation_window=dict(partial=partial,holder_identity_truncated=identity_truncated,
                source_complete_through_attempt=attempt.get('source_complete_through_attempt'),
                native_exit_to_idle_observed=attempt.get('native_animation_exit_to_idle_observed'),
                first_wall_seconds=selected[0]['wall'],last_wall_seconds=selected[-1]['wall'],
                first_engine_seconds=selected[0]['simulation'],last_engine_seconds=selected[-1]['simulation'],
                missing_tail_is_extrapolated=False),
            neutral_samples=len(before),neutral_local_pose=neutral_export,keys=keys,
            phase_observations=attempt.get('native_animation_phase_transitions',[]),
            physical_recontact_phase_end_seconds=landing['simulation']-fire_sim if landing else None,
            physical_recontact_rider_time_error_seconds=abs(landing['wall']-landings[0]) if landing else None,
            completion_signal=attempt['completion_signal'],landed_scoring_signal=attempt['landed_scoring_signal']))
    result=dict(schema=2,event='bmx_observed_holder_local_posture_tracks',target=args.target,timeline_sha256=driver['timeline_sha256'],
                source_rider_sha256=comparison.sha(args.rider.read_bytes()),coverage_sha256=comparison.sha(args.coverage.read_bytes()),
                driver_sha256=comparison.sha(args.driver.read_bytes()),native_X_reflected_once=reflect,
                neutral_and_deltas_share_export_basis=True,
                basis='+Y up,+Z forward,+X module rider-left; source holder COM/orientation removed before neutral posture subtraction',
                available_source_bone_names=sorted({name[5:] for frame in frames for name in frame['local'] if name.startswith('bone:')}),
                rejected_frames=rejected,tracks=tracks,unexported_attempts=unexported,
                limits=['Posture offsets are observed samples, not recovered original AnimationClip curves.',
                        'Reference motion is analysis-only and must not be added again to host physics/holder motion.',
                        'Neutral posture is per-attempt componentwise position median and quaternion medoid.',
                        'Bone delta rotations are relative changes in holder axes; no bone-rest retargeting is inferred.',
                        'Missing rotations, IK anchors and weight helpers remain unavailable.',
                        'Explicit partial windows end at the last available same-holder observation; missing tails are not reconstructed.',
                        'Physical recontact timing is independent of trick completion or landed scoring.'])
    with args.output.open('x',encoding='utf-8') as target:json.dump(result,target,indent=2)
    print(json.dumps(dict(output=str(args.output),tracks=len(tracks),unexported=len(unexported),source_bones=len(result['available_source_bone_names']),rejected_frames=rejected),indent=2))


def sample(track,key,name,time,max_gap=.15):
    keys=track['keys'];times=[entry['t'] for entry in keys];i=bisect.bisect_right(times,time)-1
    if i<0 or i>=len(keys):return None
    if abs(time-times[i])<1e-9:return keys[i].get(key,{}).get(name)
    if i==len(keys)-1 or times[i+1]-times[i]>max_gap:return None
    a=keys[i].get(key,{}).get(name);b=keys[i+1].get(key,{}).get(name)
    if a is None or b is None:return None
    t=(time-times[i])/(times[i+1]-times[i])
    return slerp(a,b,t) if key=='delta_rotations' else [x+(y-x)*t for x,y in zip(a,b)]


def compare(args):
    native=json.loads(args.streets.read_text(encoding='utf-8-sig'));demo=json.loads(args.demo.read_text(encoding='utf-8-sig'))
    if native['target']!='streets' or demo['target']!='demo' or native['timeline_sha256']!=demo['timeline_sha256']:
        raise ValueError('Expected matched native/demo posture exports')
    nmap={(track['set'],track['slot']):track for track in native['tracks']};dmap={(track['set'],track['slot']):track for track in demo['tracks']};results=[]
    for pair in sorted(nmap.keys()&dmap.keys()):
        a,b=nmap[pair],dmap[pair];ad=a['physical_recontact_phase_end_seconds'];bd=b['physical_recontact_phase_end_seconds']
        if not ad or not bd or ad<=0 or bd<=0:continue
        channels={};names={name for track in (a,b) for key in track['keys'] for name in key['delta_positions']}
        for name in sorted(names):
            positions=[];rotations=[]
            for step in range(51):
                phase=step/50;av=sample(a,'delta_positions',name,phase*ad);bv=sample(b,'delta_positions',name,phase*bd)
                if av is not None and bv is not None:positions.append(math.dist(av,bv))
                aq=sample(a,'delta_rotations',name,phase*ad);bq=sample(b,'delta_rotations',name,phase*bd)
                if aq is not None and bq is not None:rotations.append(angle(aq,bq))
            channels[name]=dict(position_covered_samples=len(positions),rotation_covered_samples=len(rotations),requested_samples=51,
                position_rmse_m=math.sqrt(sum(v*v for v in positions)/len(positions)) if positions else None,
                maximum_position_error_m=max(positions) if positions else None,
                rotation_rmse_degrees=math.sqrt(sum(v*v for v in rotations)/len(rotations)) if rotations else None,
                maximum_rotation_error_degrees=max(rotations) if rotations else None)
        results.append(dict(set=pair[0],slot=pair[1],name=a['name'],native_fire_to_recontact_engine_seconds=ad,
                            demo_fire_to_recontact_engine_seconds=bd,unwarped_duration_error_seconds=bd-ad,channels=channels))
    result=dict(schema=1,event='bmx_phase_normalized_posture_comparison',tracks=results,
                native_export_sha256=comparison.sha(args.streets.read_bytes()),demo_export_sha256=comparison.sha(args.demo.read_bytes()),
                native_source_bones=len(native['available_source_bone_names']),demo_source_bones=len(demo['available_source_bone_names']),
                limits=['51 samples normalize observed fire-to-physical-recontact duration; unwarped duration errors remain explicit.',
                        'No interpolation over rider sample gaps larger than150ms.',
                        'Position and quaternion coverage are independent; missing rotations are not reconstructed from limb positions.',
                        'Neutral offsets are removed, so compare absolute neutral geometry separately.',
                        'Contact-anchor channels are distinct from bone origins; IK helper weights may remain unavailable.'])
    with args.output.open('x',encoding='utf-8') as target:json.dump(result,target,indent=2)
    print(json.dumps(dict(output=str(args.output),compared_tracks=len(results),native_source_bones=result['native_source_bones'],demo_source_bones=result['demo_source_bones']),indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);commands=parser.add_subparsers(dest='command',required=True)
    export_parser=commands.add_parser('export')
    for name in ('rider','driver','coverage','output'):export_parser.add_argument('--'+name,type=Path,required=True)
    export_parser.add_argument('--target',choices=['streets','demo'],required=True)
    export_parser.add_argument('--allow-demo-body-reference',action='store_true');export_parser.set_defaults(function=export)
    export_parser.add_argument('--allow-partial',action='store_true',help='Keep explicitly marked same-holder observed windows censored by bail/source loss; never reconstruct their tails')
    compare_parser=commands.add_parser('compare')
    for name in ('streets','demo','output'):compare_parser.add_argument('--'+name,type=Path,required=True)
    compare_parser.set_defaults(function=compare);args=parser.parse_args();args.function(args)
