"""Rebuild existing native-library position tracks from original compressed resources.

Writes a new staging folder, never the source library. Rotations, scales, times and
phase markers remain unchanged. Unknown encodings/conventions fail per clip.
"""
import argparse
import concurrent.futures
import json
from pathlib import Path
import struct
import subprocess

import numpy as np

from native_piecewise import read_native_vectors, native_dof_names, require


def read_clip(path):
    blob = path.read_bytes()
    magic,n,f,up,down = struct.unpack_from('<4sIIff',blob)
    require(magic in (b'FAS2',b'FAS3') and n==395 and 0<f<=20000, 'Unsupported source clip')
    board = magic==b'FAS2' or bool(blob[20]); offset=21 if magic==b'FAS3' else 20
    times = np.frombuffer(blob,dtype='<f4',count=f,offset=offset).copy(); offset+=f*4
    if magic==b'FAS3':
        moves=np.frombuffer(blob,dtype=np.uint8,count=n,offset=offset).astype(bool); offset+=n
        fixed=int(np.sum(~moves)); frames=np.zeros((f,n,10),dtype='<f4')
        frames[:,~moves,:]=np.frombuffer(blob,dtype='<f4',count=fixed*10,offset=offset).reshape(fixed,10); offset+=fixed*40
        dynamic=np.frombuffer(blob,dtype='<f4',offset=offset).reshape(f,int(np.sum(moves))+int(board),10)
        frames[:,moves,:]=dynamic[:,:int(np.sum(moves)),:]
        boards=dynamic[:,-1,:].copy() if board else None
    else:
        frames=np.frombuffer(blob,dtype='<f4',count=f*n*10,offset=offset).reshape(f,n,10).copy(); offset+=f*n*40
        boards=np.frombuffer(blob,dtype='<f4',offset=offset).reshape(f,10).copy() if board else None
    require(np.isfinite(frames).all() and np.all(np.diff(times)>0), 'Invalid source pose/time')
    return times,frames,boards,up,down


def encode_clip(times,frames,boards,up,down):
    f,n,_=frames.shape; moves=np.any(frames!=frames[0],axis=(0,2))
    header=struct.pack('<4sIIffB',b'FAS3',n,f,up,down,int(boards is not None))
    out=bytearray(header+times.astype('<f4').tobytes()+moves.astype(np.uint8).tobytes())
    out.extend(frames[0,~moves].astype('<f4').tobytes())
    for i in range(f):
        out.extend(frames[i,moves].astype('<f4').tobytes())
        if boards is not None:out.extend(boards[i].astype('<f4').tobytes())
    return bytes(out)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game',required=True,type=Path)
    parser.add_argument('--probe',required=True,type=Path)
    parser.add_argument('--library',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--asset-index',required=True,type=Path)
    parser.add_argument('--only')
    args=parser.parse_args()
    require(not args.output.exists(), 'Choose a new staging directory')
    args.output.mkdir(parents=True)
    skeleton=args.output/'animation-skeleton.json'
    subprocess.run([str(args.probe),'--asset',str(args.game),'animation/dingo/animbase_default_skeleton',str(skeleton)],check=True)
    fields=json.loads(skeleton.read_text())['instances'][0]['object']['fields']
    table=bytes(fields['NameTable']); names=[]
    for joint in fields['Joints']:
        start=joint['fields']['Name'];names.append(table[start:table.index(0,start)].decode())
    indices={name:i for i,name in enumerate(names)}
    dof_names=native_dof_names(args.probe,args.game,args.output/'bindings',args.asset_index)
    (args.output/'dof-names.json').write_text(json.dumps(dof_names,indent=2))
    catalog=json.loads((args.library/'catalog.json').read_text())
    chosen=[e for e in catalog if not args.only or e['trick']==args.only]
    def repair(entry):
        try:
            filename=entry['file']; require(Path(filename).name==filename and filename.endswith('.clip'),'Unsafe clip name')
            vectors=read_native_vectors(args.probe,args.game,entry['asset'],args.output/'native'/Path(filename).stem,dof_names)
            times,frames,boards,up,down=read_clip(args.library/filename)
            before=frames.copy(); report=[]
            for bone,track in vectors.items():
                j=indices[bone]
                def sampled(field):return np.column_stack([np.interp(times*60,track['frames'],track[field][:,a]) for a in range(3)])
                native,legacy=sampled('values'),sampled('legacy_values')
                ne=float(np.max(np.abs(frames[:,j,7:]-native)));le=float(np.max(np.abs(frames[:,j,7:]-legacy)))
                require(min(ne,le)<3e-5,f'Unknown source position convention for {bone}: {ne}/{le}')
                frames[:,j,7:]=native
                report.append({'joint':bone,'convention':'native' if ne<3e-5 else 'extent_as_maximum','max_correction_metres':ne})
            if boards is not None:
                require(np.max(np.abs(boards[:,3:7]-frames[:,380,3:7]))<1e-5,'Source board rotation differs from joint 380')
                boards[:,7:]=frames[:,380,7:]
            require(np.array_equal(before[:,:,:7],frames[:,:,:7]),'Rotation/scale changed')
            (args.output/filename).write_bytes(encode_clip(times,frames,boards,up,down))
            return {'file':filename,'ok':True,'vectors':report,'frames':len(times)}
        except Exception as error:
            return {'file':entry['file'],'ok':False,'error':str(error)}
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        results=list(pool.map(repair,chosen))
    (args.output/'repair-report.json').write_text(json.dumps(results,indent=2))
    fixed_catalog=[]
    for entry in chosen:
        if not any(r['file']==entry['file'] and r['ok'] for r in results):continue
        controller=json.loads((args.output/'native'/Path(entry['file']).stem/'controller.json').read_text())
        looping=controller['instances'][0]['object']['fields']['IsLooping']
        fixed_catalog.append(dict(entry,position_codec='native-piec-v1-extent',native_looping=looping))
    (args.output/'catalog.json').write_text(json.dumps(fixed_catalog,indent=2))
    print(json.dumps({'repaired':sum(r['ok'] for r in results),'failed':[r for r in results if not r['ok']]},indent=2))


if __name__=='__main__':main()
