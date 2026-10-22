"""Independently audit source-attributed indexed native geometry, without a game."""
from pathlib import Path
import argparse
import hashlib
import json
import math
import struct
import numpy as np


def require(condition, issue):
    if not condition:
        raise ValueError(issue)


def unpack(resource, fmt, at):
    require(0 <= at <= len(resource)-struct.calcsize(fmt), 'resource read out of bounds')
    values = struct.unpack_from(fmt, resource, at)
    return values[0] if len(values) == 1 else values


def decode_literal(codes):
    """Independent vectorized literal compact-axis HLSL equations/signs."""
    a, b, angle, flags = codes.astype(np.float64).T
    flags = flags.astype(np.uint8)
    y = a/255*.707107-.707107+(flags & 16)*.0441942
    z = b/255*.707107-.707107+(flags & 32)*.0220971
    x = np.sqrt(np.abs(1-y*y-z*z))*np.where(flags & 1, -1, 1)
    n = np.stack((y, x, z), axis=1)
    n[(flags & 4) != 0] = np.stack((x, y, z), axis=1)[(flags & 4) != 0]
    permute = (flags & 2) == 0
    n[permute] = n[permute][:, (1, 2, 0)]
    n /= np.linalg.norm(n, axis=1)[:, None]
    ref = np.stack((n[:, 2], np.zeros(len(n)), -n[:, 0]), axis=1)
    alt = np.stack((-n[:, 1], n[:, 0], np.zeros(len(n))), axis=1)
    ref[(flags & 2) != 0] = alt[(flags & 2) != 0]
    ref /= np.linalg.norm(ref, axis=1)[:, None]
    radians = angle/255*1.5708+(flags & 192)*.0245437
    t = -ref*np.cos(radians)[:, None]-np.cross(n, ref)*np.sin(radians)[:, None]
    bitangent = -np.cross(n, t)
    bitangent[(flags & 8) != 0] *= -1
    return t, bitangent, n


def source_triangles(name, manifest, arrays, wheel_radius=.34):
    origin = np.array(manifest['anchors']['source_origin'])
    scale = manifest['anchors']['uniform_scale_to_radius_034']*(wheel_radius/.34)
    extra = next(m['id'] for m in manifest['materials'] if m['name'] == 'extra')
    positions, normals, coordinates = [], [], []
    for row in manifest['meshes']:
        source_part = {698532065: 'left_pedal', 709757344: 'right_pedal'}.get(row['geometry_id'], row['group'])
        if source_part != name:
            continue
        key = row['array_key']
        encoding = arrays[key+'_source_polygon_encoding']
        vertices = np.where(encoding < 0, -encoding-1, encoding)
        triangles = arrays[key+'_triangles_wedge_indices']
        positions.append(((arrays[key+'_world_vertices_m'][vertices[triangles]]-origin)*scale).astype('<f4'))
        normal_indices = arrays[key+'_source_normal_index'][triangles]
        normals.append(arrays[key+'_world_normals'][normal_indices])
        uv = arrays[key+'_source_uv'][arrays[key+'_source_uv_index'][triangles]].copy()
        uv[:, :, 0] = ((2048 if row['material_id'] == extra else 0)+8+2032*uv[:, :, 0])/4096
        uv[:, :, 1] = (8+2032*(1-uv[:, :, 1]))/2048
        coordinates.append(uv)
    return np.concatenate(positions), np.concatenate(normals), np.concatenate(coordinates)


