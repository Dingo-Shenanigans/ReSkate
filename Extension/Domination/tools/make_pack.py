"""Packages Domination parks as a mod: a park pack, ready for Thunderstore and the launcher's MODS page.

usage: python -I make_pack.py <out folder> <name> <version> "<description>" <park.json>... [--author <name>] [--website <url>]

Writes <out folder>/<name>-<version>.zip holding:
  manifest.json        Thunderstore's: name, version_number, website_url, description, dependencies
  README.md            the parks, the maps they need, and how to play them
  icon.png             256 x 256: the parks' spots seen from above
  domination/<id>.json the park files, as they are
A park pack ships no game data, so it loads on any game build. With it switched on in the launcher,
its parks are in the DOMINATION tab's Map and Park dropdowns. A park file overrides a built-in park
of the same id, so a pack can also update a built-in park without a new ReSkate.
<name>: letters, digits and _ (Thunderstore's rule). <version>: major.minor.patch.
Needs Pillow (pip install pillow).
"""
import io, json, math, os, re, sys, zipfile
from PIL import Image, ImageDraw, ImageFont

PALETTE = [(94, 205, 250), (255, 177, 11), (240, 72, 64), (46, 184, 92), (176, 120, 255), (255, 120, 190), (245, 245, 245)]


def footprint(spot):
    """A spot's outline (x, z) points: its polygon, or a capsule's two half circles."""
    if spot.get("shape") == "capsule" or "a" in spot:
        (ax, az), (bx, bz), r = spot["a"], spot["b"], float(spot.get("radius", spot.get("r", 1)))
        dx, dz = bx - ax, bz - az
        length = math.hypot(dx, dz) or 1.0
        nx, nz = -dz / length, dx / length
        start = math.atan2(nz, nx)
        arc = lambda cx, cz, s: [(cx + r * math.cos(s + math.pi * i / 8), cz + r * math.sin(s + math.pi * i / 8)) for i in range(9)]
        return arc(bx, bz, start - math.pi) + arc(ax, az, start)
    return [tuple(p) for p in spot["points"]]


def draw_park(draw, park, box):
    """The park's spots in a box (x0, y0, x1, y1), north up, each in a colour of the palette."""
    shapes = [footprint(s) for s in park["spots"]]
    xs = [x for shape in shapes for x, _ in shape]
    zs = [z for shape in shapes for _, z in shape]
    x0, y0, x1, y1 = box
    scale = min((x1 - x0) / max(max(xs) - min(xs), 1), (y1 - y0) / max(max(zs) - min(zs), 1)) * 0.9
    cx, cz = (max(xs) + min(xs)) / 2, (max(zs) + min(zs)) / 2
    for i, shape in enumerate(shapes):
        points = [((x0 + x1) / 2 + (x - cx) * scale, (y0 + y1) / 2 - (z - cz) * scale) for x, z in shape]
        draw.polygon(points, fill=PALETTE[i % len(PALETTE)] + (235,), outline=(20, 20, 20, 255))


def font(size):
    for name in ("arialbd.ttf", "DejaVuSans-Bold.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def icon(parks):
    im = Image.new("RGBA", (256, 256), (26, 26, 26, 255))
    draw = ImageDraw.Draw(im, "RGBA")
    draw.polygon([(0, 8), (256, 0), (256, 50), (0, 58)], fill=(255, 177, 11, 255))  # skate.'s slanted band
    draw.text((128, 30), "DOMINATION", font=font(30), fill=(20, 20, 20, 255), anchor="mm")
    cols = 1 if len(parks) == 1 else 2 if len(parks) <= 4 else 3
    rows = math.ceil(len(parks) / cols)
    cell_w, cell_h = 248 / cols, 190 / rows
    for n, park in enumerate(parks):
        c, r = n % cols, n // cols
        draw_park(draw, park, (4 + c * cell_w + 4, 62 + r * cell_h + 4, 4 + (c + 1) * cell_w - 4, 62 + (r + 1) * cell_h - 4))
    return im


def readme(name, version, description, parks):
    maps = {}
    for p in parks:
        maps.setdefault(p.get("map") or p["level"].split("/")[-1], []).append(p)
    lines = [f"# {name.replace('_', ' ')}", "", description, "", "## Parks", ""]
    for p in parks:
        lines.append(f"- **{p['park']}** ({len(p['spots'])} spots) on {p.get('map') or p['level']}")
    lines += ["", "## Maps they need", ""]
    for m, ps in maps.items():
        package = ps[0].get("package", "")
        lines.append(f"- **{m}**: " + (f"the map mod `{package}` (install it from the MODS page too)" if package else "comes with the game"))
    lines += [
        "",
        "## Playing",
        "",
        "Needs ReSkate with Domination. Switch this mod on in the launcher's MODS page, then in game:",
        "pause menu > Multiplayer > DOMINATION, pick the map and the park, and Start round.",
        "Everyone in a round needs the same version of this pack.",
        "",
        "A park here overrides a built-in park with the same id, so this pack can update one without a new ReSkate.",
        "",
        f"Version {version}.",
        "",
    ]
    return "\n".join(lines)


def main():
    args = sys.argv[1:]
    options = {}
    for flag in ("--author", "--website"):
        if flag in args:
            i = args.index(flag)
            options[flag] = args[i + 1]
            del args[i:i + 2]
    if len(args) < 5:
        sys.exit(__doc__)
    out, name, version, description, files = args[0], args[1], args[2], args[3], args[4:]
    if not re.fullmatch(r"[A-Za-z0-9_]+", name):
        sys.exit("the name may hold only letters, digits and _")
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        sys.exit("the version must be major.minor.patch")
    if len(description) > 250:
        sys.exit("Thunderstore descriptions are at most 250 characters")
    parks, ids = [], []
    for path in files:
        park = json.load(open(path, encoding="utf-8"))
        for key in ("level", "park", "spots"):
            if key not in park:
                sys.exit(f"{path}: not a park file (no {key})")
        pid = os.path.splitext(os.path.basename(path))[0]
        if pid in ids:
            sys.exit(f"{pid}: two parks with the same id")
        parks.append(park)
        ids.append(pid)
    manifest = {"name": name, "version_number": version, "website_url": options.get("--website", ""), "description": description, "dependencies": []}
    if "--author" in options:
        manifest["author"] = options["--author"]  # (Thunderstore takes the author from the uploading team; ReSkate shows this)
    os.makedirs(out, exist_ok=True)
    target = os.path.join(out, f"{name}-{version}.zip")
    png = io.BytesIO()
    icon(parks).save(png, "PNG")
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("manifest.json", json.dumps(manifest, indent=2) + "\n")
        z.writestr("README.md", readme(name, version, description, parks))
        z.writestr("icon.png", png.getvalue())
        for pid, path in zip(ids, files):
            z.write(path, f"domination/{pid}.json")
    print(f"wrote {target}: {len(parks)} park(s)")


if __name__ == "__main__":
    main()
