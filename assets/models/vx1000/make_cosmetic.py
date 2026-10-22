"""Builds the camcorder as a hand-wear cosmetic mesh (assets/models/vx1000/CREDITS.md) for ReSkate Studio's Import mesh, from the
donor glove export (its skeleton and material) and vx1000.glb (the camcorder, grip at the origin,
Y up, lens down -Z). The camcorder is placed at RightHand_Attach in the bind pose, its handle bar
through the fist, and weighted wholly to RightHand."""
import json, struct, sys, io
import numpy as np
from PIL import Image

def read(path):
    d = open(path, 'rb').read()
    jl = struct.unpack_from('<I', d, 12)[0]
    j = json.loads(d[20:20 + jl])
    b = d[20 + jl + 8:]
    return j, bytearray(b)

def acc(j, b, i):
    a = j['accessors'][i]; v = j['bufferViews'][a['bufferView']]
    n = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}[a['type']]
    ct = {5126: 'f', 5123: 'H', 5125: 'I', 5121: 'B'}[a['componentType']]
    off = v.get('byteOffset', 0) + a.get('byteOffset', 0)
    st = v.get('byteStride', 0); sz = struct.calcsize(ct) * n
    if st and st != sz:
        return np.array([struct.unpack_from('<' + ct * n, b, off + i * st) for i in range(a['count'])])
    return np.array(struct.unpack_from('<' + ct * (n * a['count']), b, off)).reshape(-1, n)

def local(n):
    if 'matrix' in n: return np.array(n['matrix'], float).reshape(4, 4).T
    M = np.eye(4); t = n.get('translation', [0, 0, 0]); x, y, z, w = n.get('rotation', [0, 0, 0, 1]); s = n.get('scale', [1, 1, 1])
    R = np.array([[1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)], [2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)], [2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)]])
    M[:3, :3] = R * np.array(s); M[:3, 3] = t; return M

def unit(v): return v / np.linalg.norm(v)