def validate_part(resource, geometry, refs, expected):
    require(unpack(resource, '<4I', 0) == (192, 176, 384, 0), 'wrong static resource layout')
    lod = unpack(resource, '<Q', 48)+16
    table = unpack(resource, '<Q', lod+12)+16
    require(unpack(resource, '<I', lod+8) == 6, 'wrong section count')
    require(unpack(resource, '<I', lod+84) == 33, 'wrong index format')
    faces, first, start, count = unpack(resource, '<4I', table+2*384+32)
    require(faces > 0 and 0 < count <= 65535 and first == 0 and start == 0, 'visible section index limits')
    indices = faces*3
    require(unpack(resource, '<4I', table+5*384+32) == (faces, indices, count*20, count), 'depth section mismatch')
    for i in (0, 1, 3, 4):
        require(unpack(resource, '<I', table+i*384+32) == 0 and unpack(resource, '<I', table+i*384+44) == 0, 'unused section active')
    index_bytes, vertex_bytes = unpack(resource, '<II', lod+88)
    require(index_bytes == indices*4 and vertex_bytes == count*32, 'buffer sizes mismatch')
    require(len(geometry) == index_bytes+vertex_bytes, 'geometry chunk size mismatch')
    visible = np.frombuffer(geometry, dtype='<f4', count=count*3).reshape(-1, 3)
    uv = np.frombuffer(geometry, dtype='<f2', offset=count*12, count=count*2).reshape(-1, 2)
    codes = np.frombuffer(geometry, dtype=np.uint8, offset=count*16, count=count*4).reshape(-1, 4)
    depth = np.frombuffer(geometry, dtype='<f4', offset=count*20, count=count*3).reshape(-1, 3)
    face = np.frombuffer(geometry, dtype='<u2', offset=vertex_bytes, count=indices).reshape(-1, 3)
    depth_index = np.frombuffer(geometry, dtype='<u2', offset=vertex_bytes+indices*2, count=indices).reshape(-1, 3)
    require(np.isfinite(visible).all() and np.isfinite(uv).all() and face.max() < count, 'nonfinite/index overflow')
    require(np.array_equal(visible, depth) and np.array_equal(face, depth_index), 'depth diverges from actual surface')
    require(np.array_equal(visible, refs['position'].astype('<f4')) and np.array_equal(face, refs['triangles']), 'source-target positions/indices differ')
    require(np.array_equal(uv, refs['uv'].astype('<f2')), 'native UV differs from target')
    source_position, source_normal, source_uv = expected
    require(np.array_equal(visible[face], source_position), 'authored triangle positions/winding changed')
    require(np.array_equal(refs['normal'][face], source_normal), 'authored corner normals changed')
    require(np.array_equal(refs['uv'][face], source_uv), 'authored corner UV/atlas mapping changed')
    nlo, nhi = unpack(resource, '<3f', 16), unpack(resource, '<3f', 32)
    require(np.array_equal(nlo, visible.min(axis=0)) and np.array_equal(nhi, visible.max(axis=0)), 'culling bounds differ from geometry')
    t, b, n = decode_literal(codes)
    normal_dot = np.sum(n*refs['normal'], axis=1)
    target_t = refs['tangent']-n*np.sum(refs['tangent']*n, axis=1)[:, None]
    target_t /= np.linalg.norm(target_t, axis=1)[:, None]
    tangent_dot = np.sum(t*target_t, axis=1)
    handed = np.sum(np.cross(t, b)*n, axis=1)
    require(np.isfinite(t).all() and np.isfinite(b).all() and np.isfinite(n).all(), 'invalid compact basis')
    require(normal_dot.min() > math.cos(math.radians(.3)), 'authored normal quantization error')
    require(tangent_dot.min() > math.cos(math.radians(.3)), 'UV tangent quantization error')
    require(np.abs(handed-refs['handedness']).max() < 1e-5, 'UV handedness mismatch')
    categories = []
    for category in range(5):
        length, offset = unpack(resource, '<IQ', lod+20+category*12)
        require(length <= 6 and offset+16+length <= len(resource), 'category bounds invalid')
        selected = list(resource[offset+16:offset+16+length])
        require(all(i in (2, 5) for i in selected), 'category selects a zero-sized section')
        categories.append(selected)
    return {'vertices': count, 'triangles': faces, 'categories': categories,
            'normal_error_max_degrees': math.degrees(math.acos(float(np.clip(normal_dot.min(), -1, 1)))),
            'tangent_error_max_degrees': math.degrees(math.acos(float(np.clip(tangent_dot.min(), -1, 1)))),
            'source_triangles_winding_normal_uv_exact': True, 'depth_visible_exact': True}


