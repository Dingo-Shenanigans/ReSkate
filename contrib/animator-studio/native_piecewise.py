"""Measured PIEC vector decoder: little-endian descriptors, big-endian u16 samples.

The final descriptor vector is an extent, not an upper bound. Only the native
vector-codec-1 and byte/big-endian-u16 times measured on this build are accepted.
"""
import struct
import json
import subprocess
import concurrent.futures
import numpy as np


def require(test, message):
    if not test:
        raise ValueError(message)


def decode_vectors(blob, channels, qualified=False, allow_linear_short=False):
    require(len(blob) >= 96, 'Truncated PIEC resource')
    nq = struct.unpack_from('<I', blob, 0)[0]
    count, offset = struct.unpack_from('<IQ', blob, 12)
    nf = struct.unpack_from('<I', blob, 24)[0]
    require(len(channels) == nq+count+nf, 'Native channel inventory mismatch')
    time_codec=struct.unpack_from('<I', blob, 48)[0]
    require(struct.unpack_from('<I', blob, 40)[0] == 1 and
            time_codec in (0,1), 'Unsupported vector/time codec')
    size, data = struct.unpack_from('<IQ', blob, 56)
    require(80 <= offset <= data and offset+48*count <= data and
            data+size <= len(blob)-16, 'Invalid PIEC descriptor/data bounds')
    result = {}
    for i, name in enumerate(channels[nq:nq+count]):
        key = name if qualified else name[:-2]
        require(name.endswith(('.t', '.s') if qualified else '.t') and key not in result, 'Invalid vector binding')
        pos = offset+48*i
        n, times, values = struct.unpack_from('<III', blob, pos)
        minimum = np.array(struct.unpack_from('<3f', blob, pos+16))
        extent = np.array(struct.unpack_from('<3f', blob, pos+32))
        if n==2:
            require(times<values and np.isfinite(minimum).all() and np.isfinite(extent).all() and (allow_linear_short or np.array_equal(minimum,extent)),
                    'Unmeasured short vector encoding; refusing to guess')
            result[key]={'frames':np.array([times,values],dtype=float),
                         'values':np.array([minimum,extent]), 'legacy_values':np.array([minimum,extent])}
            continue
        require(3 <= n <= 20000, 'Invalid vector key count')
        width=1 if time_codec==0 else 2
        require(times+width*n <= size and values+6*n <= size, 'Vector keys outside resource')
        t = np.frombuffer(blob, dtype=np.uint8 if width==1 else '>u2', count=n, offset=data+times).astype(float)
        require(np.all(np.diff(t) > 0), 'Unordered vector keys')
        require(np.isfinite(minimum).all() and np.isfinite(extent).all() and
                np.all(extent >= 0), 'Invalid vector quantization bounds')
        u = np.frombuffer(blob, dtype='>u2', count=3*n, offset=data+values).reshape(n,3).astype(float)/65535
        result[key] = {'frames': t, 'values': minimum+u*extent,
                            'legacy_values': minimum+u*(extent-minimum)}
    return result


def decode_quaternions(blob, channels):
    """Measured q1/q2: 48-bit BE or 64-bit LE dynamic samples, 64-bit LE constants."""
    require(len(blob) >= 96, 'Truncated quaternion resource')
    nq, offset = struct.unpack_from('<IQ', blob, 0)
    nv, nf = [struct.unpack_from('<I', blob, at)[0] for at in (12,24)]
    codec, time_codec = struct.unpack_from('<I', blob,36)[0], struct.unpack_from('<I',blob,48)[0]
    size,data=struct.unpack_from('<IQ',blob,56)
    require(len(channels)==nq+nv+nf and codec in (1,2) and time_codec in (0,1), 'Unsupported quaternion codec/inventory')
    require(80<=offset and offset+12*nq<=data and data+size<=len(blob)-16, 'Invalid quaternion descriptor bounds')
    result={}
    for i,name in enumerate(channels[:nq]):
        require(name.endswith('.q') and name not in result, 'Invalid quaternion binding')
        at=offset+12*i; n,times,values=struct.unpack_from('<III',blob,at)
        require(1<=n<=20000, 'Invalid quaternion key count')
        if n==1:
            packed=np.array([struct.unpack_from('<Q',blob,at+4)[0]],dtype=np.uint64)
            t=np.array([0.0])
        else:
            width=1 if time_codec==0 else 2
            require(times+width*n<=size and values+(6 if codec==1 else 8)*n<=size, 'Quaternion keys outside resource')
            t=np.frombuffer(blob,dtype=np.uint8 if width==1 else '>u2',count=n,offset=data+times).astype(float)
            require(np.all(np.diff(t)>0), 'Unordered quaternion keys')
            if codec==1:
                u=np.frombuffer(blob,dtype='>u2',count=3*n,offset=data+values).reshape(n,3).astype(np.uint32)
                missing=(u[:,0]&1)*2+(u[:,1]&1);u[:,:2]&=~np.uint32(1)
                v=(u.astype(float)-32768)/(32768*np.sqrt(2))
            else:packed=np.frombuffer(blob,dtype='<u8',count=n,offset=data+values)
        if n==1 or codec==2:
            missing=(packed&3).astype(int)
            v=np.column_stack([(((packed>>shift)&((1<<bits)-1)).astype(float)/(1<<(bits-1))-1)/np.sqrt(2)
                               for shift,bits in ((43,21),(22,21),(2,20))])
        square=np.sum(v*v,axis=1)
        require(np.all(square<=1+1e-6), 'Invalid packed quaternion')
        q=np.empty((n,4))
        for omitted in range(4):
            mask=missing==omitted
            q[np.ix_(mask,np.arange(4)!=omitted)]=v[mask]
            q[mask,omitted]=np.sqrt(np.maximum(0,1-square[mask]))
        result[name]={'frames':t,'values':q}
    return result


