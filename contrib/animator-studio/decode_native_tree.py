"""Build a typed, linked native tree and resumably decode supported skater clips.

Run anim_graph_probe --inventory GAME CACHE first. Writes only a staging directory.
Unknown codecs/rigs fail explicitly; new clips have no guessed live trick binding.
"""
import argparse
import concurrent.futures
import collections
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile

import numpy as np

from clip import Clip
from index_native_assets import classify
from make_trick_library import fnv1
from native_piecewise import cached_dof_names, correct_translations, decode_vectors, require
from native_pose import default_pose, decode_pose
from repair_library_positions import encode_clip, read_clip

REVISION = 3


def atomic_json(path, value):
    temp = path.with_suffix(path.suffix + '.tmp')
    temp.write_text(json.dumps(value, separators=(',', ':')), encoding='utf-8')
    os.replace(temp, path)


def identity(asset):
    return hashlib.sha256(asset.encode()).hexdigest()[:24]


def category(asset, root_type):
    if asset.startswith('_af/'):
        return 'Rig/Channel bindings'
    name = asset.removeprefix('animation/dingo/')
    base, _ = classify(name)
    if root_type == 'ClipControllerAsset' and 'grind_' in name:
        family = name.split('grind_', 1)[1].split('_', 1)[0]
        phase = 'Entry' if any(x in name for x in ('_into', '_lockin')) else 'Loop' if '_loop' in name else 'Exit' if any(x in name for x in ('_popout', '_exit')) else 'Other'
        return 'On board/Grinds/' + family + '/' + phase
    if root_type == 'ClipControllerAsset' and 'manual' in name:
        phase = 'Entry' if '_into' in name else 'Loop' if '_loop' in name else 'Exit' if '_exit' in name or '_popout' in name else 'Other'
        return 'On board/Manuals/' + phase
    if root_type in ('BoolAsset', 'FloatAsset', 'Vector3Asset', 'IntAsset', 'GameStateEnumerationAsset'):
        return 'Graph/Parameters/' + root_type.removesuffix('Asset')
    if root_type == 'TransitionAsset':
        return 'Graph/Transitions'
    if root_type == 'ClipControllerAsset' and ('/gameplay_facial/' in asset or name.rsplit('/', 1)[-1].startswith('f_')):
        return 'Face/' + base
    return base


def references(value, path=''):
    if isinstance(value, dict):
        if value.get('kind') in ('external', 'internal'):
            yield path, value
        else:
            for k, v in value.items():
                yield from references(v, path + '/' + k)
    elif isinstance(value, list):
        for i, v in enumerate(value):
            yield from references(v, path + '/' + str(i))


