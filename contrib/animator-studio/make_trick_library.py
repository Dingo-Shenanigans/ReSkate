"""Builds Animator Studio's trick library from the game's own trick animations.

For every trick in TRICKS it exports the game's clip for each pop (reskate_cli animation-export) and
writes it as an Animator Studio clip (.clip, "FAS2": the 395 joints of the skater, parent-local, and the
board relative to the skater's placement joint) with a catalog entry. The studio lists the catalog
(ANIMATIONS > Game tricks) and loads a trick from it: no recording needed.

    python make_trick_library.py <game folder> <library folder> [--only ollie]

The joints are matched by name: the skeleton's joints carry FNV-1 hashes of their names
(animbase_default_skeleton), the exported FBX carries the names.
"""
import json
import math
import os
import re
import struct
import subprocess
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from clip import Clip  # noqa: E402

CLI = os.environ.get("RESKATE_CLI", "reskate_cli")  # ReSkate Studio's command line tool

# Clip family -> (the game's name for the trick in the air, the name shown, the category).
TRICKS = [
    ("o_ollie", "ollie", "Ollie", "Flip tricks"),
    ("o_kickflip", "kickflip", "Kickflip", "Flip tricks"),
    ("o_heelflip", "heelflip", "Heelflip", "Flip tricks"),
    ("o_popshuv_bs", "popshuvit", "Pop Shuv-it", "Flip tricks"),
    ("o_popshuv_fs", "fspopshuvit", "FS Pop Shuv-it", "Flip tricks"),
    ("o_360popshuv_bs", "360popshuvit", "360 Pop Shuv-it", "Flip tricks"),
    ("o_360popshuv_fs", "fs360popshuvit", "FS 360 Pop Shuv-it", "Flip tricks"),
    ("o_varialflip", "varialkickflip", "Varial Kickflip", "Flip tricks"),
    ("o_varialheel", "varialheelflip", "Varial Heelflip", "Flip tricks"),
    ("o_360flip", "360flip", "360 Flip", "Flip tricks"),
    ("o_hardflip", "hardflip", "Hardflip", "Flip tricks"),
    ("o_inwardheel", "inwardheelflip", "Inward Heelflip", "Flip tricks"),
    ("o_360inwardheel", "360inwardheelflip", "360 Inward Heelflip", "Flip tricks"),
    ("o_360hardflip", "360hardflip", "360 Hardflip", "Flip tricks"),
    ("o_laserflip", "laserflip", "Laser Flip", "Flip tricks"),
    ("o_impossible", "bsimpossible", "BF Impossible", "Flip tricks"),
    ("o_frontfoot_impossible", "fsimpossible", "FF Impossible", "Flip tricks"),
    ("n_nollie", "nollie", "Nollie", "Nollie tricks"),
    ("n_kickflip", "nolliekickflip", "Nollie Kickflip", "Nollie tricks"),
    ("n_heelflip", "nollieheelflip", "Nollie Heelflip", "Nollie tricks"),
    ("n_popshuv_bs", "n_popshuvit", "Nollie Pop Shuv-it", "Nollie tricks"),
    ("n_popshuv_fs", "n_fspopshuvit", "Nollie FS Pop Shuv-it", "Nollie tricks"),
    ("n_360popshuv_bs", "n_360popshuvit", "Nollie 360 Pop Shuv-it", "Nollie tricks"),
    ("n_360popshuv_fs", "n_fs360popshuvit", "Nollie FS 360 Pop Shuv-it", "Nollie tricks"),
    ("n_varialflip", "nollievarialkickflip", "Nollie Varial Kickflip", "Nollie tricks"),
    ("n_varialheel", "nollievarialheelflip", "Nollie Varial Heelflip", "Nollie tricks"),
    ("n_360flip", "nollie360flip", "Nollie 360 Flip", "Nollie tricks"),
    ("n_hardflip", "nolliehardflip", "Nollie Hardflip", "Nollie tricks"),
    ("n_inwardheel", "nollieinwardheelflip", "Nollie Inward Heelflip", "Nollie tricks"),
    ("n_360inwardheel", "nollie360inwardheelflip", "Nollie 360 Inward Heelflip", "Nollie tricks"),
    ("n_360hardflip", "nollie360hardflip", "Nollie 360 Hardflip", "Nollie tricks"),
    ("n_laserflip", "nollielaserflip", "Nollie Laser Flip", "Nollie tricks"),
    ("n_impossible", "nolliebsimpossible", "Nollie BF Impossible", "Nollie tricks"),
]
POPS = ("low", "mid", "high")