def sample_quaternions(track, frames):
    t,q=track['frames'],track['values']
    lo=np.clip(np.searchsorted(t,frames,side='right')-1,0,len(t)-1);hi=np.minimum(lo+1,len(t)-1)
    span=t[hi]-t[lo]
    u=np.clip(np.divide(frames-t[lo],span,out=np.zeros(len(frames)),where=span!=0),0,1)
    a,b=q[lo],q[hi].copy();dot=np.sum(a*b,axis=1);b[dot<0]*=-1
    angle=np.arccos(np.clip(abs(dot),0,1));sine=np.sin(angle)
    wa=np.divide(np.sin((1-u)*angle),sine,out=(1-u).copy(),where=sine>1e-8)
    wb=np.divide(np.sin(u*angle),sine,out=u.copy(),where=sine>1e-8)
    result=a*wa[:,None]+b*wb[:,None]
    return result/np.linalg.norm(result,axis=1)[:,None]


def cached_dof_names(rig, read_document):
    """Verify DOF identities against extracted native layouts, without another game scan."""
    root=rig['instances'][0]['object']['fields'];ids=root['DofIds'];starts=root['DofSetIdIndices']
    require(len(starts)==len(root['DofSets']), 'Rig DOF range mismatch')
    result={}
    for i,ref in enumerate(root['DofSets']):
        fields=read_document(ref['partition'])['instances'][0]['object']['fields']
        words=fields['LayoutData'];blob=struct.pack('<'+str(len(words))+'Q',*words)
        text,size=struct.unpack_from('<II',blob,16)
        end=starts[i+1] if i+1<len(starts) else len(ids);count=end-starts[i]
        require(count>=0 and text==32+32*count and text+size<=len(blob), 'Unsupported cached DOF layout')
        for j in range(count):
            offset=struct.unpack_from('<I',blob,32+j*32+12)[0]
            require(offset<size, 'DOF name outside layout')
            name=blob[text+offset:blob.index(0,text+offset,text+size)].decode()
            dof=ids[starts[i]+j]
            require(dof not in result or result[dof]==name, 'Conflicting cached DOF identities')
            result[dof]=name
    return result


def correct_translations(clip, vectors):
    """Replace only source position curves after verifying the exporter convention."""
    report = []
    for bone, track in vectors.items():
        require(bone in clip.node, 'Native vector joint missing from exported skeleton')
        sample_frames = (clip.times-clip.start)*60
        def sampled(field):
            return np.column_stack([np.interp(sample_frames, track['frames'], track[field][:,a]) for a in range(3)])
        native, legacy = sampled('values'), sampled('legacy_values')
        before = clip.channel(bone, 'T')
        native_error = float(np.max(np.abs(before-native)))
        legacy_error = float(np.max(np.abs(before-legacy)))
        require(min(native_error,legacy_error) < 2e-5,
                f'Unknown exporter position convention for {bone}: native={native_error}, legacy={legacy_error}')
        clip.set_channel(bone, 'T', native)
        report.append({'joint': bone, 'exporter': 'native' if native_error < 2e-5 else 'extent_as_maximum',
                       'max_translation_correction_metres': native_error})
    return report


