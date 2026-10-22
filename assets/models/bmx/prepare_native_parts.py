"""Prepare private native-world bike geometry; no game or source assets are modified.

The native pack builder consumes triangle lists in assembled bike coordinates.
The source geometry stays under the ignored reference directory.
"""
from pathlib import Path
import argparse
import math
import struct
import json
import hashlib
import re


MATERIAL_UVS = {
    # Pixel centres in the retained frame_2 atlas. Each lies inside a uniform
    # 13x13 base-colour area and a near-flat 7x7 resident normal-map area.
    # rubber RGB=(8,12,16), normal XY=127..128;
    # metal RGB=(32,36,38), normal XY=(127..128,127..130).
    'rubber': (392.5 / 512, 48.5 / 512),
    'metal': (440.5 / 512, 488.5 / 512),
}


def grip_anchors():
    """Read the same authored tube anchors used by the native hand solver."""
    path = Path(__file__).resolve().parents[3] / 'Extension/BMX/bmx_grip_anchors.inc'
    anchors = {}
    for line in path.read_text(encoding='utf-8').splitlines():
        if not line.startswith('BMX_GRIP('):
            continue
        match = re.fullmatch(r'BMX_GRIP\((left_anchor|right_anchor),\s*([0-9eE+.,\s-]+)\)', line)
        if not match:
            raise ValueError('Malformed shared grip anchor')
        values = tuple(float(v.strip()) for v in match[2].split(','))
        if len(values) != 6 or any(not math.isfinite(v) for v in values):
            raise ValueError('Invalid shared grip anchor coordinates')
        if abs(sum(v*v for v in values[3:]) - 1) > .00001 or match[1] in anchors:
            raise ValueError('Shared grip axis must be a unique unit vector')
        anchors[match[1]] = (values[:3], values[3:])
    if set(anchors) != {'left_anchor', 'right_anchor'}:
        raise ValueError('Both shared bar grip anchors are required')
    return anchors


def validate_grip_attachment(rows, anchors):
    """Slice the imported bars inside each sleeve; reject detached grips.

    The imported bar has long triangles through its grip region, so testing only
    vertices cannot establish contact at a sleeve midpoint. Intersect triangle
    edges with three perpendicular planes through each grip instead.
    """
    report = {}
    for name, (outer, inward) in anchors.items():
        sections = []
        for along in (.01, .05, .09):
            points = []
            for row in rows:
                corners = [row[i:i+3] for i in (0, 3, 6)]
                for a, b in zip(corners, corners[1:] + corners[:1]):
                    ta = sum((a[i]-outer[i])*inward[i] for i in range(3))
                    tb = sum((b[i]-outer[i])*inward[i] for i in range(3))
                    if abs(tb-ta) < 1e-10 or not min(ta, tb) <= along <= max(ta, tb):
                        continue
                    t = (along-ta)/(tb-ta)
                    points.append(tuple(a[i]+(b[i]-a[i])*t for i in range(3)))
            if len(points) < 6:
                raise ValueError(f'{name}: no imported bar inside the grip')
            center = tuple(outer[i]+inward[i]*along for i in range(3))
            radius = max(math.dist(p, center) for p in points)
            if radius > .020:
                raise ValueError(f'{name}: grip floats off the imported bar ({radius:.6f}m)')
            sections.append(dict(along_m=along, bar_radius_from_grip_axis_m=radius,
                                 intersected_edges=len(points)))
        report[name] = sections
    return report


def native_axis_normal(code):
    """Decode only the normal from the native UByte4N compact axis-angle basis.

    Format reference: FrostyToolsuite 1.0.6.3 Shaders/UnpackNormals.hlsl,
    CalcTangentSpaceNormal_AxisAngle. UByte4N is not a packed quaternion.
    Matching original GLB positions/Half2 UVs independently verifies this normal
    decoder against all 2,386 authored frame normals (minimum dot > .99985).
    """
    a, b, _, flags = code
    y = a / 255 * .707107 - .707107 + (flags & 16) * .0441942
    z = b / 255 * .707107 - .707107 + (flags & 32) * .0220971
    x = math.sqrt(abs(1 - y*y - z*z)) * (-1 if flags & 1 else 1)
    normal = (x, y, z) if flags & 4 else (y, x, z)
    if not flags & 2:
        normal = normal[1], normal[2], normal[0]
    size = math.sqrt(sum(v*v for v in normal))
    return tuple(v / size for v in normal)


