"""Build an offline native clip replacement, keeping the original sparse channel contract.

Requires the Studio CLI and dingosdk_anim_{graph_probe,clip_channels}. The output is staged;
this script never installs a mod or writes into the game. A new output directory is required.
Native graph timing, trajectory and scalar channels are retained. This does not turn a final
render capture into a native source clip or provide live per-attempt variant selection.
"""
import argparse
import hashlib
import json
import pathlib
import re
import struct
import subprocess

import numpy as np
from clip import Clip, euler_xyz, to_euler_xyz
from native_piecewise import decode_vectors, decode_scalars, correct_translations, correct_scalar_helpers


def require(ok, message):
    if not ok:
        raise ValueError(message)


def run(args):
    result = subprocess.run([str(a) for a in args], capture_output=True, text=True, timeout=120)
    require(result.returncode == 0, result.stderr.strip() or result.stdout.strip())
    return result.stdout


def channels(info):
    rows = [(int(i), n) for i, n in re.findall(r"^dof=(\d+) name=(.+)$", info, re.M)]
    require(len(rows) == int(re.search(r"^channels=(\d+)$", info, re.M)[1]), "Incomplete channel inventory")
    require(len(set(i for i, _ in rows)) == len(rows), "Duplicate channel IDs")
    return rows


def category(name):
    return 0 if name.endswith('.q') else 1 if name.endswith(('.t', '.s')) else 2


