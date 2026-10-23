"""The MARKET's wallpaper (assets/animator_studio/market.jpg, built into ReSkate.dll): a dark concrete wall in the studio's blue, a giant spray-painted "market.", a kickflip drawn
as keyframes along its arc, blue spray paint with drips, stickers and tape. 1920 x 1080, written as a JPEG."""
import math
import random
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageChops

W, H = 1920, 1080
REPO = Path(__file__).resolve().parents[2]
FONTS = REPO / "External" / "fonts"
HEAVY = str(FONTS / "Montserrat-ExtraBold.ttf")
MARKER = str(FONTS / "PermanentMarker-Regular.ttf")
BLUE = (1, 131, 255)
OUT = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO / "assets" / "animator_studio" / "market.jpg"
rng = np.random.default_rng(23)
random.seed(23)


def noise(scale, octaves=5, persistence=0.55):
    """Fractal value noise in 0..1, by blurred random fields stacked."""
    total = np.zeros((H, W), np.float32)
    amp, norm = 1.0, 0.0
    for o in range(octaves):
        s = max(1, int(scale / (2 ** o)))
        small = rng.random((H // s + 2, W // s + 2)).astype(np.float32)
        img = Image.fromarray((small * 255).astype(np.uint8)).resize((W + 2 * s, H + 2 * s), Image.BICUBIC)
        arr = np.asarray(img, np.float32)[s:s + H, s:s + W] / 255.0
        total += arr * amp
        norm += amp
        amp *= persistence
    return total / norm


def to_img(arr):
    return Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8))


# ---- the wall ----
base = noise(220, 6)
fine = noise(6, 2)
speck = (rng.random((H, W)) > 0.9965).astype(np.float32)
speck = np.asarray(Image.fromarray((speck * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.7)), np.float32) / 255
stain = noise(420, 3)
lum = 18 + 30 * base + 10 * (fine - 0.5) - 26 * speck * (rng.random((H, W)) > 0.5) + 14 * speck * (rng.random((H, W)) > 0.5)
lum -= 14 * np.clip((stain - 0.55) * 4, 0, 1)
# A cold light from above the middle-left, and the edges falling into the dark.
yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
light = np.exp(-(((xx - W * 0.42) / (W * 0.55)) ** 2 + ((yy + H * 0.15) / (H * 0.75)) ** 2))
vignette = np.clip(1.15 - 0.85 * np.sqrt(((xx - W / 2) / (W * 0.62)) ** 2 + ((yy - H * 0.45) / (H * 0.62)) ** 2), 0.15, 1)
lum = lum * (0.55 + 0.75 * light) * vignette
r = lum * 0.82
g = lum * 0.92
b = lum * 1.12 + 10 * light
wall = np.dstack([r, g, b])
img = to_img(wall).convert("RGBA")


def layer():
    return Image.new("RGBA", (W, H), (0, 0, 0, 0))


def paste(dst, src, opacity=1.0):
    if opacity < 1:
        a = src.getchannel("A").point(lambda v: int(v * opacity))
        src = src.copy()
        src.putalpha(a)
    return Image.alpha_composite(dst, src)


