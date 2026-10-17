"""Render a `domination scan` height map (scan-*.bin) as top-down pictures.

usage: python -I render_scan.py <scan.bin> <out_prefix> [--crop x0 z0 x1 z1] [--park park.json]

Writes <out_prefix>-height.png (height colours over hillshade, 10 m grid, game x right, game z down)
and <out_prefix>-slope.png (surface slope: flat dark, walls bright), plus <out_prefix>.npz.
With --park, the spots of a park file are drawn on both.
"""
import argparse, json, struct
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.colors import LightSource
from matplotlib.patches import Polygon, Circle


def load(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data[:8] == b"DOMSCAN1", "not a domination scan"
    nx, nz = struct.unpack_from("<II", data, 8)
    x0, z0, step, top, bottom = struct.unpack_from("<5f", data, 16)
    h = np.frombuffer(data, dtype="<f4", count=nx * nz, offset=36).reshape(nz, nx).astype(np.float64)
    return h, x0, z0, step


def spot_outline(s):
    if s["shape"] == "box":
        cx, _, cz = s["centre"]; hx, _, hz = s["half"]; a = np.radians(s.get("yaw", 0.0))
        c, si = np.cos(a), np.sin(a)
        pts = [(-hx, -hz), (hx, -hz), (hx, hz), (-hx, hz)]
        return [(cx + px * c - pz * si, cz + px * si + pz * c) for px, pz in pts]
    ax, _, az = s["a"]; bx, _, bz = s["b"]; r = s["radius"]
    d = np.array([bx - ax, bz - az]); n = d / (np.linalg.norm(d) or 1); p = np.array([-n[1], n[0]]) * r
    return [tuple(np.array([ax, az]) + p), tuple(np.array([bx, bz]) + p), tuple(np.array([bx, bz]) - p), tuple(np.array([ax, az]) - p)]


def draw_park(ax, park):
    for s in park["spots"]:
        ax.add_patch(Polygon(spot_outline(s), closed=True, fill=False, ec="red", lw=1.2))
        mx, _, mz = s["marker"]
        ax.text(mx, mz, s["id"], color="white", fontsize=6, ha="center", va="center",
                bbox=dict(boxstyle="round,pad=0.15", fc="black", alpha=0.6, lw=0))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scan"); ap.add_argument("out")
    ap.add_argument("--crop", type=float, nargs=4)
    ap.add_argument("--park")
    ap.add_argument("--outline", type=float, nargs="+", help="x z pairs of a polygon to draw (the park's edge)")
    a = ap.parse_args()
    h, x0, z0, step = load(a.scan)
    nz, nx = h.shape
    xs = x0 + np.arange(nx) * step; zs = z0 + np.arange(nz) * step
    if a.crop:
        cx0, cz0, cx1, cz1 = a.crop
        i0, i1 = np.searchsorted(xs, min(cx0, cx1)), np.searchsorted(xs, max(cx0, cx1))
        j0, j1 = np.searchsorted(zs, min(cz0, cz1)), np.searchsorted(zs, max(cz0, cz1))
        h, xs, zs = h[j0:j1, i0:i1], xs[i0:i1], zs[j0:j1]
    np.savez_compressed(a.out + ".npz", h=h, xs=xs, zs=zs)
    miss = ~np.isfinite(h)
    filled = np.where(miss, np.nanmin(h), h)
    gz, gx = np.gradient(filled, step)
    slope = np.degrees(np.arctan(np.hypot(gx, gz)))
    extent = (xs[0] - step / 2, xs[-1] + step / 2, zs[0] - step / 2, zs[-1] + step / 2)
    lo, hi = np.nanpercentile(h, 1), np.nanpercentile(h, 99.5)
    park = json.load(open(a.park)) if a.park else None

    ls = LightSource(azdeg=315, altdeg=45)
    rgb = ls.shade(np.clip(filled, lo, hi), cmap=plt.cm.terrain, vert_exag=2, dx=step, dy=step, blend_mode="soft",
                   vmin=lo, vmax=hi)
    rgb[miss] = (1, 0, 1, 1)
    w = 14
    for name, image, kw in (("height", rgb, {}), ("slope", np.clip(slope, 0, 90), {"cmap": "magma", "vmin": 0, "vmax": 90})):
        fig, ax = plt.subplots(figsize=(w, w * (extent[3] - extent[2]) / (extent[1] - extent[0]) + 0.6), dpi=110)
        im = ax.imshow(image, origin="lower", extent=extent, interpolation="nearest", **kw)
        if name == "height":
            sm = plt.cm.ScalarMappable(cmap=plt.cm.terrain, norm=plt.Normalize(lo, hi))
            fig.colorbar(sm, ax=ax, fraction=0.03, label="height (m)")
        else:
            fig.colorbar(im, ax=ax, fraction=0.03, label="slope (deg)")
        ax.set_xticks(np.arange(np.ceil(extent[0] / 10) * 10, extent[1], 10))
        ax.set_yticks(np.arange(np.ceil(extent[2] / 10) * 10, extent[3], 10))
        ax.tick_params(labelsize=6); ax.grid(color="black", alpha=0.25, lw=0.5)
        # Seen from above: game x right and game z down (Blender's top view), so not mirrored.
        ax.invert_yaxis()
        ax.set_xlabel("game x (m)"); ax.set_ylabel("game z (m), increasing downward")
        ax.set_title(f"{name}: {a.scan.split(chr(92))[-1]}  step {step:g} m  misses {int(miss.sum())}")
        if park: draw_park(ax, park)
        if a.outline:
            pts = list(zip(a.outline[0::2], a.outline[1::2]))
            ax.add_patch(Polygon(pts, closed=True, fill=False, ec="red", lw=2.5, ls="--"))
        fig.tight_layout(); fig.savefig(f"{a.out}-{name}.png"); plt.close(fig)
    print("points", h.size, "misses", int(miss.sum()), "height", round(float(np.nanmin(h)), 2), round(float(np.nanmax(h)), 2))


if __name__ == "__main__":
    main()
