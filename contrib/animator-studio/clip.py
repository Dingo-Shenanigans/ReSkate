"""A trick clip as the game's exporter writes it (reskate_cli animation-export -> binary FBX): its skeleton, its
animation curves, forward kinematics, and edits written back into the same FBX so animation-import takes it.

    c = Clip(path)
    c.times                      # seconds of every frame (60 fps)
    c.world(frame)               # {bone: 4x4 world matrix}
    c.add_rotation(bone, axis, degrees_per_frame_array)   # in the bone's local frame, on top of what is there
    c.save(path)

Rotations in the FBX are Euler XYZ in degrees (no pre/post rotation in these files), translations in metres.
"""
import numpy as np

import fbxbin

TICKS = 46186158000          # FBX time units per second


def euler_xyz(rx, ry, rz):
    """FBX eEulerXYZ: rotate about X, then Y, then Z (matrix Rz @ Ry @ Rx), degrees."""
    x, y, z = np.radians([rx, ry, rz])
    cx, sx, cy, sy, cz, sz = np.cos(x), np.sin(x), np.cos(y), np.sin(y), np.cos(z), np.sin(z)
    Rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    Ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    Rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    return Rz @ Ry @ Rx


def to_euler_xyz(R):
    """Back from a rotation matrix to FBX XYZ Euler degrees."""
    sy = -R[2, 0]
    y = np.arcsin(np.clip(sy, -1, 1))
    if abs(sy) < 0.99999:
        x = np.arctan2(R[2, 1], R[2, 2])
        z = np.arctan2(R[1, 0], R[0, 0])
    else:
        x = np.arctan2(-R[1, 2], R[1, 1])
        z = 0.0
    return np.degrees([x, y, z])


def axis_angle(axis, deg):
    a = np.asarray(axis, float) / np.linalg.norm(axis)
    t = np.radians(deg)
    K = np.array([[0, -a[2], a[1]], [a[2], 0, -a[0]], [-a[1], a[0], 0]])
    return np.eye(3) + np.sin(t) * K + (1 - np.cos(t)) * K @ K