def decode_scalars(blob, channels):
    nq,nv,nf=[struct.unpack_from('<I',blob,i)[0] for i in (0,12,24)]
    require(len(channels)==nq+nv+nf, 'Scalar inventory mismatch')
    offset=struct.unpack_from('<Q',blob,28)[0]
    codec,time_codec=struct.unpack_from('<II',blob,44)
    require(codec==0 and time_codec in (0,1), 'Unsupported native scalar codec')
    size,data=struct.unpack_from('<IQ',blob,56)
    require(80<=offset and offset+nf*20<=data and data+size<=len(blob)-16, 'Invalid scalar bounds')
    result={}
    for i,name in enumerate(channels[nq+nv:]):
        n,times,values,minimum,extent=struct.unpack_from('<IIIff',blob,offset+i*20)
        require(np.isfinite((minimum,extent)).all(), 'Nonfinite scalar descriptor')
        if n==2:
            require(times<values, 'Unordered inline scalar keys')
            t=np.array([times,values],dtype=float); decoded=np.array([minimum,extent])
            legacy=decoded.copy()
        else:
            width=1 if time_codec==0 else 2
            require(3<=n<=20000 and times+width*n<=size and values+n<=size and extent>=0,'Invalid scalar key bounds')
            t=np.frombuffer(blob,dtype=np.uint8 if width==1 else '>u2',count=n,offset=data+times).astype(float)
            require(np.all(np.diff(t)>0), 'Unordered scalar keys')
            u=np.frombuffer(blob,dtype=np.uint8,count=n,offset=data+values).astype(float)/255
            decoded=minimum+u*extent; legacy=minimum+u*(extent-minimum)
        result[name]={'frames':t,'values':decoded,'legacy_values':legacy}
    return result


def correct_scalar_helpers(clip, scalars, ids):
    report=[]
    for name,track in scalars.items():
        bone=f'RS_DOF_{ids[name]}'
        require(bone in clip.node, f'Missing exported scalar helper {bone}')
        sample_frames=(clip.times-clip.start)*60
        native=np.interp(sample_frames,track['frames'],track['values'])
        legacy=np.interp(sample_frames,track['frames'],track['legacy_values'])
        before=clip.channel(bone,'T');ne=float(np.max(np.abs(before[:,0]-native)));le=float(np.max(np.abs(before[:,0]-legacy)))
        require(min(ne,le)<2e-5, f'Unknown scalar exporter convention for {name}')
        before[:,0]=native;clip.set_channel(bone,'T',before)
        report.append({'dof':name,'max_correction':ne})
    return report


def native_dof_names(probe, game, folder, asset_index):
    """Resolve serialized DofIds through the rig's ordered native DofSet layouts."""
    folder.mkdir(parents=True, exist_ok=True)
    names={entry['partition']:entry['asset'] for entry in json.loads(asset_index.read_text())}
    def read(asset,path):
        subprocess.run([str(probe),'--asset',str(game),asset,str(path)],check=True,capture_output=True)
        return json.loads(path.read_text())['instances'][0]['object']['fields']
    rig=read('animation/dingo/animbase_default_rig',folder/'rig.json')
    starts=rig['DofSetIdIndices']; ids=rig['DofIds']
    require(len(starts)==len(rig['DofSets']), 'Native rig binding ranges disagree')
    def decode(item):
        i,ref=item; guid=ref['partition']
        fields=read(names.get(guid,guid),folder/f'dof-set-{i}.json')
        words=fields['LayoutData']; blob=struct.pack('<'+str(len(words))+'Q',*words)
        text,size=struct.unpack_from('<II',blob,16)
        end=starts[i+1] if i+1<len(starts) else len(ids)
        count=end-starts[i]
        require(text==32+32*count and text+size<=len(blob), 'Unmeasured native DofSet layout')
        result={}
        for j in range(count):
            offset=struct.unpack_from('<I',blob,32+j*32+12)[0]
            require(offset<size, 'Invalid native DOF name offset')
            limit=blob.index(0,text+offset,text+size)
            result[ids[starts[i]+j]]=blob[text+offset:limit].decode()
        return result
    result={}
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        for mapping in pool.map(decode,enumerate(rig['DofSets'])):
            for key,value in mapping.items():
                require(key not in result or result[key]==value, 'Conflicting native DOF bindings')
                result[key]=value
    return result


def read_native_vectors(probe, game, asset, folder, dof_names):
    """Read original controller/map/resource through the repository's native reader."""
    folder.mkdir(parents=True, exist_ok=True)
    def run(*args):
        subprocess.run([str(probe), *map(str,args)], check=True, capture_output=True, text=True)
    controller = folder/'controller.json'
    run('--asset', game, asset, controller)
    doc = json.loads(controller.read_text())
    root=doc['instances'][0]['object']['fields']
    require(root.get('FPS')==60 and root.get('TickOffset',0)==0, 'Unmeasured native source timing')
    animations = [i['object']['fields'] for i in doc['instances'] if i['object']['type']=='EclipseAnimationAsset']
    require(len(animations)==1, 'Unsupported native controller')
    animation = animations[0]
    mapping = folder/'channel-map.json'
    run('--asset', game, animation['ChannelToDofAsset']['partition'], mapping)
    ids = json.loads(mapping.read_text())['instances'][0]['object']['fields']['ChannelToDofIds']
    channels = [dof_names.get(i, f'unknown:{i}') for i in ids]
    resource = folder/'keys.bin'
    run('--resource', game, hex(animation['Resource']['resource_id']), resource)
    vectors = decode_vectors(resource.read_bytes(), channels)
    return vectors