def fnv1(text):
    h = 0x811C9DC5
    for c in text.encode():
        h = (h * 0x01000193) & 0xFFFFFFFF
        h ^= c
    return h


def joint_hashes(game):
    """The skater's 395 joints in the game's order, as FNV-1 hashes of their names."""
    out = subprocess.run([CLI, "ebx-values", game, "animation/dingo/animbase_default_skeleton"], capture_output=True, text=True).stdout
    hashes = [int(m) for m in re.findall(r"NameHash = (\d+)", out)]
    if len(hashes) != 395:
        raise SystemExit(f"expected 395 joints, found {len(hashes)}")
    return hashes


def quat(m):
    """Unit quaternion (x, y, z, w) of a 3x3 rotation."""
    t = m[0, 0] + m[1, 1] + m[2, 2]
    if t > 0:
        s = math.sqrt(t + 1.0) * 2
        q = ((m[2, 1] - m[1, 2]) / s, (m[0, 2] - m[2, 0]) / s, (m[1, 0] - m[0, 1]) / s, 0.25 * s)
    elif m[0, 0] > m[1, 1] and m[0, 0] > m[2, 2]:
        s = math.sqrt(1.0 + m[0, 0] - m[1, 1] - m[2, 2]) * 2
        q = (0.25 * s, (m[0, 1] + m[1, 0]) / s, (m[0, 2] + m[2, 0]) / s, (m[2, 1] - m[1, 2]) / s)
    elif m[1, 1] > m[2, 2]:
        s = math.sqrt(1.0 + m[1, 1] - m[0, 0] - m[2, 2]) * 2
        q = ((m[0, 1] + m[1, 0]) / s, 0.25 * s, (m[1, 2] + m[2, 1]) / s, (m[0, 2] - m[2, 0]) / s)
    else:
        s = math.sqrt(1.0 + m[2, 2] - m[0, 0] - m[1, 1]) * 2
        q = ((m[0, 2] + m[2, 0]) / s, (m[1, 2] + m[2, 1]) / s, 0.25 * s, (m[1, 0] - m[0, 1]) / s)
    n = math.sqrt(sum(v * v for v in q))
    return tuple(v / n for v in q)


def joint_of(m):
    """{scale, quaternion, position} of a 4x4 local matrix (uniform-ish scale)."""
    r = m[:3, :3]
    scale = np.linalg.norm(r, axis=0)
    rot = r / np.where(scale > 1e-8, scale, 1)
    return (tuple(scale), quat(rot), tuple(m[:3, 3]))


def convert(fbx_path, hashes):
    c = Clip(fbx_path)
    by_hash = {fnv1(name): name for name in c.node}
    names = [by_hash.get(h) for h in hashes]
    missing = sum(1 for n in names if n is None)
    if missing > 20:
        raise RuntimeError(f"{missing} joints not in the clip")
    frames, board, root_y = [], [], []
    identity = ((1.0, 1.0, 1.0), (0.0, 0.0, 0.0, 1.0), (0.0, 0.0, 0.0))
    for f in range(len(c.times)):
        row = []
        for name in names:
            row.append(joint_of(c.local(name, f)) if name else identity)
        frames.append(row)
        # The board: Skateboard_Root hangs from AITrajectory (the placement joint) in the clip.
        b = c.local("Skateboard_Root", f)
        board.append(joint_of(b))
        w = c.world(f)
        root_y.append(w["Skateboard_Root"][1, 3])
    # The clips are in place (the physics makes the jump). Matched against a recorded ollie: the takeoff
    # is the pop, the board's lowest point under the skater as the tail strikes, and the landing the
    # board's next low point after it has come up to the feet (ollie mid: 0.22 s and 0.70 s, the
    # recording's 0.55 s air played 0.48 s).
    y = np.array(root_y)
    smooth = np.convolve(y, np.ones(5) / 5, mode="same")
    lows = [i for i in range(2, len(y) - 2) if smooth[i] <= smooth[i - 1] and smooth[i] < smooth[i + 1]]
    takeoff = lows[0] if lows else 0
    # A flip trick's air is between a quarter and about 0.85 s (the game stretches it to the real one):
    # the landing is the board's deepest low point in that window.
    first, last = takeoff + 15, min(len(y) - 1, takeoff + 51)  # 60 fps
    window = [i for i in lows if first <= i <= last]
    landing = min(window, key=lambda i: smooth[i]) if window else min(len(y) - 1, takeoff + 27)
    return c.times - c.times[0], frames, board, float(c.times[takeoff] - c.times[0]), float(c.times[landing] - c.times[0])


