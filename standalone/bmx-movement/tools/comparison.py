"""Compare matched controller runs and compose actual source frames side by side.

Metrics require completed matching driver timelines. UTC alignment is preferred;
an explicit offset is required when a telemetry clock has no UTC. Visual output
reuses measured source images, with resizing/labels and held frames only. It does
not infer hidden rider joints, game physics values, or full behavioral parity.
"""
from __future__ import annotations
import argparse
import bisect
import csv
from datetime import datetime
import hashlib
import io
import json
import math
from pathlib import Path
import struct


def sha(data): return hashlib.sha256(data).hexdigest()
def finite(value):
    try:
        result=float(value)
        return result if math.isfinite(result) else None
    except (TypeError,ValueError):return None
def utc(value):
    parsed=datetime.fromisoformat(value.replace('Z','+00:00'))
    if parsed.tzinfo is None:raise ValueError('UTC alignment needs an explicit timezone')
    return parsed.timestamp()
def boolean(value):
    if str(value).lower() in ('1','true'):return True
    if str(value).lower() in ('0','false'):return False
    return None
def chunk(tag,payload):return tag+struct.pack('<I',len(payload))+payload+(b'\0' if len(payload)&1 else b'')


def read_driver(path,target):
    result=json.loads(path.read_text(encoding='utf-8-sig'))
    if result.get('event')!='bmx_matched_virtual_controller' or result.get('target')!=target:
        raise ValueError('Expected matched driver receipt for '+target)
    if (result.get('completed') is not True or result.get('failure') or
            result.get('neutral_bootstrap_proven') is not True or
            result.get('neutral_release_succeeded') is not True or
            result.get('disconnect_readback_proven') is not True):
        raise ValueError('Successful driver run, verified neutral and cleanup required')
    reports=result.get('reports',[])
    if not reports or not all(r.get('matches') is True and not r.get('xinput_error') for r in reports):
        raise ValueError('Every driver readback must match its requested report')
    if not result.get('timeline_sha256') or not result.get('timeline_start_utc'):
        raise ValueError('Driver receipt lacks timeline identity or UTC origin')
    return result


def read_telemetry(path,driver,offset=None,origin_utc=None,allow_body_changes=False):
    with path.open(newline='',encoding='utf-8-sig') as source:rows=list(csv.DictReader(source))
    if not rows:raise ValueError('Empty telemetry')
    start=utc(driver['timeline_start_utc'])
    if finite(rows[0].get('utc_seconds')) is not None:
        clock='per-sample UTC epoch seconds relative to driver timeline';time_for=lambda r:float(r['utc_seconds'])-start
    elif rows[0].get('utc'):
        clock='per-sample UTC relative to driver timeline';time_for=lambda r:utc(r['utc'])-start
    elif origin_utc:
        shift=start-utc(origin_utc);clock='declared simulation clock UTC origin';time_for=lambda r:float(r['time'])-shift
    elif offset is not None:
        clock='explicit telemetry offset supplied by caller';time_for=lambda r:float(r['time'])-offset
    else:raise ValueError('Telemetry without UTC requires --offset or --origin-utc')
    reset_times=[];previous_reset=False
    for report in driver.get('reports',[]):
        pressed=bool(int(report['requested']['buttons'])&0x0002)
        if pressed and not previous_reset:reset_times.append(utc(report['utc'])-start)
        previous_reset=pressed
    out=[];invalid=0;duplicates=0
    for r in rows:
        if boolean(r.get('valid','1')) is False:invalid+=1;continue
        t=time_for(r)
        values={key:finite(r.get(key)) for key in ('x','y','z','vx','vy','vz','wx','wy','wz','qx','qy','qz','qw',
                                                'vehicle_com_x','vehicle_com_y','vehicle_com_z','mass')}
        values['simulation_time']=finite(r.get('simulation_seconds',r.get('time')))
        if not math.isfinite(t):raise ValueError('Nonfinite telemetry time')
        if out and t<out[-1]['t']:raise ValueError('Telemetry UTC moved backward')
        if all(values[k] is not None for k in ('vx','vz')):values['speed']=math.hypot(values['vx'],values['vz'])
        else:values['speed']=None
        sample=dict(t=t,**values,grounded=boolean(r.get('grounded')),trick_id=finite(r.get('trick_id')),
                    body_id=r.get('body_id'),raw=r,
                    segment_id=(r.get('body_id'),r.get('reset_counter'),bisect.bisect_right(reset_times,t)),
                    reset_settle_exclusion=any(reset<=t<=reset+.45 for reset in reset_times))
        if out and t==out[-1]['t']:out[-1]=sample;duplicates+=1
        else:out.append(sample)
    if len(out)<2:raise ValueError('Insufficient valid telemetry')
    identities={r['body_id'] for r in out if r['body_id']}
    if len(identities)>1 and not allow_body_changes:raise ValueError('Telemetry changed measured body identity during run; reset scenarios require --allow-body-changes')
    transitions=[dict(t=b['t'],previous_body_id=a['body_id'],body_id=b['body_id'])
                 for a,b in zip(out,out[1:]) if a['body_id']!=b['body_id']]
    return out,dict(path=str(path),sha256=sha(path.read_bytes()),rows=len(rows),valid_rows=len(out),
                    invalid_rows=invalid,duplicate_wallclock_rows_collapsed_to_last=duplicates,
                    clock_alignment=clock,first_aligned_seconds=out[0]['t'],
                    last_aligned_seconds=out[-1]['t'],body_id=next(iter(identities),None) if len(identities)<=1 else None,
                    body_ids=sorted(identities),body_identity_changes_allowed=allow_body_changes,body_identity_transitions=transitions,
                    verified_controller_reset_request_times_seconds=reset_times,
                    interpolation_exclusion_after_each_reset_seconds=.45,
                    body_name=out[0]['raw'].get('body_name'),measured_body_mass_kg=out[0]['mass'],
                    velocity_reference='Observed selected Rigidbody COM velocity; source compositions may differ')