class Clip:
    def __init__(self, path):
        self.fbx = fbxbin.read(path)
        objs = self.fbx.find("Objects")
        conns = self.fbx.find("Connections").children
        self.models = {}            # id -> name
        self.node = {}              # name -> Model node
        for c in objs.children:
            if c.name == "Model":
                name = c.props[1].split(b"\x00")[0].decode()
                self.models[c.props[0]] = name
                self.node[name] = c
        self.parent = {}
        for c in conns:
            kind, child, par = c.props[0], c.props[1], c.props[2]
            if kind == b"OO" and child in self.models and par in self.models:
                self.parent[self.models[child]] = self.models[par]
        self.defaults = {}
        for name, n in self.node.items():
            props = {p.props[0]: p.props[4:] for p in n.find("Properties70").children}
            self.defaults[name] = {"T": np.array(props.get(b"Lcl Translation", (0, 0, 0)), float),
                                   "R": np.array(props.get(b"Lcl Rotation", (0, 0, 0)), float),
                                   "S": np.array(props.get(b"Lcl Scaling", (1, 1, 1)), float)}
        # curves: model -> channel ('T'|'R'|'S') -> axis -> AnimationCurve node
        curve_nodes = {c.props[0]: c for c in objs.children if c.name == "AnimationCurveNode"}
        curves = {c.props[0]: c for c in objs.children if c.name == "AnimationCurve"}
        cn_owner = {}
        for c in conns:
            if c.props[0] == b"OP" and c.props[1] in curve_nodes and c.props[2] in self.models:
                ch = {b"Lcl Translation": "T", b"Lcl Rotation": "R", b"Lcl Scaling": "S"}.get(c.props[3])
                if ch:
                    cn_owner[c.props[1]] = (self.models[c.props[2]], ch)
        self.curves = {}
        for c in conns:
            if c.props[0] == b"OP" and c.props[1] in curves and c.props[2] in cn_owner:
                name, ch = cn_owner[c.props[2]]
                axis = {b"d|X": 0, b"d|Y": 1, b"d|Z": 2}[c.props[3]]
                self.curves.setdefault(name, {}).setdefault(ch, {})[axis] = curves[c.props[1]]
        stack = objs.find("AnimationStack")
        p70 = {p.props[0]: p.props[4:] for p in stack.find("Properties70").children}
        self.start = p70[b"LocalStart"][0] / TICKS
        self.stop = p70[b"LocalStop"][0] / TICKS
        n = int(round((self.stop - self.start) * 60)) + 1
        self.times = self.start + np.arange(n) / 60.0
        self.order = self._order()

    def _order(self):
        done, out = set(), []

        def visit(n):
            if n in done:
                return
            p = self.parent.get(n)
            if p:
                visit(p)
            done.add(n)
            out.append(n)
        for n in self.node:
            visit(n)
        return out

    @staticmethod
    def _keys(curve):
        t = curve.find("KeyTime").props[0].astype(np.float64) / TICKS
        v = curve.find("KeyValueFloat").props[0].astype(np.float64)
        return t, v

    def channel(self, bone, ch):
        """(frames, 3) values of a channel of a bone at every frame."""
        out = np.tile(self.defaults[bone][ch], (len(self.times), 1))
        for axis, curve in self.curves.get(bone, {}).get(ch, {}).items():
            t, v = self._keys(curve)
            out[:, axis] = np.interp(self.times, t, v) if len(t) > 1 else v[0]
        return out

    def local(self, bone, frame):
        d = self.defaults[bone]
        T = self.channel_at(bone, "T", frame)
        R = self.channel_at(bone, "R", frame)
        S = self.channel_at(bone, "S", frame)
        M = np.eye(4)
        M[:3, :3] = euler_xyz(*R) @ np.diag(S)
        M[:3, 3] = T
        return M

    def channel_at(self, bone, ch, frame):
        cache = self.__dict__.setdefault("_cache", {})
        key = (bone, ch)
        if key not in cache:
            cache[key] = self.channel(bone, ch)
        return cache[key][frame]

    def world(self, frame):
        W = {}
        for b in self.order:
            L = self.local(b, frame)
            p = self.parent.get(b)
            W[b] = W[p] @ L if p else L
        return W

    def set_channel(self, bone, ch, values):
        """Writes (frames, 3) values as dense keys, one a frame (keeps the curve's interpolation settings)."""
        ticks = np.round(self.times * TICKS).astype(np.int64)
        for axis in range(3):
            curve = self.curves.get(bone, {}).get(ch, {}).get(axis)
            if curve is None:
                raise KeyError(f"{bone} has no {ch} curve on axis {axis}")
            kt, kv = curve.find("KeyTime"), curve.find("KeyValueFloat")
            kt.props[0] = ticks.copy()
            kv.props[0] = values[:, axis].astype(np.float32)
            flags, data, refs = curve.find("KeyAttrFlags"), curve.find("KeyAttrDataFloat"), curve.find("KeyAttrRefCount")
            if len(flags.props[0]) == 1:
                refs.props[0] = np.array([len(ticks)], np.int32)
            else:                                   # one entry per key: keep the first key's settings for every key
                flags.props[0] = np.repeat(flags.props[0][:1], len(ticks))
                data.props[0] = np.tile(data.props[0][:4], len(ticks))
                refs.props[0] = np.ones(len(ticks), np.int32)
            curve.find("Default").props[0] = float(values[0, axis])
        self.__dict__.get("_cache", {}).pop((bone, ch), None)

    def add_rotation(self, bone, axis, degrees):
        """Turns a bone by degrees[frame] about `axis` (in its local frame, after its own rotation)."""
        R = self.channel(bone, "R")
        out = R.copy()
        for f in range(len(self.times)):
            if abs(degrees[f]) < 1e-6:
                continue
            M = euler_xyz(*R[f]) @ axis_angle(axis, degrees[f])
            e = to_euler_xyz(M)
            out[f] = e + 360 * np.round((R[f] - e) / 360)        # keep the curve continuous
        self.set_channel(bone, "R", out)

    def add_world_rotation(self, bone, axis_world, degrees, frame_world):
        """Turns a bone about a WORLD axis (as seen at each frame)."""
        R = self.channel(bone, "R")
        out = R.copy()
        for f in range(len(self.times)):
            if abs(degrees[f]) < 1e-6:
                continue
            Wp = frame_world(f).get(self.parent.get(bone), np.eye(4))[:3, :3]
            a_local = Wp.T @ np.asarray(axis_world, float)                 # the axis in the parent's frame
            M = axis_angle(a_local, degrees[f]) @ euler_xyz(*R[f])
            e = to_euler_xyz(M)
            out[f] = e + 360 * np.round((R[f] - e) / 360)
        self.set_channel(bone, "R", out)

    def prune(self, keep):
        """Drops every animation channel that is not in `keep` (names like "Hips.q", "Hips.t", "Hips.s" as
        animation-info lists the game's own clip), with its curves and connections, so the importer writes back exactly
        the channels the game had. Returns how many channels were dropped."""
        objs = self.fbx.find("Objects")
        conn = self.fbx.find("Connections")
        suffix = {"R": "q", "T": "t", "S": "s"}
        drop_ids = set()
        dropped = 0
        for bone, chs in list(self.curves.items()):
            for ch in list(chs):
                if f"{bone}.{suffix[ch]}" in keep:
                    continue
                for curve in chs[ch].values():
                    drop_ids.add(curve.props[0])
                del chs[ch]
                dropped += 1
            if not chs:
                del self.curves[bone]
        # the curve nodes whose curves are all gone
        curve_ids = {c.props[0] for c in objs.children if c.name == "AnimationCurve"} - drop_ids
        node_has_curve = set()
        for c in conn.children:
            if c.props[0] == b"OP" and c.props[1] in curve_ids:
                node_has_curve.add(c.props[2])
        for c in objs.children:
            if c.name == "AnimationCurveNode" and c.props[0] not in node_has_curve:
                drop_ids.add(c.props[0])
        objs.children = [c for c in objs.children if not (c.props and c.props[0] in drop_ids)]
        conn.children = [c for c in conn.children if c.props[1] not in drop_ids and c.props[2] not in drop_ids]
        return dropped

    def save(self, path):
        fbxbin.write(self.fbx, path)
