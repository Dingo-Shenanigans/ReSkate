"""The MARKET's art, made from scratch (no stock, no generated images): tileable grip tape, concrete, worn white paper and
blue paper; blue spray splatters; halftone and grey dots; the grunge "market." on the wall; the blue brush stroke; and
the skater from the studio's loading picture as a duotone and as newsprint dots. Everything lands in
assets/animator_studio/market/. Run it again after changing it; it is seeded, so it makes the same pictures."""
import math
import random
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

REPO = Path(__file__).resolve().parents[3]
OUT = REPO / "assets" / "animator_studio" / "market"
FONTS = REPO / "External" / "fonts"
BLUE = (1, 131, 255)
T = 1024  # a tile's side


# ---------------------------------------------------------------------------------------------- noise that tiles
def periodic(size, cells, rng):
    """Value noise in 0..1 that repeats every `size` pixels: a random grid tiled 3 x 3, smoothed, the middle kept."""
    g = rng.random((cells, cells)).astype(np.float32)
    big = Image.fromarray(np.tile(g, (3, 3)), mode="F").resize((size * 3, size * 3), Image.BICUBIC)
    return np.asarray(big, np.float32)[size:2 * size, size:2 * size]


def fbm(size, cells, octaves, rng, gain=0.5):
    total, amp, norm = np.zeros((size, size), np.float32), 1.0, 0.0
    for o in range(octaves):
        total += periodic(size, cells * 2 ** o, rng) * amp
        norm += amp
        amp *= gain
    return total / norm


def wrap_blur(a, radius):
    """A Gaussian blur that wraps round the tile's edges (done in frequency space, which wraps by nature), so it still
    tiles."""
    h, w = a.shape
    fy = np.fft.fftfreq(h)[:, None]
    fx = np.fft.fftfreq(w)[None, :]
    kernel = np.exp(-2 * (math.pi * radius) ** 2 * (fx * fx + fy * fy))
    return np.real(np.fft.ifft2(np.fft.fft2(a) * kernel)).astype(np.float32)


def specks(size, count, rng, sizes=(1,), bright=(0.3, 1.0)):
    """Single grains scattered over the tile (wrapping), each its own brightness."""
    out = np.zeros((size, size), np.float32)
    xs = rng.integers(0, size, count)
    ys = rng.integers(0, size, count)
    vs = bright[0] + (bright[1] - bright[0]) * rng.random(count) ** 3
    for s in sizes:
        pick = rng.random(count) < (1.0 / len(sizes))
        for dy in range(s):
            for dx in range(s):
                np.maximum.at(out, ((ys[pick] + dy) % size, (xs[pick] + dx) % size), vs[pick])
    return out


def wrap_lines(size, paths, width, value):
    """Thin lines drawn on the tile and on its neighbours, so a crack running off one edge comes in at the other."""
    img = Image.new("F", (size * 3, size * 3), 0)
    d = ImageDraw.Draw(img)
    for path in paths:
        for ox in (0, size, 2 * size):
            for oy in (0, size, 2 * size):
                d.line([(x + ox, y + oy) for x, y in path], fill=value, width=width)
    return np.asarray(img, np.float32)[size:2 * size, size:2 * size]


def save_rgb(arr, name, quality=92):
    Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8)).save(OUT / name, quality=quality, optimize=True)
    print("made", name)


def save_rgba(img, name):
    img.save(OUT / name, optimize=True)
    print("made", name, img.size)


