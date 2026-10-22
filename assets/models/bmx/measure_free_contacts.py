"""Measure source grip clearance and pedal contact planes; no game access."""
from pathlib import Path
import argparse
import hashlib
import json
import math
import numpy as np


def measure(intermediate, output, wheel_radius=.34):
    if not math.isfinite(wheel_radius) or wheel_radius <= 0:
        raise ValueError('Wheel radius must be a finite positive number of metres')
    path = intermediate/'manifest.json'
    manifest = json.loads(path.read_text())
    npz = intermediate/'source-geometry.npz'
    digest = hashlib.sha256(npz.read_bytes()).hexdigest()
    expected = next(row['sha256'] for row in manifest['outputs'] if row['path'].endswith('source-geometry.npz'))
    if digest != expected:
        raise ValueError('Validated intermediate identity changed')
    arrays = np.load(npz)
    anchors = manifest['anchors']
    origin = np.array(anchors['source_origin'])
    radius_factor = wheel_radius/.34
    scale = anchors['uniform_scale_to_radius_034']*radius_factor
    if not math.isfinite(scale) or scale <= 0:
        raise ValueError('Selected wheel radius produces an invalid uniform scale')
    selected_anchors = {key: (np.asarray(value)*radius_factor).tolist()
                        for key, value in anchors['normalized'].items()}
    grips, pedals, spindles = {}, {}, {}
    row = next(row for row in manifest['meshes'] if row['name'] == 'Cylinder')
    vertices = arrays[row['array_key']+'_world_vertices_m']
    for side, sign in (('left', 1), ('right', -1)):
        fit = anchors['grip_fits'][side]
        axis = np.array(fit['axis'])
        values = (vertices[vertices[:, 0]*sign > 0]-fit['center'])*scale
        along = values@axis
        radial = np.linalg.norm(values-along[:, None]*axis, axis=1)
        middle = np.abs(along) < .025
        grips[side] = {'mesh': row['name'], 'geometry_id': row['geometry_id'], 'array_key': row['array_key'],
                       'method': 'Actual vertices within +/-25mm along the recorded geometry-fit grip axis; knurl is not a perfect cylinder',
                       'center': ((np.array(fit['center'])-origin)*scale).tolist(),
                       'axis_positive_x': axis.tolist(),
                       'sample_count': int(middle.sum()),
                       'radius_quantiles_min_p05_p50_p95_max_m': np.quantile(radial[middle], [0, .05, .5, .95, 1]).tolist(),
                       'along_extent_m': [float(along.min()), float(along.max())]}
    for row in manifest['meshes']:
        if row['name'].startswith('pedalpin_low'):
            vertices = (arrays[row['array_key']+'_world_vertices_m']-origin)*scale
            lo, hi = vertices.min(axis=0), vertices.max(axis=0)
            center = (lo+hi)/2
            side = 'left' if center[0] > 0 else 'right'
            bb = np.array(selected_anchors['bb'])
            spindles[side] = {'mesh': row['name'], 'geometry_id': row['geometry_id'], 'array_key': row['array_key'],
                              'method': 'Actual transformed source cylinder bounds midpoint; BB is source frameshaft bounds midpoint',
                              'vertices': len(vertices), 'bbox': [lo.tolist(), hi.tolist()], 'center': center.tolist(),
                              'axis': [1, 0, 0], 'bb': bb.tolist(),
                              'crank_orbit_radius_yz_m': float(np.linalg.norm((center-bb)[1:]))}
        if not row['name'].startswith('pedal_low'):
            continue
        key = row['array_key']
        vertices = (arrays[key+'_world_vertices_m']-origin)*scale
        encoded = arrays[key+'_source_polygon_encoding']
        wedge = np.where(encoded < 0, -encoded-1, encoded)
        triangles = vertices[wedge[arrays[key+'_triangles_wedge_indices']]]
        cross = np.cross(triangles[:, 1]-triangles[:, 0], triangles[:, 2]-triangles[:, 0])
        area = np.linalg.norm(cross, axis=1)/2
        normals = cross/np.maximum(2*area[:, None], 1e-20)
        heights = triangles[:, :, 1].mean(axis=1)
        horizontal = (normals[:, 1] > .99) & (np.ptp(triangles[:, :, 1], axis=1) < 1e-6) & (area > 1e-9)
        planes = []
        for height in np.unique(np.round(heights[horizontal], 6)):
            mask = horizontal & (np.abs(heights-height) < 1e-6)
            planes.append({'normal': [0, 1, 0], 'triangles': int(mask.sum()), 'area_m2': float(area[mask].sum()),
                           'centroid': np.average(triangles[mask].mean(axis=1), axis=0, weights=area[mask]).tolist()})
        if not planes:
            raise ValueError('No actual horizontal pedal top plane')
        side = 'left' if vertices[:, 0].mean() > 0 else 'right'
        pedals[side] = {'mesh': row['name'], 'geometry_id': row['geometry_id'], 'array_key': row['array_key'],
                        'method': 'Actual source triangles, geometric normal Y>.99, coplanarity<=1um; broad top plane chosen by area',
                        'broad_top_plane': max(planes, key=lambda p: p['area_m2']),
                        'bbox': [vertices.min(axis=0).tolist(), vertices.max(axis=0).tolist()],
                        'all_up_planes': planes}
    row = next(row for row in manifest['meshes'] if row['geometry_id'] == 114701578)
    key = row['array_key']
    vertices = (arrays[key+'_world_vertices_m']-origin)*scale
    encoded = arrays[key+'_source_polygon_encoding']
    wedge = np.where(encoded < 0, -encoded-1, encoded)
    triangles = vertices[wedge[arrays[key+'_triangles_wedge_indices']]]
    slices = []
    selected_points = []
    for level in (-.10, -.07, -.04, -.01, .02):
        points = []
        for a, b in ((0, 1), (1, 2), (2, 0)):
            delta = triangles[:, b, 2]-triangles[:, a, 2]
            valid = np.abs(delta) > 1e-10
            fraction = np.zeros(len(triangles))
            fraction[valid] = (level-triangles[valid, a, 2])/delta[valid]
            valid &= (fraction >= 0) & (fraction <= 1)
            points.extend(triangles[valid, a]+(triangles[valid, b]-triangles[valid, a])*fraction[valid, None])
        points = np.unique(np.round(points, 9), axis=0)
        ordered_y = np.sort(points[:, 1])
        gaps = np.flatnonzero(np.diff(ordered_y) > .02)
        if not len(gaps):
            raise ValueError('Top tube is not separated from lower tube at this source slice')
        upper = points[points[:, 1] >= ordered_y[gaps[-1]+1]-1e-8]
        lo, hi = upper.min(axis=0), upper.max(axis=0)
        slices.append({'z': level, 'actual_intersections': len(upper), 'bounds': [lo.tolist(), hi.tolist()],
                       'section_center': ((lo+hi)/2).tolist()})
        selected_points.append(upper)
    centers = np.array([row['section_center'] for row in slices])
    slope, intercept = np.polyfit(centers[:, 2], centers[:, 1], 1)
    axis = np.array([0, slope, 1]);axis /= np.linalg.norm(axis)
    center = np.array([centers[:, 0].mean(), slope*(-.04)+intercept, -.04])
    radial_points = np.concatenate(selected_points)-center
    radius = np.linalg.norm(radial_points-(radial_points@axis)[:, None]*axis, axis=1)
    top_tube = {'mesh': row['name'], 'geometry_id': row['geometry_id'], 'array_key': row['array_key'],
                'method': 'Actual triangle-edge intersections at five Z planes; upper disconnected section isolated by >20mm Y gap, line fit through section centers',
                'center_at_z_minus_004': center.tolist(), 'axis_positive_z': axis.tolist(),
                'radius_from_actual_slice_points_min_max': [float(radius.min()), float(radius.max())], 'slices': slices}
    report = {'source_sha256': manifest['source_sha256'], 'manifest_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
              'wheel_radius_m': wheel_radius, 'radius_scale_factor': radius_factor, 'uniform_scale': scale,
              'selected_normalized_anchors': selected_anchors,
              'intermediate_sha256': digest, 'grips': grips, 'pedals': pedals, 'spindles': spindles,
              'top_tube': top_tube,
              'limits': ['Geometry-derived contacts; no named semantic joints in supplied FBX.',
                         'Top pin tips protrude above broad pedal platform plane; actual shoe footprint/native skinning remains unverified.',
                         'Grip radius varies across knurl; median is not an enclosing cylinder.',
                         'Separate platform geometry IDs 698532065/709757344 permit counterrotation; source pedal pins remain with crank.']}
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2)+'\n')
    print(f'{output} (wheel radius {wheel_radius:.9g}m; reference factor {radius_factor:.17g})')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--intermediate', type=Path, default=Path('build/bmx/free-model-inspection/intermediate'))
    parser.add_argument('--output', type=Path, default=Path('build/bmx/free-model-inspection/source-contacts.json'))
    parser.add_argument('--wheel-radius', type=float, default=.34, help='Selected maximum tire radius in metres; uniformly scales the immutable .34m reference')
    args = parser.parse_args()
    measure(args.intermediate, args.output, args.wheel_radius)