# ---- halftone in two corners ----
ht = layer()
d = ImageDraw.Draw(ht)
for cx, cy, reach, col in ((W - 40, H + 40, 760, BLUE), (-40, 120, 520, (255, 255, 255))):
    step = 22
    for y in range(-step, H + step, step):
        for x in range(-step, W + step, step):
            off = step / 2 if (y // step) % 2 else 0
            px, py = x + off, y
            dist = math.hypot(px - cx, py - cy)
            if dist > reach:
                continue
            rad = (1 - dist / reach) ** 1.6 * step * 0.46
            if rad < 0.6:
                continue
            d.ellipse([px - rad, py - rad, px + rad, py + rad], fill=col + (70 if col == BLUE else 26,))
img = paste(img, ht)

# ---- the giant word, sprayed on the wall ----
word = layer()
wd = ImageDraw.Draw(word)
font = ImageFont.truetype(HEAVY, 400)
text = "market."
box = wd.textbbox((0, 0), text, font=font)
tw, th = box[2] - box[0], box[3] - box[1]
tx, ty = int(W * 0.5 - tw / 2) - box[0], int(H - th - 70) - box[1]
wd.text((tx, ty), text, font=font, fill=(240, 244, 255, 255))
word = word.rotate(-3.5, resample=Image.BICUBIC, center=(W / 2, H * 0.75))
# Sprayed: soft edge, broken up by the wall, a ghost of overspray round it.
grit = (noise(5, 2) > 0.36).astype(np.float32) * (noise(60, 3) > 0.32).astype(np.float32)
alpha = np.asarray(word.getchannel("A"), np.float32) / 255
soft = np.asarray(word.getchannel("A").filter(ImageFilter.GaussianBlur(9)), np.float32) / 255
a = np.clip(alpha * (0.55 + 0.45 * grit) * 0.13 + soft * 0.05, 0, 1)
word_layer = Image.fromarray(np.dstack([np.full((H, W), 236, np.uint8), np.full((H, W), 242, np.uint8), np.full((H, W), 255, np.uint8),
                                        (a * 255).astype(np.uint8)]))
img = paste(img, word_layer)


# ---- blue spray paint: a stroke with drips, and splatter ----
def spray_stroke(points, width, density, colour, drips=0):
    lay = np.zeros((H, W), np.float32)
    pts = []
    for (x0, y0), (x1, y1) in zip(points, points[1:]):
        n = int(math.hypot(x1 - x0, y1 - y0) / 3) + 1
        for i in range(n):
            t = i / n
            pts.append((x0 + (x1 - x0) * t, y0 + (y1 - y0) * t))
    core = Image.new("L", (W, H), 0)
    cd = ImageDraw.Draw(core)
    for i, (x, y) in enumerate(pts):
        wv = width * (0.75 + 0.25 * math.sin(i * 0.05))
        cd.ellipse([x - wv, y - wv, x + wv, y + wv], fill=255)
    lay = np.asarray(core.filter(ImageFilter.GaussianBlur(width * 0.22)), np.float32) / 255
    # overspray dots
    dots = Image.new("L", (W, H), 0)
    dd = ImageDraw.Draw(dots)
    for _ in range(density):
        x, y = random.choice(pts)
        ang = random.random() * math.tau
        dist = abs(random.gauss(0, width * 1.1)) + width * 0.6
        rr = random.choice((0.8, 1.0, 1.3, 1.8, 2.6))
        px, py = x + math.cos(ang) * dist, y + math.sin(ang) * dist
        dd.ellipse([px - rr, py - rr, px + rr, py + rr], fill=random.randint(120, 255))
    lay = np.maximum(lay, np.asarray(dots, np.float32) / 255)
    # drips: runs down from the stroke's lower edge
    drip = Image.new("L", (W, H), 0)
    dr = ImageDraw.Draw(drip)
    for _ in range(drips):
        x, y = random.choice(pts)
        y += width * 0.6
        length = random.uniform(40, 220)
        wdt = random.uniform(2.5, 6)
        dr.rounded_rectangle([x - wdt / 2, y, x + wdt / 2, y + length], radius=wdt / 2, fill=255)
        dr.ellipse([x - wdt * 0.9, y + length - wdt * 0.9, x + wdt * 0.9, y + length + wdt * 0.9], fill=255)
    lay = np.maximum(lay, np.asarray(drip.filter(ImageFilter.GaussianBlur(0.8)), np.float32) / 255)
    grain = 0.78 + 0.22 * noise(3, 1)
    a = np.clip(lay * grain, 0, 1)
    out = np.zeros((H, W, 4), np.uint8)
    out[..., 0], out[..., 1], out[..., 2] = colour
    out[..., 3] = (a * 255).astype(np.uint8)
    return Image.fromarray(out)


stroke = spray_stroke([(1180, 905), (1360, 880), (1560, 892), (1760, 860), (1990, 840)], 34, 2600, BLUE, drips=9)
img = paste(img, stroke, 0.92)
stroke2 = spray_stroke([(-60, 330), (90, 300), (230, 318)], 22, 900, BLUE, drips=4)
img = paste(img, stroke2, 0.75)
splat = layer()
sd = ImageDraw.Draw(splat)
for cx, cy, n, spread in ((1650, 250, 260, 120), (300, 860, 180, 90), (980, 140, 90, 60)):
    for _ in range(n):
        ang = random.random() * math.tau
        dist = abs(random.gauss(0, spread))
        rr = max(0.8, random.expovariate(0.6))
        px, py = cx + math.cos(ang) * dist, cy + math.sin(ang) * dist
        sd.ellipse([px - rr, py - rr, px + rr, py + rr], fill=BLUE + (random.randint(110, 230),))
img = paste(img, splat, 0.8)


# ---- the kickflip, as keyframes: the board along its arc, rolling once round its length ----
def board(draw_layer, cx, cy, length, width, pitch, roll, alpha):
    """A deck seen from the side, its nose turned up by `pitch`, rolled `roll` about its own length (a kickflip): its
    edge with the wheels under it at 0, its graphic at a quarter turn, its wheels on top at a half, its grip at three
    quarters."""
    s, c = math.sin(roll), math.cos(roll)
    face = max(7.0, width * abs(s))          # how tall the deck shows
    lay = Image.new("RGBA", (int(length + 120), int(width + 120)), (0, 0, 0, 0))
    ld = ImageDraw.Draw(lay)
    ox, oy = 60, 60 + (width - face) / 2
    graphic = s > 0
    if face > 12:
        body = BLUE if graphic else (24, 26, 32)
        ld.rounded_rectangle([ox, oy, ox + length, oy + face], radius=min(face / 2, 22), fill=body + (alpha,),
                             outline=(245, 247, 252, alpha), width=3)
        if graphic and face > 24:
            f = ImageFont.truetype(HEAVY, int(min(face * 0.42, 20)))
            ld.text((ox + length / 2, oy + face / 2), "reskate.", font=f, fill=(255, 255, 255, alpha), anchor="mm")
    else:
        # on its edge: the deck's line, its kicks turned up
        ld.line([(ox + 10, oy + face / 2), (ox + length - 10, oy + face / 2)], fill=(245, 247, 252, alpha), width=7)
        ld.line([(ox - 2, oy + face / 2 - 8), (ox + 12, oy + face / 2)], fill=(245, 247, 252, alpha), width=7)
        ld.line([(ox + length - 12, oy + face / 2), (ox + length + 2, oy + face / 2 - 8)], fill=(245, 247, 252, alpha), width=7)
    # the trucks and wheels, on the side away from the grip, seen while the deck is not flat to us
    if abs(c) > 0.25:
        side = 1 if c > 0 else -1            # under (1) or over (-1) the deck
        edge = oy + face / 2 + side * (face / 2 + 2)
        reach = 18 * abs(c)
        for tx in (ox + length * 0.22, ox + length * 0.78):
            ld.rectangle([tx - 3, min(edge, edge + side * reach), tx + 3, max(edge, edge + side * reach)], fill=(200, 205, 215, alpha))
            wy = edge + side * reach
            ld.ellipse([tx - 10, wy - 9, tx + 10, wy + 9], fill=(250, 250, 250, alpha))
    lay = lay.rotate(math.degrees(pitch), resample=Image.BICUBIC, expand=True)
    draw_layer.alpha_composite(lay, (int(cx - lay.width / 2), int(cy - lay.height / 2)))


flip = layer()
frames = 7
x0, x1 = 980, 1830
base_y, peak = 470, 255
keys = []
for i in range(frames):
    t_ = i / (frames - 1)
    x = x0 + (x1 - x0) * t_
    y = base_y - 4 * (base_y - peak) * t_ * (1 - t_)
    roll = t_ * math.tau
    pitch = math.radians(18 * (1 - 2 * t_))   # nose up at the pop, level at the top, nose down to land
    fade = 255 if i in (0, frames - 1) else int(120 + 135 * math.sin(math.pi * t_))
    board(flip, x, y, 128, 44, pitch, roll, fade)
    keys.append((x, y))
# its path, dotted, and motion streaks behind the boards
pd = ImageDraw.Draw(flip)
for i in range(120):
    t = i / 119
    x = x0 + (x1 - x0) * t
    y = base_y - 4 * (base_y - peak) * t * (1 - t) + 62
    if i % 3 == 0:
        pd.ellipse([x - 2, y - 2, x + 2, y + 2], fill=(255, 255, 255, 70))
img = paste(img, flip)
# the timeline under it: a ruler and a diamond for each key
tl = layer()
td = ImageDraw.Draw(tl)
ty = 580
td.rectangle([x0 - 60, ty - 1, x1 + 60, ty + 1], fill=(255, 255, 255, 110))
for i in range(0, 61):
    x = x0 - 60 + (x1 - x0 + 120) * i / 60
    h = 10 if i % 5 == 0 else 5
    td.rectangle([x - 0.6, ty - h, x + 0.6, ty], fill=(255, 255, 255, 90))
for i, (x, y) in enumerate(keys):
    s = 9 if i in (0, len(keys) - 1) else 7
    col = (255, 196, 0, 235) if i in (0, len(keys) - 1) else (255, 255, 255, 230)
    td.polygon([(x, ty - s), (x + s, ty), (x, ty + s), (x - s, ty)], fill=col)
    td.line([(x, y + 34), (x, ty - 12)], fill=(255, 255, 255, 28), width=1)
# the playhead
px = keys[3][0] + 30
td.rectangle([px - 1.5, ty - 70, px + 1.5, ty + 16], fill=BLUE + (255,))
td.polygon([(px - 9, ty - 82), (px + 9, ty - 82), (px, ty - 68)], fill=BLUE + (255,))
small = ImageFont.truetype(HEAVY, 15)
td.text((x0 - 60, ty + 16), "KICKFLIP  -  7 KEYS", font=small, fill=(255, 255, 255, 120))
img = paste(img, tl)


# ---- stickers and tape ----
def sticker(text_lines, font_path, size, fg, bg, angle, at, pad=(26, 16), radius=14, outline=None):
    f = ImageFont.truetype(font_path, size)
    dummy = ImageDraw.Draw(Image.new("RGBA", (10, 10)))
    widths = [dummy.textbbox((0, 0), t, font=f) for t in text_lines]
    tw = max(b[2] - b[0] for b in widths)
    line_h = size * 1.08
    th = line_h * len(text_lines)
    sw, sh = int(tw + pad[0] * 2), int(th + pad[1] * 2)
    s = Image.new("RGBA", (sw + 40, sh + 40), (0, 0, 0, 0))
    sd = ImageDraw.Draw(s)
    sd.rounded_rectangle([20, 20, 20 + sw, 20 + sh], radius=radius, fill=bg)
    if outline:
        sd.rounded_rectangle([24, 24, 16 + sw, 16 + sh], radius=max(2, radius - 4), outline=outline, width=3)
    for i, t in enumerate(text_lines):
        b = sd.textbbox((0, 0), t, font=f)
        sd.text((20 + (sw - (b[2] - b[0])) / 2 - b[0], 20 + pad[1] + i * line_h - b[1] + (line_h - size) / 2), t, font=f, fill=fg)
    # worn: a little of the wall shows through
    a = np.asarray(s.getchannel("A"), np.float32)
    wear = np.asarray(Image.fromarray((rng.random((s.height, s.width)) * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(1.2)), np.float32)
    a = a * np.clip((wear - 60) / 40, 0.55, 1)
    s.putalpha(Image.fromarray(a.astype(np.uint8)))
    s = s.rotate(angle, resample=Image.BICUBIC, expand=True)
    shadow = Image.new("RGBA", s.size, (0, 0, 0, 0))
    shadow.putalpha(s.getchannel("A").point(lambda v: int(v * 0.55)).filter(ImageFilter.GaussianBlur(10)))
    lay = layer()
    lay.alpha_composite(shadow, (int(at[0] - s.width / 2 + 8), int(at[1] - s.height / 2 + 12)))
    lay.alpha_composite(s, (int(at[0] - s.width / 2), int(at[1] - s.height / 2)))
    return lay


def tape(at, length, angle, alpha=150):
    t = Image.new("RGBA", (int(length), 34), (238, 236, 226, alpha))
    td = ImageDraw.Draw(t)
    for x in range(0, int(length), 7):
        td.polygon([(x, 0), (x + 4, 0), (x + 2, 3)], fill=(0, 0, 0, 0))
        td.polygon([(x, 34), (x + 4, 34), (x + 2, 31)], fill=(0, 0, 0, 0))
    t = t.rotate(angle, resample=Image.BICUBIC, expand=True)
    lay = layer()
    lay.alpha_composite(t, (int(at[0] - t.width / 2), int(at[1] - t.height / 2)))
    return lay


img = paste(img, sticker(["FRESH", "DROPS"], HEAVY, 46, (255, 255, 255, 255), BLUE + (255,), 8, (175, 640)))
img = paste(img, tape((150, 578), 120, 30))
img = paste(img, sticker(["CREW PICKS"], HEAVY, 30, BLUE + (255,), (244, 244, 240, 255), -6, (1780, 700), outline=BLUE + (255,)))
img = paste(img, sticker(["KEY IT.", "SKATE IT.", "SHARE IT."], HEAVY, 26, (255, 255, 255, 255), (14, 14, 16, 255), -9, (420, 300), outline=(255, 255, 255, 200)))
img = paste(img, tape((470, 225), 140, -24, 130))
img = paste(img, sticker(["the trick shop"], MARKER, 40, (255, 255, 255, 255), (0, 0, 0, 0), 4, (1530, 155), pad=(10, 6)))
img = paste(img, sticker(["reskate.", "animator studio"], HEAVY, 22, (255, 255, 255, 235), (0, 0, 0, 0), 0, (1795, 1020), pad=(6, 4)))

# ---- grain over everything, and the light once more ----
rgb = np.asarray(img.convert("RGB"), np.float32)
grain = (rng.random((H, W)) - 0.5) * 16
rgb += grain[..., None]
out = Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8))
out.save(OUT, quality=90, optimize=True, progressive=True)
print("wrote", OUT, out.size)
