"""Encode frames exported by the running demo's own OpenGL framebuffer.

This does not capture the desktop or another app. It accepts only a completed
demo-owned framebuffer export (frames.csv plus capture-summary.json), preserves
source BMP hashes/times, and verifies every MJPEG AVI frame/container index.
Pillow is the only dependency. --gif adds a compact actual-frame preview.
"""
from __future__ import annotations
import argparse
import bisect
import csv
import hashlib
import io
import json
import math
from pathlib import Path
import struct

from encode_recording import chunk


def encode(directory: Path, fps: int = 10, preview_gif: bool = False) -> dict:
    from PIL import Image
    directory = directory.resolve(strict=True)
    if not directory.is_dir() or not 1 <= fps <= 30:
        raise ValueError('Expected a framebuffer directory and 1..30 presentation fps')
    ledger_path = directory/'frames.csv'
    summary_path = directory/'capture-summary.json'
    summary = json.loads(summary_path.read_text(encoding='utf-8-sig'))
    if (summary.get('completed') is not True or summary.get('rendered_live') is not True
            or summary.get('window_capture') is not False):
        raise ValueError('Completed live demo-owned framebuffer summary required')
    with ledger_path.open(encoding='utf-8-sig', newline='') as source:
        reader = csv.DictReader(source)
        if not {'index','file','elapsed_seconds','simulation_seconds','width','height'}.issubset(reader.fieldnames or []):
            raise ValueError('Frame CSV is missing required identity/timing fields')
        rows = list(reader)
    if not 2 <= len(rows) <= 3600 or summary.get('frames') != len(rows):
        raise ValueError('Frame count mismatch or outside 2..3600 frame limit')
    output = directory/'motion-demo.avi'
    audit_path = directory/'motion-demo.validation.json'
    gif_path = directory/'motion-demo.gif'
    if output.exists() or audit_path.exists() or (preview_gif and gif_path.exists()):
        raise ValueError('Existing encoded recording cannot be overwritten')
    width, height = int(rows[0]['width']), int(rows[0]['height'])
    if not 16 <= width <= 8192 or not 16 <= height <= 8192:
        raise ValueError('Frame dimensions outside expected bounds')
    encoded, times, samples = [], [], []
    source_bytes = 0
    for i, row in enumerate(rows):
        relative = Path(row['file'])
        if relative.name != row['file'] or relative.suffix.lower() != '.bmp':
            raise ValueError('Each source must be a BMP basename in the frame directory')
        file = (directory/relative).resolve(strict=True)
        if file.parent != directory or file.is_symlink():
            raise ValueError('Framebuffer path escaped source directory')
        elapsed, simulation = float(row['elapsed_seconds']), float(row['simulation_seconds'])
        if (int(row['index']) != i or int(row['width']) != width or int(row['height']) != height
                or not all(math.isfinite(x) and x >= 0 for x in (elapsed,simulation))
                or (i and (elapsed <= times[-1] or simulation < samples[-1]['simulation_seconds']))):
            raise ValueError('Frame index, dimensions, or monotonic timing failed validation')
        data = file.read_bytes(); source_bytes += len(data)
        if len(data) < 54 or len(data) > 268_435_456 or data[:2] != b'BM' or source_bytes > 2_147_483_648:
            raise ValueError('Framebuffer BMP/storage bounds exceeded')
        with Image.open(io.BytesIO(data)) as image:
            if image.format != 'BMP' or image.size != (width,height):
                raise ValueError('Decoded framebuffer dimensions/format differ from ledger')
            image.load()
            buffer=io.BytesIO();image.convert('RGB').save(buffer,format='JPEG',quality=94)
            encoded.append(buffer.getvalue())
        times.append(elapsed)
        samples.append({'index':i,'file':file.name,'sha256':hashlib.sha256(data).hexdigest(),'bytes':len(data),
                        'elapsed_seconds':elapsed,'simulation_seconds':simulation,'width':width,'height':height})
    count=math.ceil(max(0.,(times[-1]-times[0])*fps-1e-9))+1
    selections=[max(0,bisect.bisect_right(times,times[0]+i/fps)-1) for i in range(count)]
    maximum=max(map(len,encoded))
    avih=struct.pack('<14I',round(1_000_000/fps),maximum*fps,0,0x10,count,0,1,maximum,width,height,0,0,0,0)
    strh=struct.pack('<4s4sIHHIIIIIIIIhhhh',b'vids',b'MJPG',0,0,0,0,1,fps,0,count,maximum,0xffffffff,0,0,0,width,height)
    strf=struct.pack('<IiiHH4sIiiII',40,width,height,1,24,b'MJPG',width*height*3,0,0,0,0)
    hdrl=chunk(b'LIST',b'hdrl'+chunk(b'avih',avih)+chunk(b'LIST',b'strl'+chunk(b'strh',strh)+chunk(b'strf',strf)))
    movi_size=4+sum(8+len(encoded[j])+(len(encoded[j])&1) for j in selections)
    if movi_size+len(hdrl)+16*count > 1_900_000_000:
        raise ValueError('Encoded AVI exceeds portable AVI1 size limit')
    index, offset = [], 4
    with output.open('xb') as target:
        target.write(b'RIFF\0\0\0\0AVI ');target.write(hdrl)
        target.write(b'LIST'+struct.pack('<I',movi_size)+b'movi')
        for j in selections:
            payload=encoded[j];frame=chunk(b'00dc',payload);target.write(frame)
            index.append(struct.pack('<4sIII',b'00dc',0x10,offset,len(payload)));offset+=len(frame)
        target.write(chunk(b'idx1',b''.join(index)));length=target.tell()
        target.seek(4);target.write(struct.pack('<I',length-8))
    data=output.read_bytes()
    if data[:4]!=b'RIFF' or struct.unpack_from('<I',data,4)[0]!=len(data)-8 or data[8:12]!=b'AVI ':
        raise ValueError('Encoded AVI container validation failed')
    position=12+len(hdrl)+12
    for j in selections:
        n=struct.unpack_from('<I',data,position+4)[0]
        if data[position:position+4]!=b'00dc' or data[position+8:position+8+n]!=encoded[j]:
            raise ValueError('Encoded AVI frame bytes differ from encoded source')
        with Image.open(io.BytesIO(data[position+8:position+8+n])) as image:
            if image.size!=(width,height) or image.format!='JPEG':
                raise ValueError('Encoded AVI frame decoding validation failed')
            image.verify()
        position+=8+n+(n&1)
    if data[position:position+4]!=b'idx1' or struct.unpack_from('<I',data,position+4)[0]!=16*count:
        raise ValueError('Encoded AVI index validation failed')
    audit={'schema':1,'event':'bmx_standalone_framebuffer_video','video':str(output),'sha256':hashlib.sha256(data).hexdigest(),
           'bytes':len(data),'source_csv':str(ledger_path),'source_csv_sha256':hashlib.sha256(ledger_path.read_bytes()).hexdigest(),
           'source_summary':str(summary_path),'source_summary_sha256':hashlib.sha256(summary_path.read_bytes()).hexdigest(),
           'capture_method':'Actual running demo-owned OpenGL framebuffer export; not a desktop/window recording.',
           'rendered_live':True,'window_capture':False,'os_input_injected_by_encoder':False,
           'source_frames':len(rows),'source_duration_seconds':times[-1]-times[0],
           'source_sample_rate_hz':(len(rows)-1)/(times[-1]-times[0]),
           'maximum_source_gap_seconds':max(b-a for a,b in zip(times,times[1:])),
           'source_frames_with_identity_and_timing':samples,'encoded_frames':count,'presentation_fps':fps,
           'duration_seconds':count/fps,'source_frame_for_each_encoded_frame':selections,
           'constant_rate_presentation_repeats_source_frames':True,'native_60fps_capture':False,
           'container_frame_bytes_and_jpeg_decoding_verified':True,'audio_tracks':0,'gameplay_parity_proven':False}
    if preview_gif:
        preview=[]
        for j in selections:
            with Image.open(io.BytesIO(encoded[j])) as image:
                image.thumbnail((800,450));preview.append(image.convert('RGB'))
        preview[0].save(gif_path,save_all=True,append_images=preview[1:],duration=round(1000/fps),loop=0,optimize=False)
        audit['preview_gif']=str(gif_path);audit['preview_gif_sha256']=hashlib.sha256(gif_path.read_bytes()).hexdigest()
        with Image.open(gif_path) as image:
            audit['preview_gif_decoded_frames']=image.n_frames
    with audit_path.open('x',encoding='utf-8') as target:json.dump(audit,target,indent=2)
    return {k:v for k,v in audit.items() if k not in ['source_frames_with_identity_and_timing','source_frame_for_each_encoded_frame']}


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('directory',type=Path)
    parser.add_argument('--fps',type=int,default=10);parser.add_argument('--gif',action='store_true')
    args=parser.parse_args();print(json.dumps(encode(args.directory,args.fps,args.gif),indent=2))
