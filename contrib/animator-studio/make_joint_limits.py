"""Measures how far each editable joint of the skater moves in the game's own animation and writes the
limits the Animator Studio keeps edits inside (Extension/AnimationStudio/skater_joint_limits.h).

Every native library clip except bails and wipeouts is read. For each joint, its rotation is taken
relative to its average pose and split into twist (about the bone, towards its main child) and swing (the
bone tipping away). The limit is the twist range and, in each of 24 directions, how far the bone swings,
both from the 0.5th to 99.5th percentile, with a margin - so a knee comes out a hinge (almost no swing
sideways or backwards) and a shoulder a wide cone, from the game's data rather than guesses.

Usage: python make_joint_limits.py <Tricks/library folder> <output header>
"""
import json, math, os, re, struct, sys
import numpy as np

JOINTS = [42, 43, 44, 45, 101, 102, 103, 275, 276, 277, 278, 46, 47, 48, 49, 341, 342, 343, 344, 8, 9, 10, 11]
SECTORS = 24
MARGIN = math.radians(8)
SKIP = re.compile(r'bail|wipeout|ragdoll|fall|getup|get_up|death|crash|slam', re.I)

HERE = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(HERE, '..', '..', 'Extension', 'AnimationStudio', 'animation_skeleton.h')).read()
body = src[src.index('animation_parents{') + 18:]
PARENT = [int(x) for x in re.findall(r'-?\d+', body[:body.index('}')])]


def read(path):
    b = open(path, 'rb').read()
    magic = b[:4]
    J, F, _, _ = struct.unpack_from('<IIff', b, 4)
    at = 20
    v3 = magic == b'FAS3'
    board = magic == b'FAS2'
    if v3:
        board = b[at] != 0
        at += 1
    at += 4 * F
    out = np.zeros((F, J, 10), np.float32)
    if v3:
        moves = np.frombuffer(b, np.uint8, J, at); at += J
        for j in range(J):
            if not moves[j]:
                out[:, j] = np.frombuffer(b, '<f4', 10, at); at += 40
        mv = [j for j in range(J) if moves[j]]
        per = len(mv) + (1 if board else 0)
        blk = np.frombuffer(b, '<f4', F * per * 10, at).reshape(F, per, 10)
        out[:, mv] = blk[:, :len(mv)]
    else:
        out[:] = np.frombuffer(b, '<f4', F * J * 10, at).reshape(F, J, 10)
    return out


def qmul(a, b):
    ax, ay, az, aw = np.moveaxis(a, -1, 0)
    bx, by, bz, bw = np.moveaxis(b, -1, 0)
    return np.stack([aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
                     aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz], -1)


def conj(q):
    return q * np.array([-1, -1, -1, 1], q.dtype)