def vector_dot(a, b):
    return sum(x*y for x, y in zip(a, b))


def vector_cross(a, b):
    return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])


def vector_unit(value):
    size = math.sqrt(vector_dot(value, value))
    if not math.isfinite(size) or size < 1e-12:
        raise ValueError('A tangent-space direction is invalid')
    return tuple(v / size for v in value)


def native_axis_basis(code):
    """Decode T/B/N columns using the verified compact axis-angle contract."""
    normal = native_axis_normal(code)
    _, _, angle_byte, flags = code
    ref = (-normal[1], normal[0], 0) if flags & 2 else (normal[2], 0, -normal[0])
    ref = vector_unit(ref)
    perpendicular = vector_cross(normal, ref)
    angle = angle_byte / 255 * 1.5708 + (flags & 192) * .0245437
    tangent = tuple(-ref[i]*math.cos(angle) - perpendicular[i]*math.sin(angle) for i in range(3))
    # HLSL forms B=cross(N,rotatedRef), while T=-rotatedRef. Its default
    # basis is left-handed; bit3 flips B to a right-handed basis.
    bitangent = tuple(-v for v in vector_cross(normal, tangent))
    if flags & 8:
        bitangent = tuple(-v for v in bitangent)
    return vector_unit(tangent), vector_unit(bitangent), normal


def native_axis_encode(normal, tangent, handedness=1):
    """Invert compact axis-angle, retaining the native four-byte declaration.

    The largest normal component is implicit. The other two components use
    8-bit magnitudes plus sign bits. Angle byte plus quadrant bits orient the
    tangent around the quantized normal; bit3 controls basis handedness.
    """
    if len(normal) != 3 or len(tangent) != 3 or handedness not in (-1, 1) or any(not math.isfinite(v) for v in normal + tangent):
        raise ValueError('Invalid compact tangent-basis input')
    normal = vector_unit(normal)
    largest = max(range(3), key=lambda i: abs(normal[i]))
    flags = (6, 2, 4)[largest] | (1 if normal[largest] < 0 else 0)
    values = (normal[1], normal[2]) if largest == 0 else ((normal[0], normal[2]) if largest == 1 else (normal[0], normal[1]))
    packed = []
    for value, sign_bit in zip(values, (16, 32)):
        if value >= 0:
            flags |= sign_bit
        fraction = value / .707107 + (0 if value >= 0 else 1)
        packed.append(max(0, min(255, round(fraction * 255))))
    if handedness > 0:
        flags |= 8
    decoded = native_axis_normal(bytes((*packed, 0, flags)))
    # Project against the quantized normal, so quantization cannot make the
    # requested tangent and the decoded normal non-orthogonal.
    projection = vector_dot(tangent, decoded)
    projected = tuple(tangent[i] - projection*decoded[i] for i in range(3))
    ref = (-decoded[1], decoded[0], 0) if flags & 2 else (decoded[2], 0, -decoded[0])
    ref = vector_unit(ref)
    if vector_dot(projected, projected) <= 1e-12:
        # A hub centre or degenerate triangle can supply a parallel direction.
        # Choose the same projected axis for N and -N, so back faces retain the
        # tangent orientation instead of inheriting an opposite reference axis.
        axis = min(range(3), key=lambda i: abs(decoded[i]))
        projected = tuple((1 if i == axis else 0) - decoded[axis]*decoded[i] for i in range(3))
    desired = vector_unit(projected)
    opposite = tuple(-v for v in desired)
    angle = math.atan2(vector_dot(opposite, vector_cross(decoded, ref)), vector_dot(opposite, ref)) % math.tau
    # Search four angle quadrants with the decoder's constants. This also
    # handles wraparound and endpoints without dropping a quadrant carry.
    choices = []
    for quadrant in range(4):
        value = max(0, min(255, round((angle-quadrant*64*.0245437) / 1.5708 * 255)))
        decoded_angle = value / 255 * 1.5708 + quadrant*64*.0245437
        error = abs(math.atan2(math.sin(decoded_angle-angle), math.cos(decoded_angle-angle)))
        choices.append((error, value, quadrant))
    _, value, quadrant = min(choices)
    return bytes((*packed, value, flags | quadrant*64))