def interpolate(rows,key,time):
    times=[r['t'] for r in rows];i=bisect.bisect_right(times,time)-1
    if i<0 or i>=len(rows):return None
    if i==len(rows)-1:return rows[i].get(key) if abs(time-times[i])<1e-9 else None
    a,b=rows[i].get(key),rows[i+1].get(key)
    if a is None or b is None:return None
    # Do not interpolate across a lost/pause-sized observation interval.
    if (times[i+1]-times[i]>.1 or rows[i].get('body_id')!=rows[i+1].get('body_id')
            or rows[i].get('segment_id')!=rows[i+1].get('segment_id')
            or rows[i].get('reset_settle_exclusion') or rows[i+1].get('reset_settle_exclusion')):return None
    return a+(b-a)*(time-times[i])/(times[i+1]-times[i])


def ballistic_velocity_fit(rows,start,end):
    samples=[r for r in rows if start<=r['t']<=end and r['grounded'] is False and r['vy'] is not None]
    if len(samples)<8:return None
    # Contact transitions are excluded; use the interior of this observed flight.
    a,b=samples[0]['t'],samples[-1]['t'];margin=(b-a)*.12
    samples=[r for r in samples if a+margin<=r['t']<=b-margin]
    if len(samples)<5 or samples[-1]['t']-samples[0]['t']<.1:return None
    xs=[r['t']-start for r in samples];ys=[r['vy'] for r in samples]
    mx,my=sum(xs)/len(xs),sum(ys)/len(ys);variance=sum((x-mx)**2 for x in xs)
    if variance<=1e-12:return None
    slope=sum((x-mx)*(y-my) for x,y in zip(xs,ys))/variance;intercept=my-slope*mx
    error=sum((y-(intercept+slope*x))**2 for x,y in zip(xs,ys));total=sum((y-my)**2 for y in ys)
    engine_fit=None
    simulation=[finite(r.get('raw',{}).get('simulation_seconds',r.get('raw',{}).get('time'))) for r in samples]
    if all(t is not None for t in simulation) and simulation[-1]>simulation[0]:
        sx=[t-simulation[0] for t in simulation];mean=sum(sx)/len(sx);variance_sim=sum((x-mean)**2 for x in sx)
        if variance_sim>1e-12:
            slope_sim=sum((x-mean)*(y-my) for x,y in zip(sx,ys))/variance_sim;intercept_sim=my-slope_sim*mean
            err_sim=sum((y-intercept_sim-slope_sim*x)**2 for x,y in zip(sx,ys))
            engine_fit=dict(effective_downward_acceleration_mps2=-slope_sim,residual_rmse_mps=math.sqrt(err_sim/len(sx)),
                            simulation_interval_seconds=[simulation[0],simulation[-1]],
                            note='OLS against recorded engine simulation clock, which may differ from wall-clock capture pacing.')
    return dict(model='OLS measured COM vertical velocity = intercept + slope * time since request',
                observed_interior_samples=len(samples),fit_interval_seconds=[samples[0]['t'],samples[-1]['t']],
                effective_downward_acceleration_mps2=-slope,velocity_intercept_at_request_mps=intercept,
                residual_rmse_mps=math.sqrt(error/len(samples)),r_squared=1-error/total if total>1e-12 else None,
                engine_simulation_clock_fit=engine_fit,
                interpretation='Effective observed flight fit; may include controls/drag/constraints. Not proof of the original gravity or hop equation.')