def sparse_keys(blob, old_rows, wanted):
    """Filter descriptors in the CLI's PIEC codec. Keep its key-data bytes unchanged.

    The adapter intentionally supports only this measured CLI layout (q=3,v=2,f=2).
    Descriptor offsets are relative to the shared key-data region. Unused key bytes can
    remain there; removing a channel must not alter any retained scalar/contact curve.
    """
    require(len(blob) >= 80, "Short key header")
    counts = [struct.unpack_from('<I', blob, o)[0] for o in (0, 12, 24)]
    offsets = [struct.unpack_from('<Q', blob, o)[0] for o in (4, 16, 28)]
    require(struct.unpack_from('<4I', blob, 36) == (3, 2, 2, 1), "Unsupported PIEC layout")
    data_size, data_offset = struct.unpack_from('<IQ', blob, 56)
    end = data_offset + data_size
    relocations = (end+15)&~15
    require(relocations + 16 == len(blob) and not any(blob[end:relocations]) and blob[-16:] == struct.pack('<4I',4,16,28,60),
            "Invalid key-data bounds or relocation table")
    require(sum(counts) == len(old_rows), "Codec/channel map mismatch")
    groups = [old_rows[:counts[0]], old_rows[counts[0]:counts[0]+counts[1]], old_rows[counts[0]+counts[1]:]]
    require(all(category(n) == g for g, rows in enumerate(groups) for _, n in rows), "Unexpected channel ordering")
    descriptors = []
    for g, (rows, offset, stride, width) in enumerate(zip(groups, offsets, (12, 48, 20), (16, 12, 4))):
        require(offset >= 80 and offset + len(rows)*stride <= data_offset, "Invalid descriptor bounds")
        by_id = {}
        for index, (ident, _) in enumerate(rows):
            d = blob[offset+index*stride:offset+(index+1)*stride]
            count, times, values = struct.unpack_from('<3I', d)
            require(0 < count <= 65535 and times + count*2 <= data_size and values + count*width <= data_size,
                    "Invalid curve bounds")
            by_id[ident] = d
        descriptors.append(b''.join(by_id[i] for i, n in wanted if category(n) == g))
    out = bytearray(blob[:80])
    for g, (descs, stride) in enumerate(zip(descriptors, (12, 48, 20))):
        out += bytes((-len(out)) % 16)
        struct.pack_into('<IQ', out, g*12, len(descs)//stride, len(out))
        out += descs
    out += bytes((-len(out)) % 16)
    struct.pack_into('<IQ', out, 56, data_size, len(out))
    out += blob[data_offset:]  # Includes the unchanged four header-pointer relocation offsets.
    return bytes(out)


def project_payloads(blob, key_size):
    require(blob[:12] == b'RSPROJT1\x02\0\0\0', "Unsupported generated project")
    found = []
    for match in re.finditer(b'RIFF', blob):
        at = match.start()
        if at < 8 or at+12 > len(blob):
            continue
        size = struct.unpack_from('<I', blob, at+4)[0] + 8
        if blob[at+8:at+12] in (b'EBXS', b'EBX\0') and struct.unpack_from('<Q', blob, at-8)[0] == size and at+size <= len(blob):
            found.append((at, size, blob[at:at+size]))
    require(len(found) == 2 and found[0][2][8:12] == b'EBXS' and found[1][2][8:12] == b'EBX\0', "Expected two generated animation EBX payloads")
    at = len(blob) - key_size
    require(at > found[1][0]+found[1][1]+8 and struct.unpack_from('<Q', blob, at-8)[0] == key_size, "Expected final key resource")
    return found + [(at, key_size, blob[at:])]


def raw_cas(blob):
    return b''.join(struct.pack('>Q', (len(part) << 32) | 0x700000 | len(part)) + part
                    for part in (blob[i:i+65536] for i in range(0, len(blob), 65536)))


def package(template, names, old_payloads, payloads):
    """Retain the CLI's v6 resource metadata and bundle membership; rebuild data table/hashes."""
    require(template[:12] == b'FROSTY\0\x01\x06\0\0\0', "Unsupported FBMOD template")
    table, count = struct.unpack_from('<QI', template, 12)
    require(count == 3 and 24 <= table <= len(template)-count*16, "Expected three FBMOD data resources")
    meta = bytearray(template[:table])
    encoded = [raw_cas(p) for p in payloads]
    require(len(set(names)) == 3, "Duplicate resource names")
    for index, (name, old, new, cas) in enumerate(zip(names, old_payloads, payloads, encoded)):
        needle = name.encode() + b'\0'
        positions = [m.start() for m in re.finditer(re.escape(needle), bytes(meta)) if m.start() >= 5 and
                     meta[m.start()-5] == (1 if index < 2 else 2) and struct.unpack_from('<i',meta,m.start()-4)[0] == index]
        require(len(positions) == 1, "Ambiguous FBMOD resource name")
        at = positions[0]
        require(at >= 5 and meta[at-5] == (1 if index < 2 else 2) and struct.unpack_from('<i', meta, at-4)[0] == index,
                "Unexpected FBMOD resource identity")
        digest_at = at + len(needle)
        require(struct.unpack_from('<Q', meta, digest_at+20)[0] == len(old), "FBMOD original size mismatch")
        offset, size = struct.unpack_from('<QQ', template, table+index*16)
        start = table + count*16 + offset
        require(start+size <= len(template) and hashlib.sha1(template[start:start+size]).digest() == meta[digest_at:digest_at+20],
                "FBMOD input payload hash mismatch")
        meta[digest_at:digest_at+20] = hashlib.sha1(cas).digest()
        struct.pack_into('<Q', meta, digest_at+20, len(new))
        if index == 2:
            # Resource metadata locates the trailing relocation table in the decoded PIEC data.
            user_end = meta.index(0, digest_at+33)
            bundles = struct.unpack_from('<I', meta, user_end+1)[0]
            res = user_end+5+4*bundles
            require(bundles <= 256 and struct.unpack_from('<I',meta,res)[0] == 0x4088BF7E and
                    struct.unpack_from('<I',meta,res+12)[0] == 16, 'Unexpected animation RES metadata')
            relocation = res+16
            require(struct.unpack_from('<II',meta,relocation) == (len(old)-16,16), 'Invalid template relocations')
            struct.pack_into('<I',meta,relocation,len(new)-16)
    pos = 0
    for cas in encoded:
        meta += struct.pack('<QQ', pos, len(cas)); pos += len(cas)
    return bytes(meta) + b''.join(encoded)


def compare(reference, actual, maximum_error=True):
    require(set(reference.node) == set(actual.node) and reference.parent == actual.parent, "Skeleton hierarchy changed")
    require(np.allclose(reference.times, actual.times, atol=1e-7, rtol=0), "Source timing changed")
    errors = dict(position=0., scale=0., rotation_matrix=0., rotation_degrees=0.)
    for bone in reference.node:
        errors['position'] = max(errors['position'], float(np.max(np.abs(reference.channel(bone,'T')-actual.channel(bone,'T')))))
        errors['scale'] = max(errors['scale'], float(np.max(np.abs(reference.channel(bone,'S')-actual.channel(bone,'S')))))
        for a,b in zip(reference.channel(bone,'R'),actual.channel(bone,'R')):
            ar,br = euler_xyz(*a),euler_xyz(*b)
            errors['rotation_matrix'] = max(errors['rotation_matrix'],float(np.max(np.abs(ar-br))))
            degrees = float(np.degrees(np.arccos(np.clip((np.trace(ar.T@br)-1)/2,-1,1))))
            errors['rotation_degrees'] = max(errors['rotation_degrees'],degrees)
    if maximum_error is not None:
        # Bound metres and angles separately. Euler/quaternion float conversion near gimbal
        # singularities can cost hundredths of a degree; never use that budget for positions.
        require(errors['position'] < 1e-5 and errors['scale'] < 1e-5 and errors['rotation_degrees'] < 0.05,
                f"Round-trip transform mismatch: {errors}")
    return errors


def controller_contract(before, after):
    require(before['partition'] == after['partition'] and before['root_type'] == after['root_type'], 'Controller identity changed')
    require(len(before['instances']) == len(after['instances']), 'Controller instances changed')
    changed = []
    for a,b in zip(before['instances'],after['instances']):
        require(a['guid'] == b['guid'] and a['index'] == b['index'] and a['object']['type'] == b['object']['type'], 'Instance identity changed')
        af,bf = a['object']['fields'],b['object']['fields']
        require(af.keys() == bf.keys(), 'Controller field set changed')
        for name in af:
            if af[name] == bf[name]:
                continue
            require(a['object']['type'] == 'EclipseAnimationAsset' and name in ('Resource','ChannelToDofAsset'),
                    f'Native tag/timing/channel contract changed: instance {a["index"]} {name}')
            changed.append(name)
    require(sorted(changed) == ['ChannelToDofAsset','Resource'], 'Expected only native payload rebinding')
    return len(before['instances'])


def rotation(q):
    x,y,z,w = q
    return np.array([[1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w)],
                     [2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w)],
                     [2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)]])