def main(donor, camera, out, hand='Right'):
    j, b = read(donor)
    cj, cb = read(camera)
    nodes = j['nodes']; parent = {}
    for i, n in enumerate(nodes):
        for c in n.get('children', []): parent[c] = i
    cache = {}
    def glob(i):
        if i not in cache:
            M = local(nodes[i])
            if i in parent: M = glob(parent[i]) @ M
            cache[i] = M
        return cache[i]
    skin = j['skins'][0]; joints = skin['joints']; names = [nodes[k]['name'] for k in joints]
    G = lambda nm: glob(joints[names.index(nm)])
    h = G(f'{hand}Hand')[:3, 3]
    a = unit(G(f'{hand}HandMiddle1')[:3, 3] - h)               # wrist to fingers
    s = G(f'{hand}HandIndex1')[:3, 3] - G(f'{hand}HandPinky1')[:3, 3]  # pinky to index
    up = -a
    forward = unit(s - up * np.dot(s, up))
    back = -forward
    right = np.cross(up, back)
    Rc = np.column_stack([right, up, back])
    grip = G(f'{hand}Hand_Attach')[:3, 3]
    # The camcorder's triangles, by material, merged into one primitive.
    pos, nor, idx = [], [], []
    for prim in cj['meshes'][0]['primitives']:
        P = acc(cj, cb, prim['attributes']['POSITION']); N = acc(cj, cb, prim['attributes']['NORMAL']); I = acc(cj, cb, prim['indices']).ravel()
        base = sum(len(x) for x in pos)
        pos.append(P @ Rc.T + grip); nor.append(N @ Rc.T); idx.append(I + base)
    P = np.vstack(pos).astype('<f4'); N = np.vstack(nor); N = (N / np.linalg.norm(N, axis=1, keepdims=True)).astype('<f4'); I = np.concatenate(idx).astype('<u4')
    # One point of the donor's textures for every vertex: inside a large colourable island, with a flat normal.
    mat = j['materials'][0]
    def image(k):
        im = j['images'][j['textures'][k]['source']]; v = j['bufferViews'][im['bufferView']]
        return np.array(Image.open(io.BytesIO(bytes(b[v.get('byteOffset', 0):v.get('byteOffset', 0) + v['byteLength']]))).convert('RGBA')).astype(int)
    app = image(mat['pbrMetallicRoughness']['baseColorTexture']['index']); nrm = image(mat['normalTexture']['index'])
    H, W = app.shape[:2]; best = None; r = 6
    # A colourable mask (glove) is one bright colour; a real colour texture (glasses) is as dark as the
    # item is: the most even patch that is coloured, its normal facing straight out.
    for y in range(r, H - r):
        for x in range(r, W - r):
            patch = app[y-r:y+r+1, x-r:x+r+1, :3]
            if (app[y-r:y+r+1, x-r:x+r+1, 3] < 255).any() or app[y, x, :3].sum() < 60: continue
            spread = int(np.abs(patch - app[y, x, :3]).max()); tilt = int(np.abs(nrm[y-r:y+r+1, x-r:x+r+1, :2] - 128).max())
            score = spread + tilt
            if best is None or score < best[0]: best = (score, x, y)
    best = best[1:]
    uv = np.array([(best[0] + 0.5) / W, (best[1] + 0.5) / H], '<f4')
    UV = np.tile(uv, (len(P), 1)).astype('<f4')
    ji = names.index(f'{hand}Hand')
    rows = max(len(P), max((j['accessors'][q['attributes']['POSITION']]['count'] for m in j['meshes'] for q in m['primitives']), default=0))
    J0 = np.tile(np.array([ji, 0, 0, 0], '<u2'), (rows, 1)); W0 = np.tile(np.array([1, 0, 0, 0], '<f4'), (rows, 1))
    J1 = np.zeros((rows, 4), '<u2'); W1 = np.zeros((rows, 4), '<f4')
    # Append the new arrays to the donor's buffer and point the primitive at them.
    def add(arr, kind, typ, ctype, target, minmax=False):
        while len(b) % 4: b.append(0)
        raw = arr.tobytes(); j['bufferViews'].append({'buffer': 0, 'byteOffset': len(b), 'byteLength': len(raw), 'target': target}); b.extend(raw)
        accessor = {'bufferView': len(j['bufferViews']) - 1, 'componentType': ctype, 'count': len(arr), 'type': typ}
        if minmax: accessor['min'] = arr.min(0).tolist(); accessor['max'] = arr.max(0).tolist()
        j['accessors'].append(accessor); return len(j['accessors']) - 1
    prim = j['meshes'][0]['primitives'][0]
    prim['attributes'] = {
        'POSITION': add(P, 0, 'VEC3', 5126, 34962, True), 'NORMAL': add(N, 0, 'VEC3', 5126, 34962),
        'TEXCOORD_0': add(UV, 0, 'VEC2', 5126, 34962), 'TEXCOORD_1': add(UV, 0, 'VEC2', 5126, 34962), 'TEXCOORD_2': add(UV, 0, 'VEC2', 5126, 34962),
        'JOINTS_0': add(J0[:len(P)], 0, 'VEC4', 5123, 34962), 'WEIGHTS_0': add(W0[:len(P)], 0, 'VEC4', 5126, 34962),
        'JOINTS_1': add(J1[:len(P)], 0, 'VEC4', 5123, 34962), 'WEIGHTS_1': add(W1[:len(P)], 0, 'VEC4', 5126, 34962)}
    prim['indices'] = add(I, 0, 'SCALAR', 5125, 34963)
    # The donor's other sections (the glasses' lenses): folded into the grip, out of sight.
    for mesh in j['meshes'][1:]:
        for other in mesh['primitives']:
            count = j['accessors'][other['attributes']['POSITION']]['count']
            folded = np.tile(grip.astype('<f4'), (count, 1)).astype('<f4')
            other['attributes']['POSITION'] = add(folded, 0, 'VEC3', 5126, 34962, True)
            for k in list(other['attributes']):
                if k.startswith('JOINTS_'): other['attributes'][k] = add((J0 if k == 'JOINTS_0' else J1)[:count], 0, 'VEC4', 5123, 34962)
                elif k.startswith('WEIGHTS_'): other['attributes'][k] = add((W0 if k == 'WEIGHTS_0' else W1)[:count], 0, 'VEC4', 5126, 34962)
    for n in nodes:
        if 'mesh' in n: n.pop('extras', None)
    j['asset']['copyright'] = 'Camcorder Video Camera by James.Hasegawa (CC BY 4.0), modified for ReSkate'
    while len(b) % 4: b.append(0)
    j['buffers'][0]['byteLength'] = len(b)
    text = json.dumps(j, separators=(',', ':')).encode()
    while len(text) % 4: text += b' '
    with open(out, 'wb') as f:
        f.write(struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(text) + 8 + len(b)))
        f.write(struct.pack('<II', len(text), 0x4E4F534A) + text)
        f.write(struct.pack('<II', len(b), 0x004E4942) + bytes(b))
    # The camera in the hand bone's own frame, for the Filmer's arm (filmer_hold.h).
    Rh = G(f'{hand}Hand')[:3, :3]
    Rch = Rh.T @ Rc; pch = Rh.T @ (grip - h)
    w = np.sqrt(max(0, 1 + Rch[0,0] + Rch[1,1] + Rch[2,2])) / 2
    q = [(Rch[2,1]-Rch[1,2])/(4*w), (Rch[0,2]-Rch[2,0])/(4*w), (Rch[1,0]-Rch[0,1])/(4*w), w]
    print(f'colour at texel {best}: {app[best[1], best[0], :3]}')
    print(f'{out}: {len(P)} vertices, {len(I)//3} triangles, uv {uv}, weighted to {hand}Hand ({ji})')
    print('camera in hand: rotation (x,y,z,w)', np.round(q, 4), 'grip from wrist', np.round(pch, 4))

if __name__ == '__main__':
    main(*sys.argv[1:])