def run_metrics(rows,scenario):
    maximum_end=(288 if scenario=='trick_matrix_reset_fast' else 128 if '_matrix_reset_' in scenario else 230.4 if scenario=='trick_matrix'
                 else 57.6 if scenario.startswith('trick_matrix_') else 32 if scenario in ('air_rotation','controls_probe','visual_gap_retry')
                 else 18 if scenario=='suspension' else 40)
    observed=[r for r in rows if 0<=r['t']<=maximum_end]
    speeds=[r['speed'] for r in observed if r['speed'] is not None]
    y=[r['y'] for r in observed if r['y'] is not None]
    vehicle_y=[r['vehicle_com_y'] for r in observed if r['vehicle_com_y'] is not None]
    vy=[r['vy'] for r in observed if r['vy'] is not None]
    result={'observed_interval_seconds':[observed[0]['t'],observed[-1]['t']] if observed else None,
            'maximum_horizontal_speed_mps':max(speeds) if speeds else None,
            'maximum_upward_speed_mps':max(vy) if vy else None,
            'vertical_range_m':max(y)-min(y) if y else None,
            'combined_vehicle_com_vertical_range_m':max(vehicle_y)-min(vehicle_y) if vehicle_y else None,
            'grounded_samples':sum(r['grounded'] is True for r in observed),
            'clear_samples':sum(r['grounded'] is False for r in observed),
            'unknown_grounded_samples':sum(r['grounded'] is None for r in observed)}
    takeoffs=[];landings=[]
    for before,after in zip(observed,observed[1:]):
        if (before.get('body_id')!=after.get('body_id') or after['t']-before['t']>.1
                or before.get('segment_id')!=after.get('segment_id')
                or before.get('reset_settle_exclusion') or after.get('reset_settle_exclusion')):continue
        if before['grounded'] is True and after['grounded'] is False:takeoffs.append(after['t'])
        if before['grounded'] is False and after['grounded'] is True:landings.append(after['t'])
    result['observed_takeoff_times_seconds']=takeoffs;result['observed_landing_times_seconds']=landings
    if scenario in ('accel_brake','full','flat_short'):
        times={'accel_brake':(.5,4.5,6.,9.),'full':(.5,4.5,5.2,8.5),'flat_short':(.5,2.5,3.,6.)}[scenario]
        values={str(t):interpolate(rows,'speed',t) for t in times}
        result['phase_boundary_speed_mps']=values
        a,b,c,d=(values[str(t)] for t in times)
        result['mean_pedal_acceleration_mps2']=(b-a)/(times[1]-times[0]) if a is not None and b is not None else None
        result['mean_brake_deceleration_mps2']=(c-d)/(times[3]-times[2]) if c is not None and d is not None else None
        clocks=[interpolate(rows,'simulation_time',t) for t in times]
        pedal_duration=clocks[1]-clocks[0] if all(x is not None for x in clocks[:2]) else None
        brake_duration=clocks[3]-clocks[2] if all(x is not None for x in clocks[2:]) else None
        result['phase_engine_clock']=dict(pedal_simulation_seconds=pedal_duration,brake_simulation_seconds=brake_duration,
            pedal_engine_to_wall_ratio=pedal_duration/(times[1]-times[0]) if pedal_duration is not None else None,
            brake_engine_to_wall_ratio=brake_duration/(times[3]-times[2]) if brake_duration is not None else None,
            mean_pedal_acceleration_mps2=(b-a)/pedal_duration if a is not None and b is not None and pedal_duration else None,
            mean_brake_deceleration_mps2=(c-d)/brake_duration if c is not None and d is not None and brake_duration else None)
    if scenario in ('full','flat_short'):
        phases=[]
        requests=[('quick_hop',10.,11.9),('hop_barspin_request',12.,14.5),('hop_tailwhip_request',15.,17.5)] if scenario=='full' else [('quick_hop',8.,11.8)]
        for name,start,end in requests:
            subset=[r for r in observed if start<=r['t']<=end]
            baseline=interpolate(rows,'y',start)
            heights=[r['y']-baseline for r in subset if r['y'] is not None and baseline is not None]
            combined_baseline=interpolate(rows,'vehicle_com_y',start)
            combined_heights=[r['vehicle_com_y']-combined_baseline for r in subset
                              if r['vehicle_com_y'] is not None and combined_baseline is not None]
            upward=[r['vy'] for r in subset if r['vy'] is not None]
            takeoff_times=[t for t in takeoffs if start<=t<=end]
            landing_times=[t for t in landings if start<=t<=end]
            takeoff_engine=[interpolate(rows,'simulation_time',t) for t in takeoff_times]
            landing_engine=[interpolate(rows,'simulation_time',t) for t in landing_times]
            phases.append(dict(name=name,interval_seconds=[start,end],
                maximum_rise_from_phase_start_m=max(heights) if heights else None,
                maximum_combined_vehicle_com_rise_from_phase_start_m=max(combined_heights) if combined_heights else None,
                maximum_upward_speed_mps=max(upward) if upward else None,
                observed_takeoffs=takeoff_times,observed_landings=landing_times,
                takeoff_engine_seconds=takeoff_engine,landing_engine_seconds=landing_engine,
                observed_airtime_engine_seconds=(landing_engine[0]-takeoff_engine[0]
                    if len(takeoff_engine)==len(landing_engine)==1 and takeoff_engine[0] is not None
                    and landing_engine[0] is not None else None),
                interior_vertical_velocity_fit=ballistic_velocity_fit(rows,start,end),
                note='Phase input requests; these measurements do not prove trick classification or landed scoring.'))
        result['hop_request_phases']=phases
    fires=[];previous=None
    for row in observed:
        elapsed=finite(row['raw'].get('trick_v2_time_since_fire'))
        if elapsed is not None:
            if previous is not None and elapsed<previous-.05:
                fires.append(dict(time=row['t'],set=finite(row['raw'].get('trick_v2_set')),
                    slot=finite(row['raw'].get('trick_v2_slot')),time_since_fire=elapsed,
                    evidence='Observed time-since-fire reset with current set/slot; not landed trick scoring.'))
            previous=elapsed
    if fires:result['trick_v2_fire_observations']=fires
    return result