# ---------------------------------------------------------------------------------------------- grip tape
def grip_tape():
    rng = np.random.default_rng(101)
    base = 0.075 + 0.008 * fbm(T, 12, 5, rng) + 0.01 * (periodic(T, 256, rng) - 0.5)
    # Silicon carbide grit: a dense field of tiny grains, most dull, a few catching the light.
    grit = specks(T, int(T * T * 0.085), rng, sizes=(1, 1, 2), bright=(0.1, 0.55))
    sparkle = specks(T, int(T * T * 0.004), rng, sizes=(1, 2), bright=(0.55, 0.95))
    grain = wrap_blur(np.maximum(grit, sparkle), 0.45)
    # Worn: shoe scuffs, wider pale streaks where the grit is flattened.
    scuff = np.clip((fbm(T, 10, 4, rng) - 0.62) * 3.0, 0, 1) * 0.02
    v = base + grain * 0.55 + scuff
    rgb = np.dstack([v * 255 * 0.98, v * 255 * 1.0, v * 255 * 1.04])
    save_rgb(rgb, "grip.jpg")


# ---------------------------------------------------------------------------------------------- concrete
def concrete():
    rng = np.random.default_rng(202)
    v = 0.5 + 0.07 * (fbm(T, 8, 7, rng, 0.6) - 0.5) * 2
    v += 0.09 * (rng.random((T, T)).astype(np.float32) - 0.5)       # sand, pixel by pixel
    v += 0.05 * (periodic(T, 256, rng) - 0.5)
    v -= 0.06 * np.clip((fbm(T, 2, 3, rng) - 0.55) * 3, 0, 1)  # damp stains
    # Pits: air holes in the pour, dark with a light lip on the side the light comes from.
    pits = np.zeros((T, T), np.float32)
    lips = np.zeros((T, T), np.float32)
    img = Image.new("F", (T * 3, T * 3), 0)
    lip = Image.new("F", (T * 3, T * 3), 0)
    d, dl = ImageDraw.Draw(img), ImageDraw.Draw(lip)
    for _ in range(2600):
        x, y = rng.random() * T, rng.random() * T
        r = 0.5 + rng.random() ** 4 * 3.2
        # Ragged, not round: a few points round it, each its own distance out.
        pts = [(math.cos(a) * r * (0.55 + 0.6 * rng.random()), math.sin(a) * r * (0.55 + 0.6 * rng.random())) for a in np.linspace(0, math.tau, 7, endpoint=False)]
        for ox in (0, T, 2 * T):
            for oy in (0, T, 2 * T):
                d.polygon([(x + ox + px, y + oy + py) for px, py in pts], fill=1.0)
                if r > 1.2:
                    dl.arc([x + ox - r - 1, y + oy - r - 1, x + ox + r + 1, y + oy + r + 1], 200, 340, fill=1.0)
    pits = np.asarray(img, np.float32)[T:2 * T, T:2 * T]
    lips = np.asarray(lip, np.float32)[T:2 * T, T:2 * T]
    v -= 0.2 * wrap_blur(pits, 0.4)
    v += 0.06 * lips
    # Aggregate: lighter stones showing through the surface.
    v += 0.06 * specks(T, 9000, rng, sizes=(1, 2, 3), bright=(0.2, 1.0))
    # Hairline cracks wandering across.
    paths = []
    for _ in range(4):
        x, y = rng.random() * T, rng.random() * T
        ang = rng.random() * math.tau
        path = [(x, y)]
        for _ in range(int(60 + rng.random() * 120)):
            ang += rng.normal(0, 0.35)
            x += math.cos(ang) * 4
            y += math.sin(ang) * 4
            path.append((x, y))
        paths.append(path)
    v -= 0.16 * wrap_blur(wrap_lines(T, paths, 1, 1.0), 0.5)
    rgb = np.dstack([v * 255 * 0.98, v * 255 * 0.99, v * 255 * 1.02])
    save_rgb(rgb, "concrete.jpg")


