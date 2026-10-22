"""Prepare source-wedge native meshes from the user's validated FBX intermediate.

Writes a NEW private output only. It never reads a running game or replaces a pack.
The six source groups become eight articulated parts by extracting the two exact
pedal platform objects. Authored normals and polygon winding are kept;
only uniform scale/origin and the documented two-tile UV coordinate conversion apply.
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import math
import struct

import numpy as np
from PIL import Image

SOURCE_SHA = '00ce4d383a31b786837a0d229dcb8d651dc7580166aff8741308c202647caa2a'
TEMPLATE_SHA = '0974fac4e1d0c5828ec68eb02cca0b966da73625cf9389f48f4be7bc2553d0cc'
PARTS = ('frame', 'fork', 'rear_wheel', 'front_wheel', 'cranks', 'handlebars', 'left_pedal', 'right_pedal')
PEDAL_OBJECTS = {698532065: 'left_pedal', 709757344: 'right_pedal'}
TILE = 2048
GUTTER = 8
INTERIOR = TILE - 2 * GUTTER
ATLAS_SIZE = (2 * TILE, TILE)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def part_for(row):
    return PEDAL_OBJECTS.get(row['geometry_id'], row['group'])


def verified_encoder():
    path = Path(__file__).with_name('prepare_native_parts.py')
    spec = importlib.util.spec_from_file_location('verified_native_basis', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module, path


def atlas_uv(uv, tile):
    """FBX/OBJ bottom-origin V -> image/native top-origin V, with edge gutters."""
    if not np.isfinite(uv).all() or uv.min() < 0 or uv.max() > 1:
        raise ValueError('Source UV tiling requires an explicit different atlas policy')
    out = uv.copy()
    out[:, 0] = (GUTTER + tile*TILE + uv[:, 0]*INTERIOR) / ATLAS_SIZE[0]
    out[:, 1] = (GUTTER + (1-uv[:, 1])*INTERIOR) / ATLAS_SIZE[1]
    return out


def make_atlases(intermediate, output, manifest):
    target = output/'atlas'
    target.mkdir()
    channels = {'albedo': 'BaseColor', 'normal-source': 'Normal',
                'roughness-source': 'Roughness', 'metallic-source': 'Metallic'}
    reports = []
    known = {row['name']: row for row in manifest['textures']}
    for channel, suffix in channels.items():
        atlas = Image.new('RGBA', ATLAS_SIZE)
        inputs = []
        for tile, family in enumerate(('base', 'extra')):
            name = f'{family}_{suffix}.png'
            source = intermediate/'textures'/name
            digest = sha(source.read_bytes())
            if digest != known[name]['sha256']:
                raise ValueError(f'Supplied texture identity changed: {name}')
            with Image.open(source) as image:
                size = image.size
                content = image.convert('RGBA').resize((INTERIOR, INTERIOR), Image.Resampling.LANCZOS)
            if channel == 'normal-source':
                # Preserve the supplied normal-map component convention. Do not
                # silently invert green or claim the native shader contract.
                values = np.asarray(content, dtype=np.float32)/127.5-1
                vectors = values[:, :, :3]
                norm = np.linalg.norm(vectors, axis=2)
                if np.any(norm < .01):
                    raise ValueError('Source normal atlas contains invalid vectors')
                vectors /= norm[:, :, None]
                pixels = np.empty((INTERIOR, INTERIOR, 4), dtype=np.uint8)
                pixels[:, :, :3] = np.rint(np.clip(vectors*.5+.5, 0, 1)*255).astype(np.uint8)
                pixels[:, :, 3] = 255
                content = Image.fromarray(pixels)
            x = tile*TILE+GUTTER
            atlas.paste(content, (x, GUTTER))
            atlas.paste(content.crop((0, 0, 1, INTERIOR)).resize((GUTTER, INTERIOR)), (tile*TILE, GUTTER))
            atlas.paste(content.crop((INTERIOR-1, 0, INTERIOR, INTERIOR)).resize((GUTTER, INTERIOR)), (x+INTERIOR, GUTTER))
            # Fill full-width top/bottom bands after left/right edge replication.
            top = atlas.crop((tile*TILE, GUTTER, (tile+1)*TILE, GUTTER+1))
            bottom = atlas.crop((tile*TILE, GUTTER+INTERIOR-1, (tile+1)*TILE, GUTTER+INTERIOR))
            atlas.paste(top.resize((TILE, GUTTER)), (tile*TILE, 0))
            atlas.paste(bottom.resize((TILE, GUTTER)), (tile*TILE, TILE-GUTTER))
            inputs.append({'path': name, 'sha256': digest, 'source_size': size,
                           'tile': tile, 'interior_size': INTERIOR})
        path = target/f'{channel}.png'
        atlas.save(path)
        reports.append({'channel': channel, 'path': str(path.relative_to(output)),
                        'size': ATLAS_SIZE, 'sha256': sha(path.read_bytes()), 'inputs': inputs})
    flat = target/'flat-normal.png'
    Image.new('RGBA', (4, 4), (128, 128, 255, 255)).save(flat)
    return {'size': ATLAS_SIZE, 'tile_size': TILE, 'gutter': GUTTER,
            'native_material_slot': 2, 'uv': 'u=(tile*2048+8+2032*u)/4096; v=(8+2032*(1-v))/2048',
            'normal_convention': 'Supplied RGB preserved and renormalized after resize; native green/BC5 contract not claimed',
            'native_mask': 'UNCHANGED donor mask; no supplied roughness/metallic packing is inferred',
            'rul_material': 'Explicit supplied base_* basename fallback from validated intermediate',
            'channels': reports, 'flat_normal': str(flat.relative_to(output))}


def mesh_arrays(row, arrays, origin, scale, tile):
    key = row['array_key']
    unique = arrays[key+'_split_tuples']
    positions = (arrays[key+'_world_vertices_m'][unique[:, 0]]-origin)*scale
    normals = arrays[key+'_world_normals'][unique[:, 1]]
    source_uv = arrays[key+'_source_uv'][unique[:, 2]]
    uv = atlas_uv(source_uv, tile)
    triangles = arrays[key+'_wedge_to_split'][arrays[key+'_triangles_wedge_indices']]
    if not len(triangles) or not np.isfinite(positions).all() or not np.isfinite(normals).all():
        raise ValueError('Invalid source mesh arrays')
    return positions, normals, uv, triangles, source_uv


def tangents(positions, normals, uv, triangles):
    """UV-derived tangent averaging, with opposite UV handedness split first."""
    p = positions[triangles]
    e1, e2 = p[:, 1]-p[:, 0], p[:, 2]-p[:, 0]
    d1, d2 = uv[triangles[:, 1]]-uv[triangles[:, 0]], uv[triangles[:, 2]]-uv[triangles[:, 0]]
    determinant = d1[:, 0]*d2[:, 1]-d1[:, 1]*d2[:, 0]
    valid = np.abs(determinant) > 1e-14
    denominator = np.where(valid, determinant, 1)
    t = (e1*d2[:, 1, None]-e2*d1[:, 1, None])/denominator[:, None]
    b = (e2*d1[:, 0, None]-e1*d2[:, 0, None])/denominator[:, None]
    t[~valid] = 0
    b[~valid] = 0
    # Source normals can be smoothed rather than geometric. Handedness must be
    # measured in each authored corner's normal frame, not inferred from winding.
    signs = np.where(np.sum(np.cross(normals[triangles], t[:, None, :])*b[:, None, :], axis=2) < 0, -1, 1)
    keys = np.stack((triangles.ravel(), signs.ravel()), axis=1)
    unique, inverse = np.unique(keys, axis=0, return_inverse=True)
    output_triangles = inverse.reshape(-1, 3)
    output_positions = positions[unique[:, 0]]
    output_normals = normals[unique[:, 0]]
    output_uv = uv[unique[:, 0]]
    accum = np.zeros((len(unique), 3))
    # Area-weighted accumulation preserves the source normal and UV seams.
    area2 = np.linalg.norm(np.cross(e1, e2), axis=1)
    if not np.isfinite(area2).all() or np.any(area2 < 1e-14):
        raise ValueError('Degenerate source geometry requires explicit review')
    for corner in range(3):
        np.add.at(accum, output_triangles[:, corner], t*area2[:, None])
    projected = accum-output_normals*np.einsum('ij,ij->i', accum, output_normals)[:, None]
    lengths = np.linalg.norm(projected, axis=1)
    fallback = lengths < 1e-12
    if np.any(fallback):
        n = output_normals[fallback]
        axis = np.eye(3)[np.argmin(np.abs(n), axis=1)]
        projected[fallback] = axis-n*np.einsum('ij,ij->i', axis, n)[:, None]
    projected /= np.linalg.norm(projected, axis=1)[:, None]
    return output_positions, output_normals, output_uv, projected, unique[:, 1], output_triangles, int((~valid).sum())


def write_native(name, vertices, normals, uv, tangent, handedness, triangles, inspection, target, encoder):
    if not 0 < len(vertices) <= 65535 or triangles.min() < 0 or triangles.max() >= len(vertices):
        raise ValueError(f'{name}: native 16-bit section index limit exceeded')
    resource_path = inspection/'native-template.mesh'
    if not resource_path.exists():
        resource_path = inspection/'45392.bin'
    resource = bytearray(resource_path.read_bytes())
    if sha(resource) != TEMPLATE_SHA:
        raise ValueError('Validated native template identity changed')
    if struct.unpack_from('<4I', resource) != (192, 176, 384, 0):
        raise ValueError('Unsupported native static template')
    lod = struct.unpack_from('<Q', resource, 48)[0]+16
    table = struct.unpack_from('<Q', resource, lod+12)[0]+16
    if struct.unpack_from('<I', resource, lod+8)[0] != 6 or struct.unpack_from('<I', resource, lod+84)[0] != 33:
        raise ValueError('Expected six sections and 16-bit native indices')
    section = table+2*384
    if resource[section+116:section+124] != bytes((33, 6, 0, 1, 52, 13, 0, 2)):
        raise ValueError('Unsupported native UV/tangent declaration')
    codes = bytearray()
    cache = {}
    normal_min, tangent_min, basis_error = 1., 1., 0.
    for n, t, sign in zip(normals, tangent, handedness):
        values = tuple(float(v) for v in n), tuple(float(v) for v in t)
        key = values+(int(sign),)
        code = cache.get(key)
        if code is None:
            code = encoder.native_axis_encode(values[0], values[1], int(sign))
            cache[key] = code
        decoded_t, decoded_b, decoded_n = encoder.native_axis_basis(code)
        normal_min = min(normal_min, float(np.dot(decoded_n, n)))
        projected = t-np.dot(t, decoded_n)*np.array(decoded_n)
        projected /= np.linalg.norm(projected)
        tangent_min = min(tangent_min, float(np.dot(decoded_t, projected)))
        basis_error = max(basis_error, abs(float(np.dot(np.cross(decoded_t, decoded_b), decoded_n))-int(sign)))
        codes.extend(code)
    positions = np.asarray(vertices, dtype='<f4').tobytes()
    half_uv = np.asarray(uv, dtype='<f2')
    index = np.asarray(triangles, dtype='<u2').tobytes()
    geometry = positions+half_uv.tobytes()+codes+positions+index+index
    count, indices, faces = len(vertices), triangles.size, len(triangles)
    struct.pack_into('<3f', resource, 16, *vertices.min(axis=0))
    struct.pack_into('<3f', resource, 32, *vertices.max(axis=0))
    struct.pack_into('<II', resource, lod+88, len(index)*2, count*32)
    resource[lod+116:lod+132] = hashlib.sha1(('ReSkateBMXNative20261017/'+name+'/chunk').encode()).digest()[:16]
    for part in range(6):
        at = table+part*384
        active, depth = part in (2, 5), part == 5
        start = count*20 if depth else 0
        struct.pack_into('<4I', resource, at+32, faces if active else 0, indices if depth else 0, start, count if active else 0)
        struct.pack_into('<I', resource, at+84, start)
    for category in range(5):
        at = lod+20+category*12
        old_count, offset = struct.unpack_from('<IQ', resource, at)
        start = offset+16
        if old_count > 6 or start+old_count > len(resource):
            raise ValueError('Invalid native category list')
        old = resource[start:start+old_count]
        if any(i >= 6 for i in old):
            raise ValueError('Invalid native category section')
        selected = bytes(i for i in old if i in (2, 5))
        struct.pack_into('<I', resource, at, len(selected))
        resource[start:start+len(selected)] = selected
    (target/f'{name}.mesh').write_bytes(resource)
    (target/f'{name}.geom').write_bytes(geometry)
    with (target/f'{name}.tri').open('wb') as file:
        file.write(struct.pack('<I', faces))
        file.write(np.asarray(vertices[triangles], dtype='<f4').tobytes())
    np.savez(target/f'{name}.source-target.npz', position=vertices, normal=normals, uv=uv,
             tangent=tangent, handedness=handedness, triangles=triangles)
    if normal_min < .99998 or tangent_min < .99998 or basis_error > 1e-5:
        raise ValueError(f'{name}: compact tangent roundtrip failed')
    return {'part': name, 'vertices': count, 'triangles': faces,
            'bounds': [vertices.min(axis=0).tolist(), vertices.max(axis=0).tolist()],
            'normal_dot_min': normal_min, 'tangent_dot_min': tangent_min,
            'basis_handedness_error_max': basis_error,
            'uv_half_error_max': float(np.abs(half_uv.astype(np.float64)-uv).max()),
            'source_single_winding': True, 'material_sections': [2, 5],
            'files': {suffix: sha((target/f'{name}.{suffix}').read_bytes()) for suffix in ('mesh', 'geom', 'tri', 'source-target.npz')}}


def prepare(intermediate, output, inspection, geometry_only=False, wheel_radius=.34):
    if not math.isfinite(wheel_radius) or wheel_radius <= 0:
        raise ValueError('Wheel radius must be a finite positive number of metres')
    if output.exists():
        raise ValueError('Output must be a NEW separate private directory')
    manifest_path = intermediate/'manifest.json'
    manifest = json.loads(manifest_path.read_text())
    if manifest['source_sha256'] != SOURCE_SHA or sha(Path(manifest['source']).read_bytes()) != SOURCE_SHA:
        raise ValueError('Supplied FBX identity changed')
    npz = intermediate/'source-geometry.npz'
    expected = next(x['sha256'] for x in manifest['outputs'] if x['path'].endswith('source-geometry.npz'))
    if sha(npz.read_bytes()) != expected:
        raise ValueError('Validated source intermediate changed')
    arrays = np.load(npz)
    encoder, encoder_path = verified_encoder()
    origin = np.array(manifest['anchors']['source_origin'])
    radius_factor = wheel_radius/.34
    scale = manifest['anchors']['uniform_scale_to_radius_034']*radius_factor
    if not math.isfinite(scale) or scale <= 0:
        raise ValueError('Selected wheel radius produces an invalid uniform scale')
    selected_anchors = {key: (np.asarray(value)*radius_factor).tolist()
                        for key, value in manifest['anchors']['normalized'].items()}
    material_tile = {row['id']: int(row['name'] == 'extra') for row in manifest['materials']}
    output.mkdir(parents=True)
    target = output/'parts'
    target.mkdir()
    reports = []
    for part in PARTS:
        chunks = []
        vertex_offset = 0
        material_faces = {'base': 0, 'extra': 0}
        uv_degenerate = 0
        for row in manifest['meshes']:
            if part_for(row) != part:
                continue
            tile = material_tile[row['material_id']]
            p, n, uv, faces, _ = mesh_arrays(row, arrays, origin, scale, tile)
            p, n, uv, t, hand, faces, bad_uv = tangents(p, n, uv, faces)
            chunks.append((p, n, uv, t, hand, faces+vertex_offset))
            vertex_offset += len(p)
            material_faces['extra' if tile else 'base'] += len(faces)
            uv_degenerate += bad_uv
        joined = [np.concatenate([chunk[i] for chunk in chunks]) for i in range(6)]
        report = write_native(part, *joined, inspection, target, encoder)
        report.update(material_triangles=material_faces, degenerate_uv_triangles=uv_degenerate)
        reports.append(report)
        print(json.dumps({'part': part, 'vertices': report['vertices'], 'triangles': report['triangles']}), flush=True)
    atlas = None if geometry_only else make_atlases(intermediate, output, manifest)
    report = {'source': manifest['source'], 'source_sha256': SOURCE_SHA,
              'manifest_sha256': sha(manifest_path.read_bytes()), 'intermediate_sha256': expected,
              'encoder_sha256': sha(encoder_path.read_bytes()), 'uniform_scale': scale,
              'wheel_radius_m': wheel_radius, 'radius_scale_factor': radius_factor,
              'selected_normalized_anchors': selected_anchors,
              'source_origin': origin.tolist(), 'anchors': manifest['anchors'], 'parts': reports, 'atlas': atlas,
              'pedal_platform_objects': [{'geometry_id': ident, 'part': name,
                                          'source_mesh': next(row['name'] for row in manifest['meshes'] if row['geometry_id'] == ident),
                                          'pivot': 'Source pedalpin axle center; native articulation keeps platform orientation level while orbiting the BB'}
                                         for ident, name in PEDAL_OBJECTS.items()],
              'total_triangles': sum(x['triangles'] for x in reports),
              'limits': ['No source geometry simplification or double-sided triangle duplication.',
                         'Indexed authored wedge normals and UVs retained; opposite UV handedness creates an explicit extra vertex seam.',
                         'Tangent averaging is a local UV-derived adaptation, not an authored FBX tangent claim.',
                         'Source proportions preserved at the selected maximum tire radius; native physics/IK must use matching geometry. Source anchors retain the immutable .34m reference; selected_normalized_anchors use the selected radius.',
                         'Static chain and steering-crossing cable retain source grouping; exact pedal platform objects are separate parts and pins remain with cranks.',
                         'No native material roughness/metallic/normal green-channel semantics or user pixels are claimed.']}
    (output/'geometry.json').write_text(json.dumps(report, indent=2)+'\n')
    (target/'geometry.json').write_text(json.dumps(reports, indent=2)+'\n')
    (target/'part-list.txt').write_text('\n'.join(PARTS)+'\n')
    print(f'Prepared NEW private geometry at wheel radius {wheel_radius:.9g}m (reference factor {radius_factor:.17g}); no deployed pack changed.', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--intermediate', type=Path, default=Path('build/bmx/free-model-inspection/intermediate'))
    parser.add_argument('--inspection', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--geometry-only', action='store_true', help='Skip texture decoding/resizing during lightweight source preparation')
    parser.add_argument('--wheel-radius', type=float, default=.34, help='Selected maximum tire radius in metres; uniformly scales the immutable .34m reference')
    args = parser.parse_args()
    prepare(args.intermediate, args.output, args.inspection, args.geometry_only, args.wheel_radius)
