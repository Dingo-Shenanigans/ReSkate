"""Differential native pose validation against the CLI's independently exported FBX.

Checks local rotations/scales and positions after correcting the known exporter
extent defect. No package is compiled, installed or launched.
"""
import argparse
import concurrent.futures
import json
from pathlib import Path
import struct
import subprocess
import tempfile

import numpy as np

from clip import Clip
from decode_native_tree import local_frames
from native_piecewise import decode_vectors, require
from native_pose import decode_pose, default_pose


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    for name in ('cache','game','cli','dof-names','report'):ap.add_argument('--'+name,type=Path,required=True)
    ap.add_argument('--codec',type=int,default=2)
    ap.add_argument('--workers',type=int,default=4)
    args=ap.parse_args()
    root=args.cache
    index=json.loads((root/'native-asset-types.json').read_text());by_name={x['asset']:x for x in index}
    def doc(guid):return json.loads((root/'documents'/(guid+'.json')).read_text())
    rig=by_name['animation/dingo/animbase_default_rig']
    s=doc(by_name['animation/dingo/animbase_default_skeleton']['partition'])['instances'][0]['object']['fields']
    table=bytes(s['NameTable']);names=[table[x['fields']['Name']:table.index(0,x['fields']['Name'])].decode() for x in s['Joints']]
    parents=[x['fields']['Parent'] for x in s['Joints']]
    dofs={int(k):v for k,v in json.loads(args.dof_names.read_text()).items()}
    defaults=default_pose(doc(rig['partition']),dofs,names)
    jobs=[]
    for source in index:
        if source['root_type']!='ClipControllerAsset':continue
        d=doc(source['partition'])
        if (d['instances'][0]['object']['fields'].get('RigRef') or {}).get('partition')!=rig['partition']:continue
        for instance in d['instances']:
            if instance['object']['type']!='EclipseAnimationAsset':continue
            a=instance['object']['fields'];blob=(root/'resources'/(str(a['Resource']['resource_id'])+'.bin')).read_bytes()
            if struct.unpack_from('<I',blob,36)[0]==args.codec:jobs.append((source,a,blob))
    def check(job):
        source,a,blob=job
        result=dict(asset=source['asset'])
        try:
            ids=doc(a['ChannelToDofAsset']['partition'])['instances'][0]['object']['fields']['ChannelToDofIds']
            channels=[dofs[i] for i in ids]
            times,actual=decode_pose(blob,channels,defaults,names,a['EndFrame'])
            with tempfile.TemporaryDirectory(prefix='native-validation-') as temp:
                fbx=Path(temp)/'reference.fbx'
                process=subprocess.run([str(args.cli),'animation-export',str(args.game),source['asset'],str(fbx)],capture_output=True,text=True,timeout=120)
                require(process.returncode==0,'Reference exporter failed: '+(process.stderr or process.stdout)[-300:])
                expected_times,expected=local_frames(Clip(fbx),names,parents,decode_vectors(blob,channels,allow_linear_short=True))
            require(np.array_equal(times,expected_times),'Reference timing differs')
            q,z=actual[:,:,3:7].astype(float),expected[:,:,3:7].astype(float)
            q/=np.linalg.norm(q,axis=2)[:,:,None];z/=np.linalg.norm(z,axis=2)[:,:,None]
            dot=np.clip(abs(np.sum(q*z,axis=2)),0,1)
            angle=float(np.max(np.degrees(2*np.arccos(dot))))
            position=float(np.max(abs(actual[:,:,7:]-expected[:,:,7:])))
            scale=float(np.max(abs(actual[:,:,:3]-expected[:,:,:3])))
            result.update(rotation_degrees=angle,position_metres=position,scale=scale)
            require(angle<.05 and position<1e-5 and scale<1e-5,'Native/reference transform budget exceeded')
            result['passed']=True
        except Exception as error:result.update(passed=False,error=str(error))
        return result
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:results=list(pool.map(check,jobs))
    args.report.parent.mkdir(parents=True,exist_ok=True)
    args.report.write_text(json.dumps(results,indent=2))
    print(json.dumps(dict(clips=len(results),passed=sum(x['passed'] for x in results),failed=[x for x in results if not x['passed']])),flush=True)
    require(results and all(x['passed'] for x in results),'Native conformance validation failed; inspect report')


if __name__=='__main__':main()