def bake_profile(args, clip, original):
    from make_trick_library import fnv1
    out = args.output
    profile = json.loads(args.studio_profile.read_text())
    require(profile.get('source',{}).get('position_codec') == 'native-piec-v1-extent',
            'Reload the native source from the corrected library and save the profile before native compilation')
    (out/'sample-times.json').write_text(json.dumps((clip.times-clip.start).tolist()))
    run([args.tools/'dingosdk_anim_clip_channels.exe','--sample-edits',args.studio_profile,out/'sample-times.json',out/'sampled-edits.json'])
    samples = json.loads((out/'sampled-edits.json').read_text())
    require(samples['source_asset'] == args.asset and abs(samples['duration']-(clip.stop-clip.start)) < 0.001,
            'Saved profile belongs to another native source or duration')
    hashes = [int(h) for h in re.findall(r'NameHash = (\d+)',run([args.cli,'ebx-values',args.game,'animation/dingo/animbase_default_skeleton']))]
    require(len(hashes) == 395, 'Unsupported native skeleton')
    by_hash = {fnv1(n):n for n in clip.node}
    require(len(by_hash) == len(clip.node), 'Ambiguous skeleton name hash')
    allowed = {n for _,n in original}
    board_frame = euler_xyz(*clip.channel('Skateboard_Root','R')[0])
    for track in samples['tracks']:
        joint = track['joint']
        require(joint == 900 or 0 <= joint < len(hashes), 'Unsupported studio joint')
        bone = 'Skateboard_Root' if joint == 900 else by_hash.get(hashes[joint])
        require(bone is not None, 'Studio joint is missing from native skeleton')
        require(joint != 900 or clip.parent.get(bone) == 'AITrajectory', 'Unsupported board edit frame')
        values = track['samples']
        require(len(values) == len(clip.times), 'Edit sampling count mismatch')
        old_r = clip.channel(bone,'R'); new_r = old_r.copy()
        old_t = clip.channel(bone,'T'); new_t = old_t.copy()
        turns = moves = False
        for f,edit in enumerate(values):
            delta = rotation(edit['r'])
            offset = np.asarray(edit['p'],float)
            if np.max(np.abs(delta-np.eye(3))) > 1e-10:
                require(f'{bone}.q' in allowed, f'Edit would add native channel {bone}.q')
                base = euler_xyz(*old_r[f])
                matrix = board_frame @ delta @ board_frame.T @ base if joint == 900 else base @ delta
                angles = to_euler_xyz(matrix)
                new_r[f] = angles + 360*np.round((old_r[f]-angles)/360)
                turns = True
            if np.max(np.abs(offset)) > 1e-10:
                require(f'{bone}.t' in allowed, f'Edit would add native channel {bone}.t')
                new_t[f] += board_frame @ offset if joint == 900 else offset
                moves = True
        if turns:clip.set_channel(bone,'R',new_r)
        if moves:clip.set_channel(bone,'T',new_t)
    return len(samples['tracks'])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cli', required=True, type=pathlib.Path)
    ap.add_argument('--game', required=True, type=pathlib.Path)
    ap.add_argument('--tools', required=True, type=pathlib.Path)
    ap.add_argument('--asset', required=True)
    ap.add_argument('--output', required=True, type=pathlib.Path)
    source = ap.add_mutually_exclusive_group()
    source.add_argument('--input-fbx', type=pathlib.Path, help='A full edited native source clip, with unchanged timing/hierarchy')
    source.add_argument('--arm-degrees', type=float, help='Reference experiment: rotate RightArm during the airborne section')
    source.add_argument('--studio-profile', type=pathlib.Path, help='Bake a saved canonical native-asset profile using the studio key evaluator')
    args = ap.parse_args()
    require(not args.output.resolve().is_relative_to(args.game.resolve()), 'Output must be outside the game directory')
    require(not args.output.exists(), 'Output directory already exists; choose a new directory')
    require(args.asset.startswith('animation/dingo/c_') and '..' not in args.asset, 'Expected an explicit native clip controller')
    args.output.mkdir(parents=True)
    out = args.output
    base = [args.cli, 'animation-info', args.game, args.asset]
    info = run(base); (out/'original-info.txt').write_text(info)
    original = channels(info)
    require(original == sorted(original, key=lambda x: category(x[1])), 'Original channels are not grouped by codec type')
    run([args.cli,'animation-export',args.game,args.asset,out/'original.fbx'])
    clip = Clip(out/'original.fbx')
    run([args.tools/'dingosdk_anim_graph_probe.exe','--asset',args.game,args.asset,out/'original-controller.json'])
    controller=json.loads((out/'original-controller.json').read_text())
    animations=[i['object']['fields'] for i in controller['instances'] if i['object']['type']=='EclipseAnimationAsset']
    require(len(animations)==1, 'Unsupported native source controller')
    run([args.tools/'dingosdk_anim_graph_probe.exe','--resource',args.game,hex(animations[0]['Resource']['resource_id']),out/'original.keys'])
    source_bytes=(out/'original.keys').read_bytes()
    source_positions=correct_translations(clip,decode_vectors(source_bytes,[n for _,n in original]))
    source_scalars=correct_scalar_helpers(clip,decode_scalars(source_bytes,[n for _,n in original]),{n:i for i,n in original})
    clip.save(out/'original-corrected.fbx')
    profile_tracks = 0
    if args.input_fbx:
        edited = Clip(args.input_fbx)
        require(edited.parent == clip.parent and np.array_equal(edited.times,clip.times), 'Custom source must preserve native hierarchy and timing')
        allowed = {n for _,n in original} | {f'RS_DOF_{i}.t' for i,n in original if category(n)==2}
        for bone in clip.node:
            for ch,suffix in (('R','q'),('T','t'),('S','s')):
                require(f'{bone}.{suffix}' in allowed or np.allclose(clip.channel(bone,ch),edited.channel(bone,ch),atol=1e-7,rtol=0),
                        f'Custom source adds unsupported channel {bone}.{suffix}')
        clip = edited
    elif args.arm_degrees is not None:
        require(np.isfinite(args.arm_degrees) and abs(args.arm_degrees) <= 90, 'Reference arm angle out of range')
        # A visible source-motion change, smooth at both ends. Feet, board and native tags stay untouched.
        u = np.clip((clip.times-clip.start-0.1)/0.9,0,1)
        envelope = np.sin(np.pi*u)**2
        clip.add_rotation('RightArm',(0,0,1),args.arm_degrees*envelope)
    elif args.studio_profile:
        profile_tracks = bake_profile(args,clip,original)
    # The exporter represents scalar DOFs as RS_DOF_<id>.translation.x helper models.
    # They are real animation data (onboard/speed/landing), not skinned bones to discard.
    clip.prune({n for _,n in original} | {f'RS_DOF_{i}.t' for i,n in original if category(n)==2})
    clip.save(out/'authored.fbx')
    run([args.cli,'animation-import',args.game,args.asset,out/'authored.fbx',out/'expanded.fbproject'])
    imported_info = run(base + ['--project',out/'expanded.fbproject'])
    (out/'expanded-info.txt').write_text(imported_info)
    imported = channels(imported_info)
    project_info = run([args.cli,'project-info',out/'expanded.fbproject'])
    resources = re.findall(r'^[12] (.+) bytes=(\d+)(?: added)?$',project_info,re.M)
    require(len(resources) == 3 and resources[0][0].endswith('/channels') and resources[1][0] == args.asset and resources[2][0].endswith('/keys'), 'Unexpected project resources')
    project = (out/'expanded.fbproject').read_bytes()
    parts = project_payloads(project,int(resources[2][1]))
    for name,part in zip(('channels.ebx','controller.ebx','keys.bin'),parts):
        (out/name).write_bytes(part[2])
    counts = [sum(category(n)==g for _,n in original) for g in range(3)]
    (out/'selection.json').write_text(json.dumps(dict(ids=[i for i,_ in original],q=counts[0],v=counts[1],f=counts[2])))
    run([args.tools/'dingosdk_anim_clip_channels.exe',out/'channels.ebx',out/'controller.ebx',out/'selection.json',out/'sparse-channels.ebx',out/'sparse-controller.ebx'])
    payloads = [(out/'sparse-channels.ebx').read_bytes(),(out/'sparse-controller.ebx').read_bytes(),sparse_keys(parts[2][2],imported,original)]
    # Generated project v2 stores the same 16-byte RES metadata with a uint64 size prefix.
    relocation = struct.pack('<II', len(parts[2][2])-16,16)
    positions = [m.start() for m in re.finditer(re.escape(relocation),project[:parts[2][0]])]
    require(len(positions) == 1 and project[positions[0]-8:positions[0]] == struct.pack('<Q',16),
            'Ambiguous project relocation metadata')
    project = bytearray(project)
    struct.pack_into('<I',project,positions[0],len(payloads[2])-16)
    project = bytes(project)
    # Replace length-delimited generated payloads backwards; metadata/identities remain byte-for-byte intact.
    for (at,size,_),data in reversed(list(zip(parts,payloads))):
        project = project[:at-8] + struct.pack('<Q',len(data)) + data + project[at+size:]
    (out/'native.fbproject').write_bytes(project)
    verified = run(base + ['--project',out/'native.fbproject'])
    require(channels(verified) == original, 'Sparse channel contract changed')
    (out/'native-info.txt').write_text(verified)
    run([args.cli,'animation-export',args.game,args.asset,out/'verified.fbx','--project',out/'native.fbproject'])
    errors = compare(Clip(out/'authored.fbx'),Clip(out/'verified.fbx'))
    source_change = compare(Clip(out/'original-corrected.fbx'),Clip(out/'verified.fbx'),None)
    run([args.tools/'dingosdk_anim_graph_probe.exe','--asset',args.game,args.asset,out/'original-controller.json'])
    run([args.tools/'dingosdk_anim_graph_probe.exe','--document',out/'sparse-controller.ebx',out/'native-controller.json'])
    instances = controller_contract(json.loads((out/'original-controller.json').read_text()),json.loads((out/'native-controller.json').read_text()))
    # The CLI packages an expanded project; use its bundle/resource metadata as a template only.
    run([args.cli,'animation-import',args.game,args.asset,out/'authored.fbx',out/'template.fbmod'])
    mod = package((out/'template.fbmod').read_bytes(),[n for n,_ in resources],[p[2] for p in parts],payloads)
    (out/'native.fbmod').write_bytes(mod)
    run([args.cli,'compile-mod',args.game,out/'staging',out/'native.fbmod'])
    report = dict(asset=args.asset, original_channels=len(original), expanded_channels=len(imported), final_channels=len(original),
                  channel_counts=dict(quaternion=counts[0],vector=counts[1],scalar=counts[2]), roundtrip_max_error=errors,
                  preserved_controller_instances=instances, controller_changes=['ChannelToDofAsset','Resource'],
                  measured_source_change=source_change,
                  baked_studio_tracks=profile_tracks,
                  native_position_decode=source_positions, native_scalar_decode=source_scalars,
                  mod_sha256=hashlib.sha256(mod).hexdigest(), experiment_arm_degrees=args.arm_degrees,
                  gameplay_validation='pending', live_variant_selection=False)
    (out/'validation.json').write_text(json.dumps(report,indent=2))
    print(json.dumps(report,indent=2))


if __name__ == '__main__':
    main()