def compare_metrics(streets,demo,scenario):
    start=max(0,streets[0]['t'],demo[0]['t']);end=min(streets[-1]['t'],demo[-1]['t'])
    if end-start<.5:raise ValueError('Less than half a second of aligned telemetry overlap')
    errors=[];samples=[]
    for i in range(math.floor((end-start)*60)+1):
        t=start+i/60;a=interpolate(streets,'speed',t);b=interpolate(demo,'speed',t)
        if a is not None and b is not None:errors.append(b-a);samples.append(dict(t=t,streets_speed=a,demo_speed=b))
    return {'scenario':scenario,'aligned_overlap_seconds':[start,end],
            'speed_comparison':{'resampling_hz':60,'covered_samples':len(errors),
                'rmse_mps':math.sqrt(sum(x*x for x in errors)/len(errors)) if errors else None,
                'mean_signed_error_mps':sum(errors)/len(errors) if errors else None,
                'maximum_absolute_error_mps':max(map(abs,errors)) if errors else None,
                'note':'Linear resampling of measured body velocity; gaps over100ms, identity/reset changes and450ms after verified reset requests are excluded.'},
            'streets':run_metrics(streets,scenario),'demo':run_metrics(demo,scenario),
            'aligned_speed_samples':samples}


def read_frames(directory,offset=0.):
    from PIL import Image
    directory=directory.resolve(strict=True)
    with (directory/'frames.csv').open(newline='',encoding='utf-8-sig') as source:rows=list(csv.DictReader(source))
    frames=[]
    for i,r in enumerate(rows):
        relative=Path(r['file']);file=(directory/relative).resolve(strict=True)
        if relative.name!=r['file'] or file.parent!=directory or file.is_symlink():raise ValueError('Frame path escaped source directory')
        time=finite(r['elapsed_seconds'])
        if time is None or (frames and time-offset<=frames[-1]['t']):raise ValueError('Frame times must strictly increase')
        data=file.read_bytes()
        with Image.open(io.BytesIO(data)) as image:
            if image.size!=(int(r['width']),int(r['height'])):raise ValueError('Frame dimensions differ from ledger')
            image.verify()
        frames.append(dict(index=i,t=time-offset,file=str(file),sha256=sha(data),bytes=len(data)))
    if len(frames)<2:raise ValueError('At least two actual source frames required')
    return frames


