"""Build a Domination park file from a spec (hand-picked footprints) measured against a scan.

usage: python -I build_park.py <spec.json> <scan.npz> <out.json> <out.png>

spec.json:
  {"id": "...", "level": "...", "park": "Name", "map": "San Vansterdam", "package": "",
   "view": [x0, z0, x1, z1],              # the picture's area (also the default outline)
   "outline": [[x, z], ...],              # optional: spots must stay inside it
   "spots": [{"id": "...", "name": "...", "kind": "bowl",
              "points": [[x, z], ...]}   # a polygon, or
             | {"id", "name", "kind", "a": [x, z], "b": [x, z], "r": 2.0}],   # a capsule
   "starts": [[x, z, heading], ...],
   "ceiling": 210.0}                      # optional: a roof or bridge deck's underside over the park
The scan .npz is render_scan.py's (h over the grid xs, zs). Each spot's height range runs from its
floor - 1 m to its top + 5 m, and its marker floats 2.5 m above its top at the footprint's centre;
floor and top are the 2nd and 97th height percentiles inside it, so a lamp post or a tree's crown
inside a footprint does not lift it. Fails when two spots overlap, a spot leaves the outline, or a
start stands on a spot.
"""
import json, sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.colors import LightSource
from matplotlib.path import Path
from matplotlib.patches import Polygon


def outline_of(s):
    if "points" in s:
        return [tuple(p) for p in s["points"]]
    a, b, r = np.array(s["a"], float), np.array(s["b"], float), float(s["r"])
    d = b - a
    u = d / np.linalg.norm(d) if np.linalg.norm(d) > 1e-6 else np.array([1.0, 0.0])
    n = np.array([-u[1], u[0]])
    st = np.arctan2(n[1], n[0])
    arc = lambda c, start: [tuple(c + r * np.array([np.cos(t), np.sin(t)])) for t in np.linspace(start, start + np.pi, 9)]
    return arc(b, st - np.pi) + arc(a, st)


def mask_of(s, X, Z):
    pts = np.stack([X.ravel(), Z.ravel()], 1)
    if "points" in s:
        return Path(s["points"]).contains_points(pts).reshape(X.shape)
    a, b, r = np.array(s["a"], float), np.array(s["b"], float), float(s["r"])
    d = b - a
    L2 = max(float(d @ d), 1e-9)
    t = np.clip(((pts - a) @ d) / L2, 0, 1)
    near = a + t[:, None] * d
    return (np.hypot(*(pts - near).T) <= r).reshape(X.shape)


