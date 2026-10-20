"""Diagnostic only: hold both kickflip arms at an existing native 90-degree quaternion.

Patches only existing compressed quaternion values in low/mid/high resources. Resource IDs,
controller/channel maps, key times, board/foot motion and codec metadata stay unchanged.
This deliberately awkward arm marker is not a finished custom animation or a pose solver.
"""
import argparse
import hashlib
import json
import pathlib
import struct

from native_clip_pipeline import channels, raw_cas, require, run, project_payloads

TARGETS = ('LeftArm.q','RightArm.q','LeftForeArm.q','RightForeArm.q')


def mark(blob, rows, quaternion):
    require(len(quaternion)==6 and len(blob)>=80,'Invalid compressed quaternion')
    q,offset = struct.unpack_from('<IQ',blob,0)
    require(struct.unpack_from('<I',blob,36)[0] == 1 and struct.unpack_from('<I',blob,48)[0] == 0,
            'Only measured native q1 / byte-time codec supported')
    size,data = struct.unpack_from('<IQ',blob,56)
    require(offset>=80 and offset+q*12<=data and data+size<=len(blob)-16,'Invalid native key bounds')
    require(all(n.endswith('.q') for _,n in rows[:q]),'Unexpected quaternion channel inventory')
    ranges = []
    for i,(_,name) in enumerate(rows[:q]):
        count,times,values = struct.unpack_from('<3I',blob,offset+12*i)
        require(0<count<=256 and times+count<=size and values+count*6<=size,'Invalid compressed quaternion range')
        ranges.append((name,data+values,data+values+count*6))
    out = bytearray(blob); edits=[]; allowed=set()
    for name in TARGETS:
        selected=[r for r in ranges if r[0]==name]
        require(len(selected)==1,'Marker joint absent or ambiguous')
        _,a,b=selected[0]
        require(all(other==name or b<=start or a>=end for other,start,end in ranges),'Shared quaternion data cannot be marked independently')
        out[a:b] = quaternion*((b-a)//6)
        allowed.update(range(a,b))
        edits.append(dict(channel=name,keys=(b-a)//6,offset=a,length=b-a))
    changed={i for i,(a,b) in enumerate(zip(blob,out)) if a!=b}
    require(changed and changed<=allowed,'Marker changed data outside the declared arm curves')
    return bytes(out),dict(curves=edits,changed_bytes=len(changed),unchanged_byte_count=len(blob)-len(changed))


def template_metadata(template):
    require(template[:12]==b'FROSTY\0\x01\x06\0\0\0','Unsupported template')
    # The measured CLI v6 template: .NET profile string, head, six NUL-terminated details.
    require(template[24]==5 and template[25:30]==b'Skate','Unexpected profile encoding')
    pos=34
    for _ in range(6):pos=template.index(0,pos)+1
    require(struct.unpack_from('<I',template,pos)[0]==8,'Expected the supplied CLI template resources')
    at=template.index(b'/keys\0')+6
    end=template.index(0,at+33)
    count=struct.unpack_from('<I',template,end+1)[0]
    require(0<count<=256,'Invalid template bundles')
    bundles=template[end+5:end+5+4*count]
    first_name=template.index(b'reskatestudio/')
    require(template[first_name-5:first_name]==b'\x01\0\0\0\0','Unexpected first template resource')
    previews=template[pos+4:first_name-5]
    return template[:pos],previews,count,bundles


def resource_package(template, resources):
    prefix,previews,count,bundles=template_metadata(template)
    meta=bytearray(prefix+struct.pack('<I',len(resources)+5)+previews);encoded=[]
    for index,(resource,blob) in enumerate(resources):
        cas=raw_cas(blob);encoded.append(cas)
        name=resource['asset'].encode()+b'\0'
        rm=bytes(resource['resource_meta'])
        require(len(rm)==16 and struct.unpack_from('<II',rm)==(len(blob)-16,16),'Unsupported native relocation metadata')
        meta+=b'\x02'+struct.pack('<i',index)+name+hashlib.sha1(cas).digest()+struct.pack('<Q',len(blob))
        meta+=b'\0'+struct.pack('<I',0)+b'Native source arm marker\0'+struct.pack('<I',count)+bundles
        meta+=struct.pack('<IQI',resource['resource_type'],resource['resource_id'],len(rm))+rm
    struct.pack_into('<QI',meta,12,len(meta),len(resources))
    position=0
    for cas in encoded:
        meta+=struct.pack('<QQ',position,len(cas));position+=len(cas)
    return bytes(meta)+b''.join(encoded)


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--inputs',required=True,type=pathlib.Path,help='Probe output: <pop>-original.keys[.meta.json], <pop>-info.txt')
    ap.add_argument('--template',required=True,type=pathlib.Path)
    ap.add_argument('--output',required=True,type=pathlib.Path)
    ap.add_argument('--cli',required=True,type=pathlib.Path)
    ap.add_argument('--game',required=True,type=pathlib.Path)
    ap.add_argument('--verify-project',required=True,type=pathlib.Path,help='Validated high-pop native project used only for offline decoding')
    args=ap.parse_args()
    require(not args.output.exists() and not args.output.resolve().is_relative_to(args.game.resolve()),'Choose a new output folder outside the game')
    args.output.mkdir(parents=True)
    high=(args.inputs/'high-original.keys').read_bytes()
    high_rows=channels((args.inputs/'high-info.txt').read_text(encoding='utf-8-sig'))
    index=next(i for i,(_,n) in enumerate(high_rows) if n=='Skateboard_Root.q')
    offset=struct.unpack_from('<Q',high,4)[0];data=struct.unpack_from('<Q',high,60)[0]
    count,times,values=struct.unpack_from('<3I',high,offset+index*12)
    quaternion=high[data+values:data+values+6]
    require(quaternion==bytes.fromhex('8001ffff8000'),'Native marker quaternion differs from the decoded high-pop reference')
    resources=[];reports=[];marked={}
    for pop in ('low','mid','high'):
        original=(args.inputs/f'{pop}-original.keys').read_bytes()
        metadata=json.loads((args.inputs/f'{pop}-original.keys.meta.json').read_text())
        rows=channels((args.inputs/f'{pop}-info.txt').read_text(encoding='utf-8-sig'))
        result,report=mark(original,rows,quaternion)
        (args.output/f'{pop}-marked.keys').write_bytes(result)
        resources.append((metadata,result));marked[pop]=result
        reports.append(dict(pop=pop,resource=metadata,**report,original_sha256=hashlib.sha256(original).hexdigest(),marked_sha256=hashlib.sha256(result).hexdigest()))
    package=resource_package(args.template.read_bytes(),resources)
    (args.output/'native-arm-marker.fbmod').write_bytes(package)
    # Decode the high marker through the same CLI, preserving the verified project's bindings.
    project=args.verify_project.read_bytes()
    old_size=struct.unpack_from('<Q',project,len(project)-421536-8)[0]
    require(old_size==421536,'Unexpected reference project key size')
    parts=project_payloads(project,old_size)
    at,size,_=parts[2]
    relocation=struct.pack('<II',size-16,16)
    positions=[i for i in range(at) if project.startswith(relocation,i)]
    require(len(positions)==1,'Ambiguous reference relocation metadata')
    project=bytearray(project);struct.pack_into('<I',project,positions[0],len(marked['high'])-16)
    project=bytes(project[:at-8])+struct.pack('<Q',len(marked['high']))+marked['high']
    (args.output/'decode-high.fbproject').write_bytes(project)
    run([args.cli,'animation-export',args.game,'animation/dingo/c_proto_onb_trick_o_kickflip_high',args.output/'decode-high.fbx','--project',args.output/'decode-high.fbproject'])
    run([args.cli,'compile-mod',args.game,args.output/'staging',args.output/'native-arm-marker.fbmod'])
    report=dict(experiment='Diagnostic frozen arms at a native Y90 quaternion, all kickflip pop variants',
                resources=reports,preserved_resource_identity=True,controller_edits=0,channel_map_edits=0,
                changes_board_or_feet=False,compiled=True,visual_validation='pending',package_sha256=hashlib.sha256(package).hexdigest())
    (args.output/'validation.json').write_text(json.dumps(report,indent=2))
    print(json.dumps(dict(resources=len(resources),compiled=True,package_sha256=report['package_sha256']),indent=2))


if __name__=='__main__':main()