def frame_offset(directory,driver,explicit):
    if explicit is not None:return explicit,'explicit source-frame offset supplied by caller'
    summary=directory/'capture-summary.json'
    if not summary.exists():raise ValueError('Frames need explicit offset or capture-summary UTC origin')
    meta=json.loads(summary.read_text(encoding='utf-8-sig'))
    origin=finite(meta.get('start_utc_seconds'))
    if origin is None and meta.get('capture_started_utc'):origin=utc(meta['capture_started_utc'])
    if origin is None:raise ValueError('Frame summary lacks UTC origin; provide explicit offset')
    return utc(driver['timeline_start_utc'])-origin,'capture-summary UTC epoch origin relative to driver timeline'


def read_mjpeg(path,offset=0.):
    from PIL import Image
    data=path.read_bytes()
    if len(data)>1_900_000_000 or data[:4]!=b'RIFF' or data[8:12]!=b'AVI ' or struct.unpack_from('<I',data,4)[0]!=len(data)-8:
        raise ValueError('Expected bounded AVI1 MJPEG input')
    jpeg=[];microseconds=None
    def walk(start,end):
        nonlocal microseconds
        while start+8<=end:
            tag=data[start:start+4];n=struct.unpack_from('<I',data,start+4)[0];payload=start+8
            if payload+n>end:raise ValueError('AVI chunk exceeds container')
            if tag==b'avih':microseconds=struct.unpack_from('<I',data,payload)[0]
            elif tag in (b'00dc',b'00db'):
                block=data[payload:payload+n]
                if not block.startswith(b'\xff\xd8'):raise ValueError('Only MJPEG video supported')
                jpeg.append(block)
            elif tag==b'LIST' and n>=4:walk(payload+4,payload+n)
            start=payload+n+(n&1)
    walk(12,len(data))
    if not microseconds or not 2<=len(jpeg)<=3600:raise ValueError('Unsupported AVI frame timing/count')
    frames=[]
    for i,block in enumerate(jpeg):
        with Image.open(io.BytesIO(block)) as image:image.verify()
        frames.append(dict(index=i,t=i*microseconds/1e6-offset,file=str(path),sha256=sha(block),bytes=len(block),data=block))
    return frames


