"""Build ignored local binary geometry from extracted BMX Streets meshes.

Part placement is a prototype assembly, not a recovered native prefab/rig.
Original hierarchy/material exports remain alongside the mesh references.
"""
from pathlib import Path
import json
import math
import struct

ROOT = Path(__file__).resolve().parent
REFERENCE = ROOT / "reference"


def obj_mesh(path):
    vertices, faces = [], []
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if fields and fields[0] == "v":
            vertices.append(tuple(float(v) for v in fields[1:4]))
        elif fields and fields[0] == "f":
            indices = [int(v.split("/")[0]) - 1 for v in fields[1:]]
            faces.extend((indices[0], indices[i], indices[i + 1]) for i in range(1, len(indices) - 1))
    return vertices, faces


def main():
    specs = [
        ("*frames*/Mesh*.obj", 0, (0, .53, -.05), 2, 1.08),
        ("*bars*/BMX_Bars*.obj", 5, (0, .97, .39), 0, .65),
        ("*forks*/BMX_Forks*.obj", 1, (0, .55, .45), 1, .44),
        ("*tires*/BMX_Tire*.obj", 2, (0, .34, -.525), 1, .68),
        ("*tires*/BMX_Tire*.obj", 3, (0, .34, .525), 1, .68),
        ("*stems*/BMX_Stem*.obj", 1, (0, .84, .36), 2, .09),
        ("*crankarms*/BMX_CrankArm*.obj", 4, (.065, .35, -.12), 2, .17),
        ("*pegs*/BMX_Peg*.obj", 0, (.12, .34, -.525), 0, .10),
        ("*pegs*/BMX_Peg*.obj", 0, (-.12, .34, -.525), 0, .10),
    ]
    rows, report = [], []
    for pattern, part, centre, axis, extent in specs:
        candidates = sorted(REFERENCE.glob(pattern))
        if not candidates:
            continue
        path = candidates[0]
        vertices, faces = obj_mesh(path)
        if "pegs" in pattern:
            vertices = [(p[2], p[1], p[0]) for p in vertices]
        minimum = [min(p[a] for p in vertices) for a in range(3)]
        maximum = [max(p[a] for p in vertices) for a in range(3)]
        middle = [(minimum[a] + maximum[a]) / 2 for a in range(3)]
        scale = extent / (maximum[axis] - minimum[axis])
        if "frames" in pattern:
            # Fit the extracted frame's actual RearWheel_Anchor and head-tube origin
            # to the prototype wheelbase/contact layout, preserving rigid geometry.
            pitch, scale = -.2837, 1.144
            vertices = [(p[0]*scale, (p[1]*math.cos(pitch)-p[2]*math.sin(pitch))*scale+.76,
                         (p[1]*math.sin(pitch)+p[2]*math.cos(pitch))*scale+.36) for p in vertices]
        else:
            vertices = [tuple((p[a] - middle[a]) * scale + centre[a] for a in range(3)) for p in vertices]
        for face in faces:
            rows.append((part, tuple(value for i in face for value in vertices[i])))
        report.append({"mesh": str(path.relative_to(REFERENCE)), "part": part, "vertices": len(vertices), "triangles": len(faces), "original_bounds": [minimum, maximum], "prototype_centre": centre, "uniform_scale": scale})
    if not rows:
        raise SystemExit("No extracted meshes found")
    minimum = [min(values[a] for _, values in rows for a in range(axis, 9, 3)) for axis in range(3)]
    maximum = [max(values[a] for _, values in rows for a in range(axis, 9, 3)) for axis in range(3)]
    bounds = minimum + maximum
    if any(not math.isfinite(value) or abs(value) > 10 for value in bounds):
        raise SystemExit("Invalid assembled geometry bounds")
    payload = struct.pack("<8sIII6f", b"RSBMX001", 1, 40, len(rows), *bounds)
    payload += b"".join(struct.pack("<I9f", part, *values) for part, values in rows)
    (REFERENCE / "bike_geometry.bin").write_bytes(payload)
    (REFERENCE / "assembly.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({"parts": len(report), "triangles": len(rows), "details": report}, indent=2))


if __name__ == "__main__":
    main()