def main():
    spec = json.load(open(sys.argv[1], encoding="utf-8"))
    d = np.load(sys.argv[2]); h, xs, zs = d["h"], d["xs"], d["zs"]
    view = spec["view"]
    outline = spec.get("outline") or [[view[0], view[1]], [view[2], view[1]], [view[2], view[3]], [view[0], view[3]]]
    # Sample off the grid lines so footprints sharing an edge do not count as overlapping.
    X, Z = np.meshgrid(xs + 0.1237, zs + 0.1291)
    inside = Path(outline).contains_points(np.stack([X.ravel(), Z.ravel()], 1)).reshape(X.shape)
    step = float(xs[1] - xs[0])
    owner = np.full(h.shape, -1)
    spots, problems = [], []
    for k, s in enumerate(spec["spots"]):
        m = mask_of(s, X, Z)
        if not m.any():
            problems.append(f"{s['id']}: empty"); continue
        if (m & ~inside).any(): problems.append(f"{s['id']}: {int((m & ~inside).sum())} points outside the outline")
        shared = m & (owner >= 0)
        if shared.any():
            other = spec["spots"][np.bincount(owner[shared]).argmax()]["id"]
            problems.append(f"{s['id']}: overlaps {other} on {shared.sum() * step * step:.1f} m2")
        owner[m & (owner < 0)] = k
        hh = h[m]; hh = hh[np.isfinite(hh)]
        floor, top = float(np.percentile(hh, 2)), float(np.percentile(hh, 97))
        cx, cz = float(X[m].mean()), float(Z[m].mean())
        spot = {"id": s["id"], "name": s["name"], "kind": s.get("kind", "spot")}
        if "points" in s:
            spot["shape"] = "polygon"; spot["points"] = [[round(float(x), 2), round(float(z), 2)] for x, z in s["points"]]
        else:
            spot["shape"] = "capsule"; spot["a"] = [float(v) for v in s["a"]]; spot["b"] = [float(v) for v in s["b"]]; spot["radius"] = float(s["r"])
        # Under a roof or a bridge ("ceiling": its underside), the range and marker stay below it:
        # the outlines' ground rays start at the range's top.
        ceiling = spec.get("ceiling")
        y_top = top + 5.0 if ceiling is None else min(top + 5.0, ceiling - 0.5)
        marker_y = top + 2.5 if ceiling is None else min(top + 2.5, ceiling - 1.0)
        spot["y"] = [round(floor - 1.0, 2), round(y_top, 2)]
        spot["marker"] = [round(cx, 2), round(marker_y, 2), round(cz, 2)]
        spot["area"] = round(float(m.sum() * step * step), 1)
        spots.append(spot)
        print(f"{s['id']:16} {spot['kind']:10} area {spot['area']:7.1f} m2  floor {floor:7.2f}  top {top:7.2f}")
    starts = []
    for x, z, heading in spec.get("starts", []):
        j, i = int(np.argmin(abs(zs - z))), int(np.argmin(abs(xs - x)))
        starts.append({"position": [x, round(float(h[j, i]) + 0.05, 2), z], "heading": heading})
        if owner[j, i] >= 0: problems.append(f"start ({x}, {z}) is on spot {spec['spots'][owner[j, i]]['id']}")
    park = {"schema": 1, "level": spec["level"], "park": spec["park"]}
    if spec.get("map"): park["map"] = spec["map"]
    if spec.get("package"): park["package"] = spec["package"]
    park["source"] = f"{sys.argv[2]}, spots in {sys.argv[1]} (tools/build_park.py)"
    park["outline"] = [list(p) for p in outline]
    park["spots"] = spots
    park["starts"] = starts
    json.dump(park, open(sys.argv[3], "w", encoding="utf-8"), indent=1)

    x0, z0, x1, z1 = view
    i0, i1 = np.searchsorted(xs, x0), np.searchsorted(xs, x1)
    j0, j1 = np.searchsorted(zs, z0), np.searchsorted(zs, z1)
    hv = h[j0:j1, i0:i1]
    lo, hi = np.nanpercentile(hv, 1), np.nanpercentile(hv, 97)
    rgb = LightSource(315, 45).shade(np.clip(hv, lo, hi), cmap=plt.cm.terrain, vert_exag=2, dx=step, dy=step, blend_mode="soft", vmin=lo, vmax=hi)
    fig, ax = plt.subplots(figsize=(13, 13 * (z1 - z0) / max(x1 - x0, 1) + 1), dpi=110)
    ax.imshow(rgb, origin="lower", extent=(xs[i0], xs[i1 - 1], zs[j0], zs[j1 - 1]), interpolation="nearest")
    ax.invert_yaxis()
    ax.add_patch(Polygon(outline, closed=True, fill=False, ec="red", lw=2, ls="--"))
    colours = plt.cm.tab20(np.linspace(0, 1, max(len(spec["spots"]), 2)))
    for s, c, out in zip(spec["spots"], colours, spots):
        ax.add_patch(Polygon(outline_of(s), closed=True, fc=(*c[:3], 0.35), ec=c, lw=1.5))
        ax.text(out["marker"][0], out["marker"][2], s["name"], fontsize=8, ha="center", va="center", color="white",
                bbox=dict(boxstyle="round,pad=0.2", fc="black", alpha=0.7, lw=0))
    for n, st in enumerate(starts, 1):
        x, _, z = st["position"]; a = np.radians(st["heading"])
        ax.annotate("", xy=(x + 5 * np.sin(a), z + 5 * np.cos(a)), xytext=(x, z), arrowprops=dict(arrowstyle="->", color="yellow", lw=2))
        ax.text(x, z, str(n), fontsize=8, ha="center", va="center", color="black", bbox=dict(boxstyle="circle,pad=0.2", fc="yellow", lw=0))
    ax.set_xlim(x0, x1); ax.set_ylim(z1, z0)
    ax.set_xticks(np.arange(np.ceil(x0 / 10) * 10, x1 + 1, 10)); ax.set_yticks(np.arange(np.ceil(z0 / 10) * 10, z1 + 1, 10)); ax.tick_params(labelsize=7)
    ax.grid(color="black", alpha=0.2, lw=0.5)
    ax.set_title(f"Domination: {spec['park']}, {len(spots)} spots and {len(starts)} starts (yellow, facing the arrow)")
    fig.tight_layout(); fig.savefig(sys.argv[4])
    if problems:
        print("PROBLEMS:"); [print("  " + p) for p in problems]; sys.exit(1)
    print("ok:", len(spots), "spots, no overlaps")


if __name__ == "__main__":
    main()