# ---------------------------------------------------------------------------------------------- paper
def fold_slopes(rng, count, kmax, amp, cells=(2, 5)):
    """Folds: straight creases across the tile in many directions (whole-number ones, so each comes back round and the
    tile still tiles), each the slope it gives the paper either side of it, fading in and out along its length. The
    paper's slope is the sum of them; the creases are where a slope flips."""
    yy, xx = np.mgrid[0:T, 0:T].astype(np.float32) / T
    gx = np.zeros((T, T), np.float32)
    gy = np.zeros((T, T), np.float32)
    crease = np.zeros((T, T), np.float32)
    for _ in range(count):
        while True:
            kx, ky = (int(v) for v in rng.integers(-kmax, kmax + 1, 2))
            if kx != 0 and ky != 0:  # straight across or down would line up into a grid
                break
        s = kx * xx + ky * yy + rng.random()
        off = s - np.round(s)
        win = np.clip((periodic(T, int(rng.integers(cells[0], cells[1] + 1)), rng) - 0.42) * 2.2, 0, 1)
        sign = 1 if rng.random() < 0.5 else -1
        n = math.hypot(kx, ky)
        g = sign * amp * win * np.sign(off)
        gx += g * kx / n
        gy += g * ky / n
        crease += win * np.clip(1 - np.abs(off) * T * n / 2.0, 0, 1) * amp
    return gx, gy, crease


def crumple(rng):
    """Crumpled paper's light: big folds, smaller ones and many little ones, lit from the top left. Returns the light
    and where the creases are."""
    parts = [fold_slopes(rng, 12, 3, 0.16), fold_slopes(rng, 36, 6, 0.08), fold_slopes(rng, 90, 11, 0.04, (3, 8))]
    gx = sum(p[0] for p in parts)
    gy = sum(p[1] for p in parts)
    crease = sum(p[2] for p in parts)
    nz = 1 / np.sqrt(gx * gx + gy * gy + 1)
    shade = (gx * 0.5 + gy * 0.55) * nz + nz * 0.67
    return shade - shade.mean(), np.clip(crease * 4, 0, 1)


def paper_white():
    rng = np.random.default_rng(303)
    shade, crease = crumple(rng)
    fibre = periodic(T, 400, rng) - 0.5
    dirt = np.clip((fbm(T, 4, 4, rng) - 0.55) * 2.2, 0, 1)
    grain = rng.random((T, T)).astype(np.float32) - 0.5
    v = 0.9 + 0.42 * shade + 0.025 * fibre + 0.018 * grain - 0.05 * dirt
    v -= 0.05 * specks(T, 2500, rng, sizes=(1,), bright=(0.2, 1.0))
    r, g, b = v * 238, v * 234, v * 224
    # Yellowed where it has been handled.
    r += 6 * dirt
    b -= 10 * dirt
    save_rgb(np.dstack([r, g, b]), "paper_white.jpg")


def paper_blue():
    rng = np.random.default_rng(404)
    shade, ridge = crumple(rng)
    fibre = periodic(T, 420, rng) - 0.5
    grain = rng.random((T, T)).astype(np.float32) - 0.5
    light = 1.0 + 0.5 * shade + 0.03 * fibre + 0.03 * grain
    r, g, b = BLUE[0] + 14 * light, BLUE[1] * light, BLUE[2] * min(1.0, 1) * np.clip(light, 0, 1.05)
    rgb = np.dstack([np.full((T, T), 6.0) * light, 120 * light, 238 * np.minimum(light, 1.06)])
    # Worn through to the white core on the sharpest creases, and scuffed in spots.
    wear = np.clip(ridge - 0.4, 0, 1) * (periodic(T, 160, rng) > 0.6) * 0.7
    scuff = specks(T, 3500, rng, sizes=(1, 2), bright=(0.15, 0.9))
    white = np.clip(wear * 0.55 + scuff * 0.5, 0, 1)[..., None]
    rgb = rgb * (1 - white) + np.array([226, 236, 250], np.float32) * white
    save_rgb(rgb, "paper_blue.jpg")