def write_avi(path,encoded,width,height,fps):
    maximum=max(map(len,encoded));count=len(encoded)
    avih=struct.pack('<14I',round(1e6/fps),maximum*fps,0,0x10,count,0,1,maximum,width,height,0,0,0,0)
    strh=struct.pack('<4s4sIHHIIIIIIIIhhhh',b'vids',b'MJPG',0,0,0,0,1,fps,0,count,maximum,0xffffffff,0,0,0,width,height)
    strf=struct.pack('<IiiHH4sIiiII',40,width,height,1,24,b'MJPG',width*height*3,0,0,0,0)
    hdrl=chunk(b'LIST',b'hdrl'+chunk(b'avih',avih)+chunk(b'LIST',b'strl'+chunk(b'strh',strh)+chunk(b'strf',strf)))
    movi=4+sum(8+len(p)+(len(p)&1) for p in encoded)
    if movi+len(hdrl)+16*count>1_900_000_000:raise ValueError('Comparison exceeds AVI1 bounds')
    index=[];offset=4
    with path.open('xb') as f:
        f.write(b'RIFF\0\0\0\0AVI '+hdrl+b'LIST'+struct.pack('<I',movi)+b'movi')
        for payload in encoded:
            record=chunk(b'00dc',payload);f.write(record);index.append(struct.pack('<4sIII',b'00dc',0x10,offset,len(payload)));offset+=len(record)
        f.write(chunk(b'idx1',b''.join(index)));length=f.tell();f.seek(4);f.write(struct.pack('<I',length-8))
    with path.open('rb') as actual:
        header=actual.read(12)
        if header[:4]!=b'RIFF' or struct.unpack_from('<I',header,4)[0]!=length-8:raise ValueError('Comparison RIFF length mismatch')
        actual.seek(12+len(hdrl)+12)
        for payload in encoded:
            header=actual.read(8)
            if len(header)!=8:raise ValueError('Comparison frame header truncated')
            n=struct.unpack_from('<I',header,4)[0]
            if header[:4]!=b'00dc' or n!=len(payload) or actual.read(n)!=payload:raise ValueError('Comparison frame mismatch')
            if n&1:actual.read(1)
        if actual.read(4)!=b'idx1':raise ValueError('Comparison index missing')
        actual.seek(0);digest=hashlib.file_digest(actual,'sha256').hexdigest()
    return dict(path=str(path),bytes=length,sha256=digest,container_and_frame_bytes_verified=True)


