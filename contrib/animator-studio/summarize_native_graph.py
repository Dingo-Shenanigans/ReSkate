"""Summarize offline graph-probe output; never infer live addresses or decode tag flags.

Usage: python summarize_native_graph.py PROBE_DIRECTORY OUTPUT_DIRECTORY [OLD_NATIVE_INDEX]
"""
import collections
import json
import pathlib
import sys


def main():
    source, output = map(pathlib.Path, sys.argv[1:3])
    graph = json.loads((source / 'native-graph.json').read_text(encoding='utf-8-sig'))
    index = json.loads((source / 'native-asset-types.json').read_text(encoding='utf-8-sig'))
    output.mkdir(parents=True, exist_ok=True)
    assets = {a['asset']: a for a in graph['assets']}
    symbols = {(a['partition'], i['guid']): i for a in assets.values() for i in a['instances']
               if i['guid'] != '00000000-0000-0000-0000-000000000000'}

    def symbol(ref):
        if not isinstance(ref, dict):
            return ref
        found = symbols.get((ref.get('partition'), ref.get('instance')))
        if not found:
            return ref
        obj = found.get('object') or {}
        fields = obj.get('fields', {})
        label = fields.get('RuntimeName') or fields.get('Name')
        return dict(ref, instance_type=obj.get('type'), resolved_instance=found['index'],
                    symbol=label, enum_value=fields.get('IntValue'))

    edges, clips, layers, transitions = [], [], [], []
    for name, asset in assets.items():
        for instance in asset['instances']:
            obj = instance.get('object') or {}
            fields = obj.get('fields', {})

            def visit(value, path):
                if isinstance(value, dict):
                    if value.get('kind') == 'external':
                        edges.append(dict(source=name, source_instance=instance['index'], field=path,
                                          target=symbol(value)))
                    else:
                        for key, child in value.items():
                            visit(child, path + '.' + key)
                elif isinstance(value, list):
                    for i, child in enumerate(value):
                        visit(child, f'{path}[{i}]')
            visit(fields, '')
            if obj.get('type') == 'SuperLayersEntryAsset':
                keep = ('ControllerAsset', 'BlendMode', 'BlendMask', 'Weight', 'WeightOverride', 'Enable',
                        'EnableConstant', 'Mirror', 'MirrorOverride', 'EntryChangeBlendTicks', 'MatchBasePhaseWhenCreated')
                layers.append(dict(asset=name, instance=instance['index'], **{k: symbol(fields.get(k)) for k in keep}))
            if obj.get('type') == 'TransitionAsset':
                keep = ('From', 'To', 'BranchWindowType', 'BranchInWindowType', 'Blend', 'PhaseMode',
                        'DestinationPhase', 'DestinationTickOffset')
                transitions.append(dict(asset=name, **{k: symbol(fields.get(k)) for k in keep}))
        if asset['root_type'] == 'ClipControllerAsset':
            tags = []
            for instance in asset['instances']:
                obj = instance.get('object') or {}
                f = obj.get('fields', {})
                if 'StartLinkOffset' not in f:
                    continue
                fields = {k: v for k, v in f.items() if k not in ('Name', 'SplineUint8', 'XVals', 'YVals')}
                if 'ValuesRef' in fields:
                    fields['ValuesRef'] = [symbol(v) for v in fields['ValuesRef']]
                for k in ('GameStateValue', 'BranchTypeAsset', 'MotionTypesEnumRef'):
                    if k in fields:
                        fields[k] = symbol(fields[k])
                tags.append(dict(instance=instance['index'], type=obj.get('type'), fields=fields))
            clips.append(dict(asset=name, controller=asset['instances'][0]['object']['fields'], tags=tags))

    result = dict(scope=graph['scope'], decoded_assets=len(index), inspected_assets=len(assets),
                  reference_fields=len(edges), resolved_asset_references=sum(bool(e['target'].get('asset')) for e in edges),
                  resolved_instance_symbols=sum('resolved_instance' in e['target'] for e in edges),
                  type_counts=dict(collections.Counter(a['root_type'] for a in index).most_common()),
                  missing=graph['missing'], errors=graph['errors'], clips=clips, layers=layers,
                  transitions=transitions,
                  interpretation='Tag offsets/flags are raw authoring fields. No runtime phase, IK weight or graph execution order is measured here.')
    def write(name, value):
        (output / name).write_text(json.dumps(value, indent=2), encoding='utf-8')
    write('graph-summary.json', result)
    write('resolved-edges.json', edges)
    if len(sys.argv) > 3:
        original = json.loads(pathlib.Path(sys.argv[3]).read_text(encoding='utf-8-sig'))
        known = {a['asset']: a for a in index}
        for row in original:
            row['category_basis'] = 'name_hint'
            if row['asset'] in known:
                info = known[row['asset']]
                row.update(kind=info['root_type'], partition=info['partition'], payload_sha1=info['payload_sha1'],
                           toc=info['toc'], bundle=info['bundle'], type_verified=True)
            else:
                row.update(kind='unclassified', type_verified=False)
        write('native-catalog-typed.json', original)
        # The game's bounded local-profile parser consumes only these three fields.
        # Keep GUID/hash/bundle evidence in the offline typed index, not every UI row.
        write('native-catalog-runtime.json', [dict(category=row['category'], asset=row['asset'], kind=row['kind']) for row in original])
    print(f"Decoded {len(index)} assets; inspected {len(assets)}; exported {len(edges)} external field references")


if __name__ == '__main__':
    main()
