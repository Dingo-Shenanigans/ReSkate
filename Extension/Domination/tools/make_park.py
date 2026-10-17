"""Write the red-bowl park's spot file (parks/skate3improved-redbowl.json) from hand-picked
footprints, measured against the scan, and draw it.

usage: python -I make_park.py <scan.npz> <out.json> <out.png>

Footprints are game x/z (seen from above). Each spot's height range (floor - 1 m to top + 5 m)
and its marker (the footprint's centre, 2.5 m above its top) come from the scan. Fails when two
spots share any 0.25 m grid point or a spot leaves the park outline.
"""
import json, sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.colors import LightSource
from matplotlib.path import Path
from matplotlib.patches import Polygon

LEVEL = "levels/game/dingolevel_reskate_skate3improved/dingolevel_reskate_skate3improved"
OUTLINE = [(413, 357), (434, 471), (292, 497), (272, 383)]


def box(cx, cz, length, width, yaw, margin):
    """Corners of an oriented box (yaw: the long axis' angle from +x towards +z), grown by margin."""
    t = np.radians(yaw); u = np.array([np.cos(t), np.sin(t)]); n = np.array([-u[1], u[0]])
    hl, hw = length / 2 + margin, width / 2 + margin
    c = np.array([cx, cz])
    return [tuple(np.round(c + su * hl * u + sn * hw * n, 2)) for su, sn in ((-1, -1), (1, -1), (1, 1), (-1, 1))]


def hull(points):
    pts = sorted(set(points))
    def half(seq):
        out = []
        for p in seq:
            while len(out) >= 2 and (out[-1][0] - out[-2][0]) * (p[1] - out[-2][1]) - (out[-1][1] - out[-2][1]) * (p[0] - out[-2][0]) <= 0:
                out.pop()
            out.append(p)
        return out
    lower, upper = half(pts), half(reversed(pts))
    return lower[:-1] + upper[:-1]


# Ledge and pad outlines are the convex hulls of their raised cells in the scan plus 0.6 m;
# bowl edges follow the coping measured in the scan.
# id, name, kind, footprint: ("polygon", [(x, z), ...]) or ("capsule", (x, z), (x, z), radius)
# Where a round puts the skaters (x, z, heading in degrees: 0 faces +z, 90 faces +x): on the flat
# plaza around the bowls, 5 m or more from every spot, facing the park's middle (360, 425); chosen
# from the scan (slope under 4 degrees over 2 m). The height comes from the scan.
STARTS = [(343.5, 469.2, 160), (375.0, 470.0, -162), (405.0, 465.0, -132), (420.0, 445.0, -108), (415.0, 415.0, -80),
          (405.0, 385.0, -48), (380.0, 375.0, -22), (350.0, 375.0, 11), (320.0, 385.0, 45), (300.0, 395.0, 63)]