def main(library, header):
    cat = json.load(open(os.path.join(library, 'catalog.json'), encoding='utf-8'))
    files = [c['file'] for c in cat if not SKIP.search(c.get('label', '') + ' ' + c.get('category', ''))]
    rot = {j: [] for j in JOINTS}
    pos = {j: [] for j in range(395)}
    used = 0
    for fn in files:
        try:
            clip = read(os.path.join(library, fn))
        except Exception:
            continue
        if clip.shape[1] != 395:
            continue
        clip = clip[::3]
        used += 1
        for j in JOINTS:
            rot[j].append(clip[:, j, 3:7].astype(np.float64))
        if used <= 200:
            for j in range(395):
                pos[j].append(clip[:, j, 7:10].mean(0))
    print(f'{used} clips read', file=sys.stderr)
    mean_pos = {j: np.mean(pos[j], 0) for j in range(395) if pos[j]}
    limits = []
    for j in JOINTS:
        q = np.concatenate(rot[j])
        q /= np.linalg.norm(q, axis=1, keepdims=True)
        # The average pose: quaternions put on one side, then the principal direction.
        q[q[:, 3] < 0] *= -1
        w, v = np.linalg.eigh(q.T @ q)
        ref = v[:, -1]
        if ref[3] < 0:
            ref = -ref
        # The bone: towards the child that is furthest away.
        kids = [c for c in range(395) if PARENT[c] == j and c in mean_pos]
        axis = None
        if kids:
            far = max(kids, key=lambda c: np.linalg.norm(mean_pos[c]))
            if np.linalg.norm(mean_pos[far]) > 1e-4:
                axis = mean_pos[far] / np.linalg.norm(mean_pos[far])
        if axis is None:
            axis = np.array([1.0, 0.0, 0.0])
        e1 = np.cross(axis, [0, 1, 0] if abs(axis[1]) < 0.9 else [1, 0, 0])
        e1 /= np.linalg.norm(e1)
        e2 = np.cross(axis, e1)
        d = qmul(conj(np.broadcast_to(ref, q.shape)), q)
        d[d[:, 3] < 0] *= -1
        p = d[:, :3] @ axis
        twist_angle = 2 * np.arctan2(p, d[:, 3])
        twist_angle = (twist_angle + np.pi) % (2 * np.pi) - np.pi
        t = np.concatenate([axis[None, :] * p[:, None], d[:, 3:4]], 1)
        t /= np.maximum(np.linalg.norm(t, axis=1, keepdims=True), 1e-12)
        swing = qmul(d, conj(t))
        swing[swing[:, 3] < 0] *= -1
        sv = swing[:, :3]
        n = np.linalg.norm(sv, axis=1)
        angle = 2 * np.arctan2(n, swing[:, 3])
        u, vv = sv @ e1, sv @ e2
        phi = np.arctan2(vv, u)
        sector = ((phi + np.pi) / (2 * np.pi) * SECTORS).astype(int) % SECTORS
        radius = np.zeros(SECTORS)
        for s in range(SECTORS):
            a = angle[(sector == s) & (n > 1e-6)]
            radius[s] = np.percentile(a, 99.5) if len(a) >= 30 else 0.0
        # A direction the game never goes in still gets a little room, and neighbours are evened out.
        smooth = np.maximum(radius, (np.roll(radius, 1) + np.roll(radius, -1)) * 0.25)
        swing_limit = np.minimum(smooth + MARGIN, math.pi)
        tmin, tmax = np.percentile(twist_angle, [0.5, 99.5])
        limits.append((j, ref, axis, e1, e2, max(tmin - MARGIN, -math.pi), min(tmax + MARGIN, math.pi), swing_limit))
        print(f'{j:4d} twist {math.degrees(tmin):7.1f} .. {math.degrees(tmax):6.1f}   swing deg ' +
              ' '.join(f'{math.degrees(r):3.0f}' for r in swing_limit), file=sys.stderr)
    f = lambda x: f'{x:.6f}f'
    out = ['#pragma once', '// Generated by contrib/animator-studio/make_joint_limits.py from the game\'s own library clips', '// (bails and wipeouts left out). Do not edit by hand: run the script again.',
           '#include <array>', '#include <cstdint>', '', 'namespace dingosdk::anim_studio {',
           'struct JointLimit {', '    std::uint16_t joint;', '    std::array<float, 4> reference; // the average pose (x, y, z, w)',
           '    std::array<float, 3> axis, across, up; // the bone, and two directions square to it',
           '    float twist_min, twist_max; // radians about the bone', f'    std::array<float, {SECTORS}> swing; // radians, per direction round the bone from -pi',
           '};', f'inline constexpr std::array<JointLimit, {len(limits)}> joint_limits{{{{']
    for j, ref, axis, e1, e2, tmin, tmax, sw in limits:
        out.append(f'    {{{j}, {{{", ".join(f(x) for x in ref)}}}, {{{", ".join(f(x) for x in axis)}}}, {{{", ".join(f(x) for x in e1)}}}, '
                   f'{{{", ".join(f(x) for x in e2)}}}, {f(tmin)}, {f(tmax)}, {{{", ".join(f(x) for x in sw)}}}}},')
    out += ['}};', '} // namespace dingosdk::anim_studio', '']
    open(header, 'w', newline='\n').write('\n'.join(out))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