def write_clip(path, times, frames, board, takeoff, landing):
    """Animator Studio's "FAS3": joints that never move stored once, the rest and the board every frame."""
    n = len(frames[0])
    moves = [any(frames[f][j] != frames[0][j] for f in range(1, len(frames))) for j in range(n)]
    out = bytearray(b"FAS3")
    out += struct.pack("<II", n, len(times))
    out += struct.pack("<ff", takeoff, landing)
    out += struct.pack("<B", 1)
    out += struct.pack(f"<{len(times)}f", *times)
    out += bytes(1 if m else 0 for m in moves)
    for j in range(n):
        if not moves[j]:
            s, q, p = frames[0][j]
            out += struct.pack("<10f", *s, *q, *p)
    for f in range(len(frames)):
        for j in range(n):
            if moves[j]:
                s, q, p = frames[f][j]
                out += struct.pack("<10f", *s, *q, *p)
        s, q, p = board[f]
        out += struct.pack("<10f", *s, *q, *p)
    with open(path, "wb") as fh:
        fh.write(out)


def repack(library):
    """Rewrites the library's older (FAS2) clips as FAS3."""
    for name in os.listdir(library):
        if not name.endswith(".clip"):
            continue
        path = os.path.join(library, name)
        b = open(path, "rb").read()
        if b[:4] != b"FAS2":
            continue
        n, count = struct.unpack_from("<II", b, 4)
        takeoff, landing = struct.unpack_from("<ff", b, 12)
        times = struct.unpack_from(f"<{count}f", b, 20)
        off = 20 + 4 * count
        def joint(i):
            v = struct.unpack_from("<10f", b, off + i * 40)
            return (v[0:3], v[3:7], v[7:10])
        frames = [[joint(f * n + j) for j in range(n)] for f in range(count)]
        board = [joint(count * n + f) for f in range(count)]
        write_clip(path, times, frames, board, takeoff, landing)


def main():
    if sys.argv[1] == "--repack":
        repack(sys.argv[2])
        return
    game, library = sys.argv[1], sys.argv[2]
    only = sys.argv[sys.argv.index("--only") + 1] if "--only" in sys.argv else None
    os.makedirs(library, exist_ok=True)
    hashes = joint_hashes(game)
    catalog_path = os.path.join(library, "catalog.json")
    catalog = json.load(open(catalog_path)) if os.path.exists(catalog_path) else []
    done = {(e["trick"], e["pop"]) for e in catalog}
    with tempfile.TemporaryDirectory() as tmp:
        for family, trick, label, category in TRICKS:
            if only and trick != only:
                continue
            for pop in POPS:
                if (trick, pop) in done:
                    continue
                asset = f"animation/dingo/c_proto_onb_trick_{family}_{pop}"
                fbx = os.path.join(tmp, f"{trick}_{pop}.fbx")
                r = subprocess.run([CLI, "animation-export", game, asset, fbx], capture_output=True, text=True)
                if r.returncode or not os.path.exists(fbx):
                    print(f"skip {trick} {pop}: {(r.stderr or r.stdout).strip()[:120]}")
                    continue
                try:
                    times, frames, board, takeoff, landing = convert(fbx, hashes)
                except Exception as e:  # noqa: BLE001
                    print(f"skip {trick} {pop}: {e}")
                    continue
                name = f"{trick}_{pop}.clip"
                write_clip(os.path.join(library, name), times, frames, board, takeoff, landing)
                catalog.append({"category": category, "label": label, "trick": trick, "pop": pop, "file": name})
                print(f"{label} {pop}: {len(times)} frames, takeoff {takeoff:.2f} s, landing {landing:.2f} s")
                with open(catalog_path, "w") as f:
                    json.dump(catalog, f, indent=1)


if __name__ == "__main__":
    main()