# ---------------------------------------------------------------------------------------------- overlays
def spray(width, height, seed, strokes, drips=0, colour=BLUE, mist=0.0):
    """Paint thrown at a wall: a solid core of overlapping blobs, spikes and long drops flung out from it, droplets
    thinning out with distance, a fine mist of the smallest, and runs down from its foot. Drawn at twice the size and
    brought down, so its edges are hard but not jagged. `strokes`: (x, y, core radius, droplets)."""
    rng = random.Random(seed)
    S = 2
    canvas = Image.new("L", (width * S, height * S), 0)
    d = ImageDraw.Draw(canvas)

    def blob(x, y, r, v=255):
        d.ellipse([(x - r) * S, (y - r) * S, (x + r) * S, (y + r) * S], fill=v)

    for (cx, cy, r, n) in strokes:
        core = r * 0.42
        # The core: lumpy, a few blobs over each other.
        for _ in range(9):
            a = rng.random() * math.tau
            q = rng.random() * core * 0.45
            blob(cx + math.cos(a) * q, cy + math.sin(a) * q, core * rng.uniform(0.45, 0.8))
        # Spikes: paint thrown out in streaks, thin at the core and ending in a drop.
        for _ in range(rng.randint(14, 24)):
            a = rng.random() * math.tau
            start = core * rng.uniform(0.5, 0.8)
            length = core * rng.uniform(0.4, 1.6)
            w = core * rng.uniform(0.05, 0.13)
            x0, y0 = cx + math.cos(a) * start, cy + math.sin(a) * start
            x1, y1 = cx + math.cos(a) * (start + length), cy + math.sin(a) * (start + length)
            px, py = -math.sin(a) * w, math.cos(a) * w
            d.polygon([((x0 + px) * S, (y0 + py) * S), ((x0 - px) * S, (y0 - py) * S), (x1 * S, y1 * S)], fill=255)
            blob(x1 + math.cos(a) * w * 0.6, y1 + math.sin(a) * w * 0.6, w * rng.uniform(0.7, 1.3))
        # Droplets, fewer and smaller the further out; some stretched the way they flew.
        for _ in range(n):
            a = rng.random() * math.tau
            dist = core * (0.9 + rng.expovariate(1.0 / 1.1))
            if dist > r * 2.6:
                continue
            size = max(0.5, (r * 0.05) * rng.random() ** 2 * max(0.15, 1.6 - dist / (r * 1.6)))
            x, y = cx + math.cos(a) * dist, cy + math.sin(a) * dist
            if rng.random() < 0.25 and size > 1.2:
                stretch = rng.uniform(1.6, 3.2)
                ex, ey = math.cos(a) * size * stretch, math.sin(a) * size * stretch
                d.polygon([((x - ex) * S, (y - ey) * S), ((x - math.sin(a) * size) * S, (y + math.cos(a) * size) * S),
                           ((x + ex) * S, (y + ey) * S), ((x + math.sin(a) * size) * S, (y - math.cos(a) * size) * S)], fill=255)
            else:
                blob(x, y, size)
        # Mist: the can's finest spray round it all.
        for _ in range(int(n * 0.8)):
            a = rng.random() * math.tau
            dist = abs(rng.gauss(0, r * 1.1))
            blob(cx + math.cos(a) * dist, cy + math.sin(a) * dist, rng.uniform(0.35, 0.8), rng.randint(90, 200))
        # Runs from the core's foot.
        for _ in range(drips):
            x = cx + rng.uniform(-core * 0.75, core * 0.75)
            y = cy + core * rng.uniform(0.1, 0.5)
            w = rng.uniform(2.2, 5.5)
            length = rng.uniform(30, 240) * (r / 300)
            d.rounded_rectangle([(x - w / 2) * S, y * S, (x + w / 2) * S, (y + length) * S], radius=w / 2 * S, fill=255)
            blob(x, y + length, w * 0.85)
    canvas = canvas.resize((width, height), Image.LANCZOS)
    a = np.asarray(canvas, np.float32) / 255
    out = np.zeros((height, width, 4), np.uint8)
    out[..., 0], out[..., 1], out[..., 2] = colour
    out[..., 3] = (np.clip(a, 0, 1) * 255).astype(np.uint8)
    return Image.fromarray(out)


