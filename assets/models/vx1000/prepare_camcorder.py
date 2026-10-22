"""Builds vx1000.glb, the Filmer's held camera (Extension/Filmer/filmer_hold.h), from
camcorder_video_camera.glb (CREDITS.md):

    python3 assets/models/vx1000/prepare_camcorder.py assets/models/vx1000/camcorder_video_camera.glb assets/models/vx1000/vx1000.glb

What it changes: the outline copy of the model (lines) is left out; the parts are merged by material
into one mesh; the model is scaled from its own units to 0.40 m long; it is turned so Y is up and the
lens looks down -Z; and its origin is moved to the middle of the top handle, where the hand grips it
(the game places the model at the filmer's palm). The near-black plastics are lifted a little so the
body reads as dark grey in game. Prints where the front of the lens ends up from the grip, which is
filmer_hold.h's lens_from_handle.
"""
import json
import struct
import sys

LENGTH = 0.40                 # metres, front of the lens hood to the back of the body
GRIP = (0.0, 0.0, 77.0)       # the handle bar's middle, in the source's units (x, y, z; z up)
LENS = (0.0, -92.95, 23.6)    # the middle of the lens hood's front, likewise
LIFT = {"CamcorderMainPlastic_mat": 0.03, "LensHood_Mat": 0.02, "Color_G07": 0.01}


def read_glb(path):
    data = open(path, "rb").read()
    json_length = struct.unpack_from("<I", data, 12)[0]
    doc = json.loads(data[20:20 + json_length])
    blob = data[20 + json_length + 8:]
    return doc, blob


def accessor(doc, blob, index):
    a = doc["accessors"][index]
    view = doc["bufferViews"][a["bufferView"]]
    width = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[a["type"]]
    kind = {5126: "f", 5125: "I", 5123: "H", 5121: "B"}[a["componentType"]]
    offset = view.get("byteOffset", 0) + a.get("byteOffset", 0)
    stride = view.get("byteStride", 0) or width * struct.calcsize(kind)
    out = []
    for i in range(a["count"]):
        values = struct.unpack_from("<" + kind * width, blob, offset + i * stride)
        out.append(values if width > 1 else values[0])
    return out


def main(source, target):
    doc, blob = read_glb(source)
    span = None
    # The source's raw mesh units: z up, the lens toward -y. Out: x' = -x, y' = z, z' = y (a turn, not
    # a mirror), less the grip, scaled.
    lows, highs = [1e9] * 3, [-1e9] * 3
    groups = {}  # material -> positions, normals, indices
    for mesh in doc["meshes"]:
        for prim in mesh["primitives"]:
            if prim.get("mode", 4) != 4:
                continue
            positions = accessor(doc, blob, prim["attributes"]["POSITION"])
            normals = accessor(doc, blob, prim["attributes"]["NORMAL"])
            indices = accessor(doc, blob, prim["indices"])
            for p in positions:
                for k in range(3):
                    lows[k] = min(lows[k], p[k])
                    highs[k] = max(highs[k], p[k])
            p_out, n_out, i_out = groups.setdefault(prim.get("material", 0), ([], [], []))
            base = len(p_out)
            p_out.extend(positions)
            n_out.extend(normals)
            i_out.extend(base + i for i in indices)
    span = highs[1] - lows[1]
    scale = LENGTH / span

    def place(p):
        x, y, z = p[0] - GRIP[0], p[1] - GRIP[1], p[2] - GRIP[2]
        return (-x * scale, z * scale, y * scale)

    def turn(n):
        return (-n[0], n[2], n[1])

    lens = place(LENS)
    materials = []
    out_doc = {
        "asset": {"version": "2.0", "generator": "prepare_camcorder.py",
                  "copyright": "Camcorder Video Camera by James.Hasegawa (CC BY 4.0), modified for ReSkate"},
        "scene": 0, "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "vx1000", "mesh": 0}],
        "meshes": [{"name": "vx1000", "primitives": []}],
        "materials": materials, "accessors": [], "bufferViews": [], "buffers": [],
    }
    data = bytearray()

    def add_view(raw, target_kind):
        while len(data) % 4:
            data.append(0)
        out_doc["bufferViews"].append({"buffer": 0, "byteOffset": len(data), "byteLength": len(raw), "target": target_kind})
        data.extend(raw)
        return len(out_doc["bufferViews"]) - 1

    for material_index, (positions, normals, indices) in sorted(groups.items()):
        source_material = doc["materials"][material_index]
        pbr = dict(source_material.get("pbrMetallicRoughness", {}))
        name = source_material.get("name", f"material{material_index}")
        if name in LIFT:
            pbr["baseColorFactor"] = [max(c, LIFT[name]) for c in pbr.get("baseColorFactor", [0, 0, 0])[:3]] + [1.0]
        materials.append({"name": name, "pbrMetallicRoughness": pbr, "doubleSided": True})
        placed = [place(p) for p in positions]
        turned = [turn(n) for n in normals]
        p_view = add_view(b"".join(struct.pack("<3f", *p) for p in placed), 34962)
        n_view = add_view(b"".join(struct.pack("<3f", *n) for n in turned), 34962)
        wide = len(placed) > 65535
        i_view = add_view(b"".join(struct.pack("<I" if wide else "<H", i) for i in indices), 34963)
        mins = [min(p[k] for p in placed) for k in range(3)]
        maxs = [max(p[k] for p in placed) for k in range(3)]
        accessors = out_doc["accessors"]
        accessors.append({"bufferView": p_view, "componentType": 5126, "count": len(placed), "type": "VEC3", "min": mins, "max": maxs})
        accessors.append({"bufferView": n_view, "componentType": 5126, "count": len(turned), "type": "VEC3"})
        accessors.append({"bufferView": i_view, "componentType": 5125 if wide else 5123, "count": len(indices), "type": "SCALAR"})
        out_doc["meshes"][0]["primitives"].append({
            "attributes": {"POSITION": len(accessors) - 3, "NORMAL": len(accessors) - 2},
            "indices": len(accessors) - 1, "material": len(materials) - 1})
    while len(data) % 4:
        data.append(0)
    out_doc["buffers"].append({"byteLength": len(data)})
    text = json.dumps(out_doc, separators=(",", ":")).encode()
    while len(text) % 4:
        text += b" "
    total = 12 + 8 + len(text) + 8 + len(data)
    with open(target, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(text), 0x4E4F534A) + text)
        f.write(struct.pack("<II", len(data), 0x004E4942) + bytes(data))
    triangles = sum(len(g[2]) for g in groups.values()) // 3
    print(f"{target}: {triangles} triangles, {len(materials)} materials, scale {scale:.6f}")
    print(f"lens front from the grip: ({lens[0]:.3f}, {lens[1]:.3f}, {lens[2]:.3f}) m")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
