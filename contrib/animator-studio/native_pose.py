"""Source skater poses directly from the measured rig/default pose and PIEC q1/q2/v1."""
import numpy as np

from native_piecewise import decode_quaternions, decode_vectors, require, sample_quaternions


def default_pose(rig, dofs, names):
    root=rig['instances'][0]['object']['fields']
    ref=root['DefaultPoseAsset']
    require(ref['kind']=='internal', 'Unsupported default-pose reference')
    fields=rig['instances'][ref['instance']]['object']['fields']
    indices={n:i for i,n in enumerate(names)}
    pose=np.zeros((len(names),10),dtype='<f4');seen=set()
    for prefix,axes in [('Vector3',('x','y','z')),('Vector4',('x','y','z','w'))]:
        ids,values=fields[prefix+'DofIds'],fields[prefix+'Values']
        require(len(ids)==len(values), 'Default pose inventory mismatch')
        for dof,value in zip(ids,values):
            channel=dofs.get(dof,'');bone=channel[:-2]
            if bone not in indices or channel[-2:] not in ('.t','.q','.s'):continue
            start={'.s':0,'.q':3,'.t':7}[channel[-2:]]
            require((start==3)==(len(axes)==4), 'Default pose DOF type mismatch')
            require(channel not in seen, 'Duplicate default pose channel');seen.add(channel)
            pose[indices[bone],start:start+len(axes)]=[value['fields'][a] for a in axes]
    require(all(n+suffix in seen for n in names for suffix in ('.s','.q','.t')), 'Incomplete native default pose')
    require(np.isfinite(pose).all() and np.all(pose[:,:3]>0), 'Invalid native default pose')
    return pose


def decode_pose(blob, channels, defaults, names, end_frame):
    require(isinstance(end_frame,int) and 0<=end_frame<20000, 'Unsupported native frame range')
    indices={n:i for i,n in enumerate(names)}
    ticks=np.arange(end_frame+1,dtype=float)
    frames=np.tile(defaults,(len(ticks),1,1))
    quaternions=decode_quaternions(blob,channels)
    vectors=decode_vectors(blob,channels,qualified=True,allow_linear_short=True)
    mapped=0
    for name,track in quaternions.items():
        if name[:-2] in indices:
            frames[:,indices[name[:-2]],3:7]=sample_quaternions(track,ticks);mapped+=1
    for name,track in vectors.items():
        if name[:-2] not in indices:continue
        values=np.column_stack([np.interp(ticks,track['frames'],track['values'][:,a]) for a in range(3)])
        start=7 if name.endswith('.t') else 0
        frames[:,indices[name[:-2]],start:start+3]=values;mapped+=1
    require(mapped>0, 'No mapped skater pose channels')
    require(np.isfinite(frames).all() and np.all(frames[:,:,:3]>0), 'Invalid decoded pose')
    return (ticks/60).astype('<f4'),frames
