"""Reads and writes binary FBX (7.x) as a tree of nodes, so animation keys can be changed and the file written back
with every offset and the footer recomputed.

    tree = fbxbin.read(path)          # FbxFile(version, nodes)
    node.name, node.props, node.children
    fbxbin.write(tree, path)

Property values: ints, floats, bytes (S, R) and numpy arrays (f, d, l, i, b). Arrays are written uncompressed unless
they were compressed when read (then they are recompressed with zlib).
"""
import struct
import zlib

import numpy as np

MAGIC = b"Kaydara FBX Binary  \x00\x1a\x00"
FOOTER_ID = bytes([0xfa, 0xbc, 0xab, 0x09, 0xd0, 0xc8, 0xd4, 0x66, 0xb1, 0x76, 0xfb, 0x83, 0x1c, 0xf7, 0x26, 0x7e])
FOOTER_MAGIC = bytes([0xf8, 0x5a, 0x8c, 0x6a, 0xde, 0xf5, 0xd9, 0x7e, 0xec, 0xe9, 0x0c, 0xe3, 0x75, 0x8f, 0x29, 0x0b])
ARRAY_TYPES = {b"f": np.float32, b"d": np.float64, b"l": np.int64, b"i": np.int32, b"b": np.bool_}


class Node:
    __slots__ = ("name", "props", "types", "children", "compressed", "nested")

    def __init__(self, name, props=None, types=None, children=None):
        self.name = name
        self.props = props or []
        self.types = types or []
        self.children = children or []
        self.compressed = []
        self.nested = None          # whether the node closes a nested list (read from the file; None = decide)

    def find(self, name):
        for c in self.children:
            if c.name == name:
                return c
        return None

    def find_all(self, name):
        return [c for c in self.children if c.name == name]

    def __repr__(self):
        return f"Node({self.name!r}, {len(self.props)} props, {len(self.children)} children)"


class FbxFile:
    def __init__(self, version, nodes):
        self.version = version
        self.nodes = nodes

    def find(self, name):
        for n in self.nodes:
            if n.name == name:
                return n
        return None


def _read_props(data, pos, count):
    props, types, comp = [], [], []
    for _ in range(count):
        t = data[pos:pos + 1]
        pos += 1
        c = False
        if t == b"Y":
            v = struct.unpack_from("<h", data, pos)[0]; pos += 2
        elif t == b"C":
            v = bool(data[pos]); pos += 1
        elif t == b"I":
            v = struct.unpack_from("<i", data, pos)[0]; pos += 4
        elif t == b"F":
            v = struct.unpack_from("<f", data, pos)[0]; pos += 4
        elif t == b"D":
            v = struct.unpack_from("<d", data, pos)[0]; pos += 8
        elif t == b"L":
            v = struct.unpack_from("<q", data, pos)[0]; pos += 8
        elif t in ARRAY_TYPES:
            n, enc, clen = struct.unpack_from("<III", data, pos)
            pos += 12
            raw = data[pos:pos + clen]
            pos += clen
            if enc == 1:
                raw = zlib.decompress(raw)
                c = True
            v = np.frombuffer(raw, dtype=ARRAY_TYPES[t], count=n).copy()
        elif t in (b"S", b"R"):
            n = struct.unpack_from("<I", data, pos)[0]
            pos += 4
            v = bytes(data[pos:pos + n])
            pos += n
        else:
            raise ValueError(f"unknown property type {t!r} at {pos}")
        props.append(v)
        types.append(t)
        comp.append(c)
    return props, types, comp, pos


def _read_node(data, pos, wide):
    if wide:
        end, nprops, plen = struct.unpack_from("<QQQ", data, pos)
        pos += 24
    else:
        end, nprops, plen = struct.unpack_from("<III", data, pos)
        pos += 12
    nlen = data[pos]
    pos += 1
    if end == 0 and nprops == 0 and plen == 0 and nlen == 0:
        return None, pos
    name = data[pos:pos + nlen].decode("ascii", "replace")
    pos += nlen
    props, types, comp, ppos = _read_props(data, pos, nprops)
    pos += plen
    node = Node(name, props, types)
    node.compressed = comp
    node.nested = pos < end
    null = 25 if wide else 13
    while pos < end:
        if end - pos == null and not any(data[pos:end]):
            pos = end
            break
        child, pos = _read_node(data, pos, wide)
        if child is None:
            break
        node.children.append(child)
    return node, end


def read(path):
    data = open(path, "rb").read()
    if not data.startswith(MAGIC):
        raise ValueError("not a binary FBX")
    version = struct.unpack_from("<I", data, 23)[0]
    wide = version >= 7500
    pos = 27
    nodes = []
    while pos < len(data):
        node, pos = _read_node(data, pos, wide)
        if node is None:
            break
        nodes.append(node)
    return FbxFile(version, nodes)


def _write_props(node):
    out = bytearray()
    for v, t, c in zip(node.props, node.types, node.compressed or [False] * len(node.props)):
        out += t
        if t == b"Y":
            out += struct.pack("<h", v)
        elif t == b"C":
            out += bytes([1 if v else 0])
        elif t == b"I":
            out += struct.pack("<i", v)
        elif t == b"F":
            out += struct.pack("<f", v)
        elif t == b"D":
            out += struct.pack("<d", v)
        elif t == b"L":
            out += struct.pack("<q", v)
        elif t in ARRAY_TYPES:
            arr = np.ascontiguousarray(v, dtype=ARRAY_TYPES[t])
            raw = arr.tobytes()
            if c:
                z = zlib.compress(raw)
                out += struct.pack("<III", len(arr), 1, len(z)) + z
            else:
                out += struct.pack("<III", len(arr), 0, len(raw)) + raw
        elif t in (b"S", b"R"):
            out += struct.pack("<I", len(v)) + v
        else:
            raise ValueError(t)
    return bytes(out)


def _write_node(out, node, wide):
    start = len(out)
    head = 24 if wide else 12
    out += b"\x00" * head
    name = node.name.encode("ascii")
    out += bytes([len(name)]) + name
    props = _write_props(node)
    out += props
    nested = node.nested if node.nested is not None else (bool(node.children) or not node.props)
    if node.children or nested:
        for c in node.children:
            _write_node(out, c, wide)
        out += b"\x00" * (25 if wide else 13)
    end = len(out)
    fmt = "<QQQ" if wide else "<III"
    struct.pack_into(fmt, out, start, end, len(node.props), len(props))


def write(fbx, path):
    wide = fbx.version >= 7500
    out = bytearray(MAGIC + struct.pack("<I", fbx.version))
    for n in fbx.nodes:
        _write_node(out, n, wide)
    out += b"\x00" * (25 if wide else 13)
    out += FOOTER_ID
    pad = (16 - (len(out) % 16)) % 16
    if pad == 0:
        pad = 16
    out += b"\x00" * pad
    out += b"\x00" * 4 + struct.pack("<I", fbx.version) + b"\x00" * 120 + FOOTER_MAGIC
    open(path, "wb").write(bytes(out))


def dump(fbx, depth=2, show_props=3):
    def walk(n, d):
        p = ", ".join(repr(v if not isinstance(v, np.ndarray) else f"array[{len(v)}]")[:40] for v in n.props[:show_props])
        print("  " * d + f"{n.name}: {p}" + (f"  (+{len(n.children)})" if n.children else ""))
        if d < depth:
            for c in n.children[:40]:
                walk(c, d + 1)
    for n in fbx.nodes:
        walk(n, 0)