def validate(prepared, intermediate, negatives, wheel_radius=.34):
    require(math.isfinite(wheel_radius) and wheel_radius > 0, 'Wheel radius must be a finite positive number of metres')
    manifest_path = intermediate/'manifest.json'
    manifest = json.loads(manifest_path.read_text())
    npz = intermediate/'source-geometry.npz'
    digest = hashlib.sha256(npz.read_bytes()).hexdigest()
    expected_digest = next(row['sha256'] for row in manifest['outputs'] if row['path'].endswith('source-geometry.npz'))
    require(digest == expected_digest, 'Validated source intermediate changed')
    metadata = json.loads((prepared/'geometry.json').read_text())
    radius_factor = wheel_radius/.34
    scale = manifest['anchors']['uniform_scale_to_radius_034']*radius_factor
    require(math.isfinite(scale) and scale > 0, 'Selected wheel radius produces an invalid uniform scale')
    selected_anchors = {key: (np.asarray(value)*radius_factor).tolist()
                        for key, value in manifest['anchors']['normalized'].items()}
    require(metadata['wheel_radius_m'] == wheel_radius and metadata['radius_scale_factor'] == radius_factor,
            'Prepared wheel radius differs from the requested validation radius')
    require(metadata['uniform_scale'] == scale and metadata['selected_normalized_anchors'] == selected_anchors,
            'Prepared selected scale/anchors differ from the immutable source reference')
    require(metadata['anchors'] == manifest['anchors'] and metadata['source_origin'] == manifest['anchors']['source_origin'],
            'Prepared source anchors/origin differ from the immutable intermediate')
    require(metadata['manifest_sha256'] == hashlib.sha256(manifest_path.read_bytes()).hexdigest()
            and metadata['intermediate_sha256'] == digest and metadata['source_sha256'] == manifest['source_sha256'],
            'Prepared source attribution differs from the immutable intermediate')
    arrays = np.load(npz)
    report = {'scope': 'Offline source-attributed native mesh byte audit; no renderer/game/material semantics claim',
              'wheel_radius_m': wheel_radius, 'radius_scale_factor': radius_factor, 'uniform_scale': scale,
              'selected_normalized_anchors': selected_anchors, 'intermediate_sha256': digest, 'parts': []}
    for name in ('frame', 'fork', 'rear_wheel', 'front_wheel', 'cranks', 'handlebars', 'left_pedal', 'right_pedal'):
        root = prepared/'parts'
        resource, geometry = (root/f'{name}.mesh').read_bytes(), (root/f'{name}.geom').read_bytes()
        with np.load(root/f'{name}.source-target.npz') as refs:
            expected = source_triangles(name, manifest, arrays, wheel_radius)
            result = validate_part(resource, geometry, refs, expected)
            if negatives and name == 'fork':
                failures = []
                count = result['vertices']
                variants = []
                mutated = bytearray(geometry);mutated[0] ^= 1;variants.append(('position', resource, mutated))
                mutated = bytearray(geometry);mutated[count*12+1] ^= 1;variants.append(('UV', resource, mutated))
                mutated = bytearray(geometry);mutated[count*16+3] ^= 8;variants.append(('handedness', resource, mutated))
                mutated = bytearray(geometry);at=count*32;mutated[at:at+2],mutated[at+2:at+4]=mutated[at+2:at+4],mutated[at:at+2];variants.append(('winding', resource, mutated))
                for label, wrong_resource, wrong_geometry in variants:
                    try:
                        validate_part(wrong_resource, wrong_geometry, refs, expected)
                    except ValueError:
                        failures.append(label)
                    else:
                        raise ValueError('Negative fixture incorrectly accepted: '+label)
                result['negative_rejections'] = failures
        result['part'] = name
        result['mesh_sha256'] = hashlib.sha256(resource).hexdigest()
        result['geometry_sha256'] = hashlib.sha256(geometry).hexdigest()
        report['parts'].append(result)
        print(name, 'PASS', result['vertices'], result['triangles'], flush=True)
    report['passed'] = True
    report['total_triangles'] = sum(p['triangles'] for p in report['parts'])
    require(report['total_triangles'] == manifest['triangles'], 'source surface triangle count changed')
    (prepared/'independent-geometry-validation.json').write_text(json.dumps(report, indent=2)+'\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--prepared', type=Path, required=True)
    parser.add_argument('--intermediate', type=Path, default=Path('build/bmx/free-model-inspection/intermediate'))
    parser.add_argument('--negative-fixtures', action='store_true')
    parser.add_argument('--wheel-radius', type=float, default=.34, help='Expected maximum tire radius in metres; independently reconstructs source triangles at this scale')
    args = parser.parse_args()
    validate(args.prepared, args.intermediate, args.negative_fixtures, args.wheel_radius)