def native_mesh(name, rows, inspection, output, rubber_ranges=()):
    """Retain a validated static mesh declaration and its native tangent encoding.

    Encode compact native tangent bases directly from the surface directions.
    Wheel normals are smoothed across welded corners with a 50-degree crease.
    Constant material UVs avoid wrapping the donor atlas across unrelated parts.
    """
    resource_path, geometry_path = inspection/'native-template.mesh', inspection/'native-template.geom'
    if not resource_path.is_file():
        resource_path, geometry_path = inspection/'45392.bin', inspection/'45539.bin'
    resource = bytearray(resource_path.read_bytes())
    if not geometry_path.is_file():
        raise ValueError('The validated native geometry template is absent')
    if struct.unpack_from('<4I', resource) != (192, 176, 384, 0):
        raise ValueError('Unsupported static mesh declaration')
    lod = struct.unpack_from('<Q', resource, 48)[0] + 16
    table = struct.unpack_from('<Q', resource, lod+12)[0] + 16
    if struct.unpack_from('<I', resource, lod+8)[0] != 6:
        raise ValueError('Expected five visible materials and one depth section')
    cross = lambda a, b: (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
    def normal(points):
        a, b, c = points
        n = cross([b[i]-a[i] for i in range(3)], [c[i]-a[i] for i in range(3)])
        length = math.sqrt(sum(x*x for x in n))
        return tuple(x/length for x in n) if length > 1e-10 else (0, 1, 0)
    # Serialized usage0x21/format0x06 = TexCoord0/Half2; usage0x34/
    # format0x0d = TangentSpace/UByte4N. Preserve the supported declaration.
    section = table + 2*384
    if resource[section+116:section+124] != bytes((33, 6, 0, 1, 52, 13, 0, 2)):
        raise ValueError('The supported UV/tangent declaration differs')
    surfaces = ['rubber' if any(a <= i < b for a, b in rubber_ranges) else 'metal'
                for i in range(len(rows))]
    face_normals = [normal([row[i:i+3] for i in (0, 3, 6)]) for row in rows]
    # Source wheel vertices are split for triangles. Weld by position only inside
    # one surface kind; retain hard edges instead of averaging through the hub.
    smooth = name in ('rear_wheel', 'front_wheel')
    adjacency = {}
    if smooth:
        for face, row in enumerate(rows):
            for at in (0, 3, 6):
                key = (surfaces[face],) + tuple(round(v, 6) for v in row[at:at+3])
                adjacency.setdefault(key, []).append(face)
    def corner_normal(face, point):
        base = face_normals[face]
        if not smooth:
            return base
        key = (surfaces[face],) + tuple(round(v, 6) for v in point)
        neighbours = [face_normals[i] for i in adjacency[key]
                      if sum(a*b for a, b in zip(base, face_normals[i])) >= math.cos(math.radians(50))]
        averaged = tuple(sum(v[a] for v in neighbours) for a in range(3))
        size = math.sqrt(sum(v*v for v in averaged))
        return tuple(v / size for v in averaged) if size > 1e-10 else base
    # Both sides are explicit while validating the cross-engine winding contract.
    faces = [r for row in rows for r in (row, row[0:3]+row[6:9]+row[3:6])]
    count = len(faces)*3
    if count > 65535:
        raise ValueError('Native 16-bit index bound exceeded')
    positions, tangents, uv = bytearray(), bytearray(), bytearray()
    cache, normal_dots, tangent_dots, basis_errors = {}, [], [], []
    for face, row in enumerate(rows):
        points = [row[i:i+3] for i in (0, 3, 6)]
        normals = [corner_normal(face, p) for p in points]
        edge = tuple(points[1][i]-points[0][i] for i in range(3))
        if vector_dot(edge, edge) < 1e-12:
            edge = tuple(points[2][i]-points[0][i] for i in range(3))
        for order, sign in (((0, 1, 2), 1), ((0, 2, 1), -1)):
            for corner in order:
                p = points[corner]
                n = tuple(sign*v for v in normals[corner])
                desired = (0, -(p[2] - (-.525 if name == 'rear_wheel' else .525)), p[1]-.34) if smooth else edge
                if vector_dot(desired, desired) < 1e-12:
                    desired = (1, 0, 0)
                key = tuple(round(v, 6) for v in n + desired)
                if key not in cache:
                    cache[key] = native_axis_encode(n, desired)
                tangent = cache[key]
                decoded_t, decoded_b, decoded = native_axis_basis(tangent)
                normal_dots.append(vector_dot(decoded, n))
                projected = tuple(desired[i] - vector_dot(desired, decoded)*decoded[i] for i in range(3))
                if vector_dot(projected, projected) > 1e-12:
                    tangent_dots.append(vector_dot(decoded_t, vector_unit(projected)))
                basis_errors.append(max(abs(vector_dot(decoded_t, decoded_b)), abs(vector_dot(decoded_t, decoded)), abs(vector_dot(decoded_b, decoded)), abs(vector_dot(vector_cross(decoded_t, decoded_b), decoded)-1)))
                positions += struct.pack('<3f', *p)
                tangents += tangent
                uv += struct.pack('<2e', *MATERIAL_UVS[surfaces[face]])
    geometry = positions + uv + tangents + positions
    indices = struct.pack('<'+'H'*count, *range(count))
    geometry += indices + indices
    lo = [min(row[a] for row in rows for a in range(axis, 9, 3)) for axis in range(3)]
    hi = [max(row[a] for row in rows for a in range(axis, 9, 3)) for axis in range(3)]
    struct.pack_into('<3f', resource, 16, *lo)
    struct.pack_into('<3f', resource, 32, *hi)
    struct.pack_into('<II', resource, lod+88, count*4, count*32)
    chunk = hashlib.sha1(('ReSkateBMXNative20261017/'+name+'/chunk').encode()).digest()[:16]
    resource[lod+116:lod+132] = chunk
    for part in range(6):
        at = table + part*384
        is_visible, is_depth = part == 2, part == 5
        active = is_visible or is_depth
        start = count*20 if is_depth else 0
        struct.pack_into('<4I', resource, at+32, len(faces) if active else 0,
                         count if is_depth else 0, start, count if active else 0)
        struct.pack_into('<I', resource, at+84, start)
    # Keep empty material slots out of every native draw category. In the
    # ray-tracing path zero section sizes mean "use the full buffer", not
    # "empty"; retaining the donor's five surface indices creates invalid
    # acceleration-structure inputs for the four unused slots.
    for category in range(5):
        count_at = lod + 20 + category*12
        old_count, index_offset = struct.unpack_from('<IQ', resource, count_at)
        index_at = index_offset + 16
        if index_at + old_count > len(resource):
            raise ValueError('Native draw category exceeds mesh resource')
        selected = [index for index in resource[index_at:index_at+old_count] if index in (2, 5)]
        if any(index >= 6 for index in resource[index_at:index_at+old_count]):
            raise ValueError('Native draw category has an unknown section')
        struct.pack_into('<I', resource, count_at, len(selected))
        resource[index_at:index_at+len(selected)] = bytes(selected)
    (output/f'{name}.mesh').write_bytes(resource)
    (output/f'{name}.geom').write_bytes(geometry)
    return dict(material_uvs=MATERIAL_UVS, rubber_triangles=surfaces.count('rubber'),
                native_basis_encoding='compact_axis_angle', smoothed_wheel_normals=smooth,
                decoded_normal_dot_min=min(normal_dots),
                decoded_normal_dot_mean=sum(normal_dots) / len(normal_dots),
                normal_error_degrees_max=math.degrees(math.acos(max(-1, min(1, min(normal_dots))))),
                tangent_error_degrees_max=math.degrees(math.acos(max(-1, min(1, min(tangent_dots))))),
                orthonormal_handedness_error_max=max(basis_errors))


def prepare(source, output, inspection=None):
    data = source.read_bytes()
    magic, version, stride, count = struct.unpack_from('<8sIII', data)
    if (magic, version, stride) != (b'RSBMX001', 1, 40) or len(data) != 44 + count * 40:
        raise ValueError('Unsupported private geometry')
    parts = [[] for _ in range(6)]
    for index in range(count):
        part, *values = struct.unpack_from('<I9f', data, 44 + index * 40)
        if part >= len(parts) or any(not math.isfinite(x) or abs(x) > 10 for x in values):
            raise ValueError('Invalid bike triangle')
        parts[part].append(tuple(values))

    # The reference assembler bbox-centred the fork independently of the frame.
    # Recover its source coordinates, then fit the exported Forks_Anchor on the
    # frame and FrontWheel_Anchor on the fork to our unchanged contact layout.
    # A rigid rotation plus uniform scale keeps the detailed blades/steerer intact.
    source_mid = (0, (-.3336294 + .115703486) / 2, (-.0173986424 + .054687351) / 2)
    source_scale = .44 / (.115703486 + .3336294)
    frame_pitch = -.2837
    fork_origin = (0, .76 - .05726322531700134 * 1.144 * math.cos(frame_pitch),
                   .36 - .05726322531700134 * 1.144 * math.sin(frame_pitch))
    wheel_anchor = (0, -.3168274462223053, .037730444222688675)
    target = (0, .34 - fork_origin[1], .525 - fork_origin[2])
    fork_scale = math.hypot(target[1], target[2]) / math.hypot(wheel_anchor[1], wheel_anchor[2])
    fork_pitch = math.atan2(target[2], target[1]) - math.atan2(wheel_anchor[2], wheel_anchor[1])
    connected_fork = []
    for row in parts[1]:
        if min(row[1::3]) > .80:
            connected_fork.append(row)  # The separate detailed stem already meets the bars.
            continue
        if max(row[1::3]) >= .80:
            raise ValueError('Fork/stem geometry no longer matches the private reference assembly')
        points = []
        for at in (0, 3, 6):
            x, y, z = ((row[at + axis] - (0, .55, .45)[axis]) / source_scale + source_mid[axis]
                       for axis in range(3))
            points.extend((x * fork_scale,
                           fork_origin[1] + (y * math.cos(fork_pitch) - z * math.sin(fork_pitch)) * fork_scale,
                           fork_origin[2] + (y * math.sin(fork_pitch) + z * math.cos(fork_pitch)) * fork_scale))
        connected_fork.append(tuple(points))
    parts[1] = connected_fork
    # The source contains only one crank arm. Replace it with one symmetric,
    # articulated assembly, rather than overlaying a second crank on that arm.
    parts[4] = []
    rubber_ranges = {2: [(0, len(parts[2]))], 3: [(0, len(parts[3]))]}

    def tube(part, start, end, radius, sides=10):
        axis = [end[i] - start[i] for i in range(3)]
        size = math.sqrt(sum(x*x for x in axis))
        axis = [x / size for x in axis]
        ref = (0, 1, 0) if abs(axis[1]) < .9 else (1, 0, 0)
        cross = lambda a, b: (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
        u = cross(axis, ref)
        length = math.sqrt(sum(x*x for x in u))
        u = [x/length for x in u]
        v = cross(axis, u)
        for i in range(sides):
            def ring(n, center):
                angle = n * math.tau / sides
                return tuple(center[j] + radius*(math.cos(angle)*u[j] + math.sin(angle)*v[j]) for j in range(3))
            a, b, c, d = ring(i, start), ring(i, end), ring(i+1, end), ring(i+1, start)
            parts[part].extend((a+b+c, a+c+d, tuple(start)+d+a, tuple(end)+b+c))

    def box(part, lo, hi):
        points = [(x, y, z) for z in (lo[2], hi[2]) for y in (lo[1], hi[1]) for x in (lo[0], hi[0])]
        for a, b, c, d in ((0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4),
                           (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)):
            parts[part].extend((points[a] + points[b] + points[c], points[a] + points[c] + points[d]))

    # The extracted modular reference has tires but no rims/spokes, pedals or saddle.
    for part, z in ((2, -.525), (3, .525)):
        for i in range(32):
            a, b = i*math.tau/32, (i+1)*math.tau/32
            tube(part, (0, .34+math.cos(a)*.275, z+math.sin(a)*.275),
                 (0, .34+math.cos(b)*.275, z+math.sin(b)*.275), .009, 6)
            if i % 2 == 0:
                tube(part, (.015 if i%4 else -.015, .34, z),
                     (0, .34+math.cos(a)*.275, z+math.sin(a)*.275), .0025, 4)
        tube(part, (-.08, .34, z), (.08, .34, z), .025, 10)
    tube(0, (0, .55, -.22), (0, .77, -.22), .016)
    # A tapered, flattened saddle around the existing seat/post contact. The
    # broader rear and narrow nose replace the previous round cylindrical seat.
    saddle_begin = len(parts[0])
    saddle = ((-.35, .035, .785, .018), (-.32, .066, .787, .025),
              (-.25, .072, .787, .025), (-.18, .043, .778, .021),
              (-.11, .020, .775, .017), (-.09, .009, .775, .012))
    rings = [[(math.cos(i * math.tau / 12) * width,
               height + math.sin(i * math.tau / 12) * thickness, z)
              for i in range(12)] for z, width, height, thickness in saddle]
    for a, b in zip(rings, rings[1:]):
        for i in range(12):
            j = (i + 1) % 12
            parts[0].extend((a[i] + b[i] + b[j], a[i] + b[j] + a[j]))
    for ring, section in ((rings[0], saddle[0]), (rings[-1], saddle[-1])):
        center = (0, section[2], section[0])
        for i in range(12):
            parts[0].append(center + ring[i] + ring[(i + 1) % 12])
    rubber_ranges[0] = [(saddle_begin, len(parts[0]))]
    # +X is the native rider's left. At phase0 left pedal is back, right forward.
    # Frame BB_Anchor transforms to (.4024733,-.1432242) in Y/Z. Match the
    # rounded bmx_world_transform.h pivot to within .6mm, including rider soles.
    crank_y, crank_z = .403, -.143
    tube(4, (-.10, crank_y, crank_z), (.10, crank_y, crank_z), .023)
    for side in (-1, 1):
        z = crank_z - side*.165
        tube(4, (side*.10, crank_y, crank_z), (side*.10, crank_y, z), .014)
        # Platform top is the rider's pedal sole contact at the bracket height.
        # Open rectangular platforms read as pedals, rather than extra crank rods.
        x = side * .12
        for edge in (-1, 1):
            box(4, (x-.05, crank_y-.012, z+edge*.035-.004), (x+.05, crank_y, z+edge*.035+.004))
            box(4, (x+edge*.046-.004, crank_y-.012, z-.035), (x+edge*.046+.004, crank_y, z+.035))
        tube(4, (side*.10, crank_y-.006, z), (side*.15, crank_y-.006, z), .007, 8)
    grips_begin = len(parts[5])
    anchors = grip_anchors()
    grip_attachment = validate_grip_attachment(parts[5], anchors)
    for outer, inward in anchors.values():
        inner = tuple(outer[i] + inward[i] * .10 for i in range(3))
        tube(5, outer, inner, .021)
    rubber_ranges[5] = [(grips_begin, len(parts[5]))]

    output.mkdir(parents=True, exist_ok=True)
    names = ('frame', 'fork', 'rear_wheel', 'front_wheel', 'cranks', 'handlebars')
    report = []
    for part_index, (name, rows) in enumerate(zip(names, parts)):
        # The first native pack uses a validated 16-bit index template.
        if not rows or len(rows)*3 > 65535:
            raise ValueError(f'{name} exceeds native template vertex bound')
        payload = struct.pack('<I', len(rows)) + b''.join(struct.pack('<9f', *row) for row in rows)
        (output/f'{name}.tri').write_bytes(payload)
        appearance = native_mesh(name, rows, inspection, output, rubber_ranges.get(part_index, ())) if inspection else None
        lo = [min(row[a] for row in rows for a in range(axis, 9, 3)) for axis in range(3)]
        hi = [max(row[a] for row in rows for a in range(axis, 9, 3)) for axis in range(3)]
        report.append(dict(part=name, triangles=len(rows), bounds=[lo, hi], appearance=appearance,
                           grip_attachment=grip_attachment if part_index == 5 else None))
    (output/'geometry.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, default=Path(__file__).parent/'reference/bike_geometry.bin')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--inspection', type=Path)
    args = parser.parse_args()
    prepare(args.source, args.output, args.inspection)
