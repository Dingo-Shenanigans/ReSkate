"""Encode verified live demo screenshots to MJPEG AVI; no screen or input APIs.

Requires Pillow for PNG decoding. Encoding repeats actual captured frames on a
constant presentation grid; the adjacent audit preserves measured source timing.
"""
from __future__ import annotations
import argparse
import bisect
import hashlib
import io
import json
import math
from pathlib import Path
import struct


def chunk(tag: bytes, payload: bytes) -> bytes:
    return tag + struct.pack('<I', len(payload)) + payload + (b'\0' if len(payload) & 1 else b'')


def encode(manifest_path: Path, output: Path | None = None, fps: int = 10) -> dict:
    from PIL import Image
    manifest_path = manifest_path.resolve(strict=True)
    manifest = json.loads(manifest_path.read_text(encoding='utf-8-sig'))
    if (manifest.get('schema') != 1 or manifest.get('event') != 'bmx_standalone_sampled_live_capture'
            or manifest.get('sampled_live') is not True or manifest.get('completed') is not True or manifest.get('failure')):
        raise ValueError('Successful completed standalone sampled-live manifest required')
    samples = manifest.get('samples', [])
    if not 2 <= len(samples) <= 600 or not 1 <= fps <= 30:
        raise ValueError('Expected 2..600 frames and 1..30 fps')
    width, height = samples[0]['width'], samples[0]['height']
    if not 16 <= width <= 8192 or not 16 <= height <= 8192:
        raise ValueError('Invalid image dimensions')
    encoded, times = [], []
    for i, s in enumerate(samples):
        if (s['index'] != i or s['window_id'] != manifest['source_window']['id'] or
                s['app'] != manifest['source_window']['app'] or s['width'] != width or s['height'] != height):
            raise ValueError('Frame source/dimensions mismatch')
        file = (manifest_path.parent / s['file']).resolve(strict=True)
        if file.parent != manifest_path.parent or file.suffix.lower() not in ('.png', '.jpg', '.jpeg'):
            raise ValueError('Frame path escaped capture directory')
        data = file.read_bytes()
        if len(data) != s['bytes'] or hashlib.sha256(data).hexdigest() != s['sha256'].lower():
            raise ValueError('Captured frame identity mismatch')
        a, b, t = (s['capture_started_monotonic_ms'], s['capture_ended_monotonic_ms'], s['presentation_monotonic_ms'])
        if not all(math.isfinite(v) for v in (a,b,t)) or not 0 <= a <= t <= b or (i and a < samples[i-1]['capture_ended_monotonic_ms']):
            raise ValueError('Invalid measured capture interval')
        if i and t <= times[-1]:
            raise ValueError('Capture midpoint timestamps must increase')
        times.append(t)
        with Image.open(io.BytesIO(data)) as image:
            if image.size != (width,height):
                raise ValueError('Decoded source dimensions differ')
            if file.suffix.lower() in ('.jpg','.jpeg'):
                image.verify()
                encoded.append(data)
            else:
                buffer=io.BytesIO(); image.convert('RGB').save(buffer,format='JPEG',quality=94)
                encoded.append(buffer.getvalue())
    count = math.ceil((times[-1]-times[0])*fps/1000)+1
    selections = [max(0,bisect.bisect_right(times,times[0]+i*1000/fps)-1) for i in range(count)]
    maximum = max(map(len,encoded))
    avih = struct.pack('<14I',round(1000000/fps),maximum*fps,0,0x10,count,0,1,maximum,width,height,0,0,0,0)
    strh = struct.pack('<4s4sIHHIIIIIIIIhhhh',b'vids',b'MJPG',0,0,0,0,1,fps,0,count,maximum,0xffffffff,0,0,0,width,height)
    strf = struct.pack('<IiiHH4sIiiII',40,width,height,1,24,b'MJPG',width*height*3,0,0,0,0)
    hdrl = chunk(b'LIST',b'hdrl'+chunk(b'avih',avih)+chunk(b'LIST',b'strl'+chunk(b'strh',strh)+chunk(b'strf',strf)))
    output = (output or manifest_path.parent/'sampled-live.avi').resolve(strict=False)
    if output.parent != manifest_path.parent or output.suffix.lower() != '.avi':
        raise ValueError('Output must be an AVI in the capture directory')
    audit_path = output.with_suffix('.validation.json')
    if output.exists() or audit_path.exists():
        raise ValueError('Existing encoded video/audit cannot be overwritten')
    index,offset=[],4
    movi_size=4+sum(8+len(encoded[j])+(len(encoded[j])&1) for j in selections)
    if movi_size+len(hdrl)+16*count > 1_900_000_000:
        raise ValueError('AVI size limit exceeded')
    with output.open('xb') as f:
        f.write(b'RIFF\0\0\0\0AVI ');f.write(hdrl);f.write(b'LIST'+struct.pack('<I',movi_size)+b'movi')
        for j in selections:
            payload=encoded[j]; frame=chunk(b'00dc',payload);f.write(frame)
            index.append(struct.pack('<4sIII',b'00dc',0x10,offset,len(payload)));offset+=len(frame)
        f.write(chunk(b'idx1',b''.join(index)));length=f.tell();f.seek(4);f.write(struct.pack('<I',length-8))
    # Independently walk the container and check all actual frame bytes/indexes.
    data=output.read_bytes()
    if data[:4]!=b'RIFF' or struct.unpack_from('<I',data,4)[0]!=len(data)-8 or data[8:12]!=b'AVI ':
        raise ValueError('Encoded AVI header failed validation')
    pos=12+len(hdrl)+12
    for j in selections:
        n=struct.unpack_from('<I',data,pos+4)[0]
        if data[pos:pos+4]!=b'00dc' or data[pos+8:pos+8+n]!=encoded[j]:
            raise ValueError('Encoded frame validation failed')
        pos+=8+n+(n&1)
    if data[pos:pos+4]!=b'idx1' or struct.unpack_from('<I',data,pos+4)[0]!=16*count:
        raise ValueError('AVI index validation failed')
    audit={'schema':1,'event':'bmx_standalone_sampled_live_video','video':str(output),'bytes':len(data),
           'sha256':hashlib.sha256(data).hexdigest(),'source_manifest':str(manifest_path),
           'source_manifest_sha256':hashlib.sha256(manifest_path.read_bytes()).hexdigest(),
           'source_frames':len(samples),'encoded_frames':count,'presentation_fps':fps,'duration_seconds':count/fps,
           'source_sample_rate_hz':(len(samples)-1)*1000/(times[-1]-times[0]),
           'maximum_source_gap_ms':max(b-a for a,b in zip(times,times[1:])),
           'sampled_live':True,'constant_rate_presentation_repeats_source_frames':True,
           'source_frame_for_each_encoded_frame':selections,'native_60fps_capture':False,'audio_tracks':0,
           'gameplay_parity_proven':False,'container_and_frame_bytes_verified':True}
    with audit_path.open('x',encoding='utf-8') as f:json.dump(audit,f,indent=2)
    return audit


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest',type=Path)
    parser.add_argument('--output',type=Path)
    parser.add_argument('--fps',type=int,default=10)
    args=parser.parse_args()
    print(json.dumps(encode(args.manifest,args.output,args.fps),indent=2))
