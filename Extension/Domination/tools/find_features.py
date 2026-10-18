"""Find the skateable features in a rendered scan (.npz from render_scan.py) inside a park outline.

usage: python -I find_features.py <scan.npz> <out_prefix> --outline x z x z ...

Raised features (ledges, boxes, banks, rails, mounds) are what stands above the local ground
(grey opening over 8 m); dips (bowls, pools) are what lies below it (grey closing). Each connected
piece is reported with its oriented footprint (centre, length, width, yaw), height and slope.
Writes <out_prefix>-features.json and <out_prefix>-features.png.
"""
import argparse, json
import numpy as np
from scipy import ndimage
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.path import Path
from matplotlib.patches import Polygon


def footprint(px, pz):
    """Oriented box of points: centre, half-length along the long axis, half-width, yaw (deg)."""
    c = np.array([px.mean(), pz.mean()])
    pts = np.stack([px - c[0], pz - c[1]], 1)
    w, v = np.linalg.eigh(np.cov(pts.T) if len(pts) > 2 else np.eye(2))
    axis = v[:, 1]
    u = pts @ axis; n = pts @ np.array([-axis[1], axis[0]])
    mid = np.array([(u.max() + u.min()) / 2, (n.max() + n.min()) / 2])
    centre = c + mid[0] * axis + mid[1] * np.array([-axis[1], axis[0]])
    yaw = np.degrees(np.arctan2(axis[1], axis[0]))
    return centre, (u.max() - u.min()) / 2, (n.max() - n.min()) / 2, yaw, axis


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("npz"); ap.add_argument("out")
    ap.add_argument("--outline", type=float, nargs="+", required=True)
    a = ap.parse_args()
    d = np.load(a.npz); h, xs, zs = d["h"], d["xs"], d["zs"]
    step = float(xs[1] - xs[0])
    X, Z = np.meshgrid(xs, zs)
    poly = np.array(list(zip(a.outline[0::2], a.outline[1::2])))
    inside = Path(poly).contains_points(np.stack([X.ravel(), Z.ravel()], 1)).reshape(h.shape)
    filled = np.where(np.isfinite(h), h, np.nanmedian(h))
    size = int(round(8 / step)) | 1
    ground_hi = ndimage.grey_opening(filled, size=(size, size))   # ground under raised things
    ground_lo = ndimage.grey_closing(filled, size=(size, size))   # ground over dips
    raised = (filled - ground_hi > 0.12) & inside
    dip = (ground_lo - filled > 0.4) & inside
    gz, gx = np.gradient(filled, step)
    slope = np.degrees(np.arctan(np.hypot(gx, gz)))
    feats = []
    for kind, mask, rel in (("raised", raised, filled - ground_hi), ("dip", dip, ground_lo - filled)):
        mask = ndimage.binary_opening(mask, iterations=1) if kind == "dip" else mask
        lab, n = ndimage.label(mask, structure=np.ones((3, 3)))
        for k in range(1, n + 1):
            sel = lab == k
            area = sel.sum() * step * step
            if area < (0.6 if kind == "raised" else 6.0):
                continue
            centre, hl, hw, yaw, axis = footprint(X[sel], Z[sel])
            feats.append(dict(kind=kind, centre=[round(float(centre[0]), 2), round(float(np.nanmedian(h[sel])), 2), round(float(centre[1]), 2)],
                              length=round(2 * hl, 2), width=round(2 * hw, 2), yaw=round(float(yaw), 1), area=round(float(area), 1),
                              rel_height=round(float(np.percentile(rel[sel], 95)), 2), top=round(float(np.nanmax(h[sel])), 2),
                              floor=round(float(np.nanmin(h[sel])), 2), max_slope=round(float(np.percentile(slope[sel], 95)), 1),
                              fill=round(float(area / max(4 * hl * hw, 1e-6)), 2)))
    feats.sort(key=lambda f: -f["area"])
    for i, f in enumerate(feats):
        f["id"] = f"{f['kind'][0]}{i:02d}"
    json.dump(feats, open(a.out + "-features.json", "w"), indent=1)
    fig, ax = plt.subplots(figsize=(13, 11), dpi=110)
    ax.imshow(np.where(inside, filled, np.nan), origin="lower", extent=(xs[0], xs[-1], zs[0], zs[-1]), cmap="gray")
    ax.invert_yaxis()
    ax.add_patch(Polygon(poly, closed=True, fill=False, ec="red", lw=1.5, ls="--"))
    for f in feats:
        c = np.array([f["centre"][0], f["centre"][2]]); t = np.radians(f["yaw"])
        u = np.array([np.cos(t), np.sin(t)]); n = np.array([-u[1], u[0]])
        hl, hw = f["length"] / 2, f["width"] / 2
        box = [c + su * hl * u + sn * hw * n for su, sn in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
        ax.add_patch(Polygon(box, closed=True, fill=False, ec="cyan" if f["kind"] == "raised" else "orange", lw=1))
        ax.text(c[0], c[1], f["id"], fontsize=7, color="yellow", ha="center", va="center")
    ax.set_title("features found (cyan raised, orange dips)")
    fig.tight_layout(); fig.savefig(a.out + "-features.png")
    for f in feats:
        print(f"{f['id']} {f['kind']:6} c=({f['centre'][0]:.1f},{f['centre'][1]:.2f},{f['centre'][2]:.1f}) L={f['length']:.1f} W={f['width']:.1f} "
              f"yaw={f['yaw']:.0f} area={f['area']:.0f} rel={f['rel_height']:.2f} slope={f['max_slope']:.0f} fill={f['fill']}")


if __name__ == "__main__":
    main()