SPOTS = [
    ("deep-end", "Deep end", "bowl", ("polygon", [(315, 437), (337, 437), (339.5, 445), (336.5, 453.5), (327, 456.5), (317.5, 453.5), (314, 445)])),
    ("main-bowl", "Main bowl", "bowl", ("polygon", [(315, 437), (314.5, 410), (317.5, 407), (326, 406.6), (330.5, 406.3), (341, 405.5), (342, 412), (351, 412),
                                                     (352.5, 404), (357.5, 401), (369, 398), (369, 417), (372, 428), (369.5, 431), (366, 434.5), (337, 437)])),
    # The left twin ledge stands right at the snake arm's coping: both follow the coping measured
    # in the scan (bowl below 2.8 m to x 381.75 at z 400, 382.75 at 405, 383.5 at 410, 384.25 at 413).
    ("east-arm", "Snake arm", "bowl", ("polygon", [(369, 398), (368, 392), (379, 390.5), (381.5, 393.5), (382.4, 400), (383, 405),
                                                    (383.8, 410), (384.6, 414), (386, 423), (397.5, 428.5),
                                                    (398.5, 435), (392.5, 441.5), (386, 436), (384, 434.5), (382, 433), (380, 431.4), (379, 430.6),
                                                    (378, 430), (376, 428), (372, 428), (369, 417)])),
    # With the ledge strip at its top-left (x 321-329, z 392-394) and the hip it ends in
    # (x 341-351, z 404-411; the bowl floor wraps its right side up to z 404).
    ("north-lane", "Sunken lane", "transition", ("polygon", [(321, 391.2), (330, 390.7), (342.5, 390), (345, 395.5), (358, 392.5), (357.5, 401),
                                                             (352.5, 404), (351, 412), (342, 412), (341, 405.5), (330.5, 406.3), (326, 402.3), (322, 399), (320.5, 395)])),
    # The Z-shaped pad by the main bowl's coping (measured: x 315-325.5, z 399.5-406.5).
    ("zigzag-pad", "Zigzag pad", "ledge", ("polygon", [(314.5, 399), (322, 399), (326, 402.3), (326, 406.6), (317.5, 407), (314.5, 404.5)])),
    ("funbox", "Funbox", "box", ("polygon", box(353.7, 386.8, 19.4, 9.0, 165, 0.6))),
    ("top-rail", "Long rail", "rail", ("capsule", (370.5, 384.3), (397.5, 379.2), 1.8)),
    ("curved-ledge", "Curved ledge", "ledge", ("polygon", [(294.5, 418.5), (295.5, 406), (300, 399), (307, 394), (311.5, 393.5), (312.5, 397.5),
                                                          (305, 400.5), (301, 406.5), (299.5, 418.5)])),
    ("west-rail", "Hill rail", "rail", ("capsule", (295.2, 425.0), (302.8, 463.0), 2.0)),
    ("bank-curve", "Curved bank", "bank", ("polygon", [(307.5, 454.5), (318, 455.5), (331, 462.5), (330.5, 467.5), (318, 465.5), (307, 459.5)])),
    ("south-pads", "Manual pads", "ledge", ("polygon", [(348.5, 442.5), (364.5, 441.5), (370.5, 455), (370.5, 463.5), (351, 467), (347.5, 457)])),
    ("mid-pad", "Middle pad", "ledge", ("polygon", [(368.4, 433.2), (369.2, 432.4), (379, 430.8), (380.1, 435.2), (379.2, 436.2), (369.8, 438), (368.6, 437.2)])),
    ("twin-ledges", "Twin ledges", "ledge", ("polygon", [(382.4, 400), (392.5, 394.3), (399.3, 394.3), (400.8, 406.5), (390, 414.3),
                                                          (384.6, 414), (383.8, 410), (383, 405)])),
    ("kinked-ledge", "Kinked ledge", "ledge", ("polygon", [(402.6, 442.8), (405.9, 426.2), (411, 425), (411.7, 425.6), (413.1, 434.8), (406.2, 445.1), (403.2, 443.6)])),
    ("angled-ledge", "Angled ledge", "ledge", ("polygon", [(384.1, 459.6), (400.3, 446.1), (403.4, 447.9), (403.6, 449), (394.4, 462.7), (385.8, 464.7), (384.1, 460.8)])),
    # Added 2026-10-15: the four mounds and the south curb, rideable but in no spot before (a
    # trick on them counted for nothing). Mounds: short capsules (round); the curb along the edge.
    ("west-mound", "West mound", "mound", ("capsule", (303.0, 418.8), (303.5, 419.2), 3.6)),
    ("bowl-mound", "Bowl-side mound", "mound", ("capsule", (311.0, 430.6), (311.4, 431.0), 3.5)),
    ("east-mound", "East mound", "mound", ("capsule", (391.5, 420.4), (392.0, 420.8), 3.8)),
    ("south-mound", "South mound", "mound", ("capsule", (377.5, 449.7), (378.0, 450.2), 3.8)),
    ("south-curb", "South curb", "ledge", ("capsule", (351.0, 470.9), (323.0, 475.9), 1.4)),
]


def inside_mask(fp, X, Z):
    pts = np.stack([X.ravel(), Z.ravel()], 1)
    if fp[0] == "polygon":
        return Path(fp[1]).contains_points(pts).reshape(X.shape)
    a, b, r = np.array(fp[1]), np.array(fp[2]), fp[3]
    ab = b - a; t = np.clip(((pts - a) @ ab) / (ab @ ab), 0, 1)
    return (np.linalg.norm(pts - (a + t[:, None] * ab), axis=1) <= r).reshape(X.shape)


def outline_of(fp):
    if fp[0] == "polygon":
        return fp[1]
    a, b, r = np.array(fp[1]), np.array(fp[2]), fp[3]
    u = (b - a) / np.linalg.norm(b - a); n = np.array([-u[1], u[0]])
    arc = lambda c, start: [tuple(c + r * np.array([np.cos(t), np.sin(t)])) for t in np.linspace(start, start + np.pi, 9)]
    s = np.arctan2(n[1], n[0])
    return arc(b, s - np.pi) + arc(a, s)