def local_frames(clip, names, parents, vectors):
    """Vectorized equivalent of the FBX local transform reader; no inferred phases."""
    require(len(names) == 395 and all(n in clip.node for n in names), 'Unsupported preview skeleton: all 395 native joints are required')
    require(0 < len(clip.times) <= 20000, 'Unsupported frame count')
    for j, name in enumerate(names):
        expected = names[parents[j]] if parents[j] >= 0 else None
        require(clip.parent.get(name) == expected, 'Unsupported skeleton hierarchy at ' + name)
    correct_translations(clip, vectors)
    frames = np.empty((len(clip.times), 395, 10), dtype='<f4')
    for j, name in enumerate(names):
        frames[:, j, :3] = clip.channel(name, 'S')
        frames[:, j, 7:] = clip.channel(name, 'T')
        half = np.radians(clip.channel(name, 'R')) / 2
        sx, sy, sz = np.sin(half).T; cx, cy, cz = np.cos(half).T
        q = np.column_stack((sx*cy*cz-cx*sy*sz, cx*sy*cz+sx*cy*sz,
                             cx*cy*sz-sx*sy*cz, cx*cy*cz+sx*sy*sz))
        q /= np.linalg.norm(q, axis=1)[:, None]
        frames[:, j, 3:7] = q
    require(np.isfinite(frames).all() and np.all(frames[:, :, :3] > 0), 'Invalid native pose/scale')
    return (clip.times - clip.start).astype('<f4'), frames


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for field in ('cache', 'game', 'cli', 'library', 'dof-names', 'output'):
        parser.add_argument('--' + field, required=True, type=Path)
    parser.add_argument('--workers', type=int, default=4)
    parser.add_argument('--limit', type=int)
    parser.add_argument('--retry-failed', action='store_true')
    parser.add_argument('--direct', action='store_true', help='Read native q1/default pose directly instead of using FBX')
    parser.add_argument('--catalog-only', action='store_true', help='Rebuild tree/links from verified cached decode results without recomputing poses')
    args = parser.parse_args()
    require(1 <= args.workers <= 6, 'Use 1..6 workers')
    require(not args.output.resolve().is_relative_to(args.game.resolve()), 'Stage outside the game')
    require(args.output.resolve() != args.library.resolve(), 'Do not overwrite the input library')
    args.output.mkdir(parents=True, exist_ok=True)
    shutil.copytree(args.cache / 'documents', args.output / 'native-documents', dirs_exist_ok=True)
    results_dir = args.output / 'results'; results_dir.mkdir(exist_ok=True)
    index = json.loads((args.cache / 'native-asset-types.json').read_text())
    known={x['partition'] for x in index}
    for path in (args.cache / 'documents').glob('*.json'):
        if path.stem in known:continue
        doc=json.loads(path.read_text())
        if doc.get('asset','').startswith('_af/'):
            index.append(dict(asset=doc['asset'],partition=doc['partition'],root_type=doc['root_type'],instances=len(doc['instances'])))
    typed = {x['asset']: x for x in index}; by_guid = {x['partition']: x['asset'] for x in index}
    dofs = {int(k): v for k, v in json.loads(args.dof_names.read_text()).items()}
    def document(guid):
        return json.loads((args.cache / 'documents' / (guid + '.json')).read_text())
    skeleton_source=next(x for x in index if x['asset']=='animation/dingo/animbase_default_skeleton')
    skeleton = document(skeleton_source['partition'])['instances'][0]['object']['fields']
    table = bytes(skeleton['NameTable'])
    names = [table[x['fields']['Name']:table.index(0, x['fields']['Name'])].decode() for x in skeleton['Joints']]
    parents = [x['fields']['Parent'] for x in skeleton['Joints']]
    rig_source=next(x for x in index if x['asset']=='animation/dingo/animbase_default_rig')
    defaults=default_pose(document(rig_source['partition']),dofs,names)
    require(cached_dof_names(document(rig_source['partition']),document)==dofs,'Provided DOF names do not match the extracted native rig; regenerate bindings')
    context=dict(rig=rig_source['payload_sha1'],skeleton=skeleton_source['payload_sha1'],bindings=hashlib.sha256(args.dof_names.read_bytes()).hexdigest(),backend='native' if args.direct else 'fbx',
                 decoder=hashlib.sha256(b''.join((Path(__file__).parent/n).read_bytes() for n in ('decode_native_tree.py','native_pose.py','native_piecewise.py'))).hexdigest())
    input_fingerprints={}
    base = json.loads((args.library / 'catalog.json').read_text(encoding='utf-8-sig'))
    catalog = {x['asset']: x for x in base}
    for entry in base:
        require(Path(entry['file']).name == entry['file'], 'Unsafe base library path')
        require(entry.get('position_codec')=='native-piec-v1-extent','Repair the base native library position decoding before extending it')
        # A pre-existing library must still describe this game's source, not an old
        # resource under the same asset ID after an update. Preserve its bindings.
        source_doc=document(typed[entry['asset']]['partition'])
        animation=next(x['object']['fields'] for x in source_doc['instances'] if x['object']['type']=='EclipseAnimationAsset')
        channel_ids=document(animation['ChannelToDofAsset']['partition'])['instances'][0]['object']['fields']['ChannelToDofIds']
        blob=(args.cache/'resources'/(str(animation['Resource']['resource_id'])+'.bin')).read_bytes()
        times,actual=decode_pose(blob,[dofs[i] for i in channel_ids],defaults,names,animation['EndFrame'])
        old_times,old,*_=read_clip(args.library/entry['file'])
        require(np.array_equal(times,old_times) and actual.shape==old.shape,'Base library timing differs from current native source: '+entry['asset'])
        q,z=actual[:,:,3:7].astype(float),old[:,:,3:7].astype(float)
        q/=np.linalg.norm(q,axis=2)[:,:,None];z/=np.linalg.norm(z,axis=2)[:,:,None]
        require(np.min(abs(np.sum(q*z,axis=2)))>np.cos(np.radians(.05)/2) and np.max(abs(actual[:,:,7:]-old[:,:,7:]))<1e-5 and np.max(abs(actual[:,:,:3]-old[:,:,:3]))<1e-5,
                'Base library pose differs from current native source: '+entry['asset'])
        target = args.output / entry['file']
        if not target.exists(): shutil.copy2(args.library / entry['file'], target)
    old_inventory = json.loads((args.library / 'native-catalog.json').read_text(encoding='utf-8-sig')) if (args.library/'native-catalog.json').exists() else []
    assets = {x['asset'] for x in old_inventory} | typed.keys()
    metadata = {}; edges = 0; resolved = 0
    resources_dir=args.output/'native-resources';resources_dir.mkdir(exist_ok=True)
    with (args.output / 'native-links.jsonl').open('w', encoding='utf-8') as links:
        for asset in sorted(assets):
            source = typed.get(asset)
            row = dict(asset=asset, category=category(asset, source['root_type'] if source else ''),
                       kind=source['root_type'] if source else 'Unverified name', root_type=source['root_type'] if source else '',
                       state='metadata' if source else 'indexed', detail='Typed native authoring data; live binding not established' if source else 'Outside the inspected native bundles')
            if source:
                try:
                    doc = document(source['partition'])
                    row['document'] = 'native-documents/' + source['partition'] + '.json'
                    row['partition'] = source['partition']
                    row['instances'] = len(doc['instances']); count = 0
                    for path, ref in references(doc['instances']):
                        guid = ref.get('partition', source['partition'])
                        target = by_guid.get(guid)
                        links.write(json.dumps(dict(source=asset, field=path, kind=ref['kind'], target=target,
                                                    target_partition=guid, target_instance=ref['instance']), separators=(',', ':')) + '\n')
                        count += 1; edges += 1; resolved += target is not None
                    row['links'] = count
                    if source['root_type'] == 'ClipControllerAsset': row.update(state='pending', detail='Native clip awaiting preview decode')
                    for instance in doc['instances']:
                        obj=instance['object']
                        if not isinstance(obj,dict) or not obj.get('type','').endswith('AnimationAsset'):continue
                        fields=obj['fields'];resource=fields.get('Resource')
                        if not resource:continue
                        rid=resource['resource_id'];binary=args.cache/'resources'/(str(rid)+'.bin')
                        if not binary.exists():continue
                        blob=binary.read_bytes()
                        res=dict(asset=asset+' [resource '+str(rid)+']',root_type=obj['type'],resource_id=rid,bytes=len(blob),
                                 sha256=hashlib.sha256(blob).hexdigest(),codec_metadata=fields,
                                 controller=dict(kind='external',partition=source['partition'],instance=doc['instances'][0]['guid']),
                                 scope='Encoded native source bytes; source metadata is not proof of a supported visual decoder')
                        map_ref=fields.get('ChannelToDofAsset')
                        if map_ref:
                            map_file=args.cache/'documents'/(map_ref['partition']+'.json')
                            ids=document(map_ref['partition'])['instances'][0]['object']['fields'].get('ChannelToDofIds',[])
                            res['channel_names']=[dict(id=i,name=dofs.get(i,'Unknown DOF')) for i in ids]
                        else:map_file=None
                        input_fingerprints[asset]=hashlib.sha256(json.dumps(dict(context,source=source.get('payload_sha1'),resource=res['sha256'],channel_map=hashlib.sha256(map_file.read_bytes()).hexdigest() if map_file else None),sort_keys=True).encode()).hexdigest()
                        if obj['type']=='EclipseAnimationAsset' and len(blob)>=80:
                            res['piecewise_header']=dict(zip(('quaternion_codec','vector_codec','scalar_codec','time_codec'),struct.unpack_from('<4I',blob,36)))
                        atomic_json(resources_dir/(str(rid)+'.json'),res)
                        shutil.copy2(binary,resources_dir/binary.name)
                except Exception as error:
                    row.update(state='error', detail=str(error)[:400])
            metadata[asset] = row
    results = {}
    if args.catalog_only:
        for path in results_dir.glob('*.json'):
            previous=json.loads(path.read_text());source=typed.get(previous.get('asset',''))
            if not source or previous.get('revision')!=REVISION or previous.get('source_sha1')!=source.get('payload_sha1') or previous.get('input_fingerprint')!=input_fingerprints.get(source['asset']):continue
            if previous['state']=='decoded':
                clip=args.output/previous['entry']['file']
                require(clip.is_file() and hashlib.sha256(clip.read_bytes()).hexdigest()==previous['clip_sha256'],'Cached clip failed integrity check')
            results[source['asset']]=previous
    def decode(source):
        asset = source['asset']; key = identity(asset); result_path = results_dir / (key + '.json')
        if result_path.exists():
            previous = json.loads(result_path.read_text())
            if previous.get('revision') == REVISION and previous.get('source_sha1') == source['payload_sha1'] and previous.get('input_fingerprint')==input_fingerprints.get(asset):
                if previous['state'] == 'decoded' and (args.output / previous['entry']['file']).exists():
                    if hashlib.sha256((args.output / previous['entry']['file']).read_bytes()).hexdigest() == previous.get('clip_sha256'): return previous
                elif not args.retry_failed: return previous
        result = dict(asset=asset, revision=REVISION, source_sha1=source['payload_sha1'],input_fingerprint=input_fingerprints.get(asset))
        try:
            doc = document(source['partition']); root = doc['instances'][0]['object']['fields']
            require(root.get('FPS') == 60 and root.get('TickOffset', 0) == 0, 'Unsupported native source timing')
            animations = [x['object']['fields'] for x in doc['instances'] if x['object']['type'] == 'EclipseAnimationAsset']
            require(len(animations) == 1, 'Unsupported source backend: expected one EclipseAnimationAsset')
            a = animations[0]
            mapping = document(a['ChannelToDofAsset']['partition'])
            channels = [dofs.get(i, 'unknown:' + str(i)) for i in mapping['instances'][0]['object']['fields']['ChannelToDofIds']]
            blob = (args.cache / 'resources' / (str(a['Resource']['resource_id']) + '.bin')).read_bytes()
            require(struct.unpack_from('<I', blob, 36)[0] in (1,2), 'Unsupported native quaternion codec')
            if args.direct:
                require((root.get('RigRef') or {}).get('partition')==rig_source['partition'], 'Unsupported preview rig')
                require(a.get('TrimOffset',0)==0 and a.get('TrimmedDuration',a['EndFrame'])==a['EndFrame'], 'Unsupported trimmed native clip')
                times,frames=decode_pose(blob,channels,defaults,names,a['EndFrame'])
            else:
                vectors = decode_vectors(blob, channels)
                with tempfile.TemporaryDirectory(prefix='native-preview-') as temp:
                    fbx = Path(temp) / 'source.fbx'
                    process = subprocess.run([str(args.cli), 'animation-export', str(args.game), asset, str(fbx)], capture_output=True, text=True, timeout=120)
                    require(process.returncode == 0, 'CLI export failed: ' + (process.stderr or process.stdout)[-600:])
                    clip = Clip(fbx)
                    times, frames = local_frames(clip, names, parents, vectors)
            has_board = any(n.startswith('Skateboard_Root.') for n in channels)
            payload = encode_clip(times, frames, frames[:, 380] if has_board else None, -1, -1)
            filename = 'native-' + key + '.clip'; temp = args.output / (filename + '.tmp')
            temp.write_bytes(payload); os.replace(temp, args.output / filename)
            entry = dict(category=metadata[asset]['category'], label=asset.removeprefix('animation/dingo/').rsplit('/', 1)[-1],
                         trick='native:' + asset, pop='', file=filename, asset=asset, preview_only=True,
                         position_codec='native-piec-v1-extent', native_looping=bool(root.get('IsLooping', False)))
            result.update(state='decoded', detail=f'{len(times)} poses at 60 Hz; ' + ('body and board' if has_board else 'body only') + '; no live phase binding',
                          entry=entry, frames=len(times), duration=float(times[-1]), board=has_board, clip_sha256=hashlib.sha256(payload).hexdigest())
        except Exception as error:
            result.update(state='unsupported' if isinstance(error, ValueError) else 'error', detail=str(error)[:800])
        atomic_json(result_path, result)
        return result
    def checkpoint():
        for asset, result in results.items():
            metadata[asset].update(state=result['state'], detail=result['detail'][:400])
            if result['state'] == 'decoded': catalog[asset] = result['entry']
        for asset in catalog:
            if asset in metadata:
                metadata[asset]['state'] = 'decoded'
                if asset not in results: metadata[asset]['detail'] = 'Existing corrected native preview; original binding retained'
        rows = sorted(metadata.values(), key=lambda x: (x['category'], x['asset']))
        atomic_json(args.output / 'native-catalog.json', rows)
        atomic_json(args.output / 'catalog.json', sorted(catalog.values(), key=lambda x: (x['category'], x['label'], x['pop'])))
        tree = {}
        for row in rows:
            node = tree
            for part in row['category'].split('/'): node = node.setdefault('children', {}).setdefault(part, {})
            node.setdefault('assets', []).append(row['asset'])
        atomic_json(args.output / 'native-tree.json', tree)
        summary = dict(schema=1, inventory=len(rows), typed=len(index), previews=len(catalog),
                       states=dict(collections.Counter(x['state'] for x in rows)),
                       types=dict(collections.Counter(x['root_type'] for x in rows)),
                       static_links=edges, resolved_links=resolved,
                       decode_failures=dict(collections.Counter(x['detail'] for x in results.values() if x['state'] not in ('decoded','pending'))),
                       scope='Native Data in all inspected Win32 TOCs plus referenced channel maps; first asset per name; categories are hints; links are authoring dependencies, not measured execution order')
        atomic_json(args.output / 'decode-summary.json', summary)
        print(json.dumps({k: summary[k] for k in ('inventory', 'typed', 'previews', 'states', 'static_links', 'resolved_links')}), flush=True)
    candidates = [] if args.catalog_only else [x for x in index if x['root_type'] == 'ClipControllerAsset' and x['asset'] not in catalog]
    # Put movement/contact work first; all other clips are still attempted by default.
    candidates.sort(key=lambda x: (not any(s in x['asset'] for s in ('grind', 'manual', 'grab', 'trick')), x['asset']))
    if args.limit: candidates = candidates[:args.limit]
    checkpoint()
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        for i, result in enumerate(pool.map(decode, candidates), 1):
            results[result['asset']] = result
            if i % 25 == 0: print(json.dumps(dict(completed=i, total=len(candidates), states=dict(collections.Counter(x['state'] for x in results.values())))), flush=True)
            if i % 100 == 0: checkpoint()
    checkpoint()


if __name__ == '__main__': main()
