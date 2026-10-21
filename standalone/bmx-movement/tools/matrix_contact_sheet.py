"""Extract one actual paired video frame near each observed trick's enter peak."""
from __future__ import annotations
import argparse
import bisect
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct

spec=importlib.util.spec_from_file_location('matrix_sheet_clock',Path(__file__).with_name('comparison.py'))
c=importlib.util.module_from_spec(spec);spec.loader.exec_module(c)


def main(args):
    from PIL import Image,ImageDraw
    if args.output.exists() or args.output.with_suffix('.validation.json').exists():raise ValueError('Fresh contact-sheet output required')
    audit_path=args.comparison/'comparison-video.validation.json';audit=json.loads(audit_path.read_text())
    avi=Path(audit['path']);driver=c.read_driver(args.driver,'demo');origin=c.utc(driver['timeline_start_utc'])
    provenance=json.loads(args.provenance.read_text());descriptors={row['module_id']:row for row in provenance['tracks']}
    native_status={};native_coverage_hash=None
    if args.native_coverage:
        native_coverage=json.loads(args.native_coverage.read_text())
        report=json.loads((args.comparison/'comparison.json').read_text())
        if native_coverage['driver_sha256']!=report['source_driver_hashes']['streets']:
            raise ValueError('Native coverage belongs to a different controller capture')
        native_coverage_hash=c.sha(args.native_coverage.read_bytes())
        for attempt in native_coverage['attempts']:
            id=(attempt.get('historical') or {}).get('module_id')
            if id is None:continue
            if attempt.get('correct_fire_observed') is not True:native_status[id]='Native: no fire observed; see retry'
            elif attempt.get('native_animation_exit_to_idle_observed') is not True:native_status[id]='Native: completion censored; see retry'
    with avi.open('rb') as stream:
        if hashlib.file_digest(stream,'sha256').hexdigest()!=audit['sha256']:raise ValueError('Paired video changed')
    selected={}
    with args.rider.open(encoding='utf-8-sig') as stream:
        for line in stream:
            frame=json.loads(line);id=frame.get('pose_track_id',-1);track=descriptors.get(id)
            if not track or frame.get('valid') is False:continue
            clock=c.finite(frame.get('pose_sample_seconds'));target=.85*track['enter_end_seconds']
            if clock is None or clock<0:continue
            candidate=dict(id=id,name=track['name'],t=frame['utc_seconds']-origin,playback_clock=clock,target_clock=target,error=abs(clock-target))
            if id not in selected or candidate['error']<selected[id]['error']:selected[id]=candidate
    mapping=audit['source_frame_mapping'];times=[row['t'] for row in mapping];wanted={}
    for id,item in selected.items():
        index=bisect.bisect_left(times,item['t']);index=min(range(max(0,index-1),min(len(times),index+1)),key=lambda i:abs(times[i]-item['t']))
        item['comparison_frame_index']=index;item['comparison_frame_time']=times[index];item['frame_mapping']=mapping[index]
        item['native_coverage_note']=native_status.get(id)
        wanted.setdefault(index,[]).append(item)
    images={}
    with avi.open('rb') as stream:
        if stream.read(4)!=b'RIFF':raise ValueError('RIFF source required')
        stream.seek(12);movi=None
        while True:
            header=stream.read(8)
            if len(header)!=8:break
            tag,size=header[:4],struct.unpack_from('<I',header,4)[0];payload=stream.tell()
            if tag==b'LIST' and stream.read(4)==b'movi':movi=stream.tell();break
            stream.seek(payload+size+(size&1))
        if movi is None:raise ValueError('AVI movie list missing')
        for index in range(audit['encoded_frames']):
            header=stream.read(8);size=struct.unpack_from('<I',header,4)[0]
            if header[:4]!=b'00dc':raise ValueError('Unexpected paired frame chunk')
            if index in wanted:
                data=stream.read(size)
                with Image.open(io.BytesIO(data)) as image:images[index]=image.convert('RGB').copy()
                for item in wanted[index]:item['paired_frame_jpeg_sha256']=c.sha(data)
            else:stream.seek(size,1)
            if size&1:stream.seek(1,1)
    columns=4;width=480;height=185;rows=(len(selected)+columns-1)//columns
    canvas=Image.new('RGB',(columns*width,rows*height),(9,18,28));draw=ImageDraw.Draw(canvas)
    for index,(id,item) in enumerate(sorted(selected.items())):
        x=(index%columns)*width;y=(index//columns)*height
        draw.text((x+5,y+3),f'{id//10}:{id%10}  {item["name"]} | matched input +{item["comparison_frame_time"]:.2f}s',fill=(240,240,250))
        note=item.get('native_coverage_note');image_y=37 if note else 23
        if note:draw.text((x+5,y+18),note,fill=(255,183,105))
        image=images[item['comparison_frame_index']];image.thumbnail((width,height-image_y));canvas.paste(image,(x,y+image_y))
    canvas.save(args.output,quality=95)
    result=dict(schema=1,event='all_observed_tricks_actual_paired_contact_sheet',actual_paired_video_frames=True,
        source_video_sha256=audit['sha256'],source_rider_sha256=c.sha(args.rider.read_bytes()),source_driver_sha256=c.sha(args.driver.read_bytes()),
        source_native_coverage_sha256=native_coverage_hash,
        output_sha256=c.sha(args.output.read_bytes()),selected_tracks=len(selected),tracks=[item for _,item in sorted(selected.items())],
        limits=['One actual paired video frame is selected nearest85percent of the source enter boundary in the recorded module playback clock.',
            'The native panel retains the identical requested wall-time alignment; differing native engine pacing can put it at a different animation phase.',
            'Resizing and labels only; no synthesized joints, motion or substituted game frames.'])
    with args.output.with_suffix('.validation.json').open('x') as stream:json.dump(result,stream,indent=2)
    print(json.dumps(dict(output=str(args.output),observed_tracks=len(selected)),indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('comparison','driver','rider','provenance','output'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--native-coverage',type=Path,help='Pin and label missing/censored native attempts without substituting frames')
    main(parser.parse_args())