def compose(streets,demo,output,fps=10,gif=False,annotation=None,maximum_end=None,gif_maximum_seconds=12):
    from PIL import Image,ImageDraw,ImageOps
    start=max(0,streets[0]['t'],demo[0]['t']);end=min(streets[-1]['t'],demo[-1]['t'])
    full_overlap=[start,end]
    if maximum_end is not None:end=min(end,maximum_end)
    if end-start<.5:raise ValueError('Insufficient aligned frame overlap')
    count=math.floor((end-start)*fps)+1
    if count>3600:raise ValueError('More than 3600 comparison frames; reduce --fps or clip explicitly with --video-max-end')
    encoded=[];mapping=[];preview=[];sheet=[]
    preview_stride=max(1,math.ceil(fps/5))
    sheet_indices={round(index*(count-1)/11) for index in range(12)}
    st=[f['t'] for f in streets];dt=[f['t'] for f in demo]
    for i in range(count):
        t=start+i/fps;a=streets[bisect.bisect_right(st,t)-1];b=demo[bisect.bisect_right(dt,t)-1]
        canvas=Image.new('RGB',(1280,428),(10,18,28));draw=ImageDraw.Draw(canvas)
        title_y=4 if annotation else 12
        draw.text((18,title_y),'BMX STREETS / ACTUAL CAPTURE',fill=(230,240,250))
        draw.text((658,title_y),'STANDALONE C++ / ACTUAL CAPTURE',fill=(230,240,250))
        if annotation:draw.text((18,20),annotation[:180],fill=(250,190,105))
        for x,frame in ((0,a),(640,b)):
            data=frame.get('data')
            if data is None:
                data=Path(frame['file']).read_bytes()
                if len(data)!=frame['bytes'] or sha(data)!=frame['sha256']:raise ValueError('Source frame changed after validation')
            with Image.open(io.BytesIO(data)) as image:canvas.paste(ImageOps.contain(image.convert('RGB'),(640,360)),(x,34))
        draw.text((18,406),f'INPUT TIMELINE +{t:.2f}s | source ages {t-a["t"]:.3f}s / {t-b["t"]:.3f}s | resized source frames; no generated motion',fill=(160,190,210))
        buffer=io.BytesIO();canvas.save(buffer,format='JPEG',quality=94);encoded.append(buffer.getvalue())
        if i in sheet_indices:
            still=canvas.copy();still.thumbnail((640,214));sheet.append((i,still))
        if gif and i%preview_stride==0 and t-start<gif_maximum_seconds:
            still=canvas.copy();still.thumbnail((960,321));preview.append(still)
        mapping.append(dict(t=t,streets_source_index=a['index'],streets_source_sha256=a['sha256'],streets_source_time=a['t'],
                            demo_source_index=b['index'],demo_source_sha256=b['sha256'],demo_source_time=b['t']))
    audit=write_avi(output/'comparison.avi',encoded,1280,428,fps)
    audit.update(schema=1,event='bmx_measured_comparison_video',presentation_fps=fps,encoded_frames=count,
                 aligned_time_range_seconds=[start,start+(count-1)/fps],resized_actual_source_frames=True,
                 unclipped_source_overlap_seconds=full_overlap,requested_maximum_end_seconds=maximum_end,
                 generated_motion=False,held_frames_possible=True,annotation=annotation,source_frame_mapping=mapping,
                 streets_sources=[{k:v for k,v in f.items() if k!='data'} for f in streets],
                 demo_sources=[{k:v for k,v in f.items() if k!='data'} for f in demo])
    if gif:
        path=output/'comparison.gif';preview[0].save(path,save_all=True,append_images=preview[1:],duration=round(1000*preview_stride/fps),loop=0,optimize=False)
        audit['gif']=dict(path=str(path),sha256=sha(path.read_bytes()),preview_only=True,
            aligned_time_range_seconds=[start,start+(len(preview)-1)*preview_stride/fps],frames=len(preview))
    sheet_image=Image.new('RGB',(1280,math.ceil(len(sheet)/2)*214),(10,18,28))
    for index,(_,still) in enumerate(sheet):sheet_image.paste(still,((index%2)*640,(index//2)*214))
    sheet_path=output/'contact-sheet.jpg';sheet_image.save(sheet_path,quality=95)
    audit['contact_sheet']=dict(path=str(sheet_path),sha256=sha(sheet_path.read_bytes()),
        comparison_frame_indices=[index for index,_ in sheet],actual_source_frame_mapping=[mapping[index] for index,_ in sheet])
    (output/'comparison-video.validation.json').write_text(json.dumps(audit,indent=2)+'\n')
    return {k:v for k,v in audit.items() if k not in ('source_frame_mapping','streets_sources','demo_sources')}


def main(args):
    if args.video_max_end is not None and (not math.isfinite(args.video_max_end) or args.video_max_end<=0):
        raise ValueError('Video maximum end must be finite and positive')
    if not math.isfinite(args.gif_max_seconds) or not 1<=args.gif_max_seconds<=60:
        raise ValueError('GIF preview duration must be1..60 seconds')
    sd=read_driver(args.streets_driver,'streets');dd=read_driver(args.demo_driver,'demo')
    if sd['timeline_sha256']!=dd['timeline_sha256'] or sd['scenario']!=dd['scenario']:
        raise ValueError('Driver runs do not use identical requested timelines')
    if args.video_only and not ((args.streets_frames or args.streets_video) and (args.demo_frames or args.demo_video)):
        raise ValueError('--video-only requires actual frames/video for both targets')
    if not args.video_only and (args.streets_csv is None or args.demo_csv is None):
        raise ValueError('Both telemetry files are required unless --video-only is explicit')
    output=args.output.resolve(strict=False)
    output.mkdir(parents=True,exist_ok=False)
    if args.video_only:
        smeta=dmeta=dict(fixed_physics_comparison_available=False,reason='Explicit actual-frame-only comparison; no physics telemetry is used')
        metrics=dict(scenario=sd['scenario'],physics_comparison_available=False,aligned_speed_samples=[])
    else:
        streets,smeta=read_telemetry(args.streets_csv,sd,args.streets_offset,args.streets_origin_utc,args.allow_body_changes)
        demo,dmeta=read_telemetry(args.demo_csv,dd,args.demo_offset,args.demo_origin_utc,args.allow_body_changes)
        metrics=compare_metrics(streets,demo,sd['scenario'])
    result=dict(schema=1,event='bmx_matched_run_comparison',timeline_sha256=sd['timeline_sha256'],scenario=sd['scenario'],
                streets_source=smeta,demo_source=dmeta,metrics=metrics,
                calibration_eligible=False if args.calibration_ineligible_reason else None,
                calibration_exclusions=args.calibration_ineligible_reason,
                calibration_phase_eligibility={phase:(False if phase in args.exclude_calibration_phase else
                    True if phase in args.verified_calibration_phase else None) for phase in ('pedal','coast','brake','hop','rider','tricks')},
                annotation=args.annotation,
                source_driver_hashes=dict(streets=sha(args.streets_driver.read_bytes()),demo=sha(args.demo_driver.read_bytes())),
                limits=['Identical requested inputs do not establish identical effective game bindings or settings.',
                        'Camera/scene differences and unavailable fields limit visual or numerical attribution.',
                        'Body motion metrics do not establish rider animation, contact or all trick parity.',
                        'Interpret calibration only with independently verified initial state, surface and active mods.'])
    sf=args.streets_frames or args.streets_video;df=args.demo_frames or args.demo_video
    if bool(sf)!=bool(df):raise ValueError('Provide source frames/video for BOTH targets, or neither')
    if sf:
        if args.streets_frames:
            so,sc=frame_offset(sf,sd,args.streets_frames_offset);a=read_frames(sf,so)
        else:
            if args.streets_frames_offset is None:raise ValueError('AVI source needs explicit frame offset')
            so,sc=args.streets_frames_offset,'explicit AVI presentation offset';a=read_mjpeg(sf,so)
        if args.demo_frames:
            do,dc=frame_offset(df,dd,args.demo_frames_offset);b=read_frames(df,do)
        else:
            if args.demo_frames_offset is None:raise ValueError('AVI source needs explicit frame offset')
            do,dc=args.demo_frames_offset,'explicit AVI presentation offset';b=read_mjpeg(df,do)
        result['video']=compose(a,b,output,args.fps,args.gif,args.annotation,args.video_max_end,args.gif_max_seconds)
        result['video']['alignment']=dict(streets_offset=so,streets_method=sc,demo_offset=do,demo_method=dc)
    (output/'comparison.json').write_text(json.dumps(result,indent=2)+'\n')
    samples=metrics['aligned_speed_samples']
    with (output/'aligned-speeds.csv').open('w',newline='') as f:
        writer=csv.DictWriter(f,fieldnames=['t','streets_speed','demo_speed']);writer.writeheader();writer.writerows(samples)
    print(json.dumps({k:v for k,v in result.items() if k!='metrics'},indent=2))
    print(json.dumps({k:v for k,v in metrics.items() if k!='aligned_speed_samples'},indent=2))


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for target in ('streets','demo'):
        p.add_argument('--'+target+'-csv',type=Path);p.add_argument('--'+target+'-driver',type=Path,required=True)
        p.add_argument('--'+target+'-offset',type=float);p.add_argument('--'+target+'-origin-utc')
        group=p.add_mutually_exclusive_group();group.add_argument('--'+target+'-frames',type=Path);group.add_argument('--'+target+'-video',type=Path)
        p.add_argument('--'+target+'-frames-offset',type=float)
    p.add_argument('--output',type=Path,required=True);p.add_argument('--fps',type=int,choices=range(1,31),default=10);p.add_argument('--gif',action='store_true')
    p.add_argument('--calibration-ineligible-reason',action='append',default=[],help='Explicitly exclude known contaminated scenes from tuning claims')
    p.add_argument('--annotation',help='Visible comparison label, e.g. terrain-contaminated exploratory recording')
    p.add_argument('--video-max-end',type=float,help='Clip video only at this timeline second; all source frames remain in the audit')
    p.add_argument('--gif-max-seconds',type=float,default=12,help='Prefix preview duration; full AVI is preserved (default12 seconds)')
    p.add_argument('--allow-body-changes',action='store_true',help='Explicit respawn bench: retain and report each body identity; never interpolate across identity changes')
    p.add_argument('--video-only',action='store_true',help='Compare actual source frames without unavailable fixed-physics telemetry; metrics are explicitly disabled')
    p.add_argument('--exclude-calibration-phase',action='append',choices=['pedal','coast','brake','hop','rider','tricks'],default=[])
    p.add_argument('--verified-calibration-phase',action='append',choices=['pedal','coast','brake','hop','rider','tricks'],default=[])
    main(p.parse_args())