def halftone(width, height, step, centre, reach, colour, alpha, power=1.5):
    """Round dots on a slanted grid, big near `centre` and gone by `reach`: printed, sharp, at twice the size it is shown."""
    img = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    ang = math.radians(15)
    ca, sa = math.cos(ang), math.sin(ang)
    for j in range(-height // step - 4, (width + height) // step + 4):
        for i in range(-height // step - 4, (width + height) // step + 4):
            gx, gy = i * step, j * step
            x, y = gx * ca - gy * sa, gx * sa + gy * ca
            if not (-step <= x <= width + step and -step <= y <= height + step):
                continue
            dist = math.hypot(x - centre[0], y - centre[1])
            if dist >= reach:
                continue
            r = (1 - dist / reach) ** power * step * 0.5
            if r < 0.8:
                continue
            d.ellipse([x - r, y - r, x + r, y + r], fill=colour + (alpha,))
    return img


def grey_dots(width, height, seed, count):
    """Grey dots here and there: dust and the odd fleck of grey paint, mostly tiny, a few bigger, crisp."""
    rng = random.Random(seed)
    S = 2
    img = Image.new("RGBA", (width * S, height * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    for _ in range(count):
        x, y = rng.random() * width, rng.random() * height
        r = 0.7 + rng.random() ** 5 * 4.5
        g = rng.randint(110, 190)
        d.ellipse([(x - r) * S, (y - r) * S, (x + r) * S, (y + r) * S], fill=(g, g, g + 8, rng.randint(40, 130)))
    return img.resize((width, height), Image.LANCZOS)


def grunge_word(text, font_path, size, seed, erosion=0.32):
    """A word printed and worn: rough edges, ink thinning in patches, fine speckle where it flaked, a few scratches
    through it. Hard-edged where it is still there."""
    rng = np.random.default_rng(seed)
    font = ImageFont.truetype(str(font_path), size)
    probe = ImageDraw.Draw(Image.new("L", (10, 10)))
    box = probe.textbbox((0, 0), text, font=font)
    w, h = box[2] - box[0] + 60, box[3] - box[1] + 60
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).text((30 - box[0], 30 - box[1]), text, font=font, fill=255)
    m = np.asarray(mask, np.float32) / 255

    def noise(cells_x, cells_y):
        g = rng.random((cells_y, cells_x)).astype(np.float32)
        return np.asarray(Image.fromarray(g, mode="F").resize((w, h), Image.BICUBIC), np.float32)

    # Rough edges: the letters read through a little wobble.
    jx = (noise(w // 6, h // 6) - 0.5) * 5
    jy = (noise(w // 6, h // 6) - 0.5) * 5
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    sx = np.clip(xx + jx, 0, w - 1).astype(np.int32)
    sy = np.clip(yy + jy, 0, h - 1).astype(np.int32)
    m = m[sy, sx]
    # Patches where the ink is thin (soft), fine flakes gone (hard), scratches through it.
    patches = 0.5 * noise(w // 160 + 2, h // 160 + 2) + 0.3 * noise(w // 50 + 2, h // 50 + 2) + 0.2 * noise(w // 14 + 2, h // 14 + 2)
    ink = np.clip((patches - erosion) / 0.22, 0, 1)
    flakes = (rng.random((h, w)) > 0.035) & (noise(w // 5 + 2, h // 5 + 2) > 0.16)
    scratches = Image.new("L", (w, h), 0)
    sd = ImageDraw.Draw(scratches)
    for _ in range(int(w / 140)):
        x0, y0 = rng.random() * w, rng.random() * h
        ang = rng.normal(0.15, 0.5)
        length = rng.uniform(60, 420)
        sd.line([(x0, y0), (x0 + math.cos(ang) * length, y0 + math.sin(ang) * length)], fill=255, width=int(rng.integers(1, 4)))
    cut_out = np.asarray(scratches, np.float32) / 255
    a = m * ink * flakes * (1 - cut_out)
    out = np.zeros((h, w, 4), np.uint8)
    out[..., :3] = 255
    out[..., 3] = (np.clip(a, 0, 1) * 255).astype(np.uint8)
    return Image.fromarray(out)


def brush_stroke(width, height, seed):
    """A dry brush stroke in the studio's blue: bristle streaks along it, ragged where the brush came on and lifted off."""
    rng = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:height, 0:width].astype(np.float32)
    u, v = xx / width, (yy - height / 2) / (height / 2)
    # The stroke's edge wobbles along it; its ends are torn by the bristles running dry.
    top = 0.78 + 0.12 * (np.asarray(Image.fromarray((rng.random((1, 40)) * 255).astype(np.uint8)).resize((width, 1), Image.BICUBIC), np.float32)[0] / 255 - 0.5)
    bottom = 0.8 + 0.12 * (np.asarray(Image.fromarray((rng.random((1, 40)) * 255).astype(np.uint8)).resize((width, 1), Image.BICUBIC), np.float32)[0] / 255 - 0.5)
    inside = (v > -top[None, :]) & (v < bottom[None, :])
    streaks = np.asarray(Image.fromarray((rng.random((height, 8)) * 255).astype(np.uint8)).resize((width, height), Image.BICUBIC), np.float32) / 255
    dry = np.clip(np.minimum(u / 0.12, (1 - u) / 0.16), 0, 1)
    a = inside * np.clip((streaks - (1 - dry) * 0.9) * 3 + dry * 0.6, 0, 1)
    a *= (rng.random((height, width)) > 0.04 * (1 - dry) + 0.01)
    out = np.zeros((height, width, 4), np.uint8)
    out[..., 0], out[..., 1], out[..., 2] = BLUE
    out[..., 3] = (np.clip(a, 0, 1) * 255).astype(np.uint8)
    return Image.fromarray(out)


def skater():
    """The studio's skater (the loading picture's right side) as two looks for covers and the hero: a blue duotone, and
    newsprint dots."""
    src = Image.open(REPO / "assets" / "animator_studio" / "loading.jpg").convert("L")
    crop = src.crop((1700, 0, 3072, 1302))  # right of the loading picture's lettering
    arr = np.asarray(crop, np.float32) / 255
    # Duotone: black to the studio's blue to near white, the shadows kept deep.
    t = np.clip((arr - 0.04) / 0.8, 0, 1) ** 0.9
    dark, mid, light = np.array([6, 8, 14]), np.array(BLUE), np.array([235, 244, 255])
    lo = t[..., None] * 2
    rgb = np.where(t[..., None] < 0.5, dark + (mid - dark) * lo, mid + (light - mid) * (lo - 1))
    save_rgb(rgb, "skater_duotone.jpg", 90)
    # Newsprint: round dots on a 45 degree grid, sized by how light it is.
    w, h = crop.size
    step = 9
    img = Image.new("L", (w, h), 12)
    d = ImageDraw.Draw(img)
    ang = math.radians(45)
    ca, sa = math.cos(ang), math.sin(ang)
    for j in range(-(w + h) // step, (w + h) // step):
        for i in range(-(w + h) // step, (w + h) // step):
            gx, gy = i * step, j * step
            x, y = gx * ca - gy * sa, gx * sa + gy * ca
            if 0 <= x < w and 0 <= y < h:
                lum = arr[int(y), int(x)]
                r = step * 0.62 * math.sqrt(max(0.0, lum - 0.03))
                if r > 0.4:
                    d.ellipse([x - r, y - r, x + r, y + r], fill=236)
    img.convert("RGB").save(OUT / "skater_halftone.jpg", quality=90, optimize=True)
    print("made skater_halftone.jpg")
    save_rgb(np.dstack([arr * 255] * 3), "skater_bw.jpg", 90)


def wallpaper():
    """The wall behind the MARKET: dark concrete with its grain showing, grip-tape grit over it, blue spray in two
    corners and one low down, the worn "market." low across it, blue halftone in a corner, grey flecks. 3840 x 1620
    (21:9; a 16:9 screen sees its middle)."""
    W, H = 3840, 1620
    conc = np.asarray(Image.open(OUT / "concrete.jpg").convert("L"), np.float32) / 255
    grip = np.asarray(Image.open(OUT / "grip.jpg").convert("L"), np.float32) / 255
    c = np.tile(conc, (H // T + 1, W // T + 1))[:H, :W]
    g = np.tile(grip, (H // T + 1, W // T + 1))[:H, :W]
    v = 0.115 + 0.30 * (c - 0.5) + 0.25 * np.clip(g - 0.12, 0, 1)
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    light = np.exp(-(((xx - W * 0.45) / (W * 0.7)) ** 2 + ((yy - H * 0.05) / (H * 0.9)) ** 2))
    edge = np.clip(1.15 - 0.55 * np.sqrt(((xx - W / 2) / (W * 0.55)) ** 2 + ((yy - H * 0.45) / (H * 0.65)) ** 2), 0.45, 1)
    v = v * (0.75 + 0.35 * light) * edge
    rgb = np.dstack([v * 255 * 0.84, v * 255 * 0.92, v * 255 * 1.12])
    img = Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8)).convert("RGBA")
    word = grunge_word("market.", FONTS / "Montserrat-ExtraBold.ttf", 860, 7, 0.36)
    word.putalpha(word.getchannel("A").point(lambda p: int(p * 0.085)))
    img.alpha_composite(word, (int(W * 0.5 - word.width / 2), int(H - word.height + 70)))
    img.alpha_composite(halftone(1500, 1100, 22, (1500, 1100), 1250, BLUE, 110), (W - 1500, H - 1100))
    img.alpha_composite(halftone(1000, 760, 20, (0, 0), 860, (170, 178, 196), 30), (0, 0))
    img.alpha_composite(spray(1500, 1100, 11, [(1180, 760, 330, 4200), (560, 980, 120, 700)], drips=7), (W - 1500, H - 1100))
    img.alpha_composite(spray(1100, 900, 12, [(210, 230, 300, 3600)], drips=5), (-120, -60))
    img.alpha_composite(spray(700, 560, 13, [(360, 260, 120, 900)], drips=3), (int(W * 0.16), H - 560))
    img.alpha_composite(grey_dots(W, H, 14, 420))
    img.convert("RGB").save(OUT / "wallpaper.jpg", quality=92, optimize=True, progressive=True)
    print("made wallpaper.jpg", (W, H))


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    grip_tape()
    concrete()
    paper_white()
    paper_blue()
    save_rgba(spray(1400, 1000, 21, [(900, 500, 300, 3200)], drips=8), "splatter_a.png")
    save_rgba(spray(900, 700, 22, [(420, 300, 190, 1600), (720, 540, 70, 300)], drips=4), "splatter_b.png")
    save_rgba(halftone(1600, 1200, 22, (1600, 1200), 1350, BLUE, 140), "halftone_blue.png")
    save_rgba(grey_dots(1600, 1000, 31, 160), "dots_grey.png")
    save_rgba(grunge_word("market.", FONTS / "Montserrat-ExtraBold.ttf", 900, 7, 0.36), "word_market.png")
    save_rgba(grunge_word("MARKETPLACE", FONTS / "Anton-Regular.ttf", 520, 8, 0.3), "word_marketplace.png")
    save_rgba(brush_stroke(1400, 260, 41), "brush_blue.png")
    skater()
    wallpaper()


if __name__ == "__main__":
    main()