def main():
    d = np.load(sys.argv[1]); h, xs, zs = d["h"], d["xs"], d["zs"]
    # Sample between the 0.25 m grid lines, where no hand-placed edge lies, so spots that share
    # an edge are not counted as overlapping.
    X, Z = np.meshgrid(xs + 0.1237, zs + 0.1291)
    park_mask = Path(OUTLINE).contains_points(np.stack([X.ravel(), Z.ravel()], 1)).reshape(X.shape)
    owner = np.full(h.shape, -1)
    spots, problems = [], []
    for k, (sid, name, kind, fp) in enumerate(SPOTS):
        m = inside_mask(fp, X, Z)
        if not m.any(): problems.append(f"{sid}: empty"); continue
        if (m & ~park_mask).any(): problems.append(f"{sid}: {int((m & ~park_mask).sum())} points outside the park")
        shared = m & (owner >= 0)
        if shared.any():
            other = SPOTS[np.bincount(owner[shared]).argmax()][0]
            problems.append(f"{sid}: overlaps {other} on {shared.sum() * 0.0625:.1f} m2")
        owner[m & (owner < 0)] = k
        hh = h[m]; floor, top = float(np.nanmin(hh)), float(np.nanmax(hh))
        cx, cz = float(X[m].mean()), float(Z[m].mean())
        spot = {"id": sid, "name": name, "kind": kind}
        if fp[0] == "polygon":
            spot["shape"] = "polygon"; spot["points"] = [[round(float(x), 2), round(float(z), 2)] for x, z in fp[1]]
        else:
            spot["shape"] = "capsule"; spot["a"] = list(fp[1]); spot["b"] = list(fp[2]); spot["radius"] = fp[3]
        spot["y"] = [round(floor - 1.0, 2), round(top + 5.0, 2)]
        spot["marker"] = [round(cx, 2), round(top + 2.5, 2), round(cz, 2)]
        spot["area"] = round(float(m.sum() * 0.0625), 1)
        spots.append(spot)
        print(f"{sid:13} {kind:10} area {spot['area']:6.1f} m2  floor {floor:5.2f}  top {top:5.2f}")
    starts = []
    for x, z, heading in STARTS:
        j, i = np.argmin(abs(zs - z)), np.argmin(abs(xs - x))
        starts.append({"position": [x, round(float(h[j, i]), 2), z], "heading": heading})
        if owner[j, i] >= 0: problems.append(f"start ({x}, {z}) is on spot {SPOTS[owner[j, i]][0]}")
    park = {"schema": 1, "level": LEVEL, "park": "Red-bowl park", "map": "Skate 3 Improved", "package": "333-Skate_3_Improved",
            "source": "domination scan 2026-10-14 15:23 (scans/scan-20261014-152309.bin), spots chosen in tools/make_park.py",
            "outline": [list(p) for p in OUTLINE], "spots": spots, "starts": starts}
    json.dump(park, open(sys.argv[2], "w"), indent=1)
    step = float(xs[1] - xs[0])
    lo, hi = np.nanpercentile(h, 1), np.nanpercentile(h, 99.5)
    rgb = LightSource(315, 45).shade(np.clip(h, lo, hi), cmap=plt.cm.terrain, vert_exag=2, dx=step, dy=step, blend_mode="soft", vmin=lo, vmax=hi)
    fig, ax = plt.subplots(figsize=(13, 11), dpi=110)
    ax.imshow(rgb, origin="lower", extent=(xs[0], xs[-1], zs[0], zs[-1]), interpolation="nearest")
    ax.invert_yaxis()
    ax.add_patch(Polygon(OUTLINE, closed=True, fill=False, ec="red", lw=2, ls="--"))
    colours = plt.cm.tab20(np.linspace(0, 1, len(SPOTS)))
    for (sid, name, kind, fp), c, s in zip(SPOTS, colours, spots):
        ax.add_patch(Polygon(outline_of(fp), closed=True, fc=(*c[:3], 0.35), ec=c, lw=1.5))
        ax.text(s["marker"][0], s["marker"][2], name, fontsize=8, ha="center", va="center", color="white",
                bbox=dict(boxstyle="round,pad=0.2", fc="black", alpha=0.7, lw=0))
    for n, st in enumerate(starts, 1):
        x, _, z = st["position"]; a = np.radians(st["heading"])
        ax.annotate("", xy=(x + 5 * np.sin(a), z + 5 * np.cos(a)), xytext=(x, z), arrowprops=dict(arrowstyle="->", color="yellow", lw=2))
        ax.text(x, z, str(n), fontsize=8, ha="center", va="center", color="black", bbox=dict(boxstyle="circle,pad=0.2", fc="yellow", lw=0))
    ax.set_xlim(285, 440); ax.set_ylim(496, 375)
    ax.set_xticks(np.arange(290, 441, 10)); ax.set_yticks(np.arange(380, 497, 10)); ax.tick_params(labelsize=7)
    ax.grid(color="black", alpha=0.15, lw=0.5)
    ax.set_title(f"Domination: red-bowl park, {len(spots)} spots and {len(starts)} start positions (yellow, facing the arrow)")
    fig.tight_layout(); fig.savefig(sys.argv[3])
    if problems:
        print("PROBLEMS:"); [print("  " + p) for p in problems]; sys.exit(1)
    print("ok:", len(spots), "spots, no overlaps")


if __name__ == "__main__":
    main()
